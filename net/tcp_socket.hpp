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
        ssize_t     nread { 0 };
        recv_awaiter(tcp_socket& s, void* b, size_t l) : sock(s), buf(b), len(l) { }
        bool await_ready() noexcept
        {
            nread = ::recv(sock._fd, buf, len, MSG_DONTWAIT);
            if (nread >= 0)
                return true;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                nread = -1;
                return false;
            }
            return true;
        }
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h = h;
            INIT_LIST_HEAD(&this->ws_node);
            sock._reactor->add_waiter(sock._fd, EPOLLIN, this);
        }
        ssize_t await_resume() noexcept
        {
            if (nread >= 0) {
                if (nread == 0)
                    sock._rx_eof = true;
                return nread;
            }
            ssize_t r = ::recv(sock._fd, buf, len, MSG_DONTWAIT);
            if (r == 0)
                sock._rx_eof = true;
            return r;
        }
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
    // Aggregated full-buffer send: keeps awaiting until entire buffer sent or error.
    Task<ssize_t, Work_Promise<lock, ssize_t>> send_all(const void* buf, size_t len)
    {
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = co_await send((const char*)buf + sent, len - sent);
            if (n <= 0) {
                co_return (sent > 0) ? (ssize_t)sent : n; // propagate error or partial
            }
            sent += (size_t)n;
        }
        co_return (ssize_t) sent;
    }
    /**
     * @brief 聚合发送（保证尽力写完整缓冲区）。
     * @param buf 数据指针。
     * @param len 字节数。
     * @return ==len 成功；>0<len 发生错误/对端关闭前已写部分；<0 错误。
     */
    // Aggregated vectored full send: send all iov buffers sequentially
    Task<ssize_t, Work_Promise<lock, ssize_t>> send_all(const struct iovec* iov, int iovcnt)
    {
        size_t total = 0;
        for (int i = 0; i < iovcnt; ++i)
            total += iov[i].iov_len;
        size_t sent = 0;
        // Reuse sendv awaiter repeatedly until all done or error
        // Each await returns cumulative bytes progressed since start of that awaiter; we return total progress overall.
        // Simpler: use one sendv awaiter that internally drains; just co_await once.
        ssize_t n = co_await sendv(iov, iovcnt);
        if (n < 0)
            co_return n;
        sent = (size_t)n;
        if (sent == total)
            co_return (ssize_t) sent;
        // If partial (should only happen on error/EAGAIN cycles), loop until done or error
        while (sent < total) {
            // Build adjusted iov pointing into remaining region
            std::vector<iovec> rem;
            size_t             skip = sent;
            for (int i = 0; i < iovcnt; ++i) {
                if (skip >= iov[i].iov_len) {
                    skip -= iov[i].iov_len;
                    continue;
                }
                iovec v { (char*)iov[i].iov_base + skip, iov[i].iov_len - skip };
                rem.push_back(v);
                skip = 0;
            }
            n = co_await sendv(rem.data(), (int)rem.size());
            if (n <= 0) {
                if (n < 0 && sent == 0)
                    co_return n; // error early
                if (n > 0)
                    sent += (size_t)n;
                break;
            }
            sent += (size_t)n;
        }
        co_return (ssize_t) sent;
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
    }
    tcp_socket(int fd, workqueue<lock>& exec, Reactor<lock>& reactor) : _exec(exec), _reactor(&reactor), _fd(fd)
    {
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
    bool             _rx_eof { false };
    bool             _tx_shutdown { false };
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
    co_return co_await s.send_all(buf, len);
}
template <lockable lock>
/**
 * @brief 聚合发送包装（iovec 列表）。
 */
inline Task<ssize_t, Work_Promise<lock, ssize_t>>
async_sendv_all(tcp_socket<lock>& s, const struct iovec* iov, int iovcnt)
{
    co_return co_await s.send_all(iov, iovcnt);
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
