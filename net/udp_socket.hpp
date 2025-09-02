// udp_socket.hpp - UDP socket coroutine primitives
#pragma once
#ifdef __linux__
#include "epoll_reactor.hpp"
#include "io_serial.hpp"
#include "io_waiter.hpp"
#include "worker.hpp"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace co_wq::net {

template <lockable lock, template <class> class Reactor> class fd_workqueue; // fwd

/**
 * @brief UDP socket 协程封装，支持 connect（可选设定默认对端）、recv_from、send_to。
 */
template <lockable lock, template <class> class Reactor = epoll_reactor> class udp_socket {
public:
    // Helper aliases for awaiter base types
    template <class D> using tp_base         = two_phase_drain_awaiter<D, udp_socket>;
    template <class D> using slot_base       = serial_slot_awaiter<D, udp_socket>;
    udp_socket()                             = delete;
    udp_socket(const udp_socket&)            = delete;
    udp_socket& operator=(const udp_socket&) = delete;
    udp_socket(udp_socket&& o) noexcept : _exec(o._exec), _reactor(o._reactor), _fd(o._fd) { o._fd = -1; }
    udp_socket& operator=(udp_socket&& o) noexcept
    {
        if (this != &o) {
            close();
            _exec    = o._exec;
            _reactor = o._reactor;
            _fd      = o._fd;
            o._fd    = -1;
        }
        return *this;
    }
    ~udp_socket() { close(); }
    /**
     * @brief 关闭 socket 并注销 reactor。
     */
    void close()
    {
        if (_fd >= 0) {
            _closed = true;
            // 先取消 reactor 事件（唤醒 epoll 中的 waiters）
            if (_reactor)
                _reactor->remove_fd(_fd);
            // 取出内部尚未获取锁的发送/接收等待者并批量唤醒（返回取消）
            list_head pending;
            INIT_LIST_HEAD(&pending);
            serial_collect_waiters(_io_serial_lock, { &_send_q, &_recv_q }, pending);
            serial_post_pending(_exec, pending); // 在 await_resume 中检测 _closed
            ::close(_fd);
            _fd = -1;
        }
    }
    int native_handle() const { return _fd; }
    // Access for serial_slot_awaiter
    workqueue<lock>& exec() { return _exec; }
    lock&            serial_lock() { return _io_serial_lock; }
    Reactor<lock>*   reactor() { return _reactor; }
    bool             closed() const { return _closed; }

    // Optional connect to set default peer
    /**
     * @brief 可选 connect awaiter，用于设定默认发送/接收对端。
     */
    struct connect_awaiter : io_waiter_base {
        udp_socket& sock;
        std::string host;
        uint16_t    port;
        int         ret { 0 };
        connect_awaiter(udp_socket& s, std::string h, uint16_t p) : sock(s), host(std::move(h)), port(p) { }
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h = h;
            INIT_LIST_HEAD(&this->ws_node);
            sockaddr_in addr {};
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(port);
            if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
                ret = -1;
                sock._exec.post(*this);
                return;
            }
            int r = ::connect(sock._fd, (sockaddr*)&addr, sizeof(addr));
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
                return -1;
            }
            return ret;
        }
    };
    connect_awaiter connect(const std::string& host, uint16_t port) { return connect_awaiter(*this, host, port); }

    /**
     * @brief 异步 recvfrom awaiter。
     */
    struct recvfrom_awaiter : slot_base<recvfrom_awaiter> {
        void*        buf;
        size_t       len;
        sockaddr_in* out_addr;
        socklen_t*   out_len;
        ssize_t      nread { -1 }; // -1 需等待
        recvfrom_awaiter(udp_socket& s, void* b, size_t l, sockaddr_in* oa, socklen_t* ol)
            : slot_base<recvfrom_awaiter>(s, s._recv_q), buf(b), len(l), out_addr(oa), out_len(ol)
        {
        }
        int attempt_once()
        {
            nread = ::recvfrom(this->owner.native_handle(), buf, len, MSG_DONTWAIT, (sockaddr*)out_addr, out_len);
            if (nread >= 0)
                return 0;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return -1;
            return 0; // error
        }
        static void register_wait(recvfrom_awaiter* self, bool /*first*/)
        {
            self->owner.reactor()->add_waiter_custom(self->owner.native_handle(), EPOLLIN, self);
        }
        bool    await_ready() noexcept { return false; }
        ssize_t await_resume() noexcept
        {
            if (nread < 0 && this->owner.closed()) {
                errno = ECANCELED;
                return -1;
            }
            return nread;
        }
    };
    /**
     * @brief 获取 recvfrom awaiter。
     */
    recvfrom_awaiter recv_from(void* buf, size_t len, sockaddr_in* addr, socklen_t* addrlen)
    {
        return recvfrom_awaiter(*this, buf, len, addr, addrlen);
    }

    /**
     * @brief 异步 sendto awaiter，内部循环直到 EAGAIN。
     */
    struct sendto_awaiter : slot_base<sendto_awaiter> {
        const void*        buf;
        size_t             len;
        const sockaddr_in* dest;
        size_t             sent { 0 };
        sendto_awaiter(udp_socket& s, const void* b, size_t l, const sockaddr_in* d)
            : slot_base<sendto_awaiter>(s, s._send_q), buf(b), len(l), dest(d)
        {
        }
        int attempt_once()
        {
            while (sent < len) {
                ssize_t n = ::sendto(this->owner.native_handle(),
                                     (const char*)buf + sent,
                                     len - sent,
                                     MSG_DONTWAIT | MSG_NOSIGNAL,
                                     (const sockaddr*)dest,
                                     sizeof(*dest));
                if (n > 0) {
                    sent += (size_t)n;
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    return -1;
                break;
            }
            return 0; // done or error
        }
        static void register_wait(sendto_awaiter* self, bool /*first*/)
        {
            self->owner.reactor()->add_waiter_custom(self->owner.native_handle(), EPOLLOUT, self);
        }
        bool    await_ready() noexcept { return false; }
        ssize_t await_resume() noexcept
        {
            if (sent == 0 && this->owner.closed()) {
                errno = ECANCELED;
                return -1;
            }
            return (ssize_t)sent;
        }
    };
    /**
     * @brief 获取 sendto awaiter。
     */
    sendto_awaiter send_to(const void* buf, size_t len, const sockaddr_in& dest)
    {
        return sendto_awaiter(*this, buf, len, &dest);
    }

private:
    friend class fd_workqueue<lock, Reactor>;
    udp_socket(workqueue<lock>& exec, Reactor<lock>& reactor) : _exec(exec), _reactor(&reactor)
    {
        _fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (_fd < 0)
            throw std::runtime_error("udp socket failed");
        set_non_block();
        _reactor->add_fd(_fd);
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
    // IO 串行锁与队列
    lock         _io_serial_lock;
    serial_queue _send_q;
    serial_queue _recv_q;
    bool         _closed { false }; // 是否已关闭，用于取消待定操作
    // release handled via serial_slot_awaiter
};

// Convenience async wrappers
template <lockable lock, template <class> class Reactor>
inline Task<ssize_t, Work_Promise<lock, ssize_t>>
async_udp_recv_from(udp_socket<lock, Reactor>& s, void* buf, size_t len, sockaddr_in* addr, socklen_t* alen)
{
    co_return co_await s.recv_from(buf, len, addr, alen);
}

template <lockable lock, template <class> class Reactor>
inline Task<ssize_t, Work_Promise<lock, ssize_t>>
async_udp_send_to(udp_socket<lock, Reactor>& s, const void* buf, size_t len, const sockaddr_in& dest)
{
    co_return co_await s.send_to(buf, len, dest);
}

} // namespace co_wq::net
#endif
