/**
 * @file stream_socket_base.hpp
 * @brief 跨平台流式/数据报 socket 公共 CRTP 基类，统一串行化收发逻辑。
 */
#pragma once

#include "callback_wq.hpp"
#include "io_serial.hpp"
#include "io_waiter.hpp"
#include "os_compat.hpp"

#if defined(_WIN32)
#include "../io/wepoll/wepoll.h"
#else
#include <sys/epoll.h>
#endif
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace co_wq::net::detail {

using co_wq::lockable;
using os::fd_t;
using os::iovec;
using os::ssize_t;

/**
 * @brief 所有 socket 类型共享的基础核心类，负责 fd 生命周期、reactor 登记与串行队列初始化。
 *
 * 该类不直接暴露给使用者，而是作为 `stream_socket_base` / `datagram_socket_base` 的基类存在，
 * 提供如下能力：
 * - 创建或接管底层 fd，并在构造时注册到指定 reactor；
 * - 提供串行化发送/接收队列，用于保证 awaiter 的 FIFO 顺序；
 * - 在析构或 `close_fd()` 时统一清理由 awaiter 留下的回调节点。
 *
 * @tparam lock    工作队列锁类型，例如 `SpinLock` 或 `std::mutex`。
 * @tparam Reactor 反应器模板，需要实现 `add_fd/remove_fd/add_waiter[_custom]` 等接口。
 */
template <lockable lock, template <class> class Reactor> class socket_core {
public:
    socket_core(const socket_core&)            = delete;
    socket_core& operator=(const socket_core&) = delete;

    socket_core(socket_core&& o) noexcept
        : _exec(o._exec)
        , _reactor(o._reactor)
        , _fd(std::exchange(o._fd, os::invalid_fd()))
        , _closed(std::exchange(o._closed, false))
        , _family(o._family)
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
            _fd      = std::exchange(o._fd, os::invalid_fd());
            _closed  = std::exchange(o._closed, false);
            _family  = o._family;
            std::destroy_at(&_cbq);
            std::construct_at(&_cbq, _exec);
            serial_queue_init(_send_q);
            serial_queue_init(_recv_q);
        }
        return *this;
    }
    ~socket_core() { close_fd(); }

    /** @brief 返回原生文件描述符。 */
    fd_t native_handle() const { return _fd; }
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
    /** @brief 返回 socket 地址族。 */
    int family() const noexcept { return _family; }

protected:
    /**
     * @brief 以给定参数创建一个新的 socket 并注册到 reactor。
     *
     * @param exec     绑定的执行器，所有唤醒操作都会被投递到该工作队列。
     * @param reactor  负责事件驱动的反应器实例。
     * @param domain   地址族，例如 `AF_INET`、`AF_INET6`。
     * @param type     套接字类型，通常为 `SOCK_STREAM` 或 `SOCK_DGRAM`。
     * @param protocol 协议号，默认 0 意味着由系统推导。
     * @throws std::runtime_error 创建 socket 失败时抛出异常。
     */
    socket_core(workqueue<lock>& exec, Reactor<lock>& reactor, int domain, int type, int protocol = 0)
        : _exec(exec), _reactor(&reactor), _family(domain), _cbq(exec)
    {
        _fd = os::create_socket(domain, type | SOCK_CLOEXEC, protocol);
        if (_fd == os::invalid_fd())
            throw std::runtime_error("socket failed");
        init_fd();
    }
    /**
     * @brief 接管一个已经存在的非阻塞 socket。
     *
     * @param fd    需接管的文件描述符。
     * @param exec  绑定的执行器。
     * @param reactor 事件反应器，接管后会将 fd 注册进去。
     * @throws std::runtime_error 当传入的 fd 无效时抛出异常。
     */
    socket_core(fd_t fd, workqueue<lock>& exec, Reactor<lock>& reactor)
        : _exec(exec), _reactor(&reactor), _fd(fd), _cbq(exec)
    {
        if (_fd == os::invalid_fd())
            throw std::runtime_error("invalid fd");
        determine_family();
        init_fd();
    }

    /**
     * @brief 关闭底层 fd，清理串行队列并唤醒等待者。
     */
    void close_fd()
    {
        if (_fd != os::invalid_fd()) {
            _closed = true;
            if (_reactor)
                _reactor->remove_fd(_fd);
            list_head pending;
            INIT_LIST_HEAD(&pending);
            serial_collect_waiters(_io_serial_lock, { &_send_q, &_recv_q }, pending);
            serial_post_pending(_exec, pending);
            os::close_fd(_fd);
            _fd = os::invalid_fd();
        }
    }

    /**
     * @brief connect 协程的 awaiter，实现 EINPROGRESS -> 等待 EPOLLOUT 的流程。
     */
    /**
     * @brief connect 协程的 awaiter，实现 EINPROGRESS -> 等待 EPOLLOUT 的流程。
     *
     * Provider 需提供 `bool build(sockaddr_storage&, socklen_t&)` 方法，用于构造目标地址。
     */
    template <class Derived, class Provider> struct connect_awaiter : io_waiter_base {
        /** @brief Socket 派生类型的引用，用于访问 fd 与执行器。 */
        Derived& owner;
        /** @brief 构造目标地址的 provider。 */
        Provider provider;
        /** @brief connect 结果，0 表示成功，-1 表示失败。 */
        int                                        ret { 0 };
        bool                                       timed_out { false };
        static constexpr std::chrono::milliseconds kConnectTimeout { 10'000 };
        connect_awaiter(Derived& s, Provider p) : owner(s), provider(std::move(p))
        {
            auto& cbq = owner.callback_queue();
            this->store_route_guard(cbq.retain_guard());
            this->route_ctx  = cbq.context();
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        static void timeout_cb(worknode* w)
        {
            auto* self = static_cast<connect_awaiter*>(w);
            if (!self)
                return;
            self->timed_out = true;
            self->ret       = -1;
            self->clear_timeout_callback();
        }
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> resume_handle)
        {
            this->h    = resume_handle;
            this->func = &io_waiter_base::resume_cb;
            INIT_LIST_HEAD(&this->ws_node);
            sockaddr_storage addr {};
            socklen_t        len { 0 };
            if (!provider.build(addr, len)) {
                ret = -1;
                net::post_via_route(owner.exec(), *this);
                return;
            }
            int r = os::connect(owner.native_handle(), reinterpret_cast<sockaddr*>(&addr), len);
            if (r == 0) {
                ret = 0;
                net::post_via_route(owner.exec(), *this);
                return;
            }
            if (r < 0 && errno != EINPROGRESS && errno != EWOULDBLOCK) {
                ret = -1;
                net::post_via_route(owner.exec(), *this);
                return;
            }
            timed_out = false;
            this->set_timeout_callback(&timeout_cb);
            owner.reactor()->add_waiter_with_timeout(owner.native_handle(), EPOLLOUT, this, kConnectTimeout);
        }
        int await_resume() noexcept
        {
            this->clear_timeout_callback();
            if (timed_out) {
                timed_out = false;
                errno     = ETIMEDOUT;
                return -1;
            }
            if (ret == 0) {
                int       err = 0;
                socklen_t len = sizeof(err);
                if (os::getsockopt(owner.native_handle(), SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
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
        os::set_non_block(_fd);
        _reactor->add_fd(_fd);
        serial_queue_init(_send_q);
        serial_queue_init(_recv_q);
        _closed = false;
    }

    workqueue<lock>&  _exec;
    Reactor<lock>*    _reactor { nullptr };
    fd_t              _fd { os::invalid_fd() };
    bool              _closed { false };
    int               _family { AF_INET };
    lock              _io_serial_lock;
    serial_queue      _send_q;
    serial_queue      _recv_q;
    callback_wq<lock> _cbq;

    void determine_family()
    {
        sockaddr_storage local {};
        socklen_t        len = sizeof(local);
        if (os::getsockname(_fd, reinterpret_cast<sockaddr*>(&local), &len) == 0)
            _family = local.ss_family;
        else
            _family = AF_INET;
    }
};

/**
 * @brief 面向流式 socket 的公共基类，封装 connect/recv/send Awaiter 与半双工状态。
 *
 * 该基类提供：
 * - `recv`/`recv_all`、`send`/`send_all` 等常用 Awaiter；
 * - 通过串行化队列保证同一 socket 上的 awaiter 逻辑顺序；
 * - 对端关闭、发送半关闭等状态位管理。
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
        if (this->native_handle() != os::invalid_fd() && !_tx_shutdown) {
            os::shutdown(this->native_handle(), SHUT_WR);
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
    /**
     * @brief 面向流式 socket 的非阻塞读取 Awaiter。
     *
     * 支持“尽量读一些”与“必须读满”两种模式：当 `full` 为 true 时，Awaiter 会持续尝试直到
     * 读取到指定长度或遇到 EOF；否则在读取到任意正字节数后立即返回。
     */
    struct recv_awaiter : two_phase_drain_awaiter<recv_awaiter, Derived> {
        /** @brief 是否开启“必须读满”模式。 */
        bool full { false };
        /** @brief 用户提供的缓冲区指针。 */
        char* buf { nullptr };
        /** @brief 期望读取的字节数。 */
        size_t len { 0 };
        /** @brief 已经成功读取的字节数。 */
        size_t recvd { 0 };
        /** @brief 当首次读取失败时记录的错误码。 */
        ssize_t err { 0 };
        recv_awaiter(Derived& s, void* b, size_t l, bool f)
            : two_phase_drain_awaiter<recv_awaiter, Derived>(s, s.recv_queue())
            , full(f)
            , buf(static_cast<char*>(b))
            , len(l)
        {
            auto& cbq = s.callback_queue();
            this->store_route_guard(cbq.retain_guard());
            this->route_ctx  = cbq.context();
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
            ssize_t n = os::recv(this->owner.native_handle(), buf + recvd, len - recvd, 0);
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
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!full && recvd > 0)
                    return 0;
                return -1;
            }
            if (recvd == 0)
                err = n;
            return 0;
        }
        ssize_t await_resume() noexcept { return (err < 0 && recvd == 0) ? err : static_cast<ssize_t>(recvd); }
    };

    /**
     * @brief 非阻塞 send/writev 协程驱动器，支持全量发送。
     */
    /**
     * @brief 面向流式 socket 的非阻塞写入 Awaiter。
     *
     * 支持缓冲区与 `iovec` 两种输入形式，并能在“写满”为 true 时自动循环发送直到所有数据
     * 写入或出现错误。Awaiter 会自动处理 `EPIPE`/`ECONNRESET` 并标记写半部关闭。
     */
    struct send_awaiter : two_phase_drain_awaiter<send_awaiter, Derived> {
        /** @brief 是否要求写满。 */
        bool full { false };
        /** @brief 缓冲区指针，`use_vec` 为 false 时有效。 */
        const char* buf { nullptr };
        /** @brief 待写入总字节数。 */
        size_t len { 0 };
        /** @brief `writev` 模式下的向量数组。 */
        std::vector<iovec> vec;
        /** @brief 是否启用 `iovec` 发送路径。 */
        bool use_vec { false };
        /** @brief 已写入字节数。 */
        size_t sent { 0 };
        /** @brief 记录首次出现的错误。 */
        ssize_t err { 0 };
        send_awaiter(Derived& s, const void* b, size_t l, bool f)
            : two_phase_drain_awaiter<send_awaiter, Derived>(s, s.send_queue())
            , full(f)
            , buf(static_cast<const char*>(b))
            , len(l)
        {
            auto& cbq = s.callback_queue();
            this->store_route_guard(cbq.retain_guard());
            this->route_ctx  = cbq.context();
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        send_awaiter(Derived& s, const iovec* iov, int iovcnt, bool f)
            : two_phase_drain_awaiter<send_awaiter, Derived>(s, s.send_queue()), full(f), use_vec(true)
        {
            vec.reserve(static_cast<size_t>(iovcnt));
            for (int i = 0; i < iovcnt; ++i) {
                vec.push_back(iov[i]);
                len += iov[i].iov_len;
            }
            auto& cbq = s.callback_queue();
            this->store_route_guard(cbq.retain_guard());
            this->route_ctx  = cbq.context();
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
                    ssize_t n = os::send(this->owner.native_handle(), buf + sent, len - sent, MSG_NOSIGNAL);
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
                ssize_t n = os::writev(this->owner.native_handle(), vec.data(), static_cast<int>(vec.size()));
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
    send_awaiter sendv(const iovec* iov, int iovcnt)
    {
        return send_awaiter(static_cast<Derived&>(*this), iov, iovcnt, false);
    }
    /** @brief writev 版本的全量发送。 */
    send_awaiter send_all(const iovec* iov, int iovcnt)
    {
        return send_awaiter(static_cast<Derived&>(*this), iov, iovcnt, true);
    }

protected:
    stream_socket_base(workqueue<lock>& exec, Reactor<lock>& reactor, int domain, int type, int protocol = 0)
        : core(exec, reactor, domain, type, protocol)
    {
    }
    stream_socket_base(fd_t fd, workqueue<lock>& exec, Reactor<lock>& reactor) : core(fd, exec, reactor) { }

private:
    bool _rx_eof { false };
    bool _tx_shutdown { false };
};

/**
 * @brief UDP/Unix Datagram 通用基类，提供 connect/recvfrom/sendto Awaiter。
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
    /**
     * @brief datagram 套接字的接收 Awaiter，支持携带源地址返回值。
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
            auto& cbq = s.callback_queue();
            this->store_route_guard(cbq.retain_guard());
            this->route_ctx  = cbq.context();
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
            nread                        = os::recvfrom(this->owner.native_handle(), buf, len, 0, sa, len_ptr);
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
    /**
     * @brief datagram 套接字的发送 Awaiter，支持 `sendto` 与 `writev` 两种调用路径。
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
            auto& cbq = s.callback_queue();
            this->store_route_guard(cbq.retain_guard());
            this->route_ctx  = cbq.context();
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        send_awaiter(Derived& s, const void* b, size_t l, const address_type& d)
            : two_phase_drain_awaiter<send_awaiter, Derived>(s, s.send_queue())
            , dest(dest_info { d, Derived::address_length(d) })
            , buf(static_cast<const char*>(b))
            , len(l)
        {
            auto& cbq = s.callback_queue();
            this->store_route_guard(cbq.retain_guard());
            this->route_ctx  = cbq.context();
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        send_awaiter(Derived& s, const iovec* iov, int iovcnt)
            : two_phase_drain_awaiter<send_awaiter, Derived>(s, s.send_queue()), use_vec(true)
        {
            vec.reserve(static_cast<size_t>(iovcnt));
            for (int i = 0; i < iovcnt; ++i) {
                vec.push_back(iov[i]);
                len += iov[i].iov_len;
            }
            auto& cbq = s.callback_queue();
            this->store_route_guard(cbq.retain_guard());
            this->route_ctx  = cbq.context();
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        send_awaiter(Derived& s, const iovec* iov, int iovcnt, const address_type& d)
            : two_phase_drain_awaiter<send_awaiter, Derived>(s, s.send_queue())
            , use_vec(true)
            , dest(dest_info { d, Derived::address_length(d) })
        {
            vec.reserve(static_cast<size_t>(iovcnt));
            for (int i = 0; i < iovcnt; ++i) {
                vec.push_back(iov[i]);
                len += iov[i].iov_len;
            }
            auto& cbq = s.callback_queue();
            this->store_route_guard(cbq.retain_guard());
            this->route_ctx  = cbq.context();
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
                    nsent = os::sendto(this->owner.native_handle(),
                                       buf,
                                       len,
                                       MSG_NOSIGNAL,
                                       reinterpret_cast<const sockaddr*>(&dest->addr),
                                       dest->len);
                } else {
                    nsent = os::send(this->owner.native_handle(), buf, len, MSG_NOSIGNAL);
                }
            } else {
                if (dest) {
                    nsent = os::sendto_vec(this->owner.native_handle(),
                                           vec.data(),
                                           static_cast<int>(vec.size()),
                                           reinterpret_cast<const sockaddr*>(&dest->addr),
                                           dest->len,
                                           0);
                } else {
                    nsent = os::writev(this->owner.native_handle(), vec.data(), static_cast<int>(vec.size()));
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
    send_awaiter sendv(const iovec* iov, int iovcnt) { return send_awaiter(static_cast<Derived&>(*this), iov, iovcnt); }
    send_awaiter sendv_to(const iovec* iov, int iovcnt, const address_type& dest_addr)
    {
        return send_awaiter(static_cast<Derived&>(*this), iov, iovcnt, dest_addr);
    }

protected:
    datagram_socket_base(workqueue<lock>& exec, Reactor<lock>& reactor, int domain, int type, int protocol = 0)
        : core(exec, reactor, domain, type, protocol)
    {
    }
    datagram_socket_base(fd_t fd, workqueue<lock>& exec, Reactor<lock>& reactor) : core(fd, exec, reactor) { }
};

} // namespace co_wq::net::detail
