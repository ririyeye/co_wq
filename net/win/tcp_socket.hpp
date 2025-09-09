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

#include "callback_wq.hpp"
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
template <lockable lock, template <class> class Reactor = iocp_reactor> class tcp_socket {
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
    bool tx_shutdown() const noexcept { return _tx_shutdown; }
    /** @return 是否已经读到 EOF（对端关闭或 0 字节读取）*/
    bool rx_eof() const noexcept { return _rx_eof; }
    /** @return 原始 socket 句柄 */
    int native_handle() const { return _fd; }
    /** @return 关联的执行队列 */
    workqueue<lock>& exec() { return _exec; }
    /** @return 串行化用互斥锁 */
    lock& serial_lock() { return _io_serial_lock; }
    /** @return 关联的 Reactor */
    Reactor<lock>* reactor() { return _reactor; }
    /** 标记收到 EOF（内部使用）*/
    void mark_rx_eof() { _rx_eof = true; }
    /** 标记发送方向关闭（内部使用）*/
    void mark_tx_shutdown() { _tx_shutdown = true; }
    struct connect_awaiter : io_waiter_base {
        /**
         * @brief 基于 ConnectEx 的真正异步连接 awaiter。成功返回 0，失败返回 -1。
         */
        tcp_socket&    sock;
        std::string    host;
        uint16_t       port;
        int            ret { -1 };
        iocp_ovl       ovl;
        LPFN_CONNECTEX _connectex { nullptr };
        sockaddr_in    _addr {};
        bool           issued { false };
        connect_awaiter(tcp_socket& s, std::string h, uint16_t p) : sock(s), host(std::move(h)), port(p)
        {
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.waiter       = this;
            this->route_ctx  = &sock._cbq;
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        bool load_connectex()
        {
            if (_connectex)
                return true;
            GUID  guid  = WSAID_CONNECTEX;
            DWORD bytes = 0;
            if (WSAIoctl((SOCKET)sock._fd,
                         SIO_GET_EXTENSION_FUNCTION_POINTER,
                         &guid,
                         sizeof(guid),
                         &_connectex,
                         sizeof(_connectex),
                         &bytes,
                         NULL,
                         NULL)
                == SOCKET_ERROR) {
                _connectex = nullptr;
                return false;
            }
            return true;
        }
        bool await_ready() noexcept
        {
            INIT_LIST_HEAD(&this->ws_node);
            // 解析目标地址
            _addr.sin_family = AF_INET;
            _addr.sin_port   = htons(port);
            if (InetPtonA(AF_INET, host.c_str(), &_addr.sin_addr) <= 0) {
                ret = -1;
                return true;
            }
            if (!load_connectex()) {
                ret = -1;
                return true;
            }
            // ConnectEx 要求先 bind 本地地址（INADDR_ANY:0）
            sockaddr_in local {};
            local.sin_family      = AF_INET;
            local.sin_addr.s_addr = INADDR_ANY;
            local.sin_port        = 0;
            ::bind((SOCKET)sock._fd, (sockaddr*)&local, sizeof(local)); // 若已绑定可忽略错误
            // 发起 Overlapped 连接
            this->func = &io_waiter_base::resume_cb;
            BOOL ok    = _connectex((SOCKET)sock._fd, (sockaddr*)&_addr, sizeof(_addr), nullptr, 0, nullptr, &ovl);
            issued     = true;
            if (ok) {
                // 立即完成
                setsockopt((SOCKET)sock._fd, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
                ret = 0;
                return true;
            }
            int err = WSAGetLastError();
            if (err != ERROR_IO_PENDING) {
                ret = -1;
                return true;
            }
            return false; // pending
        }
        void await_suspend(std::coroutine_handle<> awaiting) { this->h = awaiting; }
        int  await_resume() noexcept
        {
            if (ret == 0)
                return 0;
            if (issued && ret != 0) {
                DWORD tr = 0, fl = 0;
                if (WSAGetOverlappedResult((SOCKET)sock._fd, &ovl, &tr, FALSE, &fl)) {
                    setsockopt((SOCKET)sock._fd, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
                    ret = 0;
                } else {
                    ret = -1;
                }
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
            ovl.waiter       = this;
            this->route_ctx  = &this->owner._cbq;
            this->route_post = &callback_wq<lock>::post_adapter;
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
            ovl.waiter       = this;
            this->route_ctx  = &this->owner._cbq;
            this->route_post = &callback_wq<lock>::post_adapter;
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
     * @brief 统一 TCP 发送 awaiter：支持单缓冲/向量 + 部分/全部语义。
     *
     * 语义：
     *  - 部分（partial）：尽力发送一次，可能返回 < 请求长度。
     *  - 全部（full）：循环推进，直到总字节数发送完成或遇到错误/对端关闭。
     *  - 向量发送使用 WSASend 的多缓冲功能，内部推进 WSABUF 指针与长度。
     * 并发：
     *  - 通过 _send_q 串行化，避免同一 socket 并发写。
     * 调度：
     *  - Overlapped/IOCP 完成后由 callback_wq 路由回主执行队列，保持回调有序。
     */
    struct send_awaiter : serial_slot_awaiter<send_awaiter, tcp_socket> {
        // 配置
        bool use_vec { false };
        bool full { false };
        // 单缓冲
        const char* buf { nullptr };
        size_t      len { 0 };
        // 向量
        std::vector<WSABUF> bufs;
        size_t              total { 0 };
        // 进度/状态
        size_t   sent { 0 };
        ssize_t  err { 0 };
        iocp_ovl ovl;
        bool     inflight { false };
        bool     finished { false };
        // 单缓冲
        send_awaiter(tcp_socket& s, const void* b, size_t l, bool full_mode)
            : serial_slot_awaiter<send_awaiter, tcp_socket>(s, s._send_q)
            , use_vec(false)
            , full(full_mode)
            , buf((const char*)b)
            , len(l)
            , total(l)
        {
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.waiter       = this;
            this->route_ctx  = &this->owner._cbq;
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        // 向量
        send_awaiter(tcp_socket& s, const struct iovec* iov, int iovcnt, bool full_mode)
            : serial_slot_awaiter<send_awaiter, tcp_socket>(s, s._send_q), use_vec(true), full(full_mode)
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
            ovl.waiter       = this;
            this->route_ctx  = &this->owner._cbq;
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        static void lock_acquired_cb(worknode* w)
        {
            auto* self = static_cast<send_awaiter*>(w);
            self->drive();
        }
        void compact()
        {
            while (!bufs.empty() && bufs.front().len == 0)
                bufs.erase(bufs.begin());
        }
        void advance(DWORD done)
        {
            sent += done;
            if (!use_vec)
                return;
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
        void issue()
        {
            DWORD done  = 0;
            DWORD flags = 0;
            this->func  = &send_awaiter::drive_cb;
            if (!use_vec) {
                size_t remain = len - sent;
                if (remain == 0) {
                    finish();
                    return;
                }
                WSABUF wb { (ULONG)remain, (CHAR*)(buf + sent) };
                int    r = WSASend((SOCKET)this->owner._fd, &wb, 1, &done, flags, &ovl, nullptr);
                if (r == 0) { // immediate
                    advance(done);
                    if (!full || sent >= total) {
                        finish();
                        return;
                    }
                    return;
                }
            } else {
                compact();
                if (bufs.empty()) {
                    finish();
                    return;
                }
                int r = WSASend((SOCKET)this->owner._fd, bufs.data(), (DWORD)bufs.size(), &done, flags, &ovl, nullptr);
                if (r == 0) { // immediate
                    if (done)
                        advance(done);
                    if (!full || sent >= total) {
                        finish();
                        return;
                    }
                    return;
                }
            }
            int e = WSAGetLastError();
            if (e == WSA_IO_PENDING) {
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
            if (!full) {
                // 部分语义：发起一次
                issue();
                if (!inflight && !finished)
                    finish();
                return;
            }
            // 全部语义：直到 total 完成
            while (!finished && sent < total && err == 0) {
                issue();
                if (inflight)
                    return;
            }
            if (!finished && (err < 0 || sent >= total))
                finish();
        }
        static void drive_cb(worknode* w)
        {
            auto* self = static_cast<send_awaiter*>(w);
            if (self->finished)
                return;
            DWORD tr = 0, fl = 0;
            if (WSAGetOverlappedResult((SOCKET)self->owner._fd, &self->ovl, &tr, FALSE, &fl)) {
                if (tr)
                    self->advance(tr);
                self->inflight = false;
                if (!self->full || self->sent >= self->total) {
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
    // API：部分/全部 + 单缓冲/向量
    send_awaiter send(const void* buf, size_t len) { return send_awaiter(*this, buf, len, /*full*/ false); }
    send_awaiter send_all(const void* buf, size_t len) { return send_awaiter(*this, buf, len, /*full*/ true); }
    send_awaiter sendv(const struct iovec* iov, int iovcnt) { return send_awaiter(*this, iov, iovcnt, /*full*/ false); }
    send_awaiter send_all(const struct iovec* iov, int iovcnt)
    {
        return send_awaiter(*this, iov, iovcnt, /*full*/ true);
    }

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
    workqueue<lock>&  _exec;
    Reactor<lock>*    _reactor { nullptr };
    int               _fd { -1 };
    bool              _rx_eof { false };
    bool              _tx_shutdown { false };
    lock              _io_serial_lock; // unify naming with linux side
    serial_queue      _send_q;         // serialized send operations
    serial_queue      _recv_q;         // serialized recv operations
    callback_wq<lock> _cbq { _exec };  // per-socket ordered callback dispatcher
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
