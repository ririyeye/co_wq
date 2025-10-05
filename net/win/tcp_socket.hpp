/**
 * @file tcp_socket.hpp
 * @brief 基于 Windows Overlapped/IOCP 的 TCP 协程原语，实现与 Linux 版本尽量一致的接口。
 */
#pragma once

#ifdef _WIN32

#include "callback_wq.hpp"
#include "io_serial.hpp"
#include "io_waiter.hpp"
#include "iocp_reactor.hpp"
#include "reactor_default.hpp"
#include "stream_socket_base.hpp"
#include "worker.hpp"
#include <basetsd.h>
#include <mswsock.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>

#ifndef _SSIZE_T_DEFINED
using ssize_t = SSIZE_T;
#define _SSIZE_T_DEFINED
#endif

#ifndef IOVEC_DEFINED_CO_WQ
struct iovec {
    void*  iov_base;
    size_t iov_len;
};
#define IOVEC_DEFINED_CO_WQ 1
#endif

namespace co_wq::net {

template <lockable lock, template <class> class Reactor> class fd_workqueue;

/**
 * @brief Windows 平台 TCP 协程 socket。
 *
 * 使用 IOCP 驱动异步 IO，并通过串行队列保证 send/recv 的单通道顺序，接口与 Linux 版本尽量保持一致。
 *
 * @tparam lock 配合 `workqueue<lock>` 的锁类型。
 * @tparam Reactor Reactor 模板（默认 `CO_WQ_DEFAULT_REACTOR`）。
 */
template <lockable lock, template <class> class Reactor = CO_WQ_DEFAULT_REACTOR>
class tcp_socket : public detail::stream_socket_base<tcp_socket<lock, Reactor>, lock, Reactor> {
    using base = detail::stream_socket_base<tcp_socket<lock, Reactor>, lock, Reactor>;

public:
    tcp_socket()                                 = delete;
    tcp_socket(const tcp_socket&)                = delete;
    tcp_socket& operator=(const tcp_socket&)     = delete;
    tcp_socket(tcp_socket&&) noexcept            = default;
    tcp_socket& operator=(tcp_socket&&) noexcept = default;
    ~tcp_socket()                                = default;

    using base::callback_queue;
    using base::close;
    using base::exec;
    using base::mark_rx_eof;
    using base::mark_tx_shutdown;
    using base::native_handle;
    using base::reactor;
    using base::recv_queue;
    using base::send_queue;
    using base::serial_lock;
    using base::shutdown_tx;
    using base::socket_handle;
    using base::tx_shutdown;

    /**
     * @brief ConnectEx Awaiter，负责协程化的 TCP 建连。
     */
    struct connect_awaiter : io_waiter_base {
        tcp_socket&    sock;
        std::string    host;
        uint16_t       port;
        int            ret { -1 };
        iocp_ovl       ovl {};
        LPFN_CONNECTEX _connectex { nullptr };
        sockaddr_in    addr {};
        bool           issued { false };
        connect_awaiter(tcp_socket& s, std::string h, uint16_t p) : sock(s), host(std::move(h)), port(p)
        {
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.waiter       = this;
            this->route_ctx  = &sock.callback_queue();
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        bool load_connectex()
        {
            if (_connectex)
                return true;
            GUID  guid  = WSAID_CONNECTEX;
            DWORD bytes = 0;
            if (WSAIoctl(sock.socket_handle(),
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
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(port);
            if (InetPtonA(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
                ret = -1;
                return true;
            }
            if (!load_connectex()) {
                ret = -1;
                return true;
            }
            sockaddr_in local {};
            local.sin_family      = AF_INET;
            local.sin_addr.s_addr = INADDR_ANY;
            local.sin_port        = 0;
            ::bind(sock.socket_handle(), reinterpret_cast<sockaddr*>(&local), sizeof(local));
            this->func = &io_waiter_base::resume_cb;
            BOOL ok    = _connectex(sock.socket_handle(),
                                 reinterpret_cast<sockaddr*>(&addr),
                                 sizeof(addr),
                                 nullptr,
                                 0,
                                 nullptr,
                                 &ovl);
            issued     = true;
            if (ok) {
                setsockopt(sock.socket_handle(), SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);
                ret = 0;
                return true;
            }
            int err = WSAGetLastError();
            if (err != ERROR_IO_PENDING) {
                ret = -1;
                return true;
            }
            return false;
        }
        void await_suspend(std::coroutine_handle<> awaiting) { this->h = awaiting; }
        int  await_resume() noexcept
        {
            if (ret == 0)
                return 0;
            if (issued && ret != 0) {
                DWORD transferred = 0;
                DWORD flags       = 0;
                if (WSAGetOverlappedResult(sock.socket_handle(), &ovl, &transferred, FALSE, &flags)) {
                    setsockopt(sock.socket_handle(), SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);
                    ret = 0;
                } else {
                    ret = -1;
                }
            }
            return ret;
        }
    };

    /** @brief 创建 Connect Awaiter。 */
    connect_awaiter connect(const std::string& host, uint16_t port) { return connect_awaiter(*this, host, port); }

    /**
     * @brief 单次接收 Awaiter：读取任意字节即返回。
     */
    struct recv_awaiter : serial_slot_awaiter<recv_awaiter, tcp_socket> {
        void*    buf { nullptr };
        size_t   len { 0 };
        ssize_t  nread { -1 };
        iocp_ovl ovl {};
        bool     started { false };
        bool     inflight { false };
        bool     finished { false };
        bool     result_ready { false };
        recv_awaiter(tcp_socket& owner, void* b, size_t l)
            : serial_slot_awaiter<recv_awaiter, tcp_socket>(owner, owner.recv_queue()), buf(b), len(l)
        {
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.waiter       = this;
            this->route_ctx  = &owner.callback_queue();
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
            inflight     = false;
            finished     = false;
            result_ready = false;
            nread        = -1;
            WSABUF wbuf { static_cast<ULONG>(len), static_cast<CHAR*>(buf) };
            DWORD  flags = 0;
            DWORD  recvd = 0;
            ovl.waiter   = this;
            this->func   = &recv_awaiter::completion_cb;
            int r        = WSARecv(socket_handle(), &wbuf, 1, &recvd, &flags, &ovl, nullptr);
            if (r == 0) {
                nread        = static_cast<ssize_t>(recvd);
                result_ready = true;
                if (recvd == 0)
                    this->owner.mark_rx_eof();
                finish();
                return;
            }
            int err = WSAGetLastError();
            if (r == SOCKET_ERROR && err == WSA_IO_PENDING) {
                inflight = true;
                return;
            }
            nread        = -1;
            result_ready = true;
            finish();
        }
        static void completion_cb(worknode* w)
        {
            auto* self = static_cast<recv_awaiter*>(w);
            if (self->finished)
                return;
            DWORD transferred = 0;
            DWORD flags       = 0;
            if (WSAGetOverlappedResult(self->socket_handle(), &self->ovl, &transferred, FALSE, &flags)) {
                self->nread = static_cast<ssize_t>(transferred);
                if (transferred == 0)
                    self->owner.mark_rx_eof();
            } else {
                self->nread = -1;
            }
            self->result_ready = true;
            self->inflight     = false;
            self->finish();
        }
        void finish()
        {
            if (finished)
                return;
            finished   = true;
            ovl.waiter = nullptr;
            this->func = &io_waiter_base::resume_cb;
            serial_slot_awaiter<recv_awaiter, tcp_socket>::release(this);
            post_via_route(this->owner.exec(), *this);
        }
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> awaiting)
        {
            this->h    = awaiting;
            this->func = &recv_awaiter::lock_acquired_cb;
            INIT_LIST_HEAD(&this->ws_node);
            serial_acquire_or_enqueue(this->q, this->owner.serial_lock(), this->owner.exec(), *this);
        }
        ssize_t await_resume() noexcept
        {
            if (!result_ready) {
                DWORD transferred = 0;
                DWORD flags       = 0;
                if (WSAGetOverlappedResult(socket_handle(), &ovl, &transferred, FALSE, &flags)) {
                    nread = static_cast<ssize_t>(transferred);
                    if (transferred == 0)
                        this->owner.mark_rx_eof();
                } else {
                    nread = -1;
                }
                result_ready = true;
            }
            if (nread == 0)
                this->owner.mark_rx_eof();
            return nread;
        }

    private:
        SOCKET socket_handle() const { return this->owner.socket_handle(); }
    };

    recv_awaiter recv(void* buf, size_t len) { return recv_awaiter(*this, buf, len); }

    /**
     * @brief 完整接收 Awaiter：循环读取直到缓冲区写满或遇到 EOF。
     */
    struct recv_all_awaiter : serial_slot_awaiter<recv_all_awaiter, tcp_socket> {
        char*    buf { nullptr };
        size_t   len { 0 };
        size_t   recvd { 0 };
        ssize_t  err { 0 };
        iocp_ovl ovl {};
        bool     inflight { false };
        bool     finished { false };
        recv_all_awaiter(tcp_socket& owner, void* b, size_t l)
            : serial_slot_awaiter<recv_all_awaiter, tcp_socket>(owner, owner.recv_queue())
            , buf(static_cast<char*>(b))
            , len(l)
        {
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.waiter       = this;
            this->route_ctx  = &owner.callback_queue();
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        static void lock_acquired_cb(worknode* w)
        {
            auto* self = static_cast<recv_all_awaiter*>(w);
            self->drive();
        }
        void issue()
        {
            WSABUF wbuf { static_cast<ULONG>(len - recvd), buf + recvd };
            DWORD  flags = 0;
            DWORD  got   = 0;
            ovl.waiter   = this;
            this->func   = &recv_all_awaiter::drive_cb;
            int r        = WSARecv(socket_handle(), &wbuf, 1, &got, &flags, &ovl, nullptr);
            if (r == 0) {
                if (got == 0) {
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
            DWORD transferred = 0;
            DWORD flags       = 0;
            if (WSAGetOverlappedResult(self->socket_handle(), &self->ovl, &transferred, FALSE, &flags)) {
                if (transferred == 0) {
                    self->owner.mark_rx_eof();
                    self->finish();
                    return;
                }
                self->recvd += transferred;
                if (self->recvd >= self->len) {
                    self->finish();
                    return;
                }
                self->inflight = false;
                self->drive();
                return;
            }
            if (self->recvd == 0)
                self->err = -1;
            self->finish();
        }
        void finish()
        {
            if (finished)
                return;
            finished   = true;
            ovl.waiter = nullptr;
            serial_slot_awaiter<recv_all_awaiter, tcp_socket>::release(this);
            this->func = &io_waiter_base::resume_cb;
            post_via_route(this->owner.exec(), *this);
        }
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> awaiting)
        {
            this->h    = awaiting;
            this->func = &recv_all_awaiter::lock_acquired_cb;
            INIT_LIST_HEAD(&this->ws_node);
            serial_acquire_or_enqueue(this->q, this->owner.serial_lock(), this->owner.exec(), *this);
        }
        ssize_t await_resume() noexcept { return (err < 0 && recvd == 0) ? err : static_cast<ssize_t>(recvd); }

    private:
        SOCKET socket_handle() const { return this->owner.socket_handle(); }
    };

    recv_all_awaiter recv_all(void* buf, size_t len) { return recv_all_awaiter(*this, buf, len); }

    /**
     * @brief 发送 Awaiter：支持缓冲区及 iovec，按需多次投递 WSASend。
     */
    struct send_awaiter : serial_slot_awaiter<send_awaiter, tcp_socket> {
        bool                use_vec { false };
        bool                full { false };
        const char*         buf { nullptr };
        size_t              len { 0 };
        std::vector<WSABUF> bufs;
        size_t              total { 0 };
        size_t              sent { 0 };
        ssize_t             err { 0 };
        iocp_ovl            ovl {};
        bool                inflight { false };
        bool                finished { false };
        send_awaiter(tcp_socket& owner, const void* b, size_t l, bool full_mode)
            : serial_slot_awaiter<send_awaiter, tcp_socket>(owner, owner.send_queue())
            , use_vec(false)
            , full(full_mode)
            , buf(static_cast<const char*>(b))
            , len(l)
            , total(l)
        {
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.waiter       = this;
            this->route_ctx  = &owner.callback_queue();
            this->route_post = &callback_wq<lock>::post_adapter;
        }
        send_awaiter(tcp_socket& owner, const struct iovec* iov, int iovcnt, bool full_mode)
            : serial_slot_awaiter<send_awaiter, tcp_socket>(owner, owner.send_queue()), use_vec(true), full(full_mode)
        {
            bufs.reserve(static_cast<size_t>(iovcnt));
            for (int i = 0; i < iovcnt; ++i) {
                WSABUF b;
                b.len = static_cast<ULONG>(iov[i].iov_len);
                b.buf = static_cast<CHAR*>(iov[i].iov_base);
                bufs.push_back(b);
                total += b.len;
            }
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.waiter       = this;
            this->route_ctx  = &owner.callback_queue();
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
            DWORD sent_now = 0;
            DWORD flags    = 0;
            ovl.waiter     = this;
            this->func     = &send_awaiter::drive_cb;
            if (!use_vec) {
                size_t remain = len - sent;
                if (remain == 0) {
                    finish();
                    return;
                }
                WSABUF wb { static_cast<ULONG>(remain), const_cast<CHAR*>(buf + sent) };
                int    r = WSASend(socket_handle(), &wb, 1, &sent_now, flags, &ovl, nullptr);
                if (r == 0) {
                    advance(sent_now);
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
                int r = WSASend(socket_handle(),
                                bufs.data(),
                                static_cast<DWORD>(bufs.size()),
                                &sent_now,
                                flags,
                                &ovl,
                                nullptr);
                if (r == 0) {
                    if (sent_now)
                        advance(sent_now);
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
            if (e == WSAECONNRESET || e == WSAENOTCONN)
                this->owner.mark_tx_shutdown();
            if (sent == 0)
                err = -1;
            finish();
        }
        void drive()
        {
            if (!full) {
                issue();
                if (!inflight && !finished)
                    finish();
                return;
            }
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
            DWORD transferred = 0;
            DWORD flags       = 0;
            if (WSAGetOverlappedResult(self->socket_handle(), &self->ovl, &transferred, FALSE, &flags)) {
                if (transferred)
                    self->advance(transferred);
                self->inflight = false;
                if (!self->full || self->sent >= self->total) {
                    self->finish();
                    return;
                }
                self->drive();
                return;
            }
            if (self->sent == 0)
                self->err = -1;
            self->finish();
        }
        void finish()
        {
            if (finished)
                return;
            finished   = true;
            ovl.waiter = nullptr;
            serial_slot_awaiter<send_awaiter, tcp_socket>::release(this);
            this->func = &io_waiter_base::resume_cb;
            post_via_route(this->owner.exec(), *this);
        }
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> awaiting)
        {
            this->h    = awaiting;
            this->func = &send_awaiter::lock_acquired_cb;
            INIT_LIST_HEAD(&this->ws_node);
            serial_acquire_or_enqueue(this->q, this->owner.serial_lock(), this->owner.exec(), *this);
        }
        ssize_t await_resume() noexcept { return err < 0 ? err : static_cast<ssize_t>(sent); }

    private:
        SOCKET socket_handle() const { return this->owner.socket_handle(); }
    };

    send_awaiter send(const void* buf, size_t len) { return send_awaiter(*this, buf, len, false); }
    send_awaiter send_all(const void* buf, size_t len) { return send_awaiter(*this, buf, len, true); }
    send_awaiter sendv(const struct iovec* iov, int iovcnt) { return send_awaiter(*this, iov, iovcnt, false); }
    send_awaiter send_all(const struct iovec* iov, int iovcnt) { return send_awaiter(*this, iov, iovcnt, true); }

private:
    friend class fd_workqueue<lock, Reactor>;
    explicit tcp_socket(workqueue<lock>& exec, Reactor<lock>& reactor)
        : base(exec, reactor, AF_INET, SOCK_STREAM, IPPROTO_TCP)
    {
    }
    tcp_socket(int fd, workqueue<lock>& exec, Reactor<lock>& reactor) : base(static_cast<SOCKET>(fd), exec, reactor) { }
};

template <lockable lock>
inline Task<int, Work_Promise<lock, int>> async_connect(tcp_socket<lock>& s, const std::string host, uint16_t port)
{
    co_return co_await s.connect(host, port);
}

/** @brief 异步发送整个缓冲区。 */
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>> async_send_all(tcp_socket<lock>& s, const void* buf, size_t len)
{
    co_return co_await s.send_all(buf, len);
}

/** @brief 读取少量数据，返回实际读取的字节数。 */
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>> async_recv_some(tcp_socket<lock>& s, void* buf, size_t len)
{
    ssize_t n = co_await s.recv(buf, len);
    co_return n;
}

/** @brief 读取固定长度数据直至缓冲区填满。 */
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>> async_recv_all(tcp_socket<lock>& s, void* buf, size_t len)
{
    co_return co_await s.recv_all(buf, len);
}

/** @brief 发送多缓冲区数据并保证全量写出。 */
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>>
async_sendv_all(tcp_socket<lock>& s, const struct iovec* iov, int iovcnt)
{
    co_return co_await s.send_all(iov, iovcnt);
}

} // namespace co_wq::net

#endif // _WIN32
