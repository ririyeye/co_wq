/**
 * @file unix_socket.hpp
 * @brief Unix Domain Stream socket 协程原语，实现 connect/send/recv Awaiter。
 */
#pragma once

#include "epoll_reactor.hpp"
#include "stream_socket_base.hpp"
#include "worker.hpp"
#include <cerrno>
#include <cstring>
#include <string>
#include <sys/un.h>

namespace co_wq::net {

template <lockable lock, template <class> class Reactor> class fd_workqueue;

/**
 * @brief Unix Domain 流式 socket 封装。
 */
template <lockable lock, template <class> class Reactor = epoll_reactor>
class unix_socket : public detail::stream_socket_base<unix_socket<lock, Reactor>, lock, Reactor> {
    using base = detail::stream_socket_base<unix_socket<lock, Reactor>, lock, Reactor>;

public:
    unix_socket()                                  = delete;
    unix_socket(const unix_socket&)                = delete;
    unix_socket& operator=(const unix_socket&)     = delete;
    unix_socket(unix_socket&&) noexcept            = default;
    unix_socket& operator=(unix_socket&&) noexcept = default;
    ~unix_socket()                                 = default;

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

    using address_type        = sockaddr_un;
    using address_length_type = socklen_t;

    /**
     * @brief 根据 sun_path 求出地址结构长度。
     */
    static address_length_type address_length(const address_type& addr)
    {
        if (addr.sun_path[0] == '\0') {
            return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + std::strlen(addr.sun_path + 1) + 1);
        }
        return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + std::strlen(addr.sun_path) + 1);
    }

    /**
     * @brief UDS 目标描述，用于 connect awaiter。
     */
    struct uds_endpoint {
        std::string path;
        bool        build(sockaddr_storage& storage, socklen_t& len) const
        {
            auto* addr       = reinterpret_cast<sockaddr_un*>(&storage);
            addr->sun_family = AF_UNIX;
            socklen_t slen   = 0;
            if (!path.empty() && path[0] == '@') {
                size_t plen = path.size();
                if (plen <= 1 || plen - 1 >= sizeof(addr->sun_path)) {
                    errno = ENAMETOOLONG;
                    return false;
                }
                addr->sun_path[0] = '\0';
                std::memcpy(addr->sun_path + 1, path.data() + 1, plen - 1);
                slen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + plen);
            } else {
                if (path.size() >= sizeof(addr->sun_path)) {
                    errno = ENAMETOOLONG;
                    return false;
                }
                std::strncpy(addr->sun_path, path.c_str(), sizeof(addr->sun_path) - 1);
                addr->sun_path[sizeof(addr->sun_path) - 1] = '\0';
                slen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + std::strlen(addr->sun_path) + 1);
            }
            len = slen;
            return true;
        }
    };

    /**
     * @brief 发起异步连接。
     */
    auto connect(const std::string& path) { return this->connect_with(uds_endpoint { path }); }

private:
    friend class fd_workqueue<lock, Reactor>;
    explicit unix_socket(workqueue<lock>& exec, Reactor<lock>& reactor) : base(exec, reactor, AF_UNIX, SOCK_STREAM) { }
    unix_socket(int fd, workqueue<lock>& exec, Reactor<lock>& reactor) : base(fd, exec, reactor) { }
};

// wrappers

/**
 * @brief Task 版 connect。
 */
template <lockable lock> inline Task<int, Work_Promise<lock, int>> async_connect(unix_socket<lock>& s, std::string path)
{
    co_return co_await s.connect(std::move(path));
}

/**
 * @brief Task 版 send_all。
 */
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>> async_send_all(unix_socket<lock>& s, const void* buf, size_t len)
{
    co_return co_await s.send_all(buf, len);
}

/**
 * @brief Task 版 send_all (writev)。
 */
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>>
async_sendv_all(unix_socket<lock>& s, const struct iovec* iov, int iovcnt)
{
    co_return co_await s.send_all(iov, iovcnt);
}

/**
 * @brief Task 版 recv（最多 len 字节）。
 */
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>> async_recv_some(unix_socket<lock>& s, void* buf, size_t len)
{
    co_return co_await s.recv(buf, len);
}

/**
 * @brief Task 版 recv_all。
 */
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>> async_recv_all(unix_socket<lock>& s, void* buf, size_t len)
{
    co_return co_await s.recv_all(buf, len);
}

} // namespace co_wq::net
