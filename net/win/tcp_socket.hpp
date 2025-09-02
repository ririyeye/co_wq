/**
 * @file tcp_socket.hpp
 * @brief 基于 Windows Overlapped/IOCP 的 TCP 协程原语实现，提供与 linux 版本尽量一致的接口。
 *
 * 主要特性：
 *  - 使用自定义 reactor (IOCP) + workqueue 协调驱动
 *  - 发送/接收操作串行化（serial_queue）保证一次只存在一个同类 awaiter 执行，防止并发写/读乱序
 *  - 提供 recv / recv_all / send / send_all / sendv / sendv_all 等 awaiter
 *  - send / recv 的 “_all” 版本内部循环直到缓冲区耗尽或遇到 EOF/错误
 *  - vectored I/O (sendv) 在 Windows 上通过 WSASend 多缓冲区支持
 *
 * 注意：当前 connect 只是简单异步模拟（监听可写事件），未使用 ConnectEx 扩展，可根据需要进一步增强。
 */
#pragma once

#include "io_serial.hpp"
#include "io_waiter.hpp"
#include "iocp_reactor.hpp"
#include "worker.hpp"
#include <basetsd.h>
#include <winsock2.h>
#include <ws2tcpip.h>

// ssize_t substitute
#ifndef _SSIZE_T_DEFINED
using ssize_t = SSIZE_T;
#define _SSIZE_T_DEFINED
#endif
#include <mswsock.h>
#include <stdexcept>
#include <string>
#include <vector>

// Provide POSIX-like iovec for Windows sendv support if not already defined
#ifndef IOVEC_DEFINED_CO_WQ
struct iovec {
    void*  iov_base;
    size_t iov_len;
};
#define IOVEC_DEFINED_CO_WQ 1
#endif

namespace co_wq::net {

template <lockable lock, template <class> class Reactor> class fd_workqueue; // fwd

// Overlapped-based tcp_socket with serialization & extended awaiters to mirror linux feature set
/**
 * @tparam lock 自定义锁类型，需满足 lockable 概念
 * @tparam Reactor 反应器模板（在 Windows 下实际为 iocp_reactor，默认模板参数与 linux 保持形式统一）
 * @brief TCP 套接字封装，提供协程 await 接口。
 */
template <lockable lock, template <class> class Reactor = epoll_reactor> class tcp_socket {
public:
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
     * @brief 关闭套接字并从 reactor 中移除。
     * @note 多次调用安全（内部用 -1 判定）。
     */
    void close()
    {
        if (_fd != -1) {
            if (_reactor)
                _reactor->remove_fd(_fd);
            ::closesocket((SOCKET)_fd);
            _fd = -1;
        }
    }
    /**
     * @brief 关闭发送方向 (半关闭)，等价于 ::shutdown(SD_SEND)。
     */
    void shutdown_tx()
    {
        if (_fd != -1 && !_tx_shutdown) {
            ::shutdown((SOCKET)_fd, SD_SEND);
            _tx_shutdown = true;
        }
    }
    /** @return 是否已经执行过发送方向关闭 */
    bool             tx_shutdown() const noexcept { return _tx_shutdown; }
    /** @return 是否已经读到 EOF（对端关闭或 0 字节读取）*/
    bool             rx_eof() const noexcept { return _rx_eof; }
    /** @return 原始 socket 句柄 */
    int              native_handle() const { return _fd; }
    /** @return 关联的执行队列 */
    workqueue<lock>& exec() { return _exec; }
    /** @return 串行化用互斥锁 */
    lock&            serial_lock() { return _io_serial_lock; }
    /** @return 关联的 Reactor */
    Reactor<lock>*   reactor() { return _reactor; }
    /** 标记收到 EOF（内部使用）*/
    void             mark_rx_eof() { _rx_eof = true; }
    /** 标记发送方向关闭（内部使用）*/
    void             mark_tx_shutdown() { _tx_shutdown = true; }
    struct connect_awaiter : io_waiter_base {
        /**
         * @brief 异步连接 awaiter（简化实现：依赖非阻塞 connect + 写事件）
         * @note 未使用 ConnectEx；若需要高性能并发连接，可后续扩展。
         * 成功返回 0，失败返回 -1。
         */
        tcp_socket& sock;
        std::string host;
        uint16_t    port;
        int         ret { 0 };
        connect_awaiter(tcp_socket& s, std::string h, uint16_t p) : sock(s), host(std::move(h)), port(p) { }
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> awaiting)
        {
            this->h = awaiting;
            INIT_LIST_HEAD(&this->ws_node);
            sockaddr_in addr {};
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(port);
            if (InetPtonA(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
                ret = -1;
                sock._exec.post(*this);
                return;
            }
            int r = ::connect((SOCKET)sock._fd, (sockaddr*)&addr, sizeof(addr));
            if (r == 0) {
                ret = 0;
                sock._exec.post(*this);
                return;
            }
            int err = WSAGetLastError();
            if (r == SOCKET_ERROR && (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS)) {
                // emulate async: use reactor immediate post after short delay (no real write readiness tracking yet)
                sock._reactor->add_waiter(sock._fd, EPOLLOUT, this);
                return;
            }
            ret = -1;
            sock._exec.post(*this);
        }
        int await_resume() noexcept
        {
            if (ret == 0) {
                int err = 0;
                int len = sizeof(err);
                if (getsockopt((SOCKET)sock._fd, SOL_SOCKET, SO_ERROR, (char*)&err, &len) == 0 && err == 0)
                    return 0;
                return -1;
            }
            return ret;
        }
    };
    connect_awaiter connect(const std::string& host, uint16_t port) { return connect_awaiter(*this, host, port); }
    /**
     * @brief 读取一次（最多 len 字节）; 可能返回部分数据；0 表示 EOF；负数表示错误。
     * @note 与 linux 版行为保持一致，不保证读满。
     */
    struct recv_awaiter : serial_slot_awaiter<recv_awaiter, tcp_socket> {
        void*    buf;
        size_t   len;
        ssize_t  nread { -1 };
        iocp_ovl ovl;
        bool     started { false };
        recv_awaiter(tcp_socket& s, void* b, size_t l)
            : serial_slot_awaiter<recv_awaiter, tcp_socket>(s, s._recv_q), buf(b), len(l)
        {
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.waiter = this;
        }
        static void lock_acquired_cb(worknode* w)
        {
            auto* self    = static_cast<recv_awaiter*>(w);
            self->started = true;
            self->issue();
        }
        void issue()
        {
            WSABUF wbuf { (ULONG)len, (CHAR*)buf };
            DWORD  flags = 0, recvd = 0;
            this->func = &io_waiter_base::resume_cb;
            int r      = WSARecv((SOCKET)this->owner._fd, &wbuf, 1, &recvd, &flags, &ovl, nullptr);
            if (r == 0) {
                nread = (ssize_t)recvd;
                finish();
                return;
            }
            int err = WSAGetLastError();
            if (r == SOCKET_ERROR && err == WSA_IO_PENDING)
                return;
            nread = -1;
            finish();
        }
        void finish()
        {
            serial_slot_awaiter<recv_awaiter, tcp_socket>::release(this);
            this->owner._reactor->post_completion(this);
        }
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h = h;
            INIT_LIST_HEAD(&this->ws_node);
            serial_acquire_or_enqueue(this->q, this->owner.serial_lock(), this->owner.exec(), *this);
        }
        ssize_t await_resume() noexcept
        {
            if (nread == -1) {
                DWORD tr = 0, fl = 0;
                if (WSAGetOverlappedResult((SOCKET)this->owner._fd, &ovl, &tr, FALSE, &fl))
                    nread = (ssize_t)tr;
            }
            if (nread == 0)
                this->owner.mark_rx_eof();
            return nread;
        }
    };
    recv_awaiter recv(void* buf, size_t len) { return recv_awaiter(*this, buf, len); }
    /**
     * @brief 反复发起 overlapped 读取直到填满缓冲区或遇到 EOF/错误。
     * @return await_resume(): 成功返回已读字节数（== len），若中途 EOF 返回已读；若第一轮即 EOF/错误返回 -1。
     */
    struct recv_all_awaiter : serial_slot_awaiter<recv_all_awaiter, tcp_socket> {
        char*    buf;
        size_t   len;
        size_t   recvd { 0 };
        ssize_t  err { 0 };
        iocp_ovl ovl;
        bool     inflight { false };
        bool     finished { false };
        recv_all_awaiter(tcp_socket& s, void* b, size_t l)
            : serial_slot_awaiter<recv_all_awaiter, tcp_socket>(s, s._recv_q), buf((char*)b), len(l)
        {
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.waiter = this;
        }
        static void lock_acquired_cb(worknode* w)
        {
            auto* self = static_cast<recv_all_awaiter*>(w);
            self->drive();
        }
        void issue()
        {
            WSABUF wb { (ULONG)(len - recvd), buf + recvd };
            DWORD  flags = 0, got = 0;
            this->func = &recv_all_awaiter::drive_cb;
            int r      = WSARecv((SOCKET)this->owner._fd, &wb, 1, &got, &flags, &ovl, nullptr);
            if (r == 0) {       // immediate completion
                if (got == 0) { // EOF
                    this->owner.mark_rx_eof();
                    finish();
                    return;
                }
                recvd += got;
                if (recvd >= len) {
                    finish();
                    return;
                }
                return;
            }
            int e = WSAGetLastError();
            if (r == SOCKET_ERROR && e == WSA_IO_PENDING) {
                inflight = true;
                return;
            }
            // error
            if (recvd == 0)
                err = -1;
            finish();
        }
        void drive()
        {
            while (!finished && recvd < len) {
                issue();
                if (inflight)
                    return;
            }
            if (!finished && recvd >= len)
                finish();
        }
        static void drive_cb(worknode* w)
        {
            auto* self = static_cast<recv_all_awaiter*>(w);
            if (self->finished)
                return;
            DWORD tr = 0, fl = 0;
            if (WSAGetOverlappedResult((SOCKET)self->owner._fd, &self->ovl, &tr, FALSE, &fl)) {
                if (tr == 0) {
                    self->owner.mark_rx_eof();
                    self->finish();
                    return;
                }
                self->recvd += tr;
                if (self->recvd >= self->len) {
                    self->finish();
                    return;
                }
                self->inflight = false;
                self->drive();
                return;
            } else {
                if (self->recvd == 0)
                    self->err = -1;
                self->finish();
                return;
            }
        }
        void finish()
        {
            if (finished)
                return;
            finished = true;
            serial_slot_awaiter<recv_all_awaiter, tcp_socket>::release(this);
            this->func = &io_waiter_base::resume_cb;
            if (this->h)
                this->h.resume();
        }
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h = h;
            INIT_LIST_HEAD(&this->ws_node);
            serial_acquire_or_enqueue(this->q, this->owner.serial_lock(), this->owner.exec(), *this);
        }
        ssize_t await_resume() noexcept { return (err < 0 && recvd == 0) ? err : (ssize_t)recvd; }
    };
    recv_all_awaiter recv_all(void* buf, size_t len) { return recv_all_awaiter(*this, buf, len); }
    /**
     * @brief 发送一次（最多 len 剩余）；可能部分发送；返回已发送字节数或错误。
     */
    struct send_awaiter : serial_slot_awaiter<send_awaiter, tcp_socket> {
        const char* buf;
        size_t      len;
        size_t      sent { 0 };
        ssize_t     err { 0 };
        iocp_ovl    ovl;
        bool        inflight { false };
        bool        finished { false };
        send_awaiter(tcp_socket& s, const void* b, size_t l)
            : serial_slot_awaiter<send_awaiter, tcp_socket>(s, s._send_q), buf((const char*)b), len(l)
        {
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.waiter = this;
        }
        static void lock_acquired_cb(worknode* w)
        {
            auto* self = static_cast<send_awaiter*>(w);
            self->drive();
        }
        void issue()
        {
            size_t remain = len - sent;
            if (remain == 0) {
                finish();
                return;
            }
            WSABUF wb { (ULONG)remain, (CHAR*)(buf + sent) };
            DWORD  done = 0, flags = 0;
            this->func = &send_awaiter::drive_cb;
            int r      = WSASend((SOCKET)this->owner._fd, &wb, 1, &done, flags, &ovl, nullptr);
            if (r == 0) { // immediate
                sent += done;
                if (sent >= len) {
                    finish();
                    return;
                }
                return;
            }
            int e = WSAGetLastError();
            if (r == SOCKET_ERROR && e == WSA_IO_PENDING) {
                inflight = true;
                return;
            }
            if (e == WSAECONNRESET)
                this->owner._tx_shutdown = true;
            if (sent == 0)
                err = -1;
            finish();
        }
        void drive()
        {
            while (!finished && sent < len && err == 0) {
                issue();
                if (inflight)
                    return;
            }
            if (!finished && (err < 0 || sent >= len))
                finish();
        }
        static void drive_cb(worknode* w)
        {
            auto* self = static_cast<send_awaiter*>(w);
            if (self->finished)
                return;
            DWORD tr = 0, fl = 0;
            if (WSAGetOverlappedResult((SOCKET)self->owner._fd, &self->ovl, &tr, FALSE, &fl)) {
                self->sent += tr;
                self->inflight = false;
                if (self->sent >= self->len) {
                    self->finish();
                    return;
                }
                self->drive();
                return;
            } else {
                if (self->sent == 0)
                    self->err = -1;
                self->finish();
                return;
            }
        }
        void finish()
        {
            if (finished)
                return;
            finished = true;
            serial_slot_awaiter<send_awaiter, tcp_socket>::release(this);
            this->func = &io_waiter_base::resume_cb;
            if (this->h)
                this->h.resume();
        }
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h = h;
            INIT_LIST_HEAD(&this->ws_node);
            serial_acquire_or_enqueue(this->q, this->owner.serial_lock(), this->owner.exec(), *this);
        }
        ssize_t await_resume() noexcept { return err < 0 ? err : (ssize_t)sent; }
    };
    send_awaiter send(const void* buf, size_t len) { return send_awaiter(*this, buf, len); }
    /**
     * @brief 循环发送直到缓冲区全部发送或遇到错误；成功返回 len。
     */
    struct send_all_awaiter : send_awaiter {
        using send_awaiter::send_awaiter;
    };
    send_all_awaiter send_all(const void* buf, size_t len) { return send_all_awaiter(*this, buf, len); }
    /**
     * @brief 多缓冲发送（WSASend 支持）；可能部分完成；内部维护进度并推进 WSABUF。
     * @note await_resume 返回已发送字节数；错误返回 -1。
     */
    struct sendv_awaiter : serial_slot_awaiter<sendv_awaiter, tcp_socket> {
        std::vector<WSABUF> bufs;
        size_t              total { 0 };
        size_t              sent { 0 };
        ssize_t             err { 0 };
        iocp_ovl            ovl;
        bool                inflight { false };
        bool                finished { false };
        sendv_awaiter(tcp_socket& s, const struct iovec* iov, int iovcnt)
            : serial_slot_awaiter<sendv_awaiter, tcp_socket>(s, s._send_q)
        {
            bufs.reserve(iovcnt);
            for (int i = 0; i < iovcnt; ++i) {
                WSABUF b;
                b.len = (ULONG)iov[i].iov_len;
                b.buf = (CHAR*)iov[i].iov_base;
                bufs.push_back(b);
                total += b.len;
            }
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.waiter = this;
        }
        static void lock_acquired_cb(worknode* w)
        {
            auto* self = static_cast<sendv_awaiter*>(w);
            self->drive();
        }
        void compact()
        {
            while (!bufs.empty() && bufs.front().len == 0)
                bufs.erase(bufs.begin());
        }
        void issue()
        {
            compact();
            if (bufs.empty()) {
                finish();
                return;
            }
            DWORD done  = 0;
            DWORD flags = 0;
            this->func  = &sendv_awaiter::drive_cb;
            int r = WSASend((SOCKET)this->owner._fd, bufs.data(), (DWORD)bufs.size(), &done, flags, &ovl, nullptr);
            if (r == 0) { // immediate completion
                if (done) {
                    advance(done);
                    if (sent >= total) {
                        finish();
                        return;
                    }
                }
                return; // let drive() loop continue
            }
            int e = WSAGetLastError();
            if (r == SOCKET_ERROR && e == WSA_IO_PENDING) {
                inflight = true;
                return;
            }
            if (e == WSAECONNRESET)
                this->owner._tx_shutdown = true;
            if (sent == 0)
                err = -1;
            finish();
        }
        void advance(DWORD done)
        {
            sent += done;
            DWORD remain = done;
            for (auto& b : bufs) {
                if (remain >= b.len) {
                    remain -= b.len;
                    b.len = 0;
                    b.buf = nullptr;
                } else {
                    b.buf += remain;
                    b.len -= remain;
                    remain = 0;
                    break;
                }
            }
        }
        void drive()
        {
            while (!finished && sent < total && err == 0) {
                issue();
                if (inflight)
                    return; // wait for IOCP
            }
            if (!finished && (err < 0 || sent >= total))
                finish();
        }
        static void drive_cb(worknode* w)
        {
            auto* self = static_cast<sendv_awaiter*>(w);
            if (self->finished)
                return;
            DWORD tr = 0, fl = 0;
            if (WSAGetOverlappedResult((SOCKET)self->owner._fd, &self->ovl, &tr, FALSE, &fl)) {
                self->advance(tr);
                self->inflight = false;
                if (self->sent >= self->total) {
                    self->finish();
                    return;
                }
                self->drive();
                return;
            } else {
                if (self->sent == 0)
                    self->err = -1;
                self->finish();
                return;
            }
        }
        void finish()
        {
            if (finished)
                return;
            finished = true;
            serial_slot_awaiter<sendv_awaiter, tcp_socket>::release(this);
            this->func = &io_waiter_base::resume_cb;
            if (this->h)
                this->h.resume();
        }
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h = h;
            INIT_LIST_HEAD(&this->ws_node);
            serial_acquire_or_enqueue(this->q, this->owner.serial_lock(), this->owner.exec(), *this);
        }
        ssize_t await_resume() noexcept { return err < 0 ? err : (ssize_t)sent; }
    };
    sendv_awaiter sendv(const struct iovec* iov, int iovcnt) { return sendv_awaiter(*this, iov, iovcnt); }
    /**
     * @brief sendv 的 “全部发送” 语义版本，复用逻辑（当前与基类一致，当需要严格区分可扩展）。
     */
    struct sendv_all_awaiter : sendv_awaiter {
        using sendv_awaiter::sendv_awaiter;
    };
    sendv_all_awaiter send_all(const struct iovec* iov, int iovcnt) { return sendv_all_awaiter(*this, iov, iovcnt); }

private:
    friend class fd_workqueue<lock, Reactor>;
    explicit tcp_socket(workqueue<lock>& exec, Reactor<lock>& reactor) : _exec(exec), _reactor(&reactor)
    {
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
            throw std::runtime_error("socket failed");
        _fd = (int)s;
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
        if (_fd != -1) {
            u_long m = 1;
            ioctlsocket((SOCKET)_fd, FIONBIO, &m);
        }
    }
    workqueue<lock>& _exec;
    Reactor<lock>*   _reactor { nullptr };
    int              _fd { -1 };
    bool             _rx_eof { false };
    bool             _tx_shutdown { false };
    lock             _io_serial_lock; // unify naming with linux side
    serial_queue     _send_q;         // serialized send operations
    serial_queue     _recv_q;         // serialized recv operations
};

template <lockable lock>
inline Task<int, Work_Promise<lock, int>> async_connect(tcp_socket<lock>& s, const std::string host, uint16_t port)
{
    co_return co_await s.connect(host, port);
}
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>> async_send_all(tcp_socket<lock>& s, const void* buf, size_t len)
{
    co_return co_await s.send_all(buf, len);
}
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>> async_recv_some(tcp_socket<lock>& s, void* buf, size_t len)
{
    ssize_t n = co_await s.recv(buf, len);
    co_return n;
}
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>> async_recv_all(tcp_socket<lock>& s, void* buf, size_t len)
{
    co_return co_await s.recv_all(buf, len);
}
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>>
async_sendv_all(tcp_socket<lock>& s, const struct iovec* iov, int iovcnt)
{
    co_return co_await s.send_all(iov, iovcnt);
}

} // namespace co_wq::net
