/**
 * @file tcp_socket.hpp
 * @brief TCP socket 协程原语，提供 connect/send/recv Awaiter 与便捷封装。
 */
#pragma once

#include "epoll_reactor.hpp"
#include "stream_socket_base.hpp"
#include "worker.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>

namespace co_wq::net {

template <lockable lock, template <class> class Reactor> class fd_workqueue;

/**
 * @brief 基于 `stream_socket_base` 的 TCP socket 实现。
 */
template <lockable lock, template <class> class Reactor = epoll_reactor>
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
     * @brief IPv4 目标描述体，用于 connect awaiter。
     */
    struct ipv4_endpoint {
        std::string host;
        uint16_t    port;
        bool        build(sockaddr_storage& storage, socklen_t& len) const
        {
            auto* addr       = reinterpret_cast<sockaddr_in*>(&storage);
            addr->sin_family = AF_INET;
            addr->sin_port   = htons(port);
            if (::inet_pton(AF_INET, host.c_str(), &addr->sin_addr) <= 0)
                return false;
            len = sizeof(sockaddr_in);
            return true;
        }
    };

    /**
     * @brief 发起异步连接。
     */
    auto connect(const std::string& host, uint16_t port) { return this->connect_with(ipv4_endpoint { host, port }); }

private:
    friend class fd_workqueue<lock, Reactor>;
    explicit tcp_socket(workqueue<lock>& exec, Reactor<lock>& reactor) : base(exec, reactor, AF_INET, SOCK_STREAM) { }
    tcp_socket(int fd, workqueue<lock>& exec, Reactor<lock>& reactor) : base(fd, exec, reactor) { }
};

// wrappers

/**
 * @brief 以 Task 形式封装 `tcp_socket::connect`。
 */
template <lockable lock>
inline Task<int, Work_Promise<lock, int>> async_connect(tcp_socket<lock>& s, const std::string host, uint16_t port)
{
    co_return co_await s.connect(host, port);
}

/**
 * @brief 发送全部缓冲区数据，返回成功字节数。
 */
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>> async_send_all(tcp_socket<lock>& s, const void* buf, size_t len)
{
    co_return co_await s.send_all(buf, len);
}

/**
 * @brief writev 版本的全量发送。
 */
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>>
async_sendv_all(tcp_socket<lock>& s, const struct iovec* iov, int iovcnt)
{
    co_return co_await s.send_all(iov, iovcnt);
}

/**
 * @brief 读取部分数据（至少一次）。
 */
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>> async_recv_some(tcp_socket<lock>& s, void* buf, size_t len)
{
    co_return co_await s.recv(buf, len);
}

/**
 * @brief 读取固定长度数据。
 */
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>> async_recv_all(tcp_socket<lock>& s, void* buf, size_t len)
{
    co_return co_await s.recv_all(buf, len);
}

} // namespace co_wq::net
