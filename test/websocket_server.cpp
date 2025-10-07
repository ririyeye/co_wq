#include "syswork.hpp"

#include "tcp_listener.hpp"
#include "tcp_socket.hpp"
#if defined(USING_SSL)
#include "tls.hpp"
#endif
#include "fd_base.hpp"
#include "websocket.hpp"
#include "worker.hpp"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#include <basetsd.h>
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

using namespace co_wq;

using NetFdWorkqueue = net::fd_workqueue<SpinLock, net::epoll_reactor>;

namespace {

inline std::string errno_message(int err)
{
    if (err <= 0)
        return {};
    std::error_code ec(err, std::system_category());
    return ec.message();
}

std::atomic_bool               g_stop { false };
std::atomic<net::os::fd_t>     g_listener_fd { net::os::invalid_fd() };
std::atomic<std::atomic_bool*> g_finished_ptr { nullptr };

#if defined(_WIN32)
static BOOL WINAPI console_ctrl_handler(DWORD type)
{
    if (type == CTRL_C_EVENT) {
        g_stop.store(true, std::memory_order_release);
        auto fd = g_listener_fd.exchange(net::os::invalid_fd(), std::memory_order_acq_rel);
        if (fd != net::os::invalid_fd())
            net::os::close_fd(fd);
        if (auto* flag = g_finished_ptr.load(std::memory_order_acquire))
            flag->store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}
#else
void sigint_handler(int)
{
    g_stop.store(true, std::memory_order_release);
    auto fd = g_listener_fd.exchange(net::os::invalid_fd(), std::memory_order_acq_rel);
    if (fd != net::os::invalid_fd())
        net::os::close_fd(fd);
    if (auto* flag = g_finished_ptr.load(std::memory_order_acquire))
        flag->store(true, std::memory_order_release);
}
#endif

template <typename Socket> Task<void, Work_Promise<SpinLock, void>> handle_websocket(Socket sock)
{
    if constexpr (requires(Socket& s) { s.handshake(); }) {
        int hs = co_await sock.handshake();
        if (hs != 0) {
            CO_WQ_LOG_ERROR("[ws] tls handshake failed, code=%d", hs);
            sock.close();
            co_return;
        }
    }

    net::websocket::server_options ws_opts;
    ws_opts.close_on_failure = true;

    auto accept_res = co_await net::websocket::accept<SpinLock>(sock, ws_opts);
    if (!accept_res.ok) {
        if (accept_res.status_code == 0) {
            CO_WQ_LOG_INFO("[ws] handshake cancelled: %s", accept_res.reason.c_str());
        } else {
            CO_WQ_LOG_ERROR("[ws] handshake failed: %s (status=%u)",
                            accept_res.reason.c_str(),
                            static_cast<unsigned>(accept_res.status_code));
            if (!accept_res.request.headers.empty()) {
                CO_WQ_LOG_ERROR("[ws] request headers:");
                for (const auto& [key, value] : accept_res.request.headers) {
                    CO_WQ_LOG_ERROR("  %s: %s", key.c_str(), value.c_str());
                }
            }
            if (!accept_res.request.raw_data.empty()) {
                std::string raw = accept_res.request.raw_data;
                if (!raw.empty() && raw.back() != '\n')
                    raw.push_back('\n');
                CO_WQ_LOG_ERROR("[ws] raw request:\n%s", raw.c_str());
            }
        }
        co_return;
    }

    std::string connect_msg = fmt::sprintf("[ws] client connected, path=%s", accept_res.request.url.c_str());
    if (!accept_res.selected_subprotocol.empty()) {
        connect_msg += fmt::sprintf(", subprotocol=%s", accept_res.selected_subprotocol.c_str());
    }
    CO_WQ_LOG_INFO("%s", connect_msg.c_str());

    for (;;) {
        auto msg_res = co_await net::websocket::read_message<SpinLock>(sock);
        if (!msg_res.ok) {
            int err = -msg_res.error;
            if (msg_res.error == -ECONNRESET || msg_res.error == -EPIPE) {
                std::string peer_msg = "[ws] peer closed connection";
                if (err > 0) {
                    const auto err_text = errno_message(err);
                    if (!err_text.empty())
                        peer_msg += fmt::sprintf(" (%d: %s)", err, err_text.c_str());
                }
                CO_WQ_LOG_INFO("%s", peer_msg.c_str());
            } else {
                std::string err_msg = fmt::sprintf("[ws] read error: %d", msg_res.error);
                if (err > 0) {
                    const auto err_text = errno_message(err);
                    if (!err_text.empty())
                        err_msg += fmt::sprintf(" (%d: %s)", err, err_text.c_str());
                }
                CO_WQ_LOG_ERROR("%s", err_msg.c_str());
            }
            break;
        }
        auto& msg = msg_res.value;
        if (msg.closed) {
            std::string close_msg = fmt::sprintf("[ws] received close, code=%u", static_cast<unsigned>(msg.close_code));
            if (!msg.close_reason.empty())
                close_msg += fmt::sprintf(", reason=%s", msg.close_reason.c_str());
            CO_WQ_LOG_INFO("%s", close_msg.c_str());
            co_await net::websocket::send_close<SpinLock>(sock, msg.close_code, msg.close_reason);
            break;
        }

        if (msg.op == net::websocket::opcode::text) {
            std::string text(msg.payload.begin(), msg.payload.end());
            CO_WQ_LOG_INFO("[ws] text message: %s", text.c_str());
            co_await net::websocket::send_text<SpinLock>(sock, text);
        } else if (msg.op == net::websocket::opcode::binary) {
            CO_WQ_LOG_INFO("[ws] binary message, size=%zu bytes", msg.payload.size());
            co_await net::websocket::send_binary<SpinLock>(sock, msg.payload);
        }
    }

    sock.close();
    co_return;
}

Task<void, Work_Promise<SpinLock, void>> websocket_server(NetFdWorkqueue& fdwq,
                                                          std::string     host,
                                                          uint16_t        port
#if defined(USING_SSL)
                                                          ,
                                                          const net::tls_context* tls_ctx
#endif
)
{
    net::tcp_listener<SpinLock> listener(fdwq.base(), fdwq.reactor());
    listener.bind_listen(host, port, 128);
    g_listener_fd.store(listener.native_handle(), std::memory_order_release);

    CO_WQ_LOG_INFO("[ws] listening on %s:%u", host.c_str(), static_cast<unsigned>(port));

    while (!g_stop.load(std::memory_order_acquire)) {
        int fd = co_await listener.accept();
        if (fd == net::k_accept_fatal) {
            CO_WQ_LOG_ERROR("[ws] accept fatal error, shutting down");
            break;
        }
        if (fd < 0)
            continue;
#if defined(USING_SSL)
        if (tls_ctx) {
            try {
                auto tls_sock = fdwq.adopt_tls_socket(fd, *tls_ctx, net::tls_mode::Server);
                auto task     = handle_websocket(std::move(tls_sock));
                post_to(task, fdwq.base());
            } catch (const std::exception& ex) {
                CO_WQ_LOG_ERROR("[ws] tls socket setup failed: %s", ex.what());
            }
        } else
#endif
        {
            auto socket = fdwq.adopt_tcp_socket(fd);
            auto task   = handle_websocket(std::move(socket));
            post_to(task, fdwq.base());
        }
    }

    listener.close();
    g_listener_fd.store(net::os::invalid_fd(), std::memory_order_release);
    co_return;
}

} // namespace

int main(int argc, char* argv[])
{
    std::string host = "0.0.0.0";
    uint16_t    port = 9000;
    std::string cert_path;
    std::string key_path;
    bool        use_tls = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--cert" && i + 1 < argc) {
            cert_path = argv[++i];
            use_tls   = true;
        } else if (arg == "--key" && i + 1 < argc) {
            key_path = argv[++i];
            use_tls  = true;
        }
    }

#if defined(_WIN32)
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    std::signal(SIGINT, sigint_handler);
#endif

    auto&          wq = get_sys_workqueue(0);
    NetFdWorkqueue fdwq(wq);

    const net::tls_context* tls_ctx_ptr = nullptr;
#if defined(USING_SSL)
    std::optional<net::tls_context> tls_ctx;
    if (use_tls) {
        if (cert_path.empty() || key_path.empty()) {
            CO_WQ_LOG_ERROR("[ws] missing --cert/--key when TLS enabled");
            return 1;
        }
        try {
            tls_ctx.emplace(net::tls_context::make_server_with_pem(cert_path, key_path));
            tls_ctx_ptr = &(*tls_ctx);
            CO_WQ_LOG_INFO("[ws] TLS enabled, cert=%s", cert_path.c_str());
        } catch (const std::exception& ex) {
            CO_WQ_LOG_ERROR("[ws] failed to setup TLS: %s", ex.what());
            return 1;
        }
    }
#endif

    auto server_task = websocket_server(fdwq,
                                        host,
                                        port
#if defined(USING_SSL)
                                        ,
                                        tls_ctx_ptr
#endif
    );
    auto             coroutine = server_task.get();
    auto&            promise   = coroutine.promise();
    std::atomic_bool finished { false };
    promise.mUserData    = &finished;
    promise.mOnCompleted = [](Promise_base& pb) {
        auto* flag = static_cast<std::atomic_bool*>(pb.mUserData);
        if (flag)
            flag->store(true, std::memory_order_release);
    };
    g_finished_ptr.store(&finished, std::memory_order_release);

    post_to(server_task, wq);

    sys_wait_until(finished);
    g_finished_ptr.store(nullptr, std::memory_order_release);
    return 0;
}
