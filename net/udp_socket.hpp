// udp_socket.hpp - UDP socket coroutine primitives
#pragma once
#ifdef __linux__
#include "epoll_reactor.hpp"
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
            // 取出内部尚未获取锁的发送/接收等待者并唤醒（返回取消）
            std::vector<worknode*> pending;
            {
                std::scoped_lock lk(_io_serial_lock);
                while (!list_empty(&_send_waiters)) {
                    auto* lh = _send_waiters.next;
                    auto* wn = list_entry(lh, worknode, ws_node);
                    list_del(lh);
                    pending.push_back(wn);
                }
                _send_locked = false;
                while (!list_empty(&_recv_waiters)) {
                    auto* lh = _recv_waiters.next;
                    auto* wn = list_entry(lh, worknode, ws_node);
                    list_del(lh);
                    pending.push_back(wn);
                }
                _recv_locked = false;
            }
            for (auto* wn : pending) {
                wn->func = &io_waiter_base::resume_cb; // 直接恢复协程，它会在 await_resume 检查 _closed
                _exec.post(*wn);
            }
            ::close(_fd);
            _fd = -1;
        }
    }
    int native_handle() const { return _fd; }

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
    struct recvfrom_awaiter : io_waiter_base {
        udp_socket&  sock;
        void*        buf;
        size_t       len;
        sockaddr_in* out_addr;
        socklen_t*   out_len;
        ssize_t      nread { -1 }; // -1 需等待
        bool         have_lock { false };
        recvfrom_awaiter(udp_socket& s, void* b, size_t l, sockaddr_in* oa, socklen_t* ol)
            : sock(s), buf(b), len(l), out_addr(oa), out_len(ol)
        {
        }
        static void lock_acquired_cb(worknode* w)
        {
            auto* self      = static_cast<recvfrom_awaiter*>(w);
            self->have_lock = true;
            self->nread     = ::recvfrom(self->sock._fd,
                                     self->buf,
                                     self->len,
                                     MSG_DONTWAIT,
                                     (sockaddr*)self->out_addr,
                                     self->out_len);
            if (self->nread >= 0) {
                self->sock.release_recv_slot();
                self->func = &io_waiter_base::resume_cb;
                if (self->h)
                    self->h.resume();
                return;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                // error
                self->sock.release_recv_slot();
                self->func = &io_waiter_base::resume_cb;
                if (self->h)
                    self->h.resume();
                return;
            }
            // 需要等待可读
            self->nread = -1;
            self->func  = &recvfrom_awaiter::drive_cb;
            self->sock._reactor->add_waiter_custom(self->sock._fd, EPOLLIN, self);
        }
        static void drive_cb(worknode* w)
        {
            auto* self  = static_cast<recvfrom_awaiter*>(w);
            self->nread = ::recvfrom(self->sock._fd,
                                     self->buf,
                                     self->len,
                                     MSG_DONTWAIT,
                                     (sockaddr*)self->out_addr,
                                     self->out_len);
            self->sock.release_recv_slot();
            self->func = &io_waiter_base::resume_cb;
            if (self->h)
                self->h.resume();
        }
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h    = h;
            this->func = &recvfrom_awaiter::lock_acquired_cb;
            INIT_LIST_HEAD(&this->ws_node);
            {
                std::scoped_lock lk(sock._io_serial_lock);
                if (!sock._recv_locked && list_empty(&sock._recv_waiters)) {
                    sock._recv_locked = true;
                    sock._exec.post(*this);
                    return;
                }
                list_add_tail(&this->ws_node, &sock._recv_waiters);
            }
        }
        ssize_t await_resume() noexcept
        {
            if (nread < 0 && sock._closed) {
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
    struct sendto_awaiter : io_waiter_base {
        udp_socket&        sock;
        const void*        buf;
        size_t             len;
        const sockaddr_in* dest;
        size_t             sent { 0 };
        bool               have_lock { false };
        sendto_awaiter(udp_socket& s, const void* b, size_t l, const sockaddr_in* d) : sock(s), buf(b), len(l), dest(d)
        {
        }
        static void lock_acquired_cb(worknode* w)
        {
            auto* self      = static_cast<sendto_awaiter*>(w);
            self->have_lock = true;
            // drain attempt
            while (self->sent < self->len) {
                ssize_t n = ::sendto(self->sock._fd,
                                     (const char*)self->buf + self->sent,
                                     self->len - self->sent,
                                     MSG_DONTWAIT | MSG_NOSIGNAL,
                                     (const sockaddr*)self->dest,
                                     sizeof(*self->dest));
                if (n > 0) {
                    self->sent += (size_t)n;
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    // need wait
                    self->func = &sendto_awaiter::drive_cb;
                    self->sock._reactor->add_waiter_custom(self->sock._fd, EPOLLOUT, self);
                    return;
                }
                // error or n==0
                break;
            }
            self->sock.release_send_slot();
            self->func = &io_waiter_base::resume_cb;
            if (self->h)
                self->h.resume();
        }
        static void drive_cb(worknode* w)
        {
            auto* self = static_cast<sendto_awaiter*>(w);
            while (self->sent < self->len) {
                ssize_t n = ::sendto(self->sock._fd,
                                     (const char*)self->buf + self->sent,
                                     self->len - self->sent,
                                     MSG_DONTWAIT | MSG_NOSIGNAL,
                                     (const sockaddr*)self->dest,
                                     sizeof(*self->dest));
                if (n > 0) {
                    self->sent += (size_t)n;
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    // still pending
                    self->func = &sendto_awaiter::drive_cb;
                    self->sock._reactor->add_waiter_custom(self->sock._fd, EPOLLOUT, self);
                    return;
                }
                break; // error or n==0
            }
            self->sock.release_send_slot();
            self->func = &io_waiter_base::resume_cb;
            if (self->h)
                self->h.resume();
        }
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h    = h;
            this->func = &sendto_awaiter::lock_acquired_cb;
            INIT_LIST_HEAD(&this->ws_node);
            {
                std::scoped_lock lk(sock._io_serial_lock);
                if (!sock._send_locked && list_empty(&sock._send_waiters)) {
                    sock._send_locked = true;
                    sock._exec.post(*this);
                    return;
                }
                list_add_tail(&this->ws_node, &sock._send_waiters);
            }
        }
        ssize_t await_resume() noexcept
        {
            if (sent == 0 && sock._closed) {
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
    lock      _io_serial_lock;
    list_head _send_waiters;
    bool      _send_locked { false };
    list_head _recv_waiters;
    bool      _recv_locked { false };
    bool      _closed { false }; // 是否已关闭，用于取消待定操作
    void      release_send_slot()
    {
        worknode* next = nullptr;
        {
            std::scoped_lock lk(_io_serial_lock);
            if (!list_empty(&_send_waiters)) {
                auto* lh = _send_waiters.next;
                auto* aw = list_entry(lh, worknode, ws_node);
                list_del(lh);
                next = aw;
            } else {
                _send_locked = false;
            }
        }
        if (next)
            _exec.post(*next);
    }
    void release_recv_slot()
    {
        worknode* next = nullptr;
        {
            std::scoped_lock lk(_io_serial_lock);
            if (!list_empty(&_recv_waiters)) {
                auto* lh = _recv_waiters.next;
                auto* aw = list_entry(lh, worknode, ws_node);
                list_del(lh);
                next = aw;
            } else {
                _recv_locked = false;
            }
        }
        if (next)
            _exec.post(*next);
    }
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
