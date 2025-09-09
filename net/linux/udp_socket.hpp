// udp_socket.hpp - UDP socket 协程原语
#pragma once

#include "callback_wq.hpp"
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
#include <sys/uio.h>
#include <unistd.h>

namespace co_wq::net {

template <lockable lock, template <class> class Reactor> class fd_workqueue; // fwd

/**
 * @brief UDP socket 协程封装，支持 connect（可选默认对端）、recv_from、send_to。
 *
 * 设计要点：
 *  - IO 全部非阻塞 + EPOLLET，内部通过串行队列保证同类操作不会并发执行；
 *  - recv_from 使用统一 two-phase 机制：首轮尝试，若 EAGAIN 则注册 EPOLLIN 等待并继续驱动；
 *  - send_to 尽量写到 EAGAIN；如需完整发送可在上层循环或未来扩展 full 模式（当前保持单次语义即可）。
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
     * @note 会批量唤醒串行队列中的等待者，并在 await_resume 中以 ECANCELED 结束。
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
     * @return 0 成功；-1 失败（SO_ERROR 非 0）。
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
            this->h          = h;
            this->route_ctx  = &sock._cbq;
            this->route_post = &callback_wq<lock>::post_adapter;
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
     * @brief 异步 recvfrom awaiter（单次语义）。
     * @details UDP 有报文边界：单次 recvfrom 即处理一个报文（可能被截断），聚合模式意义有限。
     * @return await_resume: >=0 读到字节；<0 错误（或 ECANCELED）。
     */
    struct recvfrom_awaiter : tp_base<recvfrom_awaiter> {
        void*        buf;
        size_t       len;
        sockaddr_in* out_addr;
        socklen_t*   out_len;
        ssize_t      nread { -1 }; // -1 需等待
        recvfrom_awaiter(udp_socket& s, void* b, size_t l, sockaddr_in* oa, socklen_t* ol)
            : tp_base<recvfrom_awaiter>(s, s._recv_q), buf(b), len(l), out_addr(oa), out_len(ol)
        {
            this->route_ctx  = &this->owner._cbq;
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        static void register_wait(recvfrom_awaiter* self, bool /*first*/)
        {
            self->owner.reactor()->add_waiter_custom(self->owner.native_handle(), EPOLLIN, self);
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
        // await_ready 由基类提供（返回 false）
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
     * @param buf 目标缓冲区。
     * @param len 缓冲区大小。
     * @param addr 输出对端地址。
     * @param addrlen 输入/输出地址长度。
     */
    recvfrom_awaiter recv_from(void* buf, size_t len, sockaddr_in* addr, socklen_t* addrlen)
    {
        return recvfrom_awaiter(*this, buf, len, addr, addrlen);
    }

    /**
     * @brief 统一 UDP 发送 awaiter（单缓冲/向量 + 可选目的地址）。
     * @details
     *  - 已连接：单缓冲使用 send；向量使用 writev；
     *  - 未连接：单缓冲使用 sendto；向量使用 sendmsg 设置 msg_name。
     *  - EAGAIN 时注册 EPOLLOUT 等待；完成或错误后结束。
     */
    struct send_awaiter : tp_base<send_awaiter> {
        // 数据模式
        bool               use_vec { false };
        const char*        buf { nullptr };
        size_t             len { 0 };
        std::vector<iovec> vec;
        // 目的地址（可选）
        bool        has_dest { false };
        sockaddr_in dest {};
        // 结果
        ssize_t nsent { 0 };
        // 单缓冲（已连接）
        send_awaiter(udp_socket& s, const void* b, size_t l)
            : tp_base<send_awaiter>(s, s._send_q), use_vec(false), buf(static_cast<const char*>(b)), len(l)
        {
            this->route_ctx  = &this->owner._cbq;
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        // 单缓冲 + 目的地址（未连接）
        send_awaiter(udp_socket& s, const void* b, size_t l, const sockaddr_in& d)
            : tp_base<send_awaiter>(s, s._send_q)
            , use_vec(false)
            , buf(static_cast<const char*>(b))
            , len(l)
            , has_dest(true)
            , dest(d)
        {
            this->route_ctx  = &this->owner._cbq;
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        // 向量（已连接）
        send_awaiter(udp_socket& s, const struct iovec* iov, int iovcnt)
            : tp_base<send_awaiter>(s, s._send_q), use_vec(true)
        {
            vec.reserve((size_t)iovcnt);
            for (int i = 0; i < iovcnt; ++i) {
                vec.push_back(iov[i]);
                len += iov[i].iov_len; // 复用 len 作为总字节
            }
            this->route_ctx  = &this->owner._cbq;
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        // 向量 + 目的地址（未连接）
        send_awaiter(udp_socket& s, const struct iovec* iov, int iovcnt, const sockaddr_in& d)
            : tp_base<send_awaiter>(s, s._send_q), use_vec(true), has_dest(true), dest(d)
        {
            vec.reserve((size_t)iovcnt);
            for (int i = 0; i < iovcnt; ++i) {
                vec.push_back(iov[i]);
                len += iov[i].iov_len;
            }
            this->route_ctx  = &this->owner._cbq;
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        static void register_wait(send_awaiter* self, bool /*first*/)
        {
            self->owner.reactor()->add_waiter_custom(self->owner.native_handle(), EPOLLOUT, self);
        }
        int attempt_once()
        {
            if (!use_vec) {
                if (has_dest) {
                    nsent = ::sendto(this->owner.native_handle(),
                                     buf,
                                     len,
                                     MSG_DONTWAIT | MSG_NOSIGNAL,
                                     (const sockaddr*)&dest,
                                     sizeof(dest));
                } else {
                    nsent = ::send(this->owner.native_handle(), buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
                }
            } else {
                if (has_dest) {
                    msghdr msg {};
                    msg.msg_name    = &dest;
                    msg.msg_namelen = sizeof(dest);
                    msg.msg_iov     = vec.data();
                    msg.msg_iovlen  = (size_t)vec.size();
                    nsent           = ::sendmsg(this->owner.native_handle(), &msg, MSG_DONTWAIT);
                } else {
                    nsent = ::writev(this->owner.native_handle(), vec.data(), (int)vec.size());
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
    /** @brief 已连接 UDP：单缓冲发送。*/
    send_awaiter send(const void* buf, size_t len) { return send_awaiter(*this, buf, len); }
    /** @brief 未连接 UDP：单缓冲 + 目的地址发送。*/
    send_awaiter send_to(const void* buf, size_t len, const sockaddr_in& dest)
    {
        return send_awaiter(*this, buf, len, dest);
    }
    /** @brief 已连接 UDP：向量发送。*/
    send_awaiter sendv(const struct iovec* iov, int iovcnt) { return send_awaiter(*this, iov, iovcnt); }
    /** @brief 未连接 UDP：向量 + 目的地址发送。*/
    send_awaiter sendv_to(const struct iovec* iov, int iovcnt, const sockaddr_in& dest)
    {
        return send_awaiter(*this, iov, iovcnt, dest);
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
    // IO 串行锁与队列
    lock              _io_serial_lock;
    serial_queue      _send_q;
    serial_queue      _recv_q;
    bool              _closed { false }; // 是否已关闭，用于取消待定操作
    callback_wq<lock> _cbq { _exec };
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
