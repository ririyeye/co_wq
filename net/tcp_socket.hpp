// tcp_socket.hpp - TCP socket coroutine primitives
#pragma once
#ifdef __linux__
#include "epoll_reactor.hpp" // default Reactor
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

// Forward decl for fd_workqueue with reactor parameter
template <lockable lock, template <class> class Reactor> class fd_workqueue;

// tcp_socket now parameterized by Reactor backend (must provide static instance(exec) returning reactor with
// add_fd/remove_fd/add_waiter)
/**
 * @brief 基于非阻塞 + epoll (边缘触发) 的 TCP socket 协程封装。
 *
 * 模板参数:
 *  - lock: workqueue 所使用的锁类型（需满足 lockable）
 *  - Reactor: 反应器类型，需提供 add_fd / remove_fd / add_waiter 接口 (默认 epoll_reactor)
 *
 * 设计要点:
 *  - 所有 IO 均使用非阻塞系统调用 + MSG_DONTWAIT。
 *  - 发送使用“写到 EAGAIN 再挂起”的策略，保证 EPOLLET 下不会漏掉事件。
 *  - recv/send 的 awaiter 仅对单次操作；send_all / sendv / send_all(iovec) 提供聚合发送。
 *  - 发生对端关闭 (recv=0) 标记 _rx_eof；写端错误(EPIPE/ECONNRESET/ENOTCONN) 标记 _tx_shutdown。
 */
template <lockable lock, template <class> class Reactor = epoll_reactor> class tcp_socket {
public:
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
     * @brief 关闭 socket（若有效），并从 reactor 注销。
     * @note 可安全重复调用；析构自动调用。
     */
    void close()
    {
        if (_fd >= 0) {
            if (_reactor)
                _reactor->remove_fd(_fd);
            ::close(_fd);
            _fd = -1;
        }
    }
    /**
     * @brief 关闭写方向(shutdown(SHUT_WR))。
     * @note 只会执行一次，之后 tx_shutdown() 返回 true。
     */
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
    struct connect_awaiter : io_waiter_base {
        tcp_socket& sock;
        std::string host;
        uint16_t    port;
        int         ret { 0 };
        connect_awaiter(tcp_socket& s, std::string h, uint16_t p) : sock(s), host(std::move(h)), port(p) { }
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
    /**
     * @brief 异步发起连接（IPv4）。
     * @param host 点分十进制 IP 字符串，如 "127.0.0.1"。
     * @param port 端口号。
     * @return await 后返回 0 表示成功，-1 失败 (SO_ERROR 非 0)。
     */
    connect_awaiter connect(const std::string& host, uint16_t port) { return connect_awaiter(*this, host, port); }
    struct recv_awaiter : io_waiter_base {
        tcp_socket& sock;
        void*       buf;
        size_t      len;
        ssize_t     nread { -1 }; // -1 表示需读取
        bool        have_lock { false };
        recv_awaiter(tcp_socket& s, void* b, size_t l) : sock(s), buf(b), len(l) { }
        static void lock_acquired_cb(worknode* w)
        {
            auto* self      = static_cast<recv_awaiter*>(w);
            self->have_lock = true;
            // 尝试立即读一次
            self->nread = ::recv(self->sock._fd, self->buf, self->len, MSG_DONTWAIT);
            if (self->nread >= 0 || (self->nread < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                // 完成或错误立即恢复
                if (self->nread == 0)
                    self->sock._rx_eof = true;
                self->sock.release_recv_slot();
                self->func = &io_waiter_base::resume_cb;
                if (self->h)
                    self->h.resume();
                return;
            }
            // 需要等待 EPOLLIN
            self->nread = -1; // 标记需在唤醒后再读
            self->func  = &recv_awaiter::drive_cb;
            self->sock._reactor->add_waiter_custom(self->sock._fd, EPOLLIN, self);
        }
        static void drive_cb(worknode* w)
        {
            auto* self = static_cast<recv_awaiter*>(w);
            // 被 EPOLLIN 唤醒，最终读取
            self->nread = ::recv(self->sock._fd, self->buf, self->len, MSG_DONTWAIT);
            if (self->nread == 0)
                self->sock._rx_eof = true;
            self->sock.release_recv_slot();
            self->func = &io_waiter_base::resume_cb;
            if (self->h)
                self->h.resume();
        }
        bool await_ready() noexcept { return false; } // 总是走锁流程
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h    = h;
            this->func = &recv_awaiter::lock_acquired_cb;
            INIT_LIST_HEAD(&this->ws_node);
            // 获取接收互斥
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
        ssize_t await_resume() noexcept { return nread; }
    };
    /**
     * @brief 异步接收一次（最多 len 字节）。
     * @param buf 目标缓冲区。
     * @param len 缓冲区大小。
     * @return >=0 表示读到的字节数；返回 0 表示对端关闭并置 rx_eof；<0 表示错误 (errno 保留)。
     * @note 单次 recv，不保证读满；上层可循环/协议分帧。
     */
    recv_awaiter recv(void* buf, size_t len) { return recv_awaiter(*this, buf, len); }
    struct send_awaiter : io_waiter_base {
        tcp_socket& sock;
        const void* buf;
        size_t      len;
        size_t      sent { 0 };
        send_awaiter(tcp_socket& s, const void* b, size_t l) : sock(s), buf(b), len(l) { }
        // Drain as much as possible before deciding to suspend. This is required for EPOLLET (edge-triggered)
        // correctness: we must consume the writable edge fully (until EAGAIN) otherwise we may never get
        // another EPOLLOUT event and could hang with pending data.
        bool await_ready() noexcept
        {
            while (sent < len) {
                ssize_t n = ::send(sock._fd, (char*)buf + sent, len - sent, MSG_DONTWAIT | MSG_NOSIGNAL);
                if (n > 0) {
                    sent += (size_t)n;
                    continue; // try write remaining immediately
                }
                if (n == 0) {
                    // Treat as progress impossible now; return to caller (will likely loop or error out)
                    return true;
                }
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        // Need to wait for next writable edge unless already finished
                        return sent == len; // if exactly finished (rare) treat as ready
                    }
                    if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN)
                        sock._tx_shutdown = true;
                    return true; // error - resume immediately
                }
            }
            return true; // fully sent
        }
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h = h;
            INIT_LIST_HEAD(&this->ws_node);
            sock._reactor->add_waiter(sock._fd, EPOLLOUT, this);
        }
        ssize_t await_resume() noexcept
        {
            if (sent == len)
                return (ssize_t)sent; // already complete
            while (sent < len) {
                ssize_t n = ::send(sock._fd, (char*)buf + sent, len - sent, MSG_DONTWAIT | MSG_NOSIGNAL);
                if (n > 0) {
                    sent += (size_t)n;
                    continue; // keep draining until EAGAIN or done
                }
                if (n == 0) {
                    // Unusual for send; treat as no more progress possible now
                    break;
                }
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        break; // partial send; caller may re-await
                    if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN)
                        sock._tx_shutdown = true;
                    return n; // propagate error
                }
            }
            return (ssize_t)sent; // partial or complete
        }
    };
    /**
     * @brief 异步发送一次（尝试发送 len 字节，写到 EAGAIN 或完成）。
     * @param buf 数据指针。
     * @param len 字节数。
     * @return await 后：>0 已发送字节；=len 表示全部写完；<0 系统调用错误 (errno 保留)。
     * @note EPOLLET 下 awaiter 内部会在挂起前尽量写满，防止丢事件。
     */
    send_awaiter send(const void* buf, size_t len) { return send_awaiter(*this, buf, len); }
    // Vectored send awaiter (gather write using writev)
    struct sendv_awaiter : io_waiter_base {
        tcp_socket&        sock;
        std::vector<iovec> vec;         // mutable copy we can adjust
        size_t             total { 0 }; // total bytes to send
        size_t             sent { 0 };  // bytes already sent
        sendv_awaiter(tcp_socket& s, const struct iovec* iov, int iovcnt) : sock(s)
        {
            vec.reserve((size_t)iovcnt);
            for (int i = 0; i < iovcnt; ++i) {
                vec.push_back(iov[i]);
                total += iov[i].iov_len;
            }
        }
        bool drain_once()
        {
            if (vec.empty())
                return true;
            ssize_t n = ::writev(sock._fd, vec.data(), (int)vec.size());
            if (n > 0) {
                sent += (size_t)n;
                size_t remain = (size_t)n;
                // Adjust iovec list
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
                return vec.empty();
            }
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return false; // need to wait
                if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN)
                    sock._tx_shutdown = true;
                sent = (ssize_t)n; // propagate error code (negative)
                return true;       // treat as complete with error
            }
            return true; // n == 0 unlikely; treat as done
        }
        bool await_ready() noexcept
        {
            // drain until would block or complete
            while (true) {
                bool done = drain_once();
                if (done)
                    return true; // finished or error
                // not done but would block
                return false;
            }
        }
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h = h;
            INIT_LIST_HEAD(&this->ws_node);
            sock._reactor->add_waiter(sock._fd, EPOLLOUT, this);
        }
        ssize_t await_resume() noexcept
        {
            if (!vec.empty() && sent >= 0) {
                // attempt more writes after wake
                while (true) {
                    bool done = drain_once();
                    if (done)
                        break;
                    // would block again
                    break;
                }
            }
            return (ssize_t)sent; // if error, sent holds negative errno return value
        }
    };
    /**
     * @brief 向量化发送（一次 await 覆盖多个 iovec，内部 writev + 直到 EAGAIN）。
     * @param iov iovec 数组。
     * @param iovcnt 数组元素个数。
     * @return 已发送字节数（可能 < 所有段总和，或 <0 错误）。
     * @note 内部复制一份 iovec 以便原位调整指针与长度，不修改调用者提供的数组。
     */
    sendv_awaiter sendv(const struct iovec* iov, int iovcnt) { return sendv_awaiter(*this, iov, iovcnt); }
    // ---- 全量发送 Awaiter 代码复用基类 (多次 epoll 等待直到完成/错误) ----
    template <typename Derived> struct send_full_base : io_waiter_base {
        tcp_socket& sock;
        size_t      sent { 0 }; // 已发送字节
        ssize_t     err { 0 };  // <0 表示错误（第一次出错时记录）
        bool        have_lock { false };
        send_full_base(tcp_socket& s) : sock(s) { }
        enum class step { progress, would_block, done }; // progress=继续循环, would_block=需等待, done=完成或错误
        bool drain()
        {
            while (true) {
                auto st = static_cast<Derived*>(this)->attempt();
                if (st == step::progress)
                    continue;
                if (st == step::would_block)
                    return false;
                return true; // done
            }
        }
        static void lock_acquired_cb(worknode* w)
        {
            auto* self      = static_cast<Derived*>(w);
            self->have_lock = true;
            // 开始真正的发送逻辑
            if (!self->drain()) {
                self->func = &Derived::drive_cb;
                self->sock._reactor->add_waiter_custom(self->sock._fd, EPOLLOUT, self);
                return;
            }
            // 已完成，直接 resume
            self->sock.release_send_slot();
            self->func = &io_waiter_base::resume_cb;
            if (self->h)
                self->h.resume();
        }
        static void drive_cb(worknode* w)
        {
            auto* self = static_cast<Derived*>(w);
            if (!self->drain()) {
                self->sock._reactor->add_waiter(self->sock._fd, EPOLLOUT, self);
                self->func = &Derived::drive_cb;
                return;
            }
            self->sock.release_send_slot();
            self->func = &io_waiter_base::resume_cb;
            if (self->h)
                self->h.resume();
        }
        bool await_ready() noexcept
        {
            return false; // 始终通过锁路径
        }
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h = h;
            INIT_LIST_HEAD(&this->ws_node);
            this->func = &Derived::lock_acquired_cb;
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
        ssize_t finish_result() const noexcept { return (err < 0 && sent == 0) ? err : (ssize_t)sent; }
    };
    /**
     * @brief 单缓冲区全量发送 awaiter (内部自动多次等待直到完成/错误)。
     */
    struct send_all_awaiter : send_full_base<send_all_awaiter> {
        const char* buf;
        size_t      len;
        send_all_awaiter(tcp_socket& s, const void* b, size_t l)
            : send_full_base<send_all_awaiter>(s), buf(static_cast<const char*>(b)), len(l)
        {
        }
        using step = typename send_full_base<send_all_awaiter>::step;
        step attempt()
        {
            if (this->sent >= len)
                return step::done;
            ssize_t n = ::send(this->sock._fd, buf + this->sent, len - this->sent, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n > 0) {
                this->sent += (size_t)n;
                return this->sent == len ? step::done : step::progress;
            }
            if (n == 0)
                return step::done; // 对端关闭
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return step::would_block;
            if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN)
                this->sock._tx_shutdown = true;
            this->err = n;
            return step::done;
        }
        ssize_t     await_resume() noexcept { return this->finish_result(); }
        static void drive_cb(worknode* w) { send_full_base<send_all_awaiter>::drive_cb(w); }
    };
    send_all_awaiter send_all(const void* buf, size_t len) { return send_all_awaiter(*this, buf, len); }
    /**
     * @brief 聚合发送（保证尽力写完整缓冲区）。
     * @param buf 数据指针。
     * @param len 字节数。
     * @return ==len 成功；>0<len 发生错误/对端关闭前已写部分；<0 错误。
     */
    /**
     * @brief 向量聚合发送 awaiter（完整发送 iovec 列表或在错误/对端关闭时提前返回）。
     * 返回值语义同 send_all：完成 => 总字节；部分 => 已发送；错误且未发送任何数据 => 负错误码。
     */
    struct sendv_all_awaiter : send_full_base<sendv_all_awaiter> {
        std::vector<iovec> vec;
        size_t             total { 0 };
        sendv_all_awaiter(tcp_socket& s, const struct iovec* iov, int iovcnt) : send_full_base<sendv_all_awaiter>(s)
        {
            vec.reserve((size_t)iovcnt);
            for (int i = 0; i < iovcnt; ++i) {
                vec.push_back(iov[i]);
                total += iov[i].iov_len;
            }
        }
        using step = typename send_full_base<sendv_all_awaiter>::step;
        step attempt()
        {
            if (vec.empty())
                return step::done;
            ssize_t n = ::writev(this->sock._fd, vec.data(), (int)vec.size());
            if (n > 0) {
                this->sent += (size_t)n;
                size_t remain = (size_t)n;
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
                return vec.empty() ? step::done : step::progress;
            }
            if (n == 0)
                return step::done;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return step::would_block;
            if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN)
                this->sock._tx_shutdown = true;
            this->err = n;
            return step::done;
        }
        ssize_t     await_resume() noexcept { return this->finish_result(); }
        static void drive_cb(worknode* w) { send_full_base<sendv_all_awaiter>::drive_cb(w); }
    };
    sendv_all_awaiter send_all(const struct iovec* iov, int iovcnt) { return sendv_all_awaiter(*this, iov, iovcnt); }

    // 内部串行发送队列：send_all / sendv_all 默认使用
    void release_send_slot()
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

private:
    friend class fd_workqueue<lock, Reactor>;
    explicit tcp_socket(workqueue<lock>& exec, Reactor<lock>& reactor) : _exec(exec), _reactor(&reactor)
    {
        _fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (_fd < 0)
            throw std::runtime_error("socket failed");
        set_non_block();
        _reactor->add_fd(_fd);
        INIT_LIST_HEAD(&_send_waiters);
        INIT_LIST_HEAD(&_recv_waiters);
    }
    tcp_socket(int fd, workqueue<lock>& exec, Reactor<lock>& reactor) : _exec(exec), _reactor(&reactor), _fd(fd)
    {
        set_non_block();
        _reactor->add_fd(_fd);
        INIT_LIST_HEAD(&_send_waiters);
        INIT_LIST_HEAD(&_recv_waiters);
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
    lock      _io_serial_lock;
    list_head _send_waiters;
    bool      _send_locked { false };
    list_head _recv_waiters;
    bool      _recv_locked { false };
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

} // namespace co_wq::net
#endif
