/**
 * @file tcp_socket.hpp
 * @brief TCP socket 协程原语，提供 connect/send/recv Awaiter 与便捷封装。
 */
#pragma once

#include "epoll_reactor.hpp"
#include "os_compat.hpp"
#include "stream_socket_base.hpp"
#include "worker.hpp"
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace co_wq::net {

template <co_wq::lockable lock, template <class> class Reactor> class fd_workqueue;

/**
 * @brief 基于 `stream_socket_base` 的 TCP socket 实现。
 */
template <co_wq::lockable lock, template <class> class Reactor = epoll_reactor>
class tcp_socket : public detail::stream_socket_base<tcp_socket<lock, Reactor>, lock, Reactor> {
    using base = detail::stream_socket_base<tcp_socket<lock, Reactor>, lock, Reactor>;

public:
    tcp_socket()                                 = delete;
    tcp_socket(const tcp_socket&)                = delete;
    tcp_socket& operator=(const tcp_socket&)     = delete;
    tcp_socket(tcp_socket&&) noexcept            = default;
    tcp_socket& operator=(tcp_socket&&) noexcept = default;
    ~tcp_socket()                                = default;

    using base::close;
    using base::connect_with;
    using base::exec;
    using base::family;
    using base::mark_rx_eof;
    using base::mark_tx_shutdown;
    using base::native_handle;
    using base::recv;
    using base::recv_all;
    using base::send;
    using base::send_all;
    using base::send_queue;
    using base::sendv;
    using base::serial_lock;
    using base::shutdown_tx;
    using base::tx_shutdown;

    /**
     * @brief 当前 socket 是否允许 IPv4/IPv6 双栈。
     */
    [[nodiscard]] bool dual_stack() const noexcept { return _dual_stack; }

    /**
     * @brief 通用目标描述体，根据地址族/双栈配置解析目标。
     */
    struct dns_endpoint {
        const tcp_socket& sock;
        std::string       host;
        uint16_t          port;
        bool              allow_dual { false };

        [[nodiscard]] static std::string strip_brackets(const std::string& input)
        {
            if (input.size() >= 2 && input.front() == '[' && input.back() == ']')
                return input.substr(1, input.size() - 2);
            return input;
        }

        [[nodiscard]] bool build(sockaddr_storage& storage, socklen_t& len) const
        {
            std::string node = strip_brackets(host);
            if (node.empty())
                return false;

            addrinfo hints {};
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;
            hints.ai_family   = sock.family();
            if (hints.ai_family == AF_INET6 && allow_dual)
                hints.ai_family = AF_UNSPEC;

            char service[16] {};
            std::snprintf(service, sizeof(service), "%u", static_cast<unsigned>(port));

            addrinfo* result = nullptr;
            int       rc     = ::getaddrinfo(node.c_str(), service, &hints, &result);
            if (rc != 0 || !result)
                return false;

            std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard(result, ::freeaddrinfo);
            for (auto* ai = result; ai; ai = ai->ai_next) {
                if (ai->ai_family == sock.family()) {
                    if (ai->ai_addrlen > sizeof(storage))
                        continue;
                    std::memcpy(&storage, ai->ai_addr, ai->ai_addrlen);
                    len = static_cast<socklen_t>(ai->ai_addrlen);
                    return true;
                }
                if (sock.family() == AF_INET6 && allow_dual && ai->ai_family == AF_INET) {
                    auto* v4 = reinterpret_cast<sockaddr_in*>(ai->ai_addr);
                    auto* v6 = reinterpret_cast<sockaddr_in6*>(&storage);
                    std::memset(v6, 0, sizeof(sockaddr_in6));
                    v6->sin6_family           = AF_INET6;
                    v6->sin6_port             = v4->sin_port;
                    v6->sin6_addr.s6_addr[10] = 0xFF;
                    v6->sin6_addr.s6_addr[11] = 0xFF;
                    std::memcpy(&v6->sin6_addr.s6_addr[12], &v4->sin_addr, sizeof(v4->sin_addr));
                    len = static_cast<socklen_t>(sizeof(sockaddr_in6));
                    return true;
                }
            }
            return false;
        }
    };

    struct raw_endpoint {
        sockaddr_storage address {};
        socklen_t        length { 0 };

        [[nodiscard]] bool build(sockaddr_storage& storage, socklen_t& len) const noexcept
        {
            if (length == 0 || length > sizeof(storage))
                return false;
            std::memcpy(&storage, &address, length);
            len = length;
            return true;
        }
    };

    /**
     * @brief 发起异步连接。
     */
    auto connect(const std::string& host, uint16_t port)
    {
        return this->connect_with(dns_endpoint { *this, host, port, dual_stack() });
    }

    auto connect(const sockaddr* addr, socklen_t len)
    {
        raw_endpoint endpoint;
        if (addr && len > 0 && len <= sizeof(endpoint.address)) {
            std::memcpy(&endpoint.address, addr, len);
            endpoint.length = len;
        }
        return this->connect_with(std::move(endpoint));
    }

private:
    friend class fd_workqueue<lock, Reactor>;
    explicit tcp_socket(workqueue<lock>& exec,
                        Reactor<lock>&   reactor,
                        int              fam               = AF_INET,
                        bool             enable_dual_stack = false)
        : base(exec, reactor, fam, SOCK_STREAM), _dual_stack(fam == AF_INET6 ? enable_dual_stack : false)
    {
        if (fam == AF_INET6) {
            int v6only = _dual_stack ? 0 : 1;
            os::setsockopt(this->native_handle(), IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
        }
    }
    tcp_socket(int fd, workqueue<lock>& exec, Reactor<lock>& reactor) : base(fd, exec, reactor)
    {
        if (this->family() == AF_INET6)
            _dual_stack = query_dual_stack_flag();
    }

    bool query_dual_stack_flag() const
    {
        if (this->family() != AF_INET6)
            return false;
        int       v6only = 1;
        socklen_t len    = sizeof(v6only);
        if (os::getsockopt(this->native_handle(), IPPROTO_IPV6, IPV6_V6ONLY, &v6only, &len) != 0)
            return false;
        return v6only == 0;
    }

    bool _dual_stack { false };
};

// wrappers

/**
 * @brief 以 Task 形式封装 `tcp_socket::connect`。
 */
template <co_wq::lockable lock>
inline Task<int, Work_Promise<lock, int>> async_connect(tcp_socket<lock>& s, const std::string host, uint16_t port)
{
    co_return co_await s.connect(host, port);
}

/**
 * @brief 发送全部缓冲区数据，返回成功字节数。
 */
template <co_wq::lockable lock>
inline Task<os::ssize_t, Work_Promise<lock, os::ssize_t>>
async_send_all(tcp_socket<lock>& s, const void* buf, size_t len)
{
    co_return co_await s.send_all(buf, len);
}

/**
 * @brief writev 版本的全量发送。
 */
template <co_wq::lockable lock>
inline Task<os::ssize_t, Work_Promise<lock, os::ssize_t>>
async_sendv_all(tcp_socket<lock>& s, const os::iovec* iov, int iovcnt)
{
    co_return co_await s.send_all(iov, iovcnt);
}

/**
 * @brief 读取部分数据（至少一次）。
 */
template <co_wq::lockable lock>
inline Task<os::ssize_t, Work_Promise<lock, os::ssize_t>> async_recv_some(tcp_socket<lock>& s, void* buf, size_t len)
{
    co_return co_await s.recv(buf, len);
}

/**
 * @brief 读取固定长度数据。
 */
template <co_wq::lockable lock>
inline Task<os::ssize_t, Work_Promise<lock, os::ssize_t>> async_recv_all(tcp_socket<lock>& s, void* buf, size_t len)
{
    co_return co_await s.recv_all(buf, len);
}

} // namespace co_wq::net
