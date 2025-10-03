/**
 * @file stream_socket_base.hpp
 * @brief Linux 平台流式/数据报 socket 公共 CRTP 基类，统一串行化收发逻辑。
 */
#pragma once

#include "callback_wq.hpp"
#include "io_serial.hpp"
#include "io_waiter.hpp"
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace co_wq::net::detail {

/**
 * @brief 所有 socket 类型共享的基础核心类，负责 fd 生命周期与 reactor 登记。
 *
 * @tparam lock 工作队列锁类型。
 * @tparam Reactor 反应器模板，需提供 `add_fd/remove_fd/add_waiter` 等接口。
 */
template <lockable lock, template <class> class Reactor> class socket_core {
public:
    socket_core(const socket_core&)            = delete;
    socket_core& operator=(const socket_core&) = delete;

    socket_core(socket_core&& o) noexcept
        : _exec(o._exec)
        , _reactor(o._reactor)
        , _fd(std::exchange(o._fd, -1))
        , _closed(std::exchange(o._closed, false))
        , _cbq(_exec)
    {
        serial_queue_init(_send_q);
        serial_queue_init(_recv_q);
    }
    socket_core& operator=(socket_core&& o) noexcept
    {
        if (this != &o) {
            close_fd();
            _reactor = o._reactor;
            _fd      = std::exchange(o._fd, -1);
            _closed  = std::exchange(o._closed, false);
            std::destroy_at(&_cbq);
            std::construct_at(&_cbq, _exec);
            serial_queue_init(_send_q);
            serial_queue_init(_recv_q);
        }
        return *this;
    }
    ~socket_core() { close_fd(); }

    /** @brief 返回原生文件描述符。 */
    int native_handle() const { return _fd; }
    /** @brief 获取关联执行器。 */
    workqueue<lock>& exec() { return _exec; }
    /** @brief 返回串行化所用锁实例。 */
    lock& serial_lock() { return _io_serial_lock; }
    /** @brief 获取 reactor 指针。 */
    Reactor<lock>* reactor() { return _reactor; }
    /** @brief 访问串行回调队列。 */
    callback_wq<lock>& callback_queue() { return _cbq; }
    /** @brief 发送方向串行队列。 */
    serial_queue& send_queue() { return _send_q; }
    /** @brief 接收方向串行队列。 */
    serial_queue& recv_queue() { return _recv_q; }
    /** @brief 是否已关闭。 */
    bool closed() const noexcept { return _closed; }

protected:
    socket_core(workqueue<lock>& exec, Reactor<lock>& reactor, int domain, int type, int protocol = 0)
        : _exec(exec), _reactor(&reactor), _cbq(exec)
    {
        _fd = ::socket(domain, type | SOCK_CLOEXEC, protocol);
        if (_fd < 0)
            throw std::runtime_error("socket failed");
        init_fd();
    }
    socket_core(int fd, workqueue<lock>& exec, Reactor<lock>& reactor)
        : _exec(exec), _reactor(&reactor), _fd(fd), _cbq(exec)
    {
        if (_fd < 0)
            throw std::runtime_error("invalid fd");
        init_fd();
    }

    /**
     * @brief 关闭底层 fd，清理串行队列并唤醒等待者。
     */
    void close_fd()
    {
        if (_fd >= 0) {
            _closed = true;
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

    /**
     * @brief connect 协程的 awaiter，实现 EINPROGRESS -> 等待 EPOLLOUT 的流程。
     */
    template <class Derived, class Provider> struct connect_awaiter : io_waiter_base {
        Derived& owner;
        Provider provider;
        int      ret { 0 };
        connect_awaiter(Derived& s, Provider p) : owner(s), provider(std::move(p)) { }
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h          = h;
            this->route_ctx  = &owner.callback_queue();
            this->route_post = &callback_wq<lock>::post_adapter;
            INIT_LIST_HEAD(&this->ws_node);
            sockaddr_storage addr {};
            socklen_t        len { 0 };
            if (!provider.build(addr, len)) {
                ret = -1;
                owner.exec().post(*this);
                return;
            }
            int r = ::connect(owner.native_handle(), reinterpret_cast<sockaddr*>(&addr), len);
            if (r == 0) {
                ret = 0;
                owner.exec().post(*this);
                return;
            }
            if (r < 0 && errno != EINPROGRESS) {
                ret = -1;
                owner.exec().post(*this);
                return;
            }
            owner.reactor()->add_waiter(owner.native_handle(), EPOLLOUT, this);
        }
        int await_resume() noexcept
        {
            if (ret == 0) {
                int       err = 0;
                socklen_t len = sizeof(err);
                if (::getsockopt(owner.native_handle(), SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
                    return 0;
                errno = err;
                return -1;
            }
            return ret;
        }
    };

private:
    void init_fd()
    {
        set_non_block();
        _reactor->add_fd(_fd);
        serial_queue_init(_send_q);
        serial_queue_init(_recv_q);
        _closed = false;
    }
    void set_non_block()
    {
        int flags = ::fcntl(_fd, F_GETFL, 0);
        if (flags >= 0)
            ::fcntl(_fd, F_SETFL, flags | O_NONBLOCK);
    }

    workqueue<lock>&  _exec;
    Reactor<lock>*    _reactor { nullptr };
    int               _fd { -1 };
    bool              _closed { false };
    lock              _io_serial_lock;
    serial_queue      _send_q;
    serial_queue      _recv_q;
    callback_wq<lock> _cbq;
};

/**
 * @brief 面向流式 socket 的公共基类，封装读写 awaiter 与状态位。
 */
template <class Derived, lockable lock, template <class> class Reactor>
class stream_socket_base : public socket_core<lock, Reactor> {
    using core = socket_core<lock, Reactor>;

public:
    stream_socket_base(const stream_socket_base&)                = delete;
    stream_socket_base& operator=(const stream_socket_base&)     = delete;
    stream_socket_base(stream_socket_base&&) noexcept            = default;
    stream_socket_base& operator=(stream_socket_base&&) noexcept = default;
    ~stream_socket_base()                                        = default;

    /**
     * @brief 关闭 socket 并重置状态标志。
     */
    void close()
    {
        core::close_fd();
        _rx_eof      = false;
        _tx_shutdown = false;
    }

    /**
     * @brief 主动关闭写半部。
     */
    void shutdown_tx()
    {
        if (this->native_handle() >= 0 && !_tx_shutdown) {
            ::shutdown(this->native_handle(), SHUT_WR);
            _tx_shutdown = true;
        }
    }

    /** @brief 写半部是否已关闭。 */
    bool tx_shutdown() const noexcept { return _tx_shutdown; }
    /** @brief 是否检测到对端关闭。 */
    bool rx_eof() const noexcept { return _rx_eof; }

    /** @brief 标记收到 EOF。 */
    void mark_rx_eof() { _rx_eof = true; }
    /** @brief 标记写通道不可用。 */
    void mark_tx_shutdown() { _tx_shutdown = true; }

    template <class Provider> auto connect_with(Provider provider)
    {
        return typename core::template connect_awaiter<Derived, Provider>(static_cast<Derived&>(*this),
                                                                          std::move(provider));
    }

    /**
     * @brief 非阻塞 recv 协程驱动器，支持全量/部分读取。
     */
    struct recv_awaiter : two_phase_drain_awaiter<recv_awaiter, Derived> {
        bool    full { false };
        char*   buf { nullptr };
        size_t  len { 0 };
        size_t  recvd { 0 };
        ssize_t err { 0 };
        recv_awaiter(Derived& s, void* b, size_t l, bool f)
            : two_phase_drain_awaiter<recv_awaiter, Derived>(s, s.recv_queue())
            , full(f)
            , buf(static_cast<char*>(b))
            , len(l)
        {
            this->route_ctx  = &s.callback_queue();
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
                recvd += static_cast<size_t>(n);
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
        ssize_t await_resume() noexcept { return (err < 0 && recvd == 0) ? err : static_cast<ssize_t>(recvd); }
    };

    /**
     * @brief 非阻塞 send/writev 协程驱动器，支持全量发送。
     */
    struct send_awaiter : two_phase_drain_awaiter<send_awaiter, Derived> {
        bool               full { false };
        const char*        buf { nullptr };
        size_t             len { 0 };
        std::vector<iovec> vec;
        bool               use_vec { false };
        size_t             sent { 0 };
        ssize_t            err { 0 };
        send_awaiter(Derived& s, const void* b, size_t l, bool f)
            : two_phase_drain_awaiter<send_awaiter, Derived>(s, s.send_queue())
            , full(f)
            , buf(static_cast<const char*>(b))
            , len(l)
        {
            this->route_ctx  = &s.callback_queue();
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        send_awaiter(Derived& s, const struct iovec* iov, int iovcnt, bool f)
            : two_phase_drain_awaiter<send_awaiter, Derived>(s, s.send_queue()), full(f), use_vec(true)
        {
            vec.reserve(static_cast<size_t>(iovcnt));
            for (int i = 0; i < iovcnt; ++i) {
                vec.push_back(iov[i]);
                len += iov[i].iov_len;
            }
            this->route_ctx  = &s.callback_queue();
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
                    remain = 0;
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
                        sent += static_cast<size_t>(n);
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
                ssize_t n = ::writev(this->owner.native_handle(), vec.data(), static_cast<int>(vec.size()));
                if (n > 0) {
                    sent += static_cast<size_t>(n);
                    advance_iov(static_cast<size_t>(n));
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
        ssize_t await_resume() noexcept { return (err < 0 && sent == 0) ? err : static_cast<ssize_t>(sent); }
    };

    /** @brief 读取至多 len 字节。 */
    recv_awaiter recv(void* buf, size_t len) { return recv_awaiter(static_cast<Derived&>(*this), buf, len, false); }
    /** @brief 读取恰好 len 字节或遇到 EOF。 */
    recv_awaiter recv_all(void* buf, size_t len) { return recv_awaiter(static_cast<Derived&>(*this), buf, len, true); }
    /** @brief 发送至多 len 字节。 */
    send_awaiter send(const void* buf, size_t len)
    {
        return send_awaiter(static_cast<Derived&>(*this), buf, len, false);
    }
    /** @brief 发送恰好 len 字节或错误返回。 */
    send_awaiter send_all(const void* buf, size_t len)
    {
        return send_awaiter(static_cast<Derived&>(*this), buf, len, true);
    }
    /** @brief writev 版本的部分发送。 */
    send_awaiter sendv(const struct iovec* iov, int iovcnt)
    {
        return send_awaiter(static_cast<Derived&>(*this), iov, iovcnt, false);
    }
    /** @brief writev 版本的全量发送。 */
    send_awaiter send_all(const struct iovec* iov, int iovcnt)
    {
        return send_awaiter(static_cast<Derived&>(*this), iov, iovcnt, true);
    }

protected:
    stream_socket_base(workqueue<lock>& exec, Reactor<lock>& reactor, int domain, int type, int protocol = 0)
        : core(exec, reactor, domain, type, protocol)
    {
    }
    stream_socket_base(int fd, workqueue<lock>& exec, Reactor<lock>& reactor) : core(fd, exec, reactor) { }

private:
    bool _rx_eof { false };
    bool _tx_shutdown { false };
};

/**
 * @brief UDP/Unix Datagram 通用基类，提供 recvfrom/sendto awaiter。
 */
template <class Derived, lockable lock, template <class> class Reactor, class Address, class AddressLen>
class datagram_socket_base : public socket_core<lock, Reactor> {
    using core = socket_core<lock, Reactor>;

public:
    using address_type        = Address;
    using address_length_type = AddressLen;

    datagram_socket_base(const datagram_socket_base&)                = delete;
    datagram_socket_base& operator=(const datagram_socket_base&)     = delete;
    datagram_socket_base(datagram_socket_base&&) noexcept            = default;
    datagram_socket_base& operator=(datagram_socket_base&&) noexcept = default;
    ~datagram_socket_base()                                          = default;

    void close() { core::close_fd(); }

    template <class Provider> auto connect_with(Provider provider)
    {
        return typename core::template connect_awaiter<Derived, Provider>(static_cast<Derived&>(*this),
                                                                          std::move(provider));
    }

    /**
     * @brief recvfrom 协程驱动器。
     */
    struct recvfrom_awaiter : two_phase_drain_awaiter<recvfrom_awaiter, Derived> {
        void*                buf { nullptr };
        size_t               len { 0 };
        address_type*        out_addr { nullptr };
        address_length_type* out_len { nullptr };
        ssize_t              nread { -1 };
        recvfrom_awaiter(Derived& s, void* b, size_t l, address_type* addr, address_length_type* alen)
            : two_phase_drain_awaiter<recvfrom_awaiter, Derived>(s, s.recv_queue())
            , buf(b)
            , len(l)
            , out_addr(addr)
            , out_len(alen)
        {
            this->route_ctx  = &s.callback_queue();
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        static void register_wait(recvfrom_awaiter* self, bool)
        {
            self->owner.reactor()->add_waiter_custom(self->owner.native_handle(), EPOLLIN, self);
        }
        int attempt_once()
        {
            sockaddr*            sa      = out_addr ? reinterpret_cast<sockaddr*>(out_addr) : nullptr;
            address_length_type* len_ptr = out_len;
            nread                        = ::recvfrom(this->owner.native_handle(), buf, len, MSG_DONTWAIT, sa, len_ptr);
            if (nread >= 0)
                return 0;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return -1;
            return 0;
        }
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
     * @brief send/sendto/sendmsg 协程驱动器。
     */
    struct send_awaiter : two_phase_drain_awaiter<send_awaiter, Derived> {
        struct dest_info {
            address_type        addr;
            address_length_type len;
        };
        bool                     use_vec { false };
        std::optional<dest_info> dest;
        const char*              buf { nullptr };
        size_t                   len { 0 };
        std::vector<iovec>       vec;
        ssize_t                  nsent { 0 };
        send_awaiter(Derived& s, const void* b, size_t l)
            : two_phase_drain_awaiter<send_awaiter, Derived>(s, s.send_queue())
            , buf(static_cast<const char*>(b))
            , len(l)
        {
            this->route_ctx  = &s.callback_queue();
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        send_awaiter(Derived& s, const void* b, size_t l, const address_type& d)
            : two_phase_drain_awaiter<send_awaiter, Derived>(s, s.send_queue())
            , dest(dest_info { d, Derived::address_length(d) })
            , buf(static_cast<const char*>(b))
            , len(l)
        {
            this->route_ctx  = &s.callback_queue();
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        send_awaiter(Derived& s, const struct iovec* iov, int iovcnt)
            : two_phase_drain_awaiter<send_awaiter, Derived>(s, s.send_queue()), use_vec(true)
        {
            vec.reserve(static_cast<size_t>(iovcnt));
            for (int i = 0; i < iovcnt; ++i) {
                vec.push_back(iov[i]);
                len += iov[i].iov_len;
            }
            this->route_ctx  = &s.callback_queue();
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        send_awaiter(Derived& s, const struct iovec* iov, int iovcnt, const address_type& d)
            : two_phase_drain_awaiter<send_awaiter, Derived>(s, s.send_queue())
            , use_vec(true)
            , dest(dest_info { d, Derived::address_length(d) })
        {
            vec.reserve(static_cast<size_t>(iovcnt));
            for (int i = 0; i < iovcnt; ++i) {
                vec.push_back(iov[i]);
                len += iov[i].iov_len;
            }
            this->route_ctx  = &s.callback_queue();
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        static void register_wait(send_awaiter* self, bool)
        {
            self->owner.reactor()->add_waiter_custom(self->owner.native_handle(), EPOLLOUT, self);
        }
        int attempt_once()
        {
            if (!use_vec) {
                if (dest) {
                    nsent = ::sendto(this->owner.native_handle(),
                                     buf,
                                     len,
                                     MSG_DONTWAIT | MSG_NOSIGNAL,
                                     reinterpret_cast<const sockaddr*>(&dest->addr),
                                     dest->len);
                } else {
                    nsent = ::send(this->owner.native_handle(), buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
                }
            } else {
                if (dest) {
                    msghdr msg {};
                    msg.msg_name    = &dest->addr;
                    msg.msg_namelen = dest->len;
                    msg.msg_iov     = vec.data();
                    msg.msg_iovlen  = static_cast<size_t>(vec.size());
                    nsent           = ::sendmsg(this->owner.native_handle(), &msg, MSG_DONTWAIT);
                } else {
                    nsent = ::writev(this->owner.native_handle(), vec.data(), static_cast<int>(vec.size()));
                }
            }
            if (nsent >= 0)
                return 0;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return -1;
            return 0;
        }
        ssize_t await_resume() noexcept
        {
            if (nsent < 0 && this->owner.closed()) {
                errno = ECANCELED;
                return -1;
            }
            return nsent;
        }
    };

    recvfrom_awaiter recv_from(void* buf, size_t len, address_type* addr, address_length_type* alen)
    {
        return recvfrom_awaiter(static_cast<Derived&>(*this), buf, len, addr, alen);
    }
    send_awaiter send(const void* buf, size_t len) { return send_awaiter(static_cast<Derived&>(*this), buf, len); }
    send_awaiter send_to(const void* buf, size_t len, const address_type& dest_addr)
    {
        return send_awaiter(static_cast<Derived&>(*this), buf, len, dest_addr);
    }
    send_awaiter sendv(const struct iovec* iov, int iovcnt)
    {
        return send_awaiter(static_cast<Derived&>(*this), iov, iovcnt);
    }
    send_awaiter sendv_to(const struct iovec* iov, int iovcnt, const address_type& dest_addr)
    {
        return send_awaiter(static_cast<Derived&>(*this), iov, iovcnt, dest_addr);
    }

protected:
    datagram_socket_base(workqueue<lock>& exec, Reactor<lock>& reactor, int domain, int type, int protocol = 0)
        : core(exec, reactor, domain, type, protocol)
    {
    }
    datagram_socket_base(int fd, workqueue<lock>& exec, Reactor<lock>& reactor) : core(fd, exec, reactor) { }
};

} // namespace co_wq::net::detail
