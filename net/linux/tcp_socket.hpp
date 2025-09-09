// tcp_socket.hpp - TCP socket 协程原语
#pragma once

#include "callback_wq.hpp"
#include "epoll_reactor.hpp" // 默认 Reactor
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
#include <vector>
namespace co_wq::net {

// 前置声明: 带 Reactor 模板参数的 fd_workqueue
template <lockable lock, template <class> class Reactor> class fd_workqueue;

// Reactor 后端需提供: add_fd / remove_fd / add_waiter 接口
/**
 * @brief 基于非阻塞 + epoll(ET) 的 TCP socket 协程封装。
 *
 * 模板参数:
 *  - lock: workqueue 所使用的锁类型（需满足 lockable）。
 *  - Reactor: 反应器类型，需提供 add_fd/remove_fd/add_waiter[_custom] 接口 (默认 epoll_reactor)。
 *
 * 设计要点:
 *  - 所有 IO 均使用非阻塞 + MSG_DONTWAIT，避免阻塞线程；
 *  - 采用串行队列(serial_queue)保证同类 IO(读/写)在同一 socket 上不会并发；
 *  - 发送/接收均提供“单次(partial)”与“聚合(full)”两种 await 语义：
 *    - partial：遇到 EAGAIN 立即结束当前 await，返回已处理的字节数；
 *    - full：遇到 EAGAIN 注册事件并等待，直至缓冲区耗尽/EOF/错误；
 *  - 发生对端关闭 (recv=0) 标记 rx_eof()；写端错误(EPIPE/ECONNRESET/ENOTCONN) 标记 tx_shutdown()。
 */
template <lockable lock, template <class> class Reactor = epoll_reactor> class tcp_socket {
public:
    // Helper alias to reduce template noise when declaring awaiters
    template <class D> using tp_base         = two_phase_drain_awaiter<D, tcp_socket>;
    tcp_socket()                             = delete;
    tcp_socket(const tcp_socket&)            = delete;
    tcp_socket& operator=(const tcp_socket&) = delete;
    tcp_socket(tcp_socket&& o) noexcept : _exec(o._exec), _fd(o._fd), _rx_eof(o._rx_eof), _tx_shutdown(o._tx_shutdown)
    {
        o._fd          = -1;
        o._rx_eof      = false;
        o._tx_shutdown = false;
    }
    tcp_socket& operator=(tcp_socket&& o) noexcept
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
    ~tcp_socket() { close(); }
    /**
     * @brief 关闭 socket 并从 reactor 注销。
     * @note 可安全重复调用；析构会自动调用。对尚在串行队列中的等待者会被批量唤醒并在 await_resume 中检测关闭状态。
     */
    void close()
    {
        if (_fd >= 0) {
            if (_reactor)
                _reactor->remove_fd(_fd); // 这会投递所有尚在 reactor 上等待的 waiter
            // 额外: 释放仍在串行发送/接收队列中尚未获取槽位的 awaiter
            list_head pending;
            INIT_LIST_HEAD(&pending);
            serial_collect_waiters(_io_serial_lock, { &_send_q, &_recv_q }, pending);
            serial_post_pending(_exec, pending); // awaiters 在 await_resume 中检测关闭状态
            ::close(_fd);
            _fd = -1;
        }
    }
    /**
     * @brief 关闭写方向，相当于 shutdown(SHUT_WR)。
     * @note 仅第一次调用有效，之后 tx_shutdown() 返回 true。
     */
    void shutdown_tx()
    {
        if (_fd >= 0 && !_tx_shutdown) {
            ::shutdown(_fd, SHUT_WR);
            _tx_shutdown = true;
        }
    }
    /** @return 是否已经关闭写方向（对端可能仍可读）。*/
    bool tx_shutdown() const noexcept { return _tx_shutdown; }
    /** @return 是否已经读到 EOF（对端关闭读）。*/
    bool rx_eof() const noexcept { return _rx_eof; }
    /** @return 原始文件描述符。*/
    int native_handle() const { return _fd; }
    // 提供给串行 / two_phase awaiter 的访问器
    /** @return 绑定的执行队列。*/
    workqueue<lock>& exec() { return _exec; }
    /** @return 串行化互斥锁。*/
    lock& serial_lock() { return _io_serial_lock; }
    /** @return 绑定的 Reactor 实例。*/
    Reactor<lock>* reactor() { return _reactor; }
    /** 标记收到 EOF（内部使用）。*/
    void mark_rx_eof() { _rx_eof = true; }
    /** 标记写方向关闭（内部使用）。*/
    void mark_tx_shutdown() { _tx_shutdown = true; }
    struct connect_awaiter : io_waiter_base {
        tcp_socket& sock;
        std::string host;
        uint16_t    port;
        int         ret { 0 };
        connect_awaiter(tcp_socket& s, std::string h, uint16_t p) : sock(s), host(std::move(h)), port(p) { }
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
    /**
     * @brief 异步发起连接（IPv4）。
     * @param host 点分十进制 IP 字符串，如 "127.0.0.1"。
     * @param port 端口号。
     * @return await 后返回 0 表示成功，-1 失败（SO_ERROR 非 0）。
     */
    connect_awaiter connect(const std::string& host, uint16_t port) { return connect_awaiter(*this, host, port); }
    /**
     * @brief 统一接收 awaiter（支持“单次/聚合”两种语义）。
     * @param b 目标缓冲区。
     * @param l 缓冲区长度。
     * @param f 是否聚合(full)。
     * @details
     *  - 当 f=false：执行一次非阻塞 recv；若返回 EAGAIN 立即结束当前 await（不等待）。
     *  - 当 f=true：内部多次等待可读并继续读取，直到缓冲区填满/EOF/错误。
     * @return await_resume: >=0 已读字节；0=EOF（置 rx_eof）；<0 首次错误（errno 保留）。
     */
    struct recv_awaiter : tp_base<recv_awaiter> {
        bool    full { false }; ///< 是否聚合读取
        char*   buf;
        size_t  len;
        size_t  recvd { 0 };
        ssize_t err { 0 };
        recv_awaiter(tcp_socket& s, void* b, size_t l, bool f)
            : tp_base<recv_awaiter>(s, s._recv_q), full(f), buf(static_cast<char*>(b)), len(l)
        {
            this->route_ctx  = &this->owner._cbq;
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        static void register_wait(recv_awaiter* self, bool /*first*/)
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
                    return 0;                // 单次语义：第一次读到就结束
                return recvd == len ? 0 : 1; // 聚合语义：继续 or 完成
            }
            if (n == 0) {
                this->owner.mark_rx_eof();
                return 0;
            }
            // n < 0
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return full ? -1 : 0; // 聚合等待继续；单次直接结束
            if (recvd == 0)
                err = n; // 记录首个错误
            return 0;
        }
        ssize_t await_resume() noexcept { return (err < 0 && recvd == 0) ? err : (ssize_t)recvd; }
    };
    /**
     * @brief 异步接收一次（最多 len 字节；不保证读满）。
     * @param buf 目标缓冲区。
     * @param len 缓冲区长度。
     * @return >=0 已读字节；0=EOF；<0 错误。
     */
    recv_awaiter recv(void* buf, size_t len) { return recv_awaiter(*this, buf, len, /*full*/ false); }
    /**
     * @brief 异步接收直至缓冲区填满/EOF/错误（尽力读满）。
     * @param buf 目标缓冲区。
     * @param len 缓冲区长度。
     * @return ==len 填满；0..len-1 提前结束；<0 首次错误。
     */
    recv_awaiter recv_all(void* buf, size_t len) { return recv_awaiter(*this, buf, len, /*full*/ true); }
    // 统一发送 awaiter：支持 单缓冲/向量，部分发送/全量发送 两种模式
    /**
     * @brief 统一发送 awaiter（单缓冲/向量 + partial/full）。
     * @details
     *  - partial：遇到 EAGAIN 即结束（不等待）；
     *  - full：遇到 EAGAIN 注册 EPOLLOUT 等待并继续，直至全部发送或错误；
     *  - iovec 模式会复制一份数组用于原位推进，不修改调用者数据。
     * @return await_resume: >=0 为已发送字节（含部分）；<0 为首次错误（errno 保留）。
     */
    struct send_awaiter : tp_base<send_awaiter> {
        // 模式：full=true 表示遇到 EAGAIN 时等待并继续直至完成或错误；false 表示到 EAGAIN 即结束本次 await。
        bool full { false };
        // 数据视图：二选一
        const char*        buf { nullptr };
        size_t             len { 0 };
        std::vector<iovec> vec; // 仅当 use_vec=true 时使用
        bool               use_vec { false };
        // 进度与结果
        size_t  sent { 0 };
        ssize_t err { 0 }; // <0 错误（仅在 sent==0 时对外可见）
        // 单缓冲构造
        send_awaiter(tcp_socket& s, const void* b, size_t l, bool full_mode)
            : tp_base<send_awaiter>(s, s._send_q), full(full_mode), buf(static_cast<const char*>(b)), len(l)
        {
            this->route_ctx  = &this->owner._cbq;
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        // 向量构造（复制一份以便就地推进）
        send_awaiter(tcp_socket& s, const struct iovec* iov, int iovcnt, bool full_mode)
            : tp_base<send_awaiter>(s, s._send_q), full(full_mode), use_vec(true)
        {
            vec.reserve((size_t)iovcnt);
            for (int i = 0; i < iovcnt; ++i) {
                vec.push_back(iov[i]);
                len += iov[i].iov_len; // 复用 len 记录 total
            }
            this->route_ctx  = &this->owner._cbq;
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        static void register_wait(send_awaiter* self, bool /*first*/)
        {
            self->owner.reactor()->add_waiter_custom(self->owner.native_handle(), EPOLLOUT, self);
        }
        // 推进 iovec
        void advance_iov(size_t n)
        {
            size_t remain = n;
            while (remain > 0 && !vec.empty()) {
                if (remain >= vec.front().iov_len) {
                    remain -= vec.front().iov_len;
                    vec.erase(vec.begin());
                } else {
                    vec.front().iov_base = (char*)vec.front().iov_base + remain;
                    vec.front().iov_len -= remain;
                    remain = 0;
                }
            }
        }
        int attempt_once()
        {
            if (!use_vec) {
                // 单缓冲：尽量写到 EAGAIN；full 决定是否等待
                while (sent < len) {
                    ssize_t n = ::send(this->owner.native_handle(),
                                       buf + sent,
                                       len - sent,
                                       MSG_DONTWAIT | MSG_NOSIGNAL);
                    if (n > 0) {
                        sent += (size_t)n;
                        if (sent == len)
                            return 0; // 完成
                        continue;     // 继续消耗可写边沿
                    }
                    if (n == 0)
                        return 0;
                    // n < 0
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        return full ? -1 : 0; // full 等待继续，否则结束本次 await
                    if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN)
                        this->owner.mark_tx_shutdown();
                    err = n;
                    return 0;
                }
                return 0;
            } else {
                // 向量写：循环 writev 直到 EAGAIN/完成
                while (!vec.empty()) {
                    ssize_t n = ::writev(this->owner.native_handle(), vec.data(), (int)vec.size());
                    if (n > 0) {
                        sent += (size_t)n;
                        advance_iov((size_t)n);
                        if (vec.empty())
                            return 0; // 完成
                        continue;     // 继续消耗可写边沿
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
        }
        ssize_t await_resume() noexcept { return (err < 0 && sent == 0) ? err : (ssize_t)sent; }
    };
    // API 保持不变：根据需要构造不同模式的统一 awaiter
    /** @brief 单缓冲一次发送（partial）。*/
    send_awaiter send(const void* buf, size_t len) { return send_awaiter(*this, buf, len, /*full*/ false); }
    /** @brief 向量一次发送（partial）。*/
    send_awaiter sendv(const struct iovec* iov, int iovcnt) { return send_awaiter(*this, iov, iovcnt, /*full*/ false); }
    /** @brief 单缓冲全量发送（full）。*/
    send_awaiter send_all(const void* buf, size_t len) { return send_awaiter(*this, buf, len, /*full*/ true); }
    send_awaiter send_all(const struct iovec* iov, int iovcnt)
    {
        return send_awaiter(*this, iov, iovcnt, /*full*/ true);
    }

    // release_* handled via serial_slot_awaiter

private:
    friend class fd_workqueue<lock, Reactor>;
    explicit tcp_socket(workqueue<lock>& exec, Reactor<lock>& reactor) : _exec(exec), _reactor(&reactor)
    {
        _fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (_fd < 0)
            throw std::runtime_error("socket failed");
        set_non_block();
        _reactor->add_fd(_fd);
        serial_queue_init(_send_q);
        serial_queue_init(_recv_q);
    }
    tcp_socket(int fd, workqueue<lock>& exec, Reactor<lock>& reactor) : _exec(exec), _reactor(&reactor), _fd(fd)
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
    // IO 串行锁与等待队列（发送 / 接收分别串行）
    lock              _io_serial_lock;
    serial_queue      _send_q;
    serial_queue      _recv_q;
    callback_wq<lock> _cbq { _exec };
};

// wrappers
template <lockable lock>
/**
 * @brief 异步连接包装。
 */
inline Task<int, Work_Promise<lock, int>> async_connect(tcp_socket<lock>& s, const std::string host, uint16_t port)
{
    co_return co_await s.connect(host, port);
}
template <lockable lock>
/**
 * @brief 聚合发送包装（单缓冲区）。
 */
inline Task<ssize_t, Work_Promise<lock, ssize_t>> async_send_all(tcp_socket<lock>& s, const void* buf, size_t len)
{
    co_return co_await s.send_all(buf, len); // 使用 awaiter 版本
}
template <lockable lock>
/**
 * @brief 聚合发送包装（iovec 列表）。
 */
inline Task<ssize_t, Work_Promise<lock, ssize_t>>
async_sendv_all(tcp_socket<lock>& s, const struct iovec* iov, int iovcnt)
{
    co_return co_await s.send_all(iov, iovcnt); // 使用向量 awaiter 版本
}
template <lockable lock>
/**
 * @brief 单次接收包装（可能返回 0..len）。
 */
inline Task<ssize_t, Work_Promise<lock, ssize_t>> async_recv_some(tcp_socket<lock>& s, void* buf, size_t len)
{
    ssize_t n = co_await s.recv(buf, len);
    co_return n;
}
template <lockable lock>
/**
 * @brief 读取直到缓冲区填满 / EOF / 错误。
 * @return ==len 成功填满；0..len-1 提前结束(EOF/错误)；<0 初次调用即错误。
 */
inline Task<ssize_t, Work_Promise<lock, ssize_t>> async_recv_all(tcp_socket<lock>& s, void* buf, size_t len)
{
    co_return co_await s.recv_all(buf, len);
}

} // namespace co_wq::net
