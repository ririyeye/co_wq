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
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <basetsd.h>
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

using namespace co_wq;

namespace {

std::atomic_bool               g_stop { false };
std::atomic<int>               g_listener_fd { -1 };
std::atomic<std::atomic_bool*> g_finished_ptr { nullptr };

#if defined(_WIN32)
static BOOL WINAPI console_ctrl_handler(DWORD type)
{
    if (type == CTRL_C_EVENT) {
        g_stop.store(true, std::memory_order_release);
        int fd = g_listener_fd.exchange(-1, std::memory_order_acq_rel);
        if (fd != -1)
            ::closesocket((SOCKET)fd);
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
    int fd = g_listener_fd.exchange(-1, std::memory_order_acq_rel);
    if (fd != -1)
        ::close(fd);
    if (auto* flag = g_finished_ptr.load(std::memory_order_acquire))
        flag->store(true, std::memory_order_release);
}
#endif

template <typename Socket> Task<void, Work_Promise<SpinLock, void>> handle_websocket(Socket sock)
{
    if constexpr (requires(Socket& s) { s.handshake(); }) {
        int hs = co_await sock.handshake();
        if (hs != 0) {
            std::cerr << "[ws] tls handshake failed, code=" << hs << "\n";
            sock.close();
            co_return;
        }
    }

    net::websocket::server_options ws_opts;
    ws_opts.close_on_failure = true;

    auto accept_res = co_await net::websocket::accept<SpinLock>(sock, ws_opts);
    if (!accept_res.ok) {
        if (accept_res.status_code == 0) {
            std::cout << "[ws] handshake cancelled: " << accept_res.reason << "\n";
        } else {
            std::cerr << "[ws] handshake failed: " << accept_res.reason << " (status=" << accept_res.status_code
                      << ")\n";
            if (!accept_res.request.headers.empty()) {
                std::cerr << "[ws] request headers:" << '\n';
                for (const auto& [key, value] : accept_res.request.headers) {
                    std::cerr << "  " << key << ": " << value << '\n';
                }
            }
            if (!accept_res.request.raw_data.empty()) {
                std::cerr << "[ws] raw request:" << '\n' << accept_res.request.raw_data;
                if (accept_res.request.raw_data.back() != '\n')
                    std::cerr << '\n';
            }
        }
        co_return;
    }

    std::cout << "[ws] client connected, path=" << accept_res.request.url;
    if (!accept_res.selected_subprotocol.empty())
        std::cout << ", subprotocol=" << accept_res.selected_subprotocol;
    std::cout << "\n";

    for (;;) {
        auto msg_res = co_await net::websocket::read_message<SpinLock>(sock);
        if (!msg_res.ok) {
            int err = -msg_res.error;
            if (msg_res.error == -ECONNRESET || msg_res.error == -EPIPE) {
                std::cout << "[ws] peer closed connection";
                if (err > 0)
                    std::cout << " (" << err << ": " << std::strerror(err) << ")";
                std::cout << "\n";
            } else {
                std::cerr << "[ws] read error: " << msg_res.error;
                if (err > 0)
                    std::cerr << " (" << err << ": " << std::strerror(err) << ")";
                std::cerr << "\n";
            }
            break;
        }
        auto& msg = msg_res.value;
        if (msg.closed) {
            std::cout << "[ws] received close, code=" << msg.close_code;
            if (!msg.close_reason.empty())
                std::cout << ", reason=" << msg.close_reason;
            std::cout << "\n";
            co_await net::websocket::send_close<SpinLock>(sock, msg.close_code, msg.close_reason);
            break;
        }

        if (msg.op == net::websocket::opcode::text) {
            std::string text(msg.payload.begin(), msg.payload.end());
            std::cout << "[ws] text message: " << text << "\n";
            co_await net::websocket::send_text<SpinLock>(sock, text);
        } else if (msg.op == net::websocket::opcode::binary) {
            std::cout << "[ws] binary message, size=" << msg.payload.size() << " bytes\n";
            co_await net::websocket::send_binary<SpinLock>(sock, msg.payload);
        }
    }

    sock.close();
    co_return;
}

Task<void, Work_Promise<SpinLock, void>> websocket_server(net::fd_workqueue<SpinLock>& fdwq,
                                                          std::string                  host,
                                                          uint16_t                     port
#if defined(USING_SSL)
                                                          ,
                                                          const net::tls_context* tls_ctx
#endif
)
{
    net::tcp_listener<SpinLock> listener(fdwq.base(), fdwq.reactor());
    listener.bind_listen(host, port, 128);
    g_listener_fd.store(listener.native_handle(), std::memory_order_release);

    std::cout << "[ws] listening on " << host << ':' << port << "\n";

    while (!g_stop.load(std::memory_order_acquire)) {
        int fd = co_await listener.accept();
        if (fd == net::k_accept_fatal) {
            std::cerr << "[ws] accept fatal error, shutting down\n";
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
                std::cerr << "[ws] tls socket setup failed: " << ex.what() << "\n";
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
    g_listener_fd.store(-1, std::memory_order_release);
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

    auto&                       wq = get_sys_workqueue(0);
    net::fd_workqueue<SpinLock> fdwq(wq);

    const net::tls_context* tls_ctx_ptr = nullptr;
#if defined(USING_SSL)
    std::optional<net::tls_context> tls_ctx;
    if (use_tls) {
        if (cert_path.empty() || key_path.empty()) {
            std::cerr << "[ws] missing --cert/--key when TLS enabled\n";
            return 1;
        }
        try {
            tls_ctx.emplace(net::tls_context::make_server_with_pem(cert_path, key_path));
            tls_ctx_ptr = &(*tls_ctx);
            std::cout << "[ws] TLS enabled, cert=" << cert_path << "\n";
        } catch (const std::exception& ex) {
            std::cerr << "[ws] failed to setup TLS: " << ex.what() << "\n";
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
