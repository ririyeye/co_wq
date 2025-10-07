//
// HTTP forward proxy example built on the co_wq coroutine framework.
// 支持常见的 HTTP/1.1 正向代理语义（绝对 URI + CONNECT 隧道），示例演示如何
// 使用 fd_workqueue 接受客户端连接、解析请求并桥接到上游服务器。

#include "syswork.hpp"

#include "fd_base.hpp"
#include "tcp_listener.hpp"
#include "tcp_socket.hpp"
#include "when_all.hpp"
#include "worker.hpp"

#include <llhttp.h>
#include <spdlog/spdlog.h>

#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <basetsd.h>
#include <windows.h>
#else
#include <cerrno> // NOLINT(modernize-deprecated-headers)
#include <csignal>

#endif

using namespace co_wq;

using NetFdWorkqueue = net::fd_workqueue<SpinLock, net::epoll_reactor>;

namespace {

bool parse_port(std::string_view input, uint16_t& port_out)
{
    if (input.empty() || input.size() > 5)
        return false;

    unsigned int value = 0;
    for (char ch : input) {
        if (!std::isdigit(static_cast<unsigned char>(ch)))
            return false;
        value = value * 10u + static_cast<unsigned int>(ch - '0');
        if (value > std::numeric_limits<uint16_t>::max())
            return false;
    }

    port_out = static_cast<uint16_t>(value);
    return true;
}

std::string to_lower(std::string_view input)
{
    std::string result;
    result.reserve(input.size());
    for (char ch : input) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return result;
}

bool split_host_port(std::string_view input, uint16_t default_port, std::string& host_out, uint16_t& port_out)
{
    if (input.empty())
        return false;

    std::string host;
    std::string port_str;

    if (input.front() == '[') {
        auto close = input.find(']');
        if (close == std::string::npos)
            return false;
        host        = std::string(input.substr(1, close - 1));
        size_t next = close + 1;
        if (next < input.size()) {
            if (input[next] != ':')
                return false;
            port_str = std::string(input.substr(next + 1));
        }
    } else {
        auto colon = input.find(':');
        if (colon != std::string::npos) {
            if (input.find(':', colon + 1) != std::string::npos)
                return false; // IPv6 literal must be bracketed
            host     = std::string(input.substr(0, colon));
            port_str = std::string(input.substr(colon + 1));
        } else {
            host = std::string(input);
        }
    }

    if (host.empty())
        return false;

    uint16_t port_value = default_port;
    if (!port_str.empty()) {
        if (!parse_port(port_str, port_value))
            return false;
    }

    host_out = std::move(host);
    port_out = port_value;
    return true;
}

std::string format_host_for_log(const std::string& host)
{
    if (host.find(':') != std::string::npos) {
        std::ostringstream oss;
        oss << '[' << host << ']';
        return oss.str();
    }
    return host;
}

std::filesystem::path find_project_root(const std::filesystem::path& start_dir)
{
    std::error_code ec;
    auto            current = std::filesystem::weakly_canonical(start_dir, ec);
    if (ec)
        current = start_dir;

    while (!current.empty()) {
        auto candidate_xmake = current / "xmake.lua";
        if (std::filesystem::exists(candidate_xmake, ec) && !ec)
            return current;

        auto candidate_git = current / ".git";
        if (std::filesystem::exists(candidate_git, ec) && !ec)
            return current;

        auto parent = current.parent_path();
        if (parent == current)
            break;
        current = std::move(parent);
    }

    return {};
}

struct HeaderEntry {
    std::string name;
    std::string value;
};

struct HttpProxyContext {
    std::string                                  method;
    std::string                                  url;
    std::unordered_map<std::string, std::string> headers;
    std::vector<HeaderEntry>                     header_sequence;
    std::string                                  current_field;
    std::string                                  current_value;
    std::string                                  body;
    int                                          http_major { 1 };
    int                                          http_minor { 1 };
    bool                                         headers_complete { false };
    bool                                         message_complete { false };

    void reset()
    {
        method.clear();
        url.clear();
        headers.clear();
        header_sequence.clear();
        current_field.clear();
        current_value.clear();
        body.clear();
        http_major       = 1;
        http_minor       = 1;
        headers_complete = false;
        message_complete = false;
    }
};

std::string format_peer_id(uint64_t session_id)
{
    std::ostringstream oss;
    oss << '[' << session_id << ']';
    return oss.str();
}

struct PipeOutcome {
    std::string label;
    std::string status { "pending" };
    bool        had_error { false };
};

template <typename SrcSocket, typename DstSocket>
Task<void, Work_Promise<SpinLock, void>>
pipe_data(SrcSocket& src, DstSocket& dst, bool propagate_shutdown, std::string flow_desc, PipeOutcome& outcome)
{
    const char*            flow = flow_desc.empty() ? "stream" : flow_desc.c_str();
    std::array<char, 8192> buffer {};
    outcome.label     = flow_desc;
    outcome.status    = "pending";
    outcome.had_error = false;
    while (true) {
        ssize_t n = co_await src.recv(buffer.data(), buffer.size());
        if (n == 0) {
            CO_WQ_LOG_DEBUG("[proxy] %s closed (EOF)", flow);
            outcome.status = "eof";
            if (propagate_shutdown) {
                if constexpr (requires(DstSocket& s) {
                                  s.tx_shutdown();
                                  s.shutdown_tx();
                              }) {
                    if (!dst.tx_shutdown())
                        dst.shutdown_tx();
                }
            }
            break;
        }
        if (n < 0) {
            CO_WQ_LOG_WARN("[proxy] %s recv error rc=%lld", flow, static_cast<long long>(n));
            outcome.status    = std::string("recv_error rc=") + std::to_string(static_cast<long long>(n));
            outcome.had_error = true;
            break;
        }

        size_t offset = 0;
        while (offset < static_cast<size_t>(n)) {
            ssize_t sent = co_await dst.send(buffer.data() + offset, static_cast<size_t>(n) - offset);
            if (sent <= 0) {
                CO_WQ_LOG_WARN("[proxy] %s send error rc=%lld", flow, static_cast<long long>(sent));
                outcome.status    = std::string("send_error rc=") + std::to_string(static_cast<long long>(sent));
                outcome.had_error = true;
                break;
            }
            offset += static_cast<size_t>(sent);
        }
        if (outcome.had_error)
            break;
    }

    CO_WQ_LOG_DEBUG("[proxy] %s forwarding finished", flow);
    if (outcome.status == "pending")
        outcome.status = "completed";
    co_return;
}

struct UrlParts {
    std::string host;
    uint16_t    port { 80 };
    std::string path { "/" };
};

// 解析绝对形式的 HTTP URL（scheme://host[:port]/path）。
std::optional<UrlParts> parse_http_url(const std::string& url)
{
    auto pos = url.find("://");
    if (pos == std::string::npos)
        return std::nullopt;
    std::string scheme = to_lower(url.substr(0, pos));
    if (scheme != "http")
        return std::nullopt;
    size_t rest_idx = pos + 3;
    if (rest_idx >= url.size())
        return std::nullopt;

    auto        slash_pos = url.find('/', rest_idx);
    std::string authority = slash_pos == std::string::npos ? url.substr(rest_idx)
                                                           : url.substr(rest_idx, slash_pos - rest_idx);
    std::string path      = slash_pos == std::string::npos ? std::string("/") : url.substr(slash_pos);
    if (authority.empty())
        return std::nullopt;
    UrlParts    parts;
    std::string host_value;
    uint16_t    port_value = 80;
    if (!split_host_port(authority, 80, host_value, port_value))
        return std::nullopt;
    parts.host = std::move(host_value);
    parts.port = port_value;
    if (parts.host.empty())
        return std::nullopt;
    parts.path = std::move(path);
    return parts;
}

struct ConnectTarget {
    std::string host;
    uint16_t    port { 443 };
};

// 解析 CONNECT 动词目标（host[:port]）。
std::optional<ConnectTarget> parse_connect_target(const std::string& target)
{
    if (target.empty())
        return std::nullopt;
    std::string   host;
    uint16_t      port = 443;
    ConnectTarget ct;
    if (!split_host_port(target, 443, host, port))
        return std::nullopt;
    ct.host = std::move(host);
    ct.port = port;
    return ct;
}

net::tcp_socket<SpinLock> make_upstream_socket(NetFdWorkqueue& fdwq)
{
    static std::atomic_bool warned { false };
    try {
        return fdwq.make_tcp_socket(AF_INET6, true);
    } catch (const std::exception& ex) {
        if (!warned.exchange(true)) {
            CO_WQ_LOG_WARN("[proxy] dual-stack upstream socket unavailable, falling back to IPv4: %s", ex.what());
        }
    }
    return fdwq.make_tcp_socket(AF_INET, false);
}

void configure_graceful_close(net::tcp_socket<SpinLock>& socket, const std::string& peer_id, std::string_view role)
{
#if defined(_WIN32)
    SOCKET handle = socket.native_handle();
    if (handle == INVALID_SOCKET)
        return;
    // Avoid forcing linger on non-blocking sockets; the default FIN/ACK
    // shutdown path has proven more reliable for long HTTPS transfers.
    // Allow the operating system to manage FIN/ACK without forcing a timed
    // linger. For high-throughput TLS tunnels, an enforced linger on a
    // non-blocking socket can translate into RSTs when the grace window expires.
    ::linger linger_opts {};
    // Bias toward low-latency forwarding and provide ample buffering to absorb
    // bursty upstream records without stalling the relay workqueue.
    linger_opts.l_onoff = 0;
    if (::setsockopt(handle,
                     SOL_SOCKET,
                     SO_LINGER,
                     reinterpret_cast<const char*>(&linger_opts),
                     static_cast<int>(sizeof(linger_opts)))
        == SOCKET_ERROR) {
        int err = WSAGetLastError();
        CO_WQ_LOG_WARN("%s %.*s disable SO_LINGER failed: %d", peer_id.c_str(), (int)role.size(), role.data(), err);
    }
    DWORD nodelay = 1;
    if (::setsockopt(handle,
                     IPPROTO_TCP,
                     TCP_NODELAY,
                     reinterpret_cast<const char*>(&nodelay),
                     static_cast<int>(sizeof(nodelay)))
        == SOCKET_ERROR) {
        int err = WSAGetLastError();
        CO_WQ_LOG_WARN("%s %.*s enable TCP_NODELAY failed: %d", peer_id.c_str(), (int)role.size(), role.data(), err);
    }
    int buffer_hint = 1 << 20; // 1 MiB
    if (::setsockopt(handle,
                     SOL_SOCKET,
                     SO_SNDBUF,
                     reinterpret_cast<const char*>(&buffer_hint),
                     static_cast<int>(sizeof(buffer_hint)))
        == SOCKET_ERROR) {
        int err = WSAGetLastError();
        CO_WQ_LOG_WARN("%s %.*s enlarge SO_SNDBUF failed: %d", peer_id.c_str(), (int)role.size(), role.data(), err);
    }
    if (::setsockopt(handle,
                     SOL_SOCKET,
                     SO_RCVBUF,
                     reinterpret_cast<const char*>(&buffer_hint),
                     static_cast<int>(sizeof(buffer_hint)))
        == SOCKET_ERROR) {
        int err = WSAGetLastError();
        CO_WQ_LOG_WARN("%s %.*s enlarge SO_RCVBUF failed: %d", peer_id.c_str(), (int)role.size(), role.data(), err);
    }
#else
    (void)socket;
    (void)peer_id;
    (void)role;
#endif
}

void configure_tcp_keepalive(net::tcp_socket<SpinLock>& socket, const std::string& peer_id, std::string_view role)
{
#if defined(_WIN32)
    SOCKET handle = socket.native_handle();
    if (handle == INVALID_SOCKET)
        return;
    tcp_keepalive settings {};
    settings.onoff             = 1;
    settings.keepalivetime     = 60'000; // 60s idle
    settings.keepaliveinterval = 5'000;  // retry every 5s
    DWORD       ignored        = 0;
    const DWORD bytes          = 0;
    (void)bytes;
    if (WSAIoctl(handle,
                 SIO_KEEPALIVE_VALS,
                 &settings,
                 static_cast<DWORD>(sizeof(settings)),
                 nullptr,
                 0,
                 &ignored,
                 nullptr,
                 nullptr)
        == SOCKET_ERROR) {
        int err = WSAGetLastError();
        CO_WQ_LOG_WARN("%s %.*s configure keepalive failed: %d", peer_id.c_str(), (int)role.size(), role.data(), err);
    }
#else
    (void)socket;
    (void)peer_id;
    (void)role;
#endif
}

Task<int, Work_Promise<SpinLock, int>>
connect_upstream(net::tcp_socket<SpinLock>& socket, NetFdWorkqueue& fdwq, const std::string& host, uint16_t port)
{
    (void)fdwq;
    co_return co_await socket.connect(host, port);
}

// 构造最小化的 HTTP/1.1 错误/控制响应。
std::string build_http_response(int status_code, const std::string& reason, const std::string& body)
{
    std::string response;
    response.reserve(128 + body.size());
    response.append("HTTP/1.1 ");
    response.append(std::to_string(status_code));
    response.push_back(' ');
    response.append(reason);
    response.append("\r\n");
    response.append("Content-Type: text/plain; charset=utf-8\r\n");
    response.append("Content-Length: ");
    response.append(std::to_string(body.size()));
    response.append("\r\nConnection: close\r\n\r\n");
    response.append(body);
    return response;
}

int on_message_begin(llhttp_t* parser)
{
    if (auto* ctx = static_cast<HttpProxyContext*>(parser->data))
        ctx->reset();
    return 0;
}

int on_method(llhttp_t* parser, const char* at, size_t length)
{
    auto* ctx = static_cast<HttpProxyContext*>(parser->data);
    ctx->method.append(at, length);
    return 0;
}

int on_url(llhttp_t* parser, const char* at, size_t length)
{
    auto* ctx = static_cast<HttpProxyContext*>(parser->data);
    ctx->url.append(at, length);
    return 0;
}

int on_header_field(llhttp_t* parser, const char* at, size_t length)
{
    auto* ctx = static_cast<HttpProxyContext*>(parser->data);
    ctx->current_field.append(at, length);
    return 0;
}

int on_header_value(llhttp_t* parser, const char* at, size_t length)
{
    auto* ctx = static_cast<HttpProxyContext*>(parser->data);
    ctx->current_value.append(at, length);
    return 0;
}

int on_header_value_complete(llhttp_t* parser)
{
    auto* ctx = static_cast<HttpProxyContext*>(parser->data);
    if (!ctx->current_field.empty()) {
        ctx->headers[to_lower(ctx->current_field)] = ctx->current_value;
        ctx->header_sequence.push_back(HeaderEntry { ctx->current_field, ctx->current_value });
    }
    ctx->current_field.clear();
    ctx->current_value.clear();
    return 0;
}

int on_headers_complete(llhttp_t* parser)
{
    auto* ctx             = static_cast<HttpProxyContext*>(parser->data);
    ctx->headers_complete = true;
    ctx->http_major       = parser->http_major;
    ctx->http_minor       = parser->http_minor;
    return 0;
}

int on_body(llhttp_t* parser, const char* at, size_t length)
{
    auto* ctx = static_cast<HttpProxyContext*>(parser->data);
    ctx->body.append(at, length);
    return 0;
}

int on_message_complete(llhttp_t* parser)
{
    auto* ctx             = static_cast<HttpProxyContext*>(parser->data);
    ctx->message_complete = true;
    return 0;
}

// 进程级运行状态标记，配合信号处理实现 Ctrl+C 安全退出。
std::atomic_bool               g_stop { false };
std::atomic<net::os::fd_t>     g_listener_fd { net::os::invalid_fd() };
std::atomic<std::atomic_bool*> g_finished_ptr { nullptr };
std::atomic_int                g_active_sessions { 0 };
std::atomic<uint64_t>          g_next_session_id { 1 };

void on_connection_task_completed(co_wq::Promise_base& promise)
{
    if (auto* counter = static_cast<std::atomic_int*>(promise.mUserData)) {
        counter->fetch_sub(1, std::memory_order_acq_rel);
    }
}

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

// 将解析得到的绝对 URI 请求转换为上游服务器期望的 origin-form。
std::string build_upstream_request(const HttpProxyContext& ctx, const UrlParts& parts)
{
    std::string request;
    request.reserve(ctx.method.size() + parts.path.size() + ctx.body.size() + 128);
    request.append(ctx.method);
    request.push_back(' ');
    request.append(parts.path);
    request.append(" HTTP/");
    request.append(std::to_string(ctx.http_major));
    request.push_back('.');
    request.append(std::to_string(ctx.http_minor));
    request.append("\r\n");

    auto append_host_literal = [](std::string& dest, const std::string& host) {
        if (host.find(':') != std::string::npos) {
            dest.push_back('[');
            dest.append(host);
            dest.push_back(']');
        } else {
            dest.append(host);
        }
    };

    bool has_host_header = false;
    for (const auto& entry : ctx.header_sequence) {
        std::string lower = to_lower(entry.name);
        if (lower == "proxy-connection")
            continue;
        if (lower == "connection")
            continue;
        if (lower == "host")
            has_host_header = true;
        request.append(entry.name);
        request.append(": ");
        request.append(entry.value);
        request.append("\r\n");
    }
    if (!has_host_header) {
        request.append("Host: ");
        append_host_literal(request, parts.host);
        if (parts.port != 80) {
            request.push_back(':');
            request.append(std::to_string(parts.port));
        }
        request.append("\r\n");
    }
    request.append("Connection: close\r\n");
    request.append("Proxy-Connection: close\r\n");
    request.append("\r\n");
    request.append(ctx.body);
    return request;
}

// 建立 CONNECT 隧道，将客户端/上游 sockets 互相转发，直到任一方关闭。
Task<void, Work_Promise<SpinLock, void>> handle_connect(net::tcp_socket<SpinLock>& client,
                                                        NetFdWorkqueue&            fdwq,
                                                        const ConnectTarget&       target,
                                                        const std::string&         peer_id)
{
    auto upstream = make_upstream_socket(fdwq);
    CO_WQ_LOG_INFO("[proxy] %s CONNECT %s:%u",
                   peer_id.c_str(),
                   format_host_for_log(target.host).c_str(),
                   static_cast<unsigned>(target.port));
    int rc = co_await connect_upstream(upstream, fdwq, target.host, target.port);
    if (rc != 0) {
        CO_WQ_LOG_WARN("[proxy] %s CONNECT upstream failed %s:%u rc=%d",
                       peer_id.c_str(),
                       target.host.c_str(),
                       static_cast<unsigned>(target.port),
                       rc);
        std::string response = build_http_response(502, "Bad Gateway", "Failed to connect upstream\n");
        (void)co_await client.send_all(response.data(), response.size());
        client.close();
        CO_WQ_LOG_INFO("[proxy] %s tunnel closed status=upstream_connect_failed rc=%d", peer_id.c_str(), rc);
        co_return;
    }

    static constexpr std::string_view established
        = "HTTP/1.1 200 Connection Established\r\nProxy-Agent: co_wq-proxy\r\n\r\n";
    if (co_await client.send_all(established.data(), established.size()) <= 0) {
        client.close();
        upstream.close();
        CO_WQ_LOG_INFO("[proxy] %s tunnel closed status=client_write_failed", peer_id.c_str());
        co_return;
    }

    configure_graceful_close(client, peer_id, "client");
    configure_graceful_close(upstream, peer_id, "upstream");
    configure_tcp_keepalive(client, peer_id, "client");
    configure_tcp_keepalive(upstream, peer_id, "upstream");

    PipeOutcome client_to_upstream_outcome;
    PipeOutcome upstream_to_client_outcome;
    auto        client_to_upstream = pipe_data(client,
                                        upstream,
                                        false,
                                        peer_id + " client->upstream",
                                        client_to_upstream_outcome);
    auto        upstream_to_client = pipe_data(upstream,
                                        client,
                                        true,
                                        peer_id + " upstream->client " + format_host_for_log(target.host) + ':'
                                            + std::to_string(target.port),
                                        upstream_to_client_outcome);
    co_await co_wq::when_all(client_to_upstream, upstream_to_client);

    bool        tunnel_error  = client_to_upstream_outcome.had_error || upstream_to_client_outcome.had_error;
    std::string tunnel_status = tunnel_error ? "error" : "completed";
    CO_WQ_LOG_INFO("[proxy] %s tunnel closed status=%s client->upstream=%s upstream->client=%s",
                   peer_id.c_str(),
                   tunnel_status.c_str(),
                   client_to_upstream_outcome.status.c_str(),
                   upstream_to_client_outcome.status.c_str());
    client.close();
    upstream.close();
    co_return;
}

// 处理常规 HTTP 请求：重新构造请求行/头部并回源，然后将响应回写给客户端。
Task<void, Work_Promise<SpinLock, void>> handle_http_request(HttpProxyContext&          ctx,
                                                             net::tcp_socket<SpinLock>& client,
                                                             NetFdWorkqueue&            fdwq,
                                                             const UrlParts&            parts,
                                                             const std::string&         peer_id)
{
    auto upstream = make_upstream_socket(fdwq);
    CO_WQ_LOG_INFO("[proxy] %s %s %s:%u%s",
                   peer_id.c_str(),
                   ctx.method.c_str(),
                   format_host_for_log(parts.host).c_str(),
                   static_cast<unsigned>(parts.port),
                   parts.path.c_str());
    int rc = co_await connect_upstream(upstream, fdwq, parts.host, parts.port);
    if (rc != 0) {
        CO_WQ_LOG_WARN("[proxy] %s upstream connect failed %s:%u rc=%d",
                       peer_id.c_str(),
                       parts.host.c_str(),
                       static_cast<unsigned>(parts.port),
                       rc);
        std::string response = build_http_response(502, "Bad Gateway", "Failed to connect upstream\n");
        (void)co_await client.send_all(response.data(), response.size());
        client.close();
        CO_WQ_LOG_INFO("[proxy] %s response closed status=upstream_connect_failed rc=%d", peer_id.c_str(), rc);
        co_return;
    }

    std::string request = build_upstream_request(ctx, parts);
    if (co_await upstream.send_all(request.data(), request.size()) <= 0) {
        std::string response = build_http_response(502, "Bad Gateway", "Failed to send request upstream\n");
        (void)co_await client.send_all(response.data(), response.size());
        client.close();
        upstream.close();
        CO_WQ_LOG_INFO("[proxy] %s response closed status=upstream_send_failed", peer_id.c_str());
        co_return;
    }

    configure_graceful_close(client, peer_id, "client");
    configure_graceful_close(upstream, peer_id, "upstream");
    configure_tcp_keepalive(client, peer_id, "client");
    configure_tcp_keepalive(upstream, peer_id, "upstream");

    PipeOutcome response_outcome;
    co_await pipe_data(upstream,
                       client,
                       false,
                       peer_id + " upstream->client " + format_host_for_log(parts.host) + ':'
                           + std::to_string(parts.port),
                       response_outcome);

    std::string response_status = response_outcome.had_error ? "error" : "completed";
    CO_WQ_LOG_INFO("[proxy] %s %s:%u response closed status=%s upstream->client=%s",
                   peer_id.c_str(),
                   format_host_for_log(parts.host).c_str(),
                   static_cast<unsigned>(parts.port),
                   response_status.c_str(),
                   response_outcome.status.c_str());
    client.close();
    upstream.close();
    co_return;
}

// 单个客户端连接生命周期：解析首个请求并根据方法调度处理逻辑。
Task<void, Work_Promise<SpinLock, void>>
handle_proxy_connection(NetFdWorkqueue& fdwq, net::tcp_socket<SpinLock> client, std::string peer_id)
{
    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_message_begin         = on_message_begin;
    settings.on_method                = on_method;
    settings.on_url                   = on_url;
    settings.on_header_field          = on_header_field;
    settings.on_header_value          = on_header_value;
    settings.on_header_value_complete = on_header_value_complete;
    settings.on_headers_complete      = on_headers_complete;
    settings.on_body                  = on_body;
    settings.on_message_complete      = on_message_complete;

    llhttp_t parser;
    llhttp_init(&parser, HTTP_REQUEST, &settings);

    HttpProxyContext ctx;
    parser.data = &ctx;

    std::array<char, 4096> buffer {};
    bool                   parse_error { false };
    std::string            error_reason;
    bool                   received_any_data { false };

    while (!ctx.message_complete) {
        ssize_t n = co_await client.recv(buffer.data(), buffer.size());
        if (n < 0) {
            parse_error  = true;
            error_reason = "socket read error";
            break;
        }
        if (n == 0) {
            if (!received_any_data) {
                client.close();
                co_return;
            }
            llhttp_errno_t finish_err = llhttp_finish(&parser);
            if (finish_err != HPE_OK) {
                parse_error  = true;
                error_reason = llhttp_errno_name(finish_err);
            }
            break;
        }
        received_any_data  = true;
        llhttp_errno_t err = llhttp_execute(&parser, buffer.data(), static_cast<size_t>(n));
        if (err == HPE_PAUSED_UPGRADE || err == HPE_PAUSED) {
            // CONNECT/Upgrade 请求在 headers 完成后会触发暂停；标记完成并退出循环。
            llhttp_resume_after_upgrade(&parser);
            ctx.message_complete = true;
            break;
        } else if (err != HPE_OK) {
            parse_error  = true;
            error_reason = llhttp_get_error_reason(&parser);
            if (error_reason.empty())
                error_reason = llhttp_errno_name(err);
            break;
        }
    }

    if (!ctx.message_complete && !parse_error) {
        parse_error  = true;
        error_reason = "incomplete request";
    }

    if (parse_error) {
        CO_WQ_LOG_ERROR("[proxy] %s parse error: %s", peer_id.c_str(), error_reason.c_str());
        std::string response = build_http_response(400, "Bad Request", "Failed to parse request\n");
        (void)co_await client.send_all(response.data(), response.size());
        client.close();
        co_return;
    }

    CO_WQ_LOG_DEBUG("[proxy] %s %s %s", peer_id.c_str(), ctx.method.c_str(), ctx.url.c_str());

    if (ctx.method == "CONNECT") {
        auto target = parse_connect_target(ctx.url);
        if (!target) {
            CO_WQ_LOG_WARN("[proxy] %s invalid CONNECT target: %s", peer_id.c_str(), ctx.url.c_str());
            std::string response = build_http_response(400, "Bad Request", "Invalid CONNECT target\n");
            (void)co_await client.send_all(response.data(), response.size());
            client.close();
            CO_WQ_LOG_INFO("[proxy] %s tunnel closed status=invalid_connect_target", peer_id.c_str());
            co_return;
        }
        co_await handle_connect(client, fdwq, *target, peer_id);
        co_return;
    }

    auto parts = parse_http_url(ctx.url);
    if (!parts) {
        CO_WQ_LOG_WARN("[proxy] %s received non-absolute URI: %s", peer_id.c_str(), ctx.url.c_str());
        std::string response = build_http_response(400, "Bad Request", "Proxy requires absolute URI\n");
        (void)co_await client.send_all(response.data(), response.size());
        client.close();
        co_return;
    }

    co_await handle_http_request(ctx, client, fdwq, *parts, peer_id);
    co_return;
}

// 监听入站代理端口并为每个连接派生协程处理。
Task<void, Work_Promise<SpinLock, void>> proxy_server(NetFdWorkqueue& fdwq, const std::string& host, uint16_t port)
{
    auto analyze_listen_host = [](const std::string& input) {
        struct Config {
            int         family { AF_INET };
            bool        dual_stack { false };
            std::string bind_host;
        } cfg;
        std::string host = input;
        if (host.empty())
            host = "";
        std::string view = host;
        if (!view.empty() && view.front() == '[' && view.back() == ']')
            view = view.substr(1, view.size() - 2);
        bool host_unspecified = view.empty() || view == "0.0.0.0" || view == "*" || view == "::";
        bool host_ipv6        = view.find(':') != std::string::npos;
        if (host_unspecified) {
            cfg.family     = AF_INET6;
            cfg.dual_stack = true;
            cfg.bind_host  = "::";
        } else if (host_ipv6) {
            cfg.family    = AF_INET6;
            cfg.bind_host = host;
        } else {
            cfg.family    = AF_INET;
            cfg.bind_host = host;
        }
        return cfg;
    };

    auto                        listen_cfg = analyze_listen_host(host);
    net::tcp_listener<SpinLock> listener(fdwq.base(), fdwq.reactor(), listen_cfg.family);
    try {
        listener.bind_listen(listen_cfg.bind_host, port, 128, listen_cfg.dual_stack);
    } catch (const std::exception& ex) {
        std::ostringstream oss;
        oss << "[proxy] failed to bind " << (listen_cfg.bind_host.empty() ? host : listen_cfg.bind_host) << ':' << port
            << ": " << ex.what();
#if defined(_WIN32)
        int wsa_err = WSAGetLastError();
        if (wsa_err != 0) {
            oss << " (WSA error " << wsa_err << ')';
        }
#else
        int sys_err = errno;
        if (sys_err != 0) {
            oss << " (" << std::strerror(sys_err) << " errno=" << sys_err << ')';
        }
#endif
        auto msg = oss.str();
        CO_WQ_LOG_ERROR("%s", msg.c_str());
        listener.close();
        g_listener_fd.store(net::os::invalid_fd(), std::memory_order_release);
        co_return;
    }
    g_listener_fd.store(listener.native_handle(), std::memory_order_release);

    std::string log_host = listen_cfg.bind_host.empty() ? host : listen_cfg.bind_host;
    std::string log_suffix;
    if (listen_cfg.dual_stack && listen_cfg.family == AF_INET6)
        log_suffix = " (dual-stack)";
    auto formatted_host = format_host_for_log(log_host);

    CO_WQ_LOG_INFO("[proxy] listening on %s:%u%s",
                   formatted_host.c_str(),
                   static_cast<unsigned>(port),
                   log_suffix.c_str());

    while (!g_stop.load(std::memory_order_acquire)) {
        int fd = co_await listener.accept();
        if (fd == net::k_accept_fatal) {
            CO_WQ_LOG_ERROR("[proxy] accept fatal error, exiting");
            break;
        }
        if (fd < 0)
            continue;
        uint64_t    session_id = g_next_session_id.fetch_add(1, std::memory_order_relaxed);
        std::string peer_id    = format_peer_id(session_id);
        CO_WQ_LOG_DEBUG("[proxy] %s accepted (fd=%d)", peer_id.c_str(), fd);
        auto socket = fdwq.adopt_tcp_socket(fd);
        auto task   = handle_proxy_connection(fdwq, std::move(socket), std::move(peer_id));
        if (auto coroutine = task.get()) {
            auto& promise = coroutine.promise();
            if (promise.mOnCompleted != nullptr) {
                CO_WQ_LOG_WARN(
                    "[proxy] connection task already has completion callback; skipping active session tracking");
            } else {
                promise.mUserData    = &g_active_sessions;
                promise.mOnCompleted = &on_connection_task_completed;
                g_active_sessions.fetch_add(1, std::memory_order_acq_rel);
            }
        }
        post_to(task, fdwq.base());
    }

    CO_WQ_LOG_INFO("[proxy] stopping");

    listener.close();
    g_listener_fd.store(net::os::invalid_fd(), std::memory_order_release);
    if (int remaining = g_active_sessions.load(std::memory_order_acquire); remaining > 0) {
        CO_WQ_LOG_INFO("[proxy] waiting for %d active session(s) to drain", remaining);
    }
    while (g_active_sessions.load(std::memory_order_acquire) > 0) {
        co_await co_wq::delay_ms(get_sys_timer(), 10);
    }
    co_return;
}

} // namespace

// 程序入口：解析命令行参数并启动主协程。
int main(int argc, char* argv[])
{
    std::string               host                = "0.0.0.0";
    uint16_t                  port                = 8081;
    std::string               log_file_path       = "logs/proxy.log";
    bool                      log_truncate        = true;
    spdlog::level::level_enum requested_log_level = spdlog::level::debug;

    std::filesystem::path project_root;
    std::filesystem::path cwd_path;
    {
        std::error_code cwd_ec;
        cwd_path = std::filesystem::current_path(cwd_ec);
        if (cwd_ec)
            cwd_path.clear();
        if (!cwd_path.empty())
            project_root = find_project_root(cwd_path);
        if (project_root.empty())
            project_root = cwd_path;
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--verbose" || arg == "--debug-log") {
            requested_log_level = spdlog::level::debug;
        } else if (arg == "--no-verbose") {
            requested_log_level = spdlog::level::info;
        } else if (arg == "--quiet") {
            requested_log_level = spdlog::level::warn;
        } else if (arg == "--log-file" && i + 1 < argc) {
            log_file_path = argv[++i];
        } else if (arg == "--log-truncate") {
            log_truncate = true;
        } else if (arg == "--log-append" || arg == "--no-log-truncate") {
            log_truncate = false;
        }
    }

    bool log_configured = false;
    try {
        std::filesystem::path cli_log_path(log_file_path);

        auto cwd = cwd_path;

        std::filesystem::path resolved = cli_log_path;
        if (!resolved.is_absolute()) {
            if (!project_root.empty()) {
                resolved = project_root / resolved;
            } else if (!cwd.empty()) {
                resolved = cwd / resolved;
            }
        }

        resolved = resolved.lexically_normal();

        if (!resolved.empty()) {
            auto parent = resolved.parent_path();
            if (!parent.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(parent, ec);
                if (ec) {
                    std::fprintf(stderr,
                                 "[proxy] failed to create log directory %s: %s\n",
                                 parent.string().c_str(),
                                 ec.message().c_str());
                }
            }
        }

        co_wq::log::configure_file_logging(resolved.string(), log_truncate, true);
        log_configured = true;
        log_file_path  = resolved.string();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[proxy] failed to initialize log file %s: %s\n", log_file_path.c_str(), ex.what());
    }

    co_wq::log::set_level(requested_log_level);
    if (log_configured) {
        CO_WQ_LOG_INFO("[proxy] logging to %s (truncate=%d)", log_file_path.c_str(), log_truncate ? 1 : 0);
    }

#if defined(_WIN32)
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    HMODULE self_module = ::GetModuleHandleW(nullptr);
    CO_WQ_LOG_INFO("[proxy] module base=%p", static_cast<void*>(self_module));
#else
    std::signal(SIGINT, sigint_handler);
#endif

    auto&          wq = get_sys_workqueue(0);
    NetFdWorkqueue fdwq(wq);

    CO_WQ_LOG_INFO("[proxy] starting on %s:%u", host.c_str(), static_cast<unsigned>(port));

    auto  proxy_task = proxy_server(fdwq, host, port);
    auto  coroutine  = proxy_task.get();
    auto& promise    = coroutine.promise();

    std::atomic_bool finished { false };
    promise.mUserData    = &finished;
    promise.mOnCompleted = [](Promise_base& pb) {
        auto* flag = static_cast<std::atomic_bool*>(pb.mUserData);
        if (flag)
            flag->store(true, std::memory_order_release);
    };

    g_finished_ptr.store(&finished, std::memory_order_release);
    post_to(proxy_task, wq);

    sys_wait_until(finished);
    g_finished_ptr.store(nullptr, std::memory_order_release);
    return 0;
}
