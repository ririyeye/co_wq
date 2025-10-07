/**
 * @file udp_socket.hpp
 * @brief UDP socket 协程原语，封装 recvfrom/sendto Awaiter。
 */
#pragma once

#include "epoll_reactor.hpp"
#include "stream_socket_base.hpp"
#include "worker.hpp"
#include <string>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace co_wq::net {

template <lockable lock, template <class> class Reactor> class fd_workqueue;

/**
 * @brief 基于 `datagram_socket_base` 的 IPv4 UDP socket 封装。
 *
 * 封装了常见的 `send_to`/`recv_from` Awaiter，并提供一个可选的 `connect` 接口，使得用户
 * 可以像流式套接字一样调用 `send`/`recv`。该实现跨平台复用相同代码，确保在 Linux 与
 * Windows 上具有一致行为。
 */
template <lockable lock, template <class> class Reactor = epoll_reactor>
class udp_socket
    : public detail::datagram_socket_base<udp_socket<lock, Reactor>, lock, Reactor, sockaddr_in, socklen_t> {
    using base = detail::datagram_socket_base<udp_socket<lock, Reactor>, lock, Reactor, sockaddr_in, socklen_t>;

public:
    using address_type        = sockaddr_in;
    using address_length_type = socklen_t;

    /** @brief 返回 IPv4 地址结构长度。 */
    static address_length_type address_length(const address_type&) { return sizeof(address_type); }

    udp_socket()                                 = delete;
    udp_socket(const udp_socket&)                = delete;
    udp_socket& operator=(const udp_socket&)     = delete;
    udp_socket(udp_socket&&) noexcept            = default;
    udp_socket& operator=(udp_socket&&) noexcept = default;
    ~udp_socket()                                = default;

    using base::close;
    using base::connect_with;
    using base::exec;
    using base::native_handle;
    using base::recv_from;
    using base::send;
    using base::send_queue;
    using base::send_to;
    using base::sendv;
    using base::sendv_to;
    using base::serial_lock;

    /**
     * @brief IPv4 目标描述体，用于 connect awaiter。
     */
    /**
     * @brief IPv4 目标描述体，用于 `connect` Awaiter。
     */
    struct ipv4_endpoint {
        std::string host;
        uint16_t    port;
        bool        build(sockaddr_storage& storage, socklen_t& len) const
        {
            auto* addr       = reinterpret_cast<sockaddr_in*>(&storage);
            addr->sin_family = AF_INET;
            addr->sin_port   = htons(port);
            if (inet_pton_ipv4(host, &addr->sin_addr) <= 0)
                return false;
            len = sizeof(sockaddr_in);
            return true;
        }
    };

    /**
     * @brief 连接至远端（可选）。
     *
     * 调用后可直接使用 `send`/`recv` 系列 Awaiter，无需每次传入地址。
     */
    auto connect(const std::string& host, uint16_t port) { return this->connect_with(ipv4_endpoint { host, port }); }

private:
    friend class fd_workqueue<lock, Reactor>;
    /** @brief 由 fd 工作队列构造新的 UDP socket。 */
    explicit udp_socket(workqueue<lock>& exec, Reactor<lock>& reactor) : base(exec, reactor, AF_INET, SOCK_DGRAM) { }

    static int inet_pton_ipv4(const std::string& host, void* addr)
    {
#if defined(_WIN32)
        return ::InetPtonA(AF_INET, host.c_str(), addr);
#else
        return ::inet_pton(AF_INET, host.c_str(), addr);
#endif
    }
};

// Convenience async wrappers

/**
 * @brief 以 Task 形式封装 `recv_from`。
 *
 * @param s    UDP socket 实例。
 * @param buf  存放数据的缓冲区。
 * @param len  缓冲区长度。
 * @param addr 若非空，将写入来源地址。
 * @param alen 地址长度输出参数。
 */
template <lockable lock, template <class> class Reactor>
inline Task<os::ssize_t, Work_Promise<lock, os::ssize_t>>
async_udp_recv_from(udp_socket<lock, Reactor>& s, void* buf, size_t len, sockaddr_in* addr, socklen_t* alen)
{
    co_return co_await s.recv_from(buf, len, addr, alen);
}

/**
 * @brief 以 Task 形式封装 `send_to`。
 *
 * @param s    UDP socket 实例。
 * @param buf  待发送数据。
 * @param len  数据长度。
 * @param dest 目标地址。
 */
template <lockable lock, template <class> class Reactor>
inline Task<os::ssize_t, Work_Promise<lock, os::ssize_t>>
async_udp_send_to(udp_socket<lock, Reactor>& s, const void* buf, size_t len, const sockaddr_in& dest)
{
    co_return co_await s.send_to(buf, len, dest);
}

} // namespace co_wq::net
