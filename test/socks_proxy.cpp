//
// SOCKS4/SOCKS5 proxy example built on the co_wq coroutine framework.
// Demonstrates handling of CONNECT commands for both protocol versions,
// negotiating with clients, establishing upstream TCP connections, and
// bridging data bidirectionally using the workqueue reactor.
//

#include "syswork.hpp"
#include "test_sys_stats_logger.hpp"

#include "dns_resolver.hpp"
#include "tcp_listener.hpp"
#include "tcp_socket.hpp"
#include "when_all.hpp"
#include "worker.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <basetsd.h>
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#endif

using namespace co_wq;

using NetFdWorkqueue = net::fd_workqueue<SpinLock, net::epoll_reactor>;

namespace {

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

std::string format_peer_id(int fd)
{
    std::ostringstream oss;
    oss << "[client fd=" << fd << ']';
    return oss.str();
}

net::tcp_socket<SpinLock> make_upstream_socket(NetFdWorkqueue& fdwq)
{
    static std::atomic_bool warned { false };
    try {
        return fdwq.make_tcp_socket(AF_INET6, true);
    } catch (const std::exception& ex) {
        if (!warned.exchange(true)) {
            CO_WQ_LOG_WARN("[socks] dual-stack upstream socket unavailable, falling back to IPv4: %s", ex.what());
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

    ::linger linger_opts {};
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

    int buffer_hint = 1 << 20;
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
    settings.keepalivetime     = 60'000;
    settings.keepaliveinterval = 5'000;
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

template <typename SrcSocket, typename DstSocket>
Task<void, Work_Promise<SpinLock, void>>
pipe_data(SrcSocket& src, DstSocket& dst, bool propagate_shutdown, std::string flow_desc)
{
    const char*            flow = flow_desc.empty() ? "stream" : flow_desc.c_str();
    std::array<char, 8192> buffer {};
    while (true) {
        ssize_t n = co_await src.recv(buffer.data(), buffer.size());
        if (n == 0) {
            CO_WQ_LOG_DEBUG("[socks] %s closed (EOF)", flow);
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
            CO_WQ_LOG_WARN("[socks] %s recv error rc=%lld", flow, static_cast<long long>(n));
            break;
        }

        size_t offset = 0;
        while (offset < static_cast<size_t>(n)) {
            ssize_t sent = co_await dst.send(buffer.data() + offset, static_cast<size_t>(n) - offset);
            if (sent <= 0) {
                CO_WQ_LOG_WARN("[socks] %s send error rc=%lld", flow, static_cast<long long>(sent));
                co_return;
            }
            offset += static_cast<size_t>(sent);
        }
    }

    CO_WQ_LOG_DEBUG("[socks] %s forwarding finished", flow);
    co_return;
}

Task<int, Work_Promise<SpinLock, int>>
connect_upstream(net::tcp_socket<SpinLock>& socket, NetFdWorkqueue& fdwq, const std::string& host, uint16_t port)
{
    (void)fdwq;
    net::dns::resolve_options opts;
    opts.family           = socket.family();
    opts.allow_dual_stack = socket.dual_stack();
    auto resolved         = net::dns::resolve_sync(host, port, opts);
    if (!resolved.success)
        co_return -1;
    co_return co_await socket.connect(reinterpret_cast<const sockaddr*>(&resolved.storage), resolved.length);
}

struct SocksRequest {
    enum class Version { Socks4, Socks5 } version { Version::Socks5 };
    std::string            host;
    uint16_t               port { 0 };
    std::array<uint8_t, 4> socks4_addr { 0, 0, 0, 0 };
};

Task<bool, Work_Promise<SpinLock, bool>> read_exact(net::tcp_socket<SpinLock>& socket, void* buffer, size_t length)
{
    auto*  bytes  = static_cast<std::byte*>(buffer);
    size_t offset = 0;
    while (offset < length) {
        ssize_t n = co_await socket.recv(reinterpret_cast<char*>(bytes + offset), length - offset);
        if (n <= 0)
            co_return false;
        offset += static_cast<size_t>(n);
    }
    co_return true;
}

Task<std::optional<std::string>, Work_Promise<SpinLock, std::optional<std::string>>>
read_cstring(net::tcp_socket<SpinLock>& socket, size_t max_len)
{
    std::string result;
    result.reserve(16);
    while (result.size() < max_len) {
        char    ch = 0;
        ssize_t n  = co_await socket.recv(&ch, 1);
        if (n <= 0)
            co_return std::nullopt;
        if (ch == '\0')
            break;
        result.push_back(ch);
    }
    if (result.size() >= max_len)
        co_return std::nullopt;
    co_return result;
}

Task<std::optional<SocksRequest>, Work_Promise<SpinLock, std::optional<SocksRequest>>>
handle_socks4_handshake(net::tcp_socket<SpinLock>& client, const std::string& peer_id)
{
    uint8_t cmd = 0;
    if (!co_await read_exact(client, &cmd, 1))
        co_return std::nullopt;
    if (cmd != 0x01) {
        CO_WQ_LOG_WARN("[socks] %s unsupported SOCKS4 command %u", peer_id.c_str(), static_cast<unsigned>(cmd));
        std::array<uint8_t, 8> response { uint8_t { 0x00 }, uint8_t { 0x5B }, uint8_t { 0x00 }, uint8_t { 0x00 },
                                          uint8_t { 0x00 }, uint8_t { 0x00 }, uint8_t { 0x00 }, uint8_t { 0x00 } };
        (void)co_await client.send_all(reinterpret_cast<const char*>(response.data()), response.size());
        co_return std::nullopt;
    }

    std::array<uint8_t, 2> port_bytes {};
    if (!co_await read_exact(client, port_bytes.data(), port_bytes.size()))
        co_return std::nullopt;
    uint16_t port = static_cast<uint16_t>((static_cast<uint16_t>(port_bytes[0]) << 8) | port_bytes[1]);

    std::array<uint8_t, 4> addr_bytes {};
    if (!co_await read_exact(client, addr_bytes.data(), addr_bytes.size()))
        co_return std::nullopt;

    auto user_id = co_await read_cstring(client, 256);
    if (!user_id)
        co_return std::nullopt;

    std::string host;
    if (addr_bytes[0] == 0 && addr_bytes[1] == 0 && addr_bytes[2] == 0 && addr_bytes[3] != 0) {
        auto domain = co_await read_cstring(client, 256);
        if (!domain) {
            std::array<uint8_t, 8> response { uint8_t { 0x00 }, uint8_t { 0x5B }, uint8_t { 0x00 }, uint8_t { 0x00 },
                                              uint8_t { 0x00 }, uint8_t { 0x00 }, uint8_t { 0x00 }, uint8_t { 0x00 } };
            (void)co_await client.send_all(reinterpret_cast<const char*>(response.data()), response.size());
            co_return std::nullopt;
        }
        host = *domain;
    } else {
        std::ostringstream oss;
        oss << static_cast<int>(addr_bytes[0]) << '.' << static_cast<int>(addr_bytes[1]) << '.'
            << static_cast<int>(addr_bytes[2]) << '.' << static_cast<int>(addr_bytes[3]);
        host = oss.str();
    }

    SocksRequest req;
    req.version     = SocksRequest::Version::Socks4;
    req.host        = std::move(host);
    req.port        = port;
    req.socks4_addr = addr_bytes;
    CO_WQ_LOG_DEBUG("[socks] %s SOCKS4 request host=%s port=%u", peer_id.c_str(), req.host.c_str(), port);
    co_return req;
}

Task<bool, Work_Promise<SpinLock, bool>> handle_socks5_method_negotiation(net::tcp_socket<SpinLock>& client,
                                                                          const std::string&         peer_id)
{
    uint8_t method_count = 0;
    if (!co_await read_exact(client, &method_count, 1))
        co_return false;
    std::vector<uint8_t> methods(method_count);
    if (method_count > 0) {
        if (!co_await read_exact(client, methods.data(), methods.size()))
            co_return false;
    }

    bool supports_no_auth = false;
    for (uint8_t method : methods) {
        if (method == 0x00)
            supports_no_auth = true;
    }
    std::array<uint8_t, 2> response { uint8_t { 0x05 }, uint8_t { 0x00 } };
    response[1] = supports_no_auth ? uint8_t { 0x00 } : uint8_t { 0xFF };
    if (co_await client.send_all(reinterpret_cast<const char*>(response.data()), response.size()) <= 0)
        co_return false;
    if (!supports_no_auth) {
        CO_WQ_LOG_WARN("[socks] %s SOCKS5 no authentication method not offered", peer_id.c_str());
        co_return false;
    }
    co_return true;
}

Task<std::optional<SocksRequest>, Work_Promise<SpinLock, std::optional<SocksRequest>>>
handle_socks5_handshake(net::tcp_socket<SpinLock>& client, const std::string& peer_id)
{
    if (!co_await handle_socks5_method_negotiation(client, peer_id))
        co_return std::nullopt;

    std::array<uint8_t, 4> header {};
    if (!co_await read_exact(client, header.data(), header.size()))
        co_return std::nullopt;

    if (header[0] != 0x05) {
        CO_WQ_LOG_WARN("[socks] %s invalid SOCKS5 version %u", peer_id.c_str(), static_cast<unsigned>(header[0]));
        co_return std::nullopt;
    }
    if (header[1] != 0x01) {
        CO_WQ_LOG_WARN("[socks] %s unsupported SOCKS5 command %u", peer_id.c_str(), static_cast<unsigned>(header[1]));
        std::array<uint8_t, 10> response { uint8_t { 0x05 }, uint8_t { 0x07 }, uint8_t { 0x00 }, uint8_t { 0x01 },
                                           uint8_t { 0x00 }, uint8_t { 0x00 }, uint8_t { 0x00 }, uint8_t { 0x00 },
                                           uint8_t { 0x00 }, uint8_t { 0x00 } };
        (void)co_await client.send_all(reinterpret_cast<const char*>(response.data()), response.size());
        co_return std::nullopt;
    }

    SocksRequest req;
    req.version = SocksRequest::Version::Socks5;

    switch (header[3]) {
    case 0x01: { // IPv4
        std::array<uint8_t, 4> addr {};
        if (!co_await read_exact(client, addr.data(), addr.size()))
            co_return std::nullopt;
        std::ostringstream oss;
        oss << static_cast<int>(addr[0]) << '.' << static_cast<int>(addr[1]) << '.' << static_cast<int>(addr[2]) << '.'
            << static_cast<int>(addr[3]);
        req.host = oss.str();
        break;
    }
    case 0x03: { // Domain
        uint8_t len = 0;
        if (!co_await read_exact(client, &len, 1))
            co_return std::nullopt;
        std::string domain(len, '\0');
        if (len > 0) {
            if (!co_await read_exact(client, domain.data(), domain.size()))
                co_return std::nullopt;
        }
        req.host = std::move(domain);
        break;
    }
    case 0x04: { // IPv6
        std::array<uint8_t, 16> addr {};
        if (!co_await read_exact(client, addr.data(), addr.size()))
            co_return std::nullopt;
        std::ostringstream oss;
        for (size_t i = 0; i < addr.size(); i += 2) {
            uint16_t segment = static_cast<uint16_t>(addr[i] << 8 | addr[i + 1]);
            oss << std::hex << segment;
            if (i + 2 < addr.size())
                oss << ':';
        }
        req.host = oss.str();
        break;
    }
    default:
        CO_WQ_LOG_WARN("[socks] %s unsupported SOCKS5 address type %u",
                       peer_id.c_str(),
                       static_cast<unsigned>(header[3]));
        std::array<uint8_t, 10> response { uint8_t { 0x05 }, uint8_t { 0x08 }, uint8_t { 0x00 }, uint8_t { 0x01 },
                                           uint8_t { 0x00 }, uint8_t { 0x00 }, uint8_t { 0x00 }, uint8_t { 0x00 },
                                           uint8_t { 0x00 }, uint8_t { 0x00 } };
        (void)co_await client.send_all(reinterpret_cast<const char*>(response.data()), response.size());
        co_return std::nullopt;
    }

    std::array<uint8_t, 2> port_bytes {};
    if (!co_await read_exact(client, port_bytes.data(), port_bytes.size()))
        co_return std::nullopt;
    req.port = static_cast<uint16_t>((static_cast<uint16_t>(port_bytes[0]) << 8) | port_bytes[1]);

    CO_WQ_LOG_DEBUG("[socks] %s SOCKS5 request host=%s port=%u", peer_id.c_str(), req.host.c_str(), req.port);
    co_return req;
}

Task<std::optional<SocksRequest>, Work_Promise<SpinLock, std::optional<SocksRequest>>>
negotiate_socks(net::tcp_socket<SpinLock>& client, const std::string& peer_id)
{
    uint8_t version = 0;
    if (!co_await read_exact(client, &version, 1))
        co_return std::nullopt;

    if (version == 0x04) {
        CO_WQ_LOG_DEBUG("[socks] %s negotiating SOCKS4", peer_id.c_str());
        co_return co_await handle_socks4_handshake(client, peer_id);
    }
    if (version == 0x05) {
        CO_WQ_LOG_DEBUG("[socks] %s negotiating SOCKS5", peer_id.c_str());
        co_return co_await handle_socks5_handshake(client, peer_id);
    }

    CO_WQ_LOG_WARN("[socks] %s unsupported SOCKS version %u", peer_id.c_str(), static_cast<unsigned>(version));
    co_return std::nullopt;
}

std::array<uint8_t, 8> build_socks4_reply(bool success, uint16_t port, const std::array<uint8_t, 4>& addr)
{
    std::array<uint8_t, 8> response {};
    response[0] = 0x00;
    response[1] = success ? 0x5A : 0x5B;
    response[2] = static_cast<uint8_t>((port >> 8) & 0xFF);
    response[3] = static_cast<uint8_t>(port & 0xFF);
    response[4] = addr[0];
    response[5] = addr[1];
    response[6] = addr[2];
    response[7] = addr[3];
    return response;
}

std::array<uint8_t, 10> build_socks5_reply(uint8_t reply_code)
{
    std::array<uint8_t, 10> response { 0x05, reply_code, 0x00, 0x01, 0, 0, 0, 0, 0, 0 };
    return response;
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

Task<void, Work_Promise<SpinLock, void>>
handle_socks_connection(NetFdWorkqueue& fdwq, net::tcp_socket<SpinLock> client, std::string peer_id)
{
    auto request_opt = co_await negotiate_socks(client, peer_id);
    if (!request_opt) {
        client.close();
        co_return;
    }
    auto request = *request_opt;

    if (request.port == 0 || request.host.empty()) {
        CO_WQ_LOG_WARN("[socks] %s invalid request host=%s port=%u",
                       peer_id.c_str(),
                       request.host.c_str(),
                       static_cast<unsigned>(request.port));
        if (request.version == SocksRequest::Version::Socks4) {
            auto response = build_socks4_reply(false, request.port, request.socks4_addr);
            (void)co_await client.send_all(reinterpret_cast<const char*>(response.data()), response.size());
        } else {
            auto response = build_socks5_reply(0x01);
            (void)co_await client.send_all(reinterpret_cast<const char*>(response.data()), response.size());
        }
        client.close();
        co_return;
    }

    CO_WQ_LOG_INFO("[socks] %s CONNECT %s:%u",
                   peer_id.c_str(),
                   format_host_for_log(request.host).c_str(),
                   static_cast<unsigned>(request.port));

    auto upstream = make_upstream_socket(fdwq);
    int  rc       = co_await connect_upstream(upstream, fdwq, request.host, request.port);
    if (rc != 0) {
        CO_WQ_LOG_WARN("[socks] %s upstream connect failed host=%s port=%u rc=%d",
                       peer_id.c_str(),
                       request.host.c_str(),
                       static_cast<unsigned>(request.port),
                       rc);
        if (request.version == SocksRequest::Version::Socks4) {
            auto response = build_socks4_reply(false, request.port, request.socks4_addr);
            (void)co_await client.send_all(reinterpret_cast<const char*>(response.data()), response.size());
        } else {
            auto response = build_socks5_reply(0x01);
            (void)co_await client.send_all(reinterpret_cast<const char*>(response.data()), response.size());
        }
        client.close();
        upstream.close();
        co_return;
    }

    if (request.version == SocksRequest::Version::Socks4) {
        auto response = build_socks4_reply(true, request.port, request.socks4_addr);
        if (co_await client.send_all(reinterpret_cast<const char*>(response.data()), response.size()) <= 0) {
            client.close();
            upstream.close();
            co_return;
        }
    } else {
        auto response = build_socks5_reply(0x00);
        if (co_await client.send_all(reinterpret_cast<const char*>(response.data()), response.size()) <= 0) {
            client.close();
            upstream.close();
            co_return;
        }
    }

    configure_graceful_close(client, peer_id, "client");
    configure_graceful_close(upstream, peer_id, "upstream");
    configure_tcp_keepalive(client, peer_id, "client");
    configure_tcp_keepalive(upstream, peer_id, "upstream");

    auto client_to_upstream = pipe_data(client, upstream, false, peer_id + " client->upstream");
    auto upstream_to_client = pipe_data(upstream,
                                        client,
                                        true,
                                        peer_id + " upstream->client " + format_host_for_log(request.host) + ':'
                                            + std::to_string(request.port));
    co_await co_wq::when_all(client_to_upstream, upstream_to_client);

    CO_WQ_LOG_INFO("[socks] %s tunnel closed", peer_id.c_str());
    client.close();
    upstream.close();
    co_return;
}

Task<void, Work_Promise<SpinLock, void>> socks_server(NetFdWorkqueue& fdwq, const std::string& host, uint16_t port)
{
    auto analyze_listen_host = [](const std::string& input) {
        struct Config {
            int         family { AF_INET };
            bool        dual_stack { false };
            std::string bind_host;
        } cfg;
        std::string host_copy = input;
        if (host_copy.empty())
            host_copy = "";
        std::string view = host_copy;
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
            cfg.bind_host = host_copy;
        } else {
            cfg.family    = AF_INET;
            cfg.bind_host = host_copy;
        }
        return cfg;
    };

    auto                        listen_cfg = analyze_listen_host(host);
    net::tcp_listener<SpinLock> listener(fdwq.base(), fdwq.reactor(), listen_cfg.family);
    try {
        listener.bind_listen(listen_cfg.bind_host, port, 128, listen_cfg.dual_stack);
    } catch (const std::exception& ex) {
        std::ostringstream oss;
        oss << "[socks] failed to bind " << (listen_cfg.bind_host.empty() ? host : listen_cfg.bind_host) << ':' << port
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

    CO_WQ_LOG_INFO("[socks] listening on %s:%u%s",
                   formatted_host.c_str(),
                   static_cast<unsigned>(port),
                   log_suffix.c_str());

    while (!g_stop.load(std::memory_order_acquire)) {
        int fd = co_await listener.accept();
        if (fd == net::k_accept_fatal) {
            CO_WQ_LOG_ERROR("[socks] accept fatal error, exiting");
            break;
        }
        if (fd < 0)
            continue;
        std::string peer_id = format_peer_id(fd);
        CO_WQ_LOG_DEBUG("[socks] %s accepted (fd=%d)", peer_id.c_str(), fd);
        auto socket = fdwq.adopt_tcp_socket(fd);
        auto task   = handle_socks_connection(fdwq, std::move(socket), std::move(peer_id));
        post_to(task, fdwq.base());
    }

    CO_WQ_LOG_INFO("[socks] stopping");

    listener.close();
    g_listener_fd.store(net::os::invalid_fd(), std::memory_order_release);
    co_return;
}

} // namespace

int main(int argc, char* argv[])
{
    std::string               host                = "0.0.0.0";
    uint16_t                  port                = 1080;
    std::string               log_file_path       = "logs/socks_proxy.log";
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
                                 "[socks] failed to create log directory %s: %s\n",
                                 parent.string().c_str(),
                                 ec.message().c_str());
                }
            }
        }

        co_wq::log::configure_file_logging(resolved.string(), log_truncate, true);
        log_configured = true;
        log_file_path  = resolved.string();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[socks] failed to initialize log file %s: %s\n", log_file_path.c_str(), ex.what());
    }

    co_wq::log::set_level(requested_log_level);
    if (log_configured) {
        CO_WQ_LOG_INFO("[socks] logging to %s (truncate=%d)", log_file_path.c_str(), log_truncate ? 1 : 0);
    }

#if defined(_WIN32)
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    HMODULE self_module = ::GetModuleHandleW(nullptr);
    CO_WQ_LOG_INFO("[socks] module base=%p", static_cast<void*>(self_module));
#else
    std::signal(SIGINT, sigint_handler);
#endif

    co_wq::test::SysStatsLogger stats_logger("socks_proxy");
    auto&                       wq = get_sys_workqueue(0);
    NetFdWorkqueue              fdwq(wq);

    CO_WQ_LOG_INFO("[socks] starting on %s:%u", host.c_str(), static_cast<unsigned>(port));

    auto  proxy_task = socks_server(fdwq, host, port);
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
