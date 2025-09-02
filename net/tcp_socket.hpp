// tcp_socket.hpp - TCP socket 协程原语
#pragma once
#ifdef __linux__
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

// Reactor 后端需提供: static instance(exec) / add_fd / remove_fd / add_waiter 接口
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
    // 提供给串行 / two_phase awaiter 的访问器
    workqueue<lock>& exec() { return _exec; }
    lock&            serial_lock() { return _io_serial_lock; }
    Reactor<lock>*   reactor() { return _reactor; }
    void             mark_rx_eof() { _rx_eof = true; }
    void             mark_tx_shutdown() { _tx_shutdown = true; }
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
    /**
     * @brief 单次接收 awaiter（使用 two_phase_drain_awaiter 模式）。
     *
     * 语义保持为“执行一次 recv”：不会在同一 await 中循环读取到 EAGAIN，只做一次系统调用；
     * 这样上层可以更明确地控制协议分帧或字节流节奏。
     */
    struct recv_awaiter : two_phase_drain_awaiter<recv_awaiter, tcp_socket> {
        void*   buf;
        size_t  len;
        ssize_t nread { -1 };
        recv_awaiter(tcp_socket& s, void* b, size_t l)
            : two_phase_drain_awaiter<recv_awaiter, tcp_socket>(s, s._recv_q), buf(b), len(l)
        {
        }
        // attempt_once: >0 继续；0 完成；-1 需等待。这里一次 recv 完成后直接返回 0。
        int attempt_once()
        {
            nread = ::recv(this->owner.native_handle(), buf, len, MSG_DONTWAIT);
            if (nread > 0)
                return 0; // 读到数据 => 完成
            if (nread == 0) {
                this->owner.mark_rx_eof();
                return 0;
            }
            // nread < 0
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                nread = -1;
                return -1;
            }
            // 其他错误：nread 保存 -1，errno 供调用方使用
            return 0;
        }
        static void arm(recv_awaiter* self, bool /*first*/)
        {
            self->owner.reactor()->add_waiter_custom(self->owner.native_handle(), EPOLLIN, self);
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
    /**
     * @brief 读取直到缓冲区填满 / EOF / 错误 的 awaiter。
     * 返回值:
     *  - ==len 读取成功填满；
     *  - 0..len-1 读取到 EOF 或 错误前的部分（若出现错误且读取了部分数据，优先返回已读字节数）；
     *  - <0 在首个系统调用就出错且未读取任何数据（返回 -1，errno 保留）。
     */
    struct recv_all_awaiter : two_phase_drain_awaiter<recv_all_awaiter, tcp_socket> {
        char*   buf;
        size_t  len;
        size_t  recvd { 0 };
        ssize_t err { 0 }; // 仅在 recvd == 0 且系统调用返回 <0 (非 EAGAIN) 时保存
        recv_all_awaiter(tcp_socket& s, void* b, size_t l)
            : two_phase_drain_awaiter<recv_all_awaiter, tcp_socket>(s, s._recv_q), buf(static_cast<char*>(b)), len(l)
        {
        }
        int attempt_once()
        {
            if (recvd >= len)
                return 0;
            ssize_t n = ::recv(this->owner.native_handle(), buf + recvd, len - recvd, MSG_DONTWAIT);
            if (n > 0) {
                recvd += (size_t)n;
                return recvd == len ? 0 : 1; // 继续 or 完成
            }
            if (n == 0) {
                this->owner.mark_rx_eof();
                return 0;
            }
            // n < 0
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return -1; // 等待可读
            if (recvd == 0)
                err = n; // 记录首个错误
            return 0;
        }
        static void arm(recv_all_awaiter* self, bool /*first*/)
        {
            self->owner.reactor()->add_waiter_custom(self->owner.native_handle(), EPOLLIN, self);
        }
        ssize_t await_resume() noexcept { return (err < 0 && recvd == 0) ? err : (ssize_t)recvd; }
    };
    recv_all_awaiter recv_all(void* buf, size_t len) { return recv_all_awaiter(*this, buf, len); }
    struct send_awaiter : io_waiter_base {
        tcp_socket& sock;
        const void* buf;
        size_t      len;
        size_t      sent { 0 };
        send_awaiter(tcp_socket& s, const void* b, size_t l) : sock(s), buf(b), len(l) { }
        // 尽可能写到 EAGAIN 再决定挂起：EPOLLET 下必须耗尽当前可写边沿，否则可能失去后续事件导致挂起。
        bool await_ready() noexcept
        {
            while (sent < len) {
                ssize_t n = ::send(sock._fd, (char*)buf + sent, len - sent, MSG_DONTWAIT | MSG_NOSIGNAL);
                if (n > 0) {
                    sent += (size_t)n;
                    continue; // 继续写剩余数据
                }
                if (n == 0) {
                    // 视为无进展；返回调用方（可能重试或处理错误）
                    return true;
                }
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        // 需要等待下一个可写事件（除非已经正好写完）
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
                return (ssize_t)sent; // 已完成
            while (sent < len) {
                ssize_t n = ::send(sock._fd, (char*)buf + sent, len - sent, MSG_DONTWAIT | MSG_NOSIGNAL);
                if (n > 0) {
                    sent += (size_t)n;
                    continue; // 持续写直到 EAGAIN 或完成
                }
                if (n == 0) {
                    // 罕见：视为暂不可进展
                    break;
                }
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        break; // 部分发送；调用方可再次 await
                    if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN)
                        sock._tx_shutdown = true;
                    return n; // propagate error
                }
            }
            return (ssize_t)sent; // 部分或全部完成
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
    // 向量化发送 awaiter（writev 聚合写）
    struct sendv_awaiter : io_waiter_base {
        tcp_socket&        sock;
        std::vector<iovec> vec;         // 可变副本
        size_t             total { 0 }; // 总字节数
        size_t             sent { 0 };  // 已发送字节
        sendv_awaiter(tcp_socket& s, const struct iovec* iov, int iovcnt) : sock(s)
        {
            vec.reserve((size_t)iovcnt);
            for (int i = 0; i < iovcnt; ++i) {
                vec.push_back(iov[i]);
                total += iov[i].iov_len;
            }
        }
        bool drain_once() // 单次写尝试；返回 true 表示完成或错误
        {
            if (vec.empty())
                return true;
            ssize_t n = ::writev(sock._fd, vec.data(), (int)vec.size());
            if (n > 0) {
                sent += (size_t)n;
                size_t remain = (size_t)n;
                // 调整 iovec 列表裁掉已发送部分
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
                    return false; // 需等待
                if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN)
                    sock._tx_shutdown = true;
                sent = (ssize_t)n; // 记录错误（负值）
                return true;       // 视为终止
            }
            return true; // n==0 罕见；视为结束
        }
        bool await_ready() noexcept
        {
            // 循环写直至完成或将阻塞
            while (true) {
                bool done = drain_once();
                if (done)
                    return true; // 完成或错误
                // 未完成但会阻塞
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
                // 唤醒后再次写
                while (true) {
                    bool done = drain_once();
                    if (done)
                        break;
                    // 再次将阻塞
                    break;
                }
            }
            return (ssize_t)sent; // 若 sent<0 表示错误
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
    // ---- 全量发送 Awaiter 基类 (多次等待直至完成或错误) ----
    template <typename Derived> struct send_full_base : two_phase_drain_awaiter<Derived, tcp_socket> {
        size_t  sent { 0 };
        ssize_t err { 0 }; // <0 记录错误
        send_full_base(tcp_socket& s) : two_phase_drain_awaiter<Derived, tcp_socket>(s, s._send_q) { }
        // attempt_once: >0 有进展继续；0 完成；-1 需等待
        ssize_t     finish_result() const noexcept { return (err < 0 && sent == 0) ? err : (ssize_t)sent; }
        static void arm(send_full_base* self, bool /*first*/)
        {
            self->owner.reactor()->add_waiter_custom(self->owner.native_handle(), EPOLLOUT, self);
        }
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
        int attempt_once()
        {
            if (this->sent >= len)
                return 0;
            ssize_t n = ::send(this->owner.native_handle(),
                               buf + this->sent,
                               len - this->sent,
                               MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n > 0) {
                this->sent += (size_t)n;
                return this->sent == len ? 0 : 1;
            }
            if (n == 0)
                return 0; // treat as done
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return -1;
            if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN)
                this->owner.mark_tx_shutdown();
            this->err = n;
            return 0;
        }
        ssize_t await_resume() noexcept { return this->finish_result(); }
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
        int attempt_once()
        {
            if (vec.empty())
                return 0;
            ssize_t n = ::writev(this->owner.native_handle(), vec.data(), (int)vec.size());
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
                return vec.empty() ? 0 : 1;
            }
            if (n == 0)
                return 0;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return -1;
            if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN)
                this->owner.mark_tx_shutdown();
            this->err = n;
            return 0;
        }
        ssize_t await_resume() noexcept { return this->finish_result(); }
    };
    sendv_all_awaiter send_all(const struct iovec* iov, int iovcnt) { return sendv_all_awaiter(*this, iov, iovcnt); }

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
    lock         _io_serial_lock;
    serial_queue _send_q;
    serial_queue _recv_q;
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
#endif
