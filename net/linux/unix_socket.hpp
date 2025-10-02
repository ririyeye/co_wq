// unix_socket.hpp - Unix Domain Stream socket 协程原语
#pragma once

#include "callback_wq.hpp"
#include "epoll_reactor.hpp" // 默认 Reactor
#include "io_serial.hpp"
#include "io_waiter.hpp"
#include "worker.hpp"
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace co_wq::net {

// 前置声明: 带 Reactor 模板参数的 fd_workqueue
template <lockable lock, template <class> class Reactor> class fd_workqueue;

/**
 * @brief 基于非阻塞 + epoll(ET) 的 Unix Domain(AF_UNIX) 流式 socket 协程封装。
 *
 * 设计思路与 tcp_socket 基本一致，提供相同的 send/recv awaiter 语义，
 * 以便在本地 IPC 场景获得统一的协程 IO 体验。
 */
template <lockable lock, template <class> class Reactor = epoll_reactor> class unix_socket {
public:
    template <class D> using tp_base         = two_phase_drain_awaiter<D, unix_socket>;
    unix_socket()                            = delete;
    unix_socket(const unix_socket&)          = delete;
    unix_socket& operator=(const unix_socket&) = delete;
    unix_socket(unix_socket&& o) noexcept : _exec(o._exec), _fd(o._fd), _rx_eof(o._rx_eof), _tx_shutdown(o._tx_shutdown)
    {
        o._fd          = -1;
        o._rx_eof      = false;
        o._tx_shutdown = false;
    }
    unix_socket& operator=(unix_socket&& o) noexcept
    {
        if (this != &o) {
            close();
            _exec          = o._exec;
            _fd            = o._fd;
            _rx_eof        = o._rx_eof;
            _tx_shutdown   = o._tx_shutdown;
            o._fd          = -1;
            o._rx_eof      = false;
            o._tx_shutdown = false;
        }
        return *this;
    }
    ~unix_socket() { close(); }
    /**
     * @brief 关闭 socket 并从 reactor 注销。
     */
    void close()
    {
        if (_fd >= 0) {
            if (_reactor)
                _reactor->remove_fd(_fd);
            list_head pending;
            INIT_LIST_HEAD(&pending);
            serial_collect_waiters(_io_serial_lock, { &_send_q, &_recv_q }, pending);
            serial_post_pending(_exec, pending);
            ::close(_fd);
            _fd = -1;
        }
    }
    void shutdown_tx()
    {
        if (_fd >= 0 && !_tx_shutdown) {
            ::shutdown(_fd, SHUT_WR);
            _tx_shutdown = true;
        }
    }
    bool tx_shutdown() const noexcept { return _tx_shutdown; }
    bool rx_eof() const noexcept { return _rx_eof; }
    int  native_handle() const { return _fd; }
    workqueue<lock>& exec() { return _exec; }
    lock&            serial_lock() { return _io_serial_lock; }
    Reactor<lock>*   reactor() { return _reactor; }
    void             mark_rx_eof() { _rx_eof = true; }
    void             mark_tx_shutdown() { _tx_shutdown = true; }

    struct connect_awaiter : io_waiter_base {
        unix_socket& sock;
        std::string  path;
        int          ret { 0 };
        connect_awaiter(unix_socket& s, std::string p) : sock(s), path(std::move(p)) { }
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h          = h;
            this->route_ctx  = &sock._cbq;
            this->route_post = &callback_wq<lock>::post_adapter;
            INIT_LIST_HEAD(&this->ws_node);
            sockaddr_un addr {};
            addr.sun_family = AF_UNIX;
            socklen_t slen;
            if (!path.empty() && path[0] == '@') {
                size_t len = path.size();
                if (len <= 1 || len - 1 >= sizeof(addr.sun_path)) {
                    errno = ENAMETOOLONG;
                    ret   = -1;
                    sock._exec.post(*this);
                    return;
                }
                addr.sun_path[0] = '\0';
                std::memcpy(addr.sun_path + 1, path.data() + 1, len - 1);
                slen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + len);
            } else {
                if (path.size() >= sizeof(addr.sun_path)) {
                    errno = ENAMETOOLONG;
                    ret   = -1;
                    sock._exec.post(*this);
                    return;
                }
                std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
                addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
                slen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + std::strlen(addr.sun_path) + 1);
            }
            int r = ::connect(sock._fd, reinterpret_cast<sockaddr*>(&addr), slen);
            if (r == 0) {
                ret = 0;
                sock._exec.post(*this);
                return;
            }
            if (r < 0 && errno != EINPROGRESS) {
                ret = -1;
                sock._exec.post(*this);
                return;
            }
            sock._reactor->add_waiter(sock._fd, EPOLLOUT, this);
        }
        int await_resume() noexcept
        {
            if (ret == 0) {
                int       err = 0;
                socklen_t len = sizeof(err);
                if (::getsockopt(sock._fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
                    return 0;
                errno = err;
                return -1;
            }
            return ret;
        }
    };

    /**
     * @brief 连接到指定 Unix 域 socket 路径。
     * @param path 文件路径；若以 '@' 开头，使用 Linux 抽象命名空间。
     * @return await 后 0 表示成功，-1 表示失败（errno 提供错误原因）。
     */
    connect_awaiter connect(const std::string& path) { return connect_awaiter(*this, path); }

    struct recv_awaiter : tp_base<recv_awaiter> {
        bool    full { false };
        char*   buf;
        size_t  len;
        size_t  recvd { 0 };
        ssize_t err { 0 };
        recv_awaiter(unix_socket& s, void* b, size_t l, bool f)
            : tp_base<recv_awaiter>(s, s._recv_q), full(f), buf(static_cast<char*>(b)), len(l)
        {
            this->route_ctx  = &this->owner._cbq;
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        static void register_wait(recv_awaiter* self, bool)
        {
            self->owner.reactor()->add_waiter_custom(self->owner.native_handle(), EPOLLIN, self);
        }
        int attempt_once()
        {
            if (recvd >= len)
                return 0;
            ssize_t n = ::recv(this->owner.native_handle(), buf + recvd, len - recvd, MSG_DONTWAIT);
            if (n > 0) {
                recvd += (size_t)n;
                if (!full)
                    return 0;
                return recvd == len ? 0 : 1;
            }
            if (n == 0) {
                this->owner.mark_rx_eof();
                return 0;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return full ? -1 : 0;
            if (recvd == 0)
                err = n;
            return 0;
        }
        ssize_t await_resume() noexcept { return (err < 0 && recvd == 0) ? err : (ssize_t)recvd; }
    };

    recv_awaiter recv(void* buf, size_t len) { return recv_awaiter(*this, buf, len, /*full*/ false); }
    recv_awaiter recv_all(void* buf, size_t len) { return recv_awaiter(*this, buf, len, /*full*/ true); }

    struct send_awaiter : tp_base<send_awaiter> {
        bool               full { false };
        const char*        buf { nullptr };
        size_t             len { 0 };
        std::vector<iovec> vec;
        bool               use_vec { false };
        size_t             sent { 0 };
        ssize_t            err { 0 };
        send_awaiter(unix_socket& s, const void* b, size_t l, bool full_mode)
            : tp_base<send_awaiter>(s, s._send_q), full(full_mode), buf(static_cast<const char*>(b)), len(l)
        {
            this->route_ctx  = &this->owner._cbq;
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        send_awaiter(unix_socket& s, const struct iovec* iov, int iovcnt, bool full_mode)
            : tp_base<send_awaiter>(s, s._send_q), full(full_mode), use_vec(true)
        {
            vec.reserve((size_t)iovcnt);
            for (int i = 0; i < iovcnt; ++i) {
                vec.push_back(iov[i]);
                len += iov[i].iov_len;
            }
            this->route_ctx  = &this->owner._cbq;
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        static void register_wait(send_awaiter* self, bool)
        {
            self->owner.reactor()->add_waiter_custom(self->owner.native_handle(), EPOLLOUT, self);
        }
        void advance_iov(size_t n)
        {
            size_t remain = n;
            while (remain > 0 && !vec.empty()) {
                if (remain >= vec.front().iov_len) {
                    remain -= vec.front().iov_len;
                    vec.erase(vec.begin());
                } else {
                    vec.front().iov_base = static_cast<char*>(vec.front().iov_base) + remain;
                    vec.front().iov_len -= remain;
                    remain               = 0;
                }
            }
        }
        int attempt_once()
        {
            if (!use_vec) {
                while (sent < len) {
                    ssize_t n = ::send(this->owner.native_handle(),
                                       buf + sent,
                                       len - sent,
                                       MSG_DONTWAIT | MSG_NOSIGNAL);
                    if (n > 0) {
                        sent += (size_t)n;
                        if (sent == len)
                            return 0;
                        continue;
                    }
                    if (n == 0)
                        return 0;
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        return full ? -1 : 0;
                    if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN)
                        this->owner.mark_tx_shutdown();
                    err = n;
                    return 0;
                }
                return 0;
            }
            while (!vec.empty()) {
                ssize_t n = ::writev(this->owner.native_handle(), vec.data(), (int)vec.size());
                if (n > 0) {
                    sent += (size_t)n;
                    advance_iov((size_t)n);
                    if (vec.empty())
                        return 0;
                    continue;
                }
                if (n == 0)
                    return 0;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return full ? -1 : 0;
                if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN)
                    this->owner.mark_tx_shutdown();
                err = n;
                return 0;
            }
            return 0;
        }
        ssize_t await_resume() noexcept { return (err < 0 && sent == 0) ? err : (ssize_t)sent; }
    };

    send_awaiter send(const void* buf, size_t len) { return send_awaiter(*this, buf, len, /*full*/ false); }
    send_awaiter sendv(const struct iovec* iov, int iovcnt) { return send_awaiter(*this, iov, iovcnt, /*full*/ false); }
    send_awaiter send_all(const void* buf, size_t len) { return send_awaiter(*this, buf, len, /*full*/ true); }
    send_awaiter send_all(const struct iovec* iov, int iovcnt)
    {
        return send_awaiter(*this, iov, iovcnt, /*full*/ true);
    }

private:
    friend class fd_workqueue<lock, Reactor>;
    explicit unix_socket(workqueue<lock>& exec, Reactor<lock>& reactor) : _exec(exec), _reactor(&reactor)
    {
        _fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (_fd < 0)
            throw std::runtime_error("socket failed");
        set_non_block();
        _reactor->add_fd(_fd);
        serial_queue_init(_send_q);
        serial_queue_init(_recv_q);
    }
    unix_socket(int fd, workqueue<lock>& exec, Reactor<lock>& reactor) : _exec(exec), _reactor(&reactor), _fd(fd)
    {
        set_non_block();
        _reactor->add_fd(_fd);
        serial_queue_init(_send_q);
        serial_queue_init(_recv_q);
    }
    void set_non_block()
    {
        int flags = ::fcntl(_fd, F_GETFL, 0);
        if (flags >= 0)
            ::fcntl(_fd, F_SETFL, flags | O_NONBLOCK);
    }
    workqueue<lock>& _exec;
    Reactor<lock>*   _reactor { nullptr };
    int              _fd { -1 };
    bool             _rx_eof { false };
    bool             _tx_shutdown { false };
    lock              _io_serial_lock;
    serial_queue      _send_q;
    serial_queue      _recv_q;
    callback_wq<lock> _cbq { _exec };
};

// wrappers
template <lockable lock>
inline Task<int, Work_Promise<lock, int>> async_connect(unix_socket<lock>& s, std::string path)
{
    co_return co_await s.connect(path);
}
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>> async_send_all(unix_socket<lock>& s, const void* buf, size_t len)
{
    co_return co_await s.send_all(buf, len);
}
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>>
async_sendv_all(unix_socket<lock>& s, const struct iovec* iov, int iovcnt)
{
    co_return co_await s.send_all(iov, iovcnt);
}
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>> async_recv_some(unix_socket<lock>& s, void* buf, size_t len)
{
    ssize_t n = co_await s.recv(buf, len);
    co_return n;
}
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>> async_recv_all(unix_socket<lock>& s, void* buf, size_t len)
{
    co_return co_await s.recv_all(buf, len);
}

} // namespace co_wq::net
