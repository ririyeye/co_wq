
// echo.cpp
#include "co_syswork.hpp"
#include "co_test_sys_stats_logger.hpp"

#if defined(USING_NET)
#include "dns_resolver.hpp"
#include "fd_base.hpp"
#include "tcp_listener.hpp"
#include "tcp_socket.hpp"
#include "udp_socket.hpp"
#include "websocket.hpp"
#if defined(USING_SSL)
#include "tls.hpp"
#endif
#include <array>
#include <atomic>
#include <chrono>
#ifndef _WIN32
#include <csignal>
#endif
#include <cctype>
#include <cstring>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <basetsd.h>
using ssize_t = SSIZE_T;
#endif

using namespace co_wq;

// ---- Server global control & stats ----
static std::atomic_bool                      g_stop { false };
static std::atomic<uint64_t>                 g_conn_count { 0 };
static std::atomic<uint64_t>                 g_bytes_echoed { 0 };
static std::atomic<net::os::fd_t>            g_listener_fd { net::os::invalid_fd() }; // for forced close
static std::atomic<net::os::fd_t>            g_ws_listener_fd { net::os::invalid_fd() };
static std::chrono::steady_clock::time_point g_server_start;

using NetFdWorkqueue = net::fd_workqueue<SpinLock, net::epoll_reactor>;

namespace {

std::string base64_encode(const unsigned char* data, size_t len)
{
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string           out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < len) {
        unsigned triple = (static_cast<unsigned>(data[i]) << 16) | (static_cast<unsigned>(data[i + 1]) << 8)
            | static_cast<unsigned>(data[i + 2]);
        out.push_back(table[(triple >> 18) & 0x3F]);
        out.push_back(table[(triple >> 12) & 0x3F]);
        out.push_back(table[(triple >> 6) & 0x3F]);
        out.push_back(table[triple & 0x3F]);
        i += 3;
    }
    if (i < len) {
        unsigned triple = static_cast<unsigned>(data[i]) << 16;
        out.push_back(table[(triple >> 18) & 0x3F]);
        if (i + 1 < len) {
            triple |= static_cast<unsigned>(data[i + 1]) << 8;
            out.push_back(table[(triple >> 12) & 0x3F]);
            out.push_back(table[(triple >> 6) & 0x3F]);
            out.push_back('=');
        } else {
            out.push_back(table[(triple >> 12) & 0x3F]);
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

std::string generate_websocket_key()
{
    std::array<unsigned char, 16> random_bytes {};
    std::random_device            rd;
    for (auto& byte : random_bytes)
        byte = static_cast<unsigned char>(rd() & 0xFF);
    return base64_encode(random_bytes.data(), random_bytes.size());
}

std::string trim_copy(std::string_view sv)
{
    size_t begin = 0;
    size_t end   = sv.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(sv[begin])))
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(sv[end - 1])))
        --end;
    return std::string(sv.substr(begin, end - begin));
}

#if defined(USING_SSL)
void apply_tls_sni(net::tls_socket<SpinLock>& socket, const std::string& host)
{
    if (!host.empty())
        SSL_set_tlsext_host_name(socket.ssl_handle(), host.c_str());
}
#endif

} // namespace
#ifdef _WIN32
#include <windows.h>
static BOOL WINAPI console_ctrl_handler(DWORD type)
{
    if (type == CTRL_C_EVENT) {
        g_stop.store(true, std::memory_order_release);
        auto fd = g_listener_fd.exchange(net::os::invalid_fd(), std::memory_order_acq_rel);
        if (fd != net::os::invalid_fd())
            net::os::close_fd(fd);
        auto ws_fd = g_ws_listener_fd.exchange(net::os::invalid_fd(), std::memory_order_acq_rel);
        if (ws_fd != net::os::invalid_fd())
            net::os::close_fd(ws_fd);
        return TRUE;
    }
    return FALSE;
}
#else
static void sigint_handler(int)
{
    g_stop.store(true, std::memory_order_release);
    auto fd = g_listener_fd.exchange(net::os::invalid_fd(), std::memory_order_acq_rel);
    if (fd != net::os::invalid_fd())
        net::os::close_fd(fd);
    auto ws_fd = g_ws_listener_fd.exchange(net::os::invalid_fd(), std::memory_order_acq_rel);
    if (ws_fd != net::os::invalid_fd())
        net::os::close_fd(ws_fd);
}
#endif

// 简单 echo 客户端协程: 连接 127.0.0.1:12345 发送 "hello" 并读取回显
// fd_workqueue 由外部构造并传入，它自身绑定 reactor 线程；协程继续运行在主系统 workqueue 上
// connection handler: echo back whatever is received
template <typename Socket> static Task<void, Work_Promise<SpinLock, void>> echo_stream_connection(Socket sock)
{
    if constexpr (requires(Socket& s) { s.handshake(); }) {
        int handshake_rc = co_await sock.handshake();
        if (handshake_rc != 0) {
            CO_WQ_LOG_ERROR("[echo] tls handshake failed: %d", handshake_rc);
            sock.close();
            co_return;
        }
    }

    char buf[512];
    while (true) {
        ssize_t n = co_await sock.recv(buf, sizeof(buf));
        if (n <= 0)
            break;
        g_bytes_echoed.fetch_add((uint64_t)n, std::memory_order_relaxed);
        ssize_t m = co_await sock.send(buf, (size_t)n);
        if (m <= 0)
            break;
    }
    sock.close();
    co_return;
}

template <typename Socket>
static Task<bool, Work_Promise<SpinLock, bool>>
websocket_client_handshake(Socket& sock, const std::string& host, uint16_t port, const std::string& path, bool secure)
{
    std::string key          = generate_websocket_key();
    std::string host_header  = host;
    const bool  default_port = (secure && port == 443) || (!secure && port == 80);
    if (!default_port) {
        host_header.push_back(':');
        host_header.append(std::to_string(port));
    }

    std::string request;
    request.reserve(128 + host.size() + path.size());
    request.append("GET ");
    request.append(path.empty() ? "/" : path);
    request.append(" HTTP/1.1\r\n");
    request.append("Host: ");
    request.append(host_header);
    request.append("\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n");
    request.append("Sec-WebSocket-Key: ");
    request.append(key);
    request.append("\r\nSec-WebSocket-Version: 13\r\n\r\n");

    ssize_t sent = co_await sock.send_all(request.data(), request.size());
    if (sent < 0) {
        CO_WQ_LOG_ERROR("[ws client] handshake send failed: %lld", static_cast<long long>(sent));
        co_return false;
    }

    std::string           response;
    std::array<char, 512> buffer {};
    while (response.find("\r\n\r\n") == std::string::npos && response.size() <= 16384) {
        ssize_t n = co_await sock.recv(buffer.data(), buffer.size());
        if (n <= 0) {
            CO_WQ_LOG_ERROR("[ws client] handshake recv failed: %lld", static_cast<long long>(n));
            co_return false;
        }
        response.append(buffer.data(), static_cast<size_t>(n));
    }

    size_t end_pos = response.find("\r\n\r\n");
    if (end_pos == std::string::npos) {
        CO_WQ_LOG_ERROR("[ws client] handshake response incomplete");
        co_return false;
    }

    std::istringstream header_stream(response.substr(0, end_pos));
    std::string        line;
    if (!std::getline(header_stream, line)) {
        CO_WQ_LOG_ERROR("[ws client] handshake missing status line");
        co_return false;
    }
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    if (line.find("101") == std::string::npos) {
        CO_WQ_LOG_ERROR("[ws client] unexpected status: %s", line.c_str());
        co_return false;
    }

    std::string accept_header;
    while (std::getline(header_stream, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string_view line_view(line);
        auto             name_view  = line_view.substr(0, colon);
        auto             value_view = line_view.substr(colon + 1);
        auto             name_lower = net::websocket::detail::to_lower(trim_copy(name_view));
        if (name_lower == "sec-websocket-accept")
            accept_header = trim_copy(value_view);
    }

    std::string expected_accept = net::websocket::detail::compute_accept_key(key);
    if (accept_header.empty() || accept_header != expected_accept) {
        CO_WQ_LOG_ERROR("[ws client] invalid accept header");
        co_return false;
    }

    co_return true;
}

template <typename Socket> static Task<void, Work_Promise<SpinLock, void>> websocket_echo_handler(Socket sock)
{
    if constexpr (requires(Socket& s) { s.handshake(); }) {
        int handshake_rc = co_await sock.handshake();
        if (handshake_rc != 0) {
            CO_WQ_LOG_ERROR("[ws] tls handshake failed: %d", handshake_rc);
            sock.close();
            co_return;
        }
    }

    net::websocket::server_options ws_opts;
    ws_opts.close_on_failure = true;

    auto accept_res = co_await net::websocket::accept<SpinLock>(sock, ws_opts);
    if (!accept_res.ok) {
        if (accept_res.status_code == 0)
            CO_WQ_LOG_INFO("[ws] handshake cancelled: %s", accept_res.reason.c_str());
        else
            CO_WQ_LOG_ERROR("[ws] handshake failed: %s (status=%u)",
                            accept_res.reason.c_str(),
                            static_cast<unsigned>(accept_res.status_code));
        sock.close();
        co_return;
    }

    g_conn_count.fetch_add(1, std::memory_order_relaxed);
    CO_WQ_LOG_INFO("[ws] client connected, path=%s", accept_res.request.url.c_str());

    for (;;) {
        auto msg_res = co_await net::websocket::read_message<SpinLock>(sock);
        if (!msg_res.ok) {
            CO_WQ_LOG_ERROR("[ws] read error: %d", msg_res.error);
            break;
        }
        auto& msg = msg_res.value;
        if (msg.closed) {
            co_await net::websocket::send_close<SpinLock>(sock, msg.close_code, msg.close_reason);
            break;
        }
        g_bytes_echoed.fetch_add(msg.payload.size(), std::memory_order_relaxed);
        if (msg.op == net::websocket::opcode::binary) {
            co_await net::websocket::send_binary<SpinLock>(sock, msg.payload);
        } else {
            std::string text(msg.payload.begin(), msg.payload.end());
            co_await net::websocket::send_text<SpinLock>(sock, text);
        }
    }

    sock.close();
    co_return;
}

static Task<void, Work_Promise<SpinLock, void>> websocket_server(NetFdWorkqueue& fdwq,
                                                                 std::string     host,
                                                                 uint16_t        port
#if defined(USING_SSL)
                                                                 ,
                                                                 const net::tls_context* tls_ctx
#endif
)
{
    net::tcp_listener<SpinLock> listener(fdwq.base(), fdwq.reactor());
    listener.bind_listen(host, port, 64);
    g_ws_listener_fd.store(listener.native_handle(), std::memory_order_release);
    CO_WQ_LOG_INFO("[ws] listening on %s:%u", host.c_str(), static_cast<unsigned>(port));

    while (!g_stop.load(std::memory_order_acquire)) {
        int fd = co_await listener.accept();
        if (fd == net::k_accept_fatal) {
            CO_WQ_LOG_ERROR("[ws] accept fatal error");
            break;
        }
        if (fd < 0)
            continue;
#if defined(USING_SSL)
        if (tls_ctx) {
            try {
                auto sock = fdwq.adopt_tls_socket(fd, *tls_ctx, net::tls_mode::Server);
                auto task = websocket_echo_handler(std::move(sock));
                post_to(task, fdwq.base());
            } catch (const std::exception& ex) {
                CO_WQ_LOG_ERROR("[ws] tls adopt failed: %s", ex.what());
                net::os::close_fd(fd);
            }
            continue;
        }
#endif
        auto socket = fdwq.adopt_tcp_socket(fd);
        auto task   = websocket_echo_handler(std::move(socket));
        post_to(task, fdwq.base());
    }

    listener.close();
    g_ws_listener_fd.store(net::os::invalid_fd(), std::memory_order_release);
    co_return;
}

static Task<void, Work_Promise<SpinLock, void>> websocket_client(NetFdWorkqueue& fdwq,
                                                                 std::string     host,
                                                                 uint16_t        port,
                                                                 std::string     path,
                                                                 bool            secure
#if defined(USING_SSL)
                                                                 ,
                                                                 const net::tls_context* tls_ctx
#endif
)
{
#if defined(USING_SSL)
    if (secure) {
        if (!tls_ctx) {
            CO_WQ_LOG_ERROR("[ws client] tls requested but context missing");
            co_return;
        }
        auto                      tls_sock = fdwq.make_tls_socket(*tls_ctx, net::tls_mode::Client);
        auto&                     base     = tls_sock.underlying();
        net::dns::resolve_options opts;
        opts.family           = base.family();
        opts.allow_dual_stack = base.dual_stack();
        auto resolved         = net::dns::resolve_sync(host, port, opts);
        if (!resolved.success) {
            CO_WQ_LOG_ERROR("[ws client] dns error=%s (%d)", resolved.error_message.c_str(), resolved.error_code);
            co_return;
        }
        apply_tls_sni(tls_sock, host);
        int rc = co_await base.connect(reinterpret_cast<const sockaddr*>(&resolved.storage), resolved.length);
        if (rc != 0) {
            CO_WQ_LOG_ERROR("[ws client] connect failed rc=%d", rc);
            co_return;
        }
        int handshake_rc = co_await tls_sock.handshake();
        if (handshake_rc != 0) {
            CO_WQ_LOG_ERROR("[ws client] tls handshake rc=%d", handshake_rc);
            co_return;
        }
        bool ok = co_await websocket_client_handshake(tls_sock, host, port, path, true);
        if (!ok) {
            tls_sock.close();
            co_return;
        }
        net::websocket::send_options send_opts;
        send_opts.mask      = true;
        const char* payload = "hello-ws";
        co_await net::websocket::send_text<SpinLock>(tls_sock, payload, send_opts);
        net::websocket::message_read_options read_opts;
        read_opts.require_masked_client = false;
        auto msg_res                    = co_await net::websocket::read_message<SpinLock>(tls_sock, read_opts);
        if (msg_res.ok && !msg_res.value.closed) {
            std::string text(msg_res.value.payload.begin(), msg_res.value.payload.end());
            CO_WQ_LOG_INFO("[ws client] recv: %s", text.c_str());
            g_bytes_echoed.fetch_add(msg_res.value.payload.size(), std::memory_order_relaxed);
        }
        co_await net::websocket::send_close<SpinLock>(tls_sock, 1000, "bye", send_opts);
        tls_sock.close();
        co_return;
    }
#else
    if (secure) {
        CO_WQ_LOG_ERROR("[ws client] TLS support not built");
        co_return;
    }
#endif

    auto                      sock = fdwq.make_tcp_socket();
    net::dns::resolve_options opts;
    opts.family           = sock.family();
    opts.allow_dual_stack = sock.dual_stack();
    auto resolved         = net::dns::resolve_sync(host, port, opts);
    if (!resolved.success) {
        CO_WQ_LOG_ERROR("[ws client] dns error=%s (%d)", resolved.error_message.c_str(), resolved.error_code);
        co_return;
    }
    int rc = co_await sock.connect(reinterpret_cast<const sockaddr*>(&resolved.storage), resolved.length);
    if (rc != 0) {
        CO_WQ_LOG_ERROR("[ws client] connect failed rc=%d", rc);
        co_return;
    }
    bool ok = co_await websocket_client_handshake(sock, host, port, path, false);
    if (!ok) {
        sock.close();
        co_return;
    }
    net::websocket::send_options send_opts;
    send_opts.mask      = true;
    const char* payload = "hello-ws";
    co_await net::websocket::send_text<SpinLock>(sock, payload, send_opts);
    net::websocket::message_read_options read_opts;
    read_opts.require_masked_client = false;
    auto msg_res                    = co_await net::websocket::read_message<SpinLock>(sock, read_opts);
    if (msg_res.ok && !msg_res.value.closed) {
        std::string text(msg_res.value.payload.begin(), msg_res.value.payload.end());
        CO_WQ_LOG_INFO("[ws client] recv: %s", text.c_str());
        g_bytes_echoed.fetch_add(msg_res.value.payload.size(), std::memory_order_relaxed);
    }
    co_await net::websocket::send_close<SpinLock>(sock, 1000, "bye", send_opts);
    sock.close();
    co_return;
}

// server coroutine: listen on 127.0.0.1:12345 and accept a single client then exit
static Task<void, Work_Promise<SpinLock, void>> echo_server(NetFdWorkqueue& fdwq,
                                                            std::string     host,
                                                            uint16_t        port,
                                                            int             max_conn
#if defined(USING_SSL)
                                                            ,
                                                            const net::tls_context* tls_ctx
#endif
)
{
    net::tcp_listener<SpinLock> lst(fdwq.base(), fdwq.reactor());
    lst.bind_listen(host, port, 16);
    g_server_start = std::chrono::steady_clock::now();
    g_listener_fd.store(lst.native_handle(), std::memory_order_release);
    int accepted = 0;
    while ((max_conn <= 0 || accepted < max_conn) && !g_stop.load(std::memory_order_acquire)) {
        int fd = co_await lst.accept();
        if (fd < 0) {
            // fatal error or shutdown
            break;
        }
        ++accepted;
        g_conn_count.fetch_add(1, std::memory_order_relaxed);
#if defined(USING_SSL)
        if (tls_ctx) {
            try {
                auto sock = fdwq.adopt_tls_socket(fd, *tls_ctx, net::tls_mode::Server);
                auto t    = echo_stream_connection(std::move(sock));
                post_to(t, fdwq.base());
            } catch (const std::exception& ex) {
                CO_WQ_LOG_ERROR("[server] tls adopt failed: %s", ex.what());
                net::os::close_fd(fd);
            }
            continue;
        }
#endif
        auto sock = fdwq.adopt_tcp_socket(fd);
        auto t    = echo_stream_connection(std::move(sock));
        post_to(t, fdwq.base());
    }
    lst.close(); // server exits after reaching max_conn (if specified)
    g_listener_fd.store(net::os::invalid_fd(), std::memory_order_release);
    // Print statistics on unlimited mode exit via Ctrl+C or after finishing limited accepts.
    auto     dur   = std::chrono::steady_clock::now() - g_server_start;
    double   sec   = std::chrono::duration_cast<std::chrono::duration<double>>(dur).count();
    uint64_t bytes = g_bytes_echoed.load(std::memory_order_relaxed);
    uint64_t conns = g_conn_count.load(std::memory_order_relaxed);
    double   mbps  = sec > 0 ? (bytes / (1024.0 * 1024.0)) / sec : 0.0;
    CO_WQ_LOG_INFO("[server] connections=%llu bytes=%llu elapsed(s)=%.3f throughput(MiB/s)=%.3f",
                   static_cast<unsigned long long>(conns),
                   static_cast<unsigned long long>(bytes),
                   sec,
                   mbps);
    co_return;
}

static Task<void, Work_Promise<SpinLock, void>> echo_client(NetFdWorkqueue& fdwq,
                                                            std::string     host,
                                                            uint16_t        port
#if defined(USING_SSL)
                                                            ,
                                                            const net::tls_context* tls_ctx
#endif
)
{
#if defined(USING_SSL)
    if (tls_ctx) {
        auto                      tls_sock = fdwq.make_tls_socket(*tls_ctx, net::tls_mode::Client);
        auto&                     base     = tls_sock.underlying();
        net::dns::resolve_options opts;
        opts.family           = base.family();
        opts.allow_dual_stack = base.dual_stack();
        auto resolved         = net::dns::resolve_sync(host, port, opts);
        if (!resolved.success) {
            CO_WQ_LOG_ERROR("connect failed: dns error=%s (%d)", resolved.error_message.c_str(), resolved.error_code);
            co_return;
        }
        apply_tls_sni(tls_sock, host);
        int rc = co_await base.connect(reinterpret_cast<const sockaddr*>(&resolved.storage), resolved.length);
        if (rc != 0) {
            CO_WQ_LOG_ERROR("connect failed rc=%d", rc);
            co_return;
        }
        int handshake_rc = co_await tls_sock.handshake();
        if (handshake_rc != 0) {
            CO_WQ_LOG_ERROR("tls handshake failed rc=%d", handshake_rc);
            co_return;
        }
        char buf[256];
        while (true) {
            ssize_t n = co_await tls_sock.recv(buf, sizeof(buf));
            if (n <= 0) {
                CO_WQ_LOG_INFO("recv end: %lld", static_cast<long long>(n));
                break;
            }
            CO_WQ_LOG_INFO("recv: %.*s", static_cast<int>(n), buf);
            ssize_t sent = co_await tls_sock.send(buf, (size_t)n);
            CO_WQ_LOG_INFO("sent: %lld bytes", static_cast<long long>(sent));
            if (sent <= 0) {
                CO_WQ_LOG_ERROR("send error: %lld", static_cast<long long>(sent));
                break;
            }
        }
        tls_sock.close();
        co_return;
    }
#endif
    auto                      sock = fdwq.make_tcp_socket();
    net::dns::resolve_options opts;
    opts.family           = sock.family();
    opts.allow_dual_stack = sock.dual_stack();
    auto resolved         = net::dns::resolve_sync(host, port, opts);
    if (!resolved.success) {
        CO_WQ_LOG_ERROR("connect failed: dns error=%s (%d)", resolved.error_message.c_str(), resolved.error_code);
        co_return;
    }
    int rc = co_await sock.connect(reinterpret_cast<const sockaddr*>(&resolved.storage), resolved.length);
    if (rc != 0) {
        CO_WQ_LOG_ERROR("connect failed rc=%d", rc);
        co_return;
    }
    char buf[256];
    while (true) {
        ssize_t n = co_await sock.recv(buf, sizeof(buf));
        if (n <= 0) {
            CO_WQ_LOG_INFO("recv end: %lld", static_cast<long long>(n));
            break;
        }
        CO_WQ_LOG_INFO("recv: %.*s", static_cast<int>(n), buf);
        ssize_t sent = co_await sock.send(buf, (size_t)n);
        CO_WQ_LOG_INFO("sent: %lld bytes", static_cast<long long>(sent));
        if (sent <= 0) {
            CO_WQ_LOG_ERROR("send error: %lld", static_cast<long long>(sent));
            break;
        }
    }
    co_return;
}

struct EchoOptions {
    bool        run_server { true };
    bool        run_client { true };
    bool        run_udp_server { false }; // 新增：UDP server
    bool        run_udp_client { false }; // 新增：UDP client
    bool        run_ws_server { false };
    bool        run_ws_client { false };
    std::string host { "127.0.0.1" };
    uint16_t    port { 12345 };
    uint16_t    udp_port { 12346 }; // UDP 默认端口（避免与 TCP 冲突）
    uint16_t    ws_port { 9000 };
    std::string ws_path { "/" };
    bool        use_tls { false };
    bool        tls_verify_peer { false };
    bool        ws_use_tls { false };
    std::string tls_cert;
    std::string tls_key;
    int         max_conn { 0 }; // 0 or negative => unlimited
};

static void print_usage(const char* prog)
{
    CO_WQ_LOG_INFO("Usage: %s [--server|--client|--both] [--host HOST] [--port TCP_PORT] [--udp-server] [--udp-client] "
                   "[--udp-port UDP_PORT] [--max-conn N] [--tls] [--tls-cert FILE] [--tls-key FILE] [--tls-verify] "
                   "[--ws-server] [--ws-client] [--ws-port PORT] [--ws-path PATH] [--ws-tls]",
                   prog);
}

static EchoOptions parse_args(int argc, char* argv[])
{
    EchoOptions opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--server") {
            opt.run_server = true;
            opt.run_client = false;
        } else if (a == "--client") {
            opt.run_server = false;
            opt.run_client = true;
        } else if (a == "--both") {
            opt.run_server = true;
            opt.run_client = true;
        } else if (a == "--host" && i + 1 < argc) {
            opt.host = argv[++i];
        } else if (a == "--port" && i + 1 < argc) {
            opt.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (a == "--max-conn" && i + 1 < argc) {
            opt.max_conn = std::stoi(argv[++i]);
        } else if (a == "--udp-server") {
            opt.run_udp_server = true;
        } else if (a == "--udp-client") {
            opt.run_udp_client = true;
        } else if (a == "--udp-port" && i + 1 < argc) {
            opt.udp_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (a == "--ws-server") {
            opt.run_ws_server = true;
        } else if (a == "--ws-client") {
            opt.run_ws_client = true;
        } else if (a == "--ws-port" && i + 1 < argc) {
            opt.ws_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (a == "--ws-path" && i + 1 < argc) {
            opt.ws_path = argv[++i];
            if (opt.ws_path.empty())
                opt.ws_path = "/";
            if (!opt.ws_path.empty() && opt.ws_path.front() != '/')
                opt.ws_path.insert(opt.ws_path.begin(), '/');
        } else if (a == "--ws-tls") {
            opt.ws_use_tls = true;
        } else if (a == "--tls") {
            opt.use_tls = true;
        } else if (a == "--tls-cert" && i + 1 < argc) {
            opt.tls_cert = argv[++i];
        } else if (a == "--tls-key" && i + 1 < argc) {
            opt.tls_key = argv[++i];
        } else if (a == "--tls-verify") {
            opt.tls_verify_peer = true;
        } else if (a == "--help" || a == "-h") {
            print_usage(argv[0]);
        }
    }
    return opt;
}

int main(int argc, char* argv[])
{
    auto       options             = parse_args(argc, argv);
    const bool need_signal_handler = (options.run_server && options.max_conn <= 0) || options.run_ws_server;
    if (need_signal_handler) {
#ifdef _WIN32
        SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
        std::signal(SIGINT, sigint_handler);
#endif
    }
    // 0 => 自动检测线程数
    co_wq::test::SysStatsLogger stats_logger("echo");
    auto&                       wq = get_sys_workqueue(0);
    NetFdWorkqueue              fdwq(wq); // 外部创建并传入协程
#if defined(USING_SSL)
    const bool need_server_tls_ctx = (options.use_tls && options.run_server)
        || (options.ws_use_tls && options.run_ws_server);
    const bool need_client_tls_ctx = (options.use_tls && options.run_client)
        || (options.ws_use_tls && options.run_ws_client);
    std::optional<net::tls_context> tls_server_ctx;
    std::optional<net::tls_context> tls_client_ctx;
    if (need_server_tls_ctx) {
        if (options.tls_cert.empty() || options.tls_key.empty()) {
            CO_WQ_LOG_ERROR("[echo] TLS server requires --tls-cert/--tls-key");
            return 1;
        }
        try {
            tls_server_ctx.emplace(net::tls_context::make_server_with_pem(options.tls_cert, options.tls_key));
        } catch (const std::exception& ex) {
            CO_WQ_LOG_ERROR("[echo] failed to load TLS cert/key: %s", ex.what());
            return 1;
        }
    }
    if (need_client_tls_ctx) {
        try {
            tls_client_ctx.emplace(net::tls_context::make(net::tls_mode::Client));
        } catch (const std::exception& ex) {
            CO_WQ_LOG_ERROR("[echo] failed to init TLS client ctx: %s", ex.what());
            return 1;
        }
        if (SSL_CTX* ctx = tls_client_ctx->native_handle()) {
            if (options.tls_verify_peer) {
                SSL_CTX_set_default_verify_paths(ctx);
                SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
            } else {
                SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
                CO_WQ_LOG_WARN("[echo] TLS peer verification disabled");
            }
        }
    }
    const net::tls_context* tcp_server_ctx_ptr = (options.use_tls && options.run_server && tls_server_ctx)
        ? &*tls_server_ctx
        : nullptr;
    const net::tls_context* tcp_client_ctx_ptr = (options.use_tls && options.run_client && tls_client_ctx)
        ? &*tls_client_ctx
        : nullptr;
    const net::tls_context* ws_server_ctx_ptr  = (options.ws_use_tls && options.run_ws_server && tls_server_ctx)
         ? &*tls_server_ctx
         : nullptr;
    const net::tls_context* ws_client_ctx_ptr  = (options.ws_use_tls && options.run_ws_client && tls_client_ctx)
         ? &*tls_client_ctx
         : nullptr;
#else
    if (options.use_tls || options.ws_use_tls) {
        CO_WQ_LOG_ERROR("[echo] TLS requested but build lacks USING_SSL");
        return 1;
    }
#endif
    Task<void, Work_Promise<SpinLock, void>> server_task { nullptr };
    Task<void, Work_Promise<SpinLock, void>> client_task { nullptr };
    Task<void, Work_Promise<SpinLock, void>> udp_server_task { nullptr };
    Task<void, Work_Promise<SpinLock, void>> udp_client_task { nullptr };
    Task<void, Work_Promise<SpinLock, void>> ws_server_task { nullptr };
    Task<void, Work_Promise<SpinLock, void>> ws_client_task { nullptr };
    if (options.run_server)
#if defined(USING_SSL)
        server_task = echo_server(fdwq, options.host, options.port, options.max_conn, tcp_server_ctx_ptr);
#else
        server_task = echo_server(fdwq, options.host, options.port, options.max_conn);
#endif
    if (options.run_client)
#if defined(USING_SSL)
        client_task = echo_client(fdwq, options.host, options.port, tcp_client_ctx_ptr);
#else
        client_task = echo_client(fdwq, options.host, options.port);
#endif
    if (options.run_udp_server) {
        // 简单 UDP echo server：收 -> 发回
        udp_server_task = [&fdwq, options]() -> Task<void, Work_Promise<SpinLock, void>> {
            auto usock = fdwq.make_udp_socket();
            // 绑定
            sockaddr_in addr {};
            addr.sin_family      = AF_INET;
            addr.sin_addr.s_addr = inet_addr(options.host.c_str());
            addr.sin_port        = htons(options.udp_port);
            if (::bind(usock.native_handle(), (sockaddr*)&addr, sizeof(addr)) != 0) {
                CO_WQ_LOG_ERROR("UDP bind failed");
                co_return;
            }
            CO_WQ_LOG_INFO("[udp server] listening on %s:%u",
                           options.host.c_str(),
                           static_cast<unsigned>(options.udp_port));
            char        buf[1500];
            sockaddr_in peer;
            socklen_t   plen = sizeof(peer);
            while (!g_stop.load(std::memory_order_acquire)) {
                ssize_t n = co_await usock.recv_from(buf, sizeof(buf), &peer, &plen);
                if (n <= 0)
                    continue; // ignore errors
                co_await usock.send_to(buf, (size_t)n, peer);
            }
            co_return;
        }();
    }
    if (options.run_ws_server)
#if defined(USING_SSL)
        ws_server_task = websocket_server(fdwq, options.host, options.ws_port, ws_server_ctx_ptr);
#else
        ws_server_task = websocket_server(fdwq, options.host, options.ws_port);
#endif
    if (options.run_ws_client)
#if defined(USING_SSL)
        ws_client_task = websocket_client(fdwq,
                                          options.host,
                                          options.ws_port,
                                          options.ws_path,
                                          options.ws_use_tls,
                                          ws_client_ctx_ptr);
#else
        ws_client_task = websocket_client(fdwq, options.host, options.ws_port, options.ws_path, options.ws_use_tls);
#endif
    if (options.run_udp_client) {
        udp_client_task = [&fdwq, options]() -> Task<void, Work_Promise<SpinLock, void>> {
            auto usock = fdwq.make_udp_socket();
            // 可选 connect (方便后面直接 recv)
            if (co_await usock.connect(options.host, options.udp_port) != 0) {
                CO_WQ_LOG_WARN("UDP connect failed (non-fatal, fallback to send_to)");
            }
            sockaddr_in peer {};
            peer.sin_family      = AF_INET;
            peer.sin_addr.s_addr = inet_addr(options.host.c_str());
            peer.sin_port        = htons(options.udp_port);
            const char* msg      = "ping-udp";
            for (int i = 0; i < 5; ++i) {
                co_await usock.send_to(msg, strlen(msg), peer);
                char        rbuf[256];
                sockaddr_in from;
                socklen_t   flen = sizeof(from);
                ssize_t     rn   = co_await usock.recv_from(rbuf, sizeof(rbuf), &from, &flen);
                if (rn > 0)
                    CO_WQ_LOG_INFO("[udp client] recv: %.*s", static_cast<int>(rn), rbuf);
            }
            co_return;
        }();
    }
    // choose a promise to wait on (prefer client tasks, then servers)
    // 选择一个用来同步退出；优先各类客户端，其次服务器
    auto             ch      = (options.run_ws_client        ? ws_client_task.get()
                                    : options.run_client     ? client_task.get()
                                    : options.run_udp_client ? udp_client_task.get()
                                    : options.run_ws_server  ? ws_server_task.get()
                                    : options.run_udp_server ? udp_server_task.get()
                                                             : server_task.get());
    auto&            promise = ch.promise();
    std::atomic_bool finished { false };
    promise.mUserData    = &finished;
    promise.mOnCompleted = [](Promise_base& pb) {
        auto* f = static_cast<std::atomic_bool*>(pb.mUserData);
        if (f)
            f->store(true, std::memory_order_release);
    };
    // 投递到执行队列
    if (options.run_server) {
        post_to(server_task, wq);
    }
    if (options.run_client) {
        post_to(client_task, wq);
    }
    if (options.run_udp_server) {
        post_to(udp_server_task, wq);
    }
    if (options.run_udp_client) {
        post_to(udp_client_task, wq);
    }
    if (options.run_ws_server) {
        post_to(ws_server_task, wq);
    }
    if (options.run_ws_client) {
        post_to(ws_client_task, wq);
    }

    // 事件循环: 处理工作项，等待协程完成
    sys_wait_until(finished);
    return 0;
}

#else // USING_NET
int main()
{
    CO_WQ_LOG_WARN("co_echo disabled (requires Linux + USING_NET)");
    return 0;
}
#endif
