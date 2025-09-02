/**
 * @file udp_socket.hpp
 * @brief Windows 下基于 Overlapped/IOCP 的 UDP 协程封装，提供 recv_from / send_to 以及可选 connect。
 *
 * 设计目标：
 *  - 与 linux 版本接口/语义尽量保持一致；
 *  - 使用 serial_queue 串行化 send / recv，避免多协程并发对同一 socket 操作造成乱序；
 *  - 采用 Overlapped + IOCP，无需显式循环占用线程；
 *  - close() 支持取消尚未开始的等待操作（未获取串行槽位的 awaiter）。
 */
#pragma once

#include "io_serial.hpp"
#include "io_waiter.hpp"
#include "iocp_reactor.hpp"
#include "worker.hpp"
#include <basetsd.h>
#include <stdexcept>
#include <winsock2.h>
#include <ws2tcpip.h>

#ifndef _SSIZE_T_DEFINED
using ssize_t = SSIZE_T;
#define _SSIZE_T_DEFINED
#endif

namespace co_wq::net {

template <lockable lock, template <class> class Reactor> class fd_workqueue; // fwd

template <lockable lock, template <class> class Reactor = epoll_reactor> class udp_socket {
public:
    udp_socket()                             = delete;
    udp_socket(const udp_socket&)            = delete;
    udp_socket& operator=(const udp_socket&) = delete;
    udp_socket(udp_socket&& o) noexcept : _exec(o._exec), _reactor(o._reactor), _sock(o._sock), _closed(o._closed)
    {
        o._sock   = INVALID_SOCKET;
        o._closed = true;
    }
    udp_socket& operator=(udp_socket&& o) noexcept
    {
        if (this != &o) {
            close();
            _exec     = o._exec;
            _reactor  = o._reactor;
            _sock     = o._sock;
            _closed   = o._closed;
            o._sock   = INVALID_SOCKET;
            o._closed = true;
        }
        return *this;
    }
    ~udp_socket() { close(); }
    /**
     * @brief 关闭 socket，并取消尚未进入 IO 的等待者。
     */
    void close()
    {
        if (_sock != INVALID_SOCKET) {
            _closed = true;
            if (_reactor)
                _reactor->remove_fd((int)_sock);
            // 收集串行队列中尚未获得槽位的等待者（不会影响正在执行的 overlapped，后者由 IOCP 自行回调）。
            list_head pending;
            INIT_LIST_HEAD(&pending);
            serial_collect_waiters(_io_serial_lock, { &_send_q, &_recv_q }, pending);
            serial_post_pending(_exec, pending); // resume 后检测 closed()
            ::closesocket(_sock);
            _sock = INVALID_SOCKET;
        }
    }
    int              native_handle() const { return (int)_sock; }
    workqueue<lock>& exec() { return _exec; }
    lock&            serial_lock() { return _io_serial_lock; }
    Reactor<lock>*   reactor() { return _reactor; }
    bool             closed() const { return _closed; }
    /**
     * @brief 可选的 connect（为 UDP 设定默认对端）。成功返回0，失败-1。
     */
    struct connect_awaiter : io_waiter_base {
        udp_socket& us;
        std::string host;
        uint16_t    port;
        int         ret { 0 };
        connect_awaiter(udp_socket& s, std::string h, uint16_t p) : us(s), host(std::move(h)), port(p) { }
        bool await_ready() const noexcept { return false; }
        // 注意: 参数名避免与基类 io_waiter_base::h 成员同名引发 MSVC C4458 警告
        void await_suspend(std::coroutine_handle<> coro)
        {
            this->h = coro;
            INIT_LIST_HEAD(&this->ws_node);
            sockaddr_in addr {};
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(port);
            if (InetPtonA(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
                ret = -1;
                us._exec.post(*this);
                return;
            }
            int r = ::connect(us._sock, (sockaddr*)&addr, sizeof(addr));
            if (r == 0) {
                ret = 0;
            } else {
                int e = WSAGetLastError();
                if (e != 0)
                    ret = -1;
            }
            us._exec.post(*this);
        }
        int await_resume() noexcept { return ret; }
    };
    connect_awaiter connect(const std::string& host, uint16_t port) { return connect_awaiter(*this, host, port); }

    /**
     * @brief UDP recv_from awaiter：一次性接收一个数据报；返回接收字节数，失败 -1。
     * @note 使用串行队列防止多个 recv_from 并发重入。
     */
    struct recvfrom_awaiter : serial_slot_awaiter<recvfrom_awaiter, udp_socket> {
        void*        buf;
        size_t       len;
        sockaddr_in* out_addr;
        socklen_t*   out_len;
        ssize_t      nrd { -1 }; // <0 表示尚未完成
        iocp_ovl     ovl;
        WSABUF       wbuf;
        DWORD        flags { 0 };
        bool         inflight { false };
        bool         finished { false };
        recvfrom_awaiter(udp_socket& s, void* b, size_t l, sockaddr_in* oa, socklen_t* ol)
            : serial_slot_awaiter<recvfrom_awaiter, udp_socket>(s, s._recv_q), buf(b), len(l), out_addr(oa), out_len(ol)
        {
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.waiter = this;
            wbuf.len   = (ULONG)l;
            wbuf.buf   = (CHAR*)b;
        }
        static void lock_acquired_cb(worknode* w)
        {
            auto* self = static_cast<recvfrom_awaiter*>(w);
            self->issue();
        }
        void issue()
        {
            int   fromlen = sizeof(sockaddr_in);
            DWORD recvd   = 0;
            this->func    = &recvfrom_awaiter::completion_cb;
            int r         = WSARecvFrom((SOCKET)this->owner._sock,
                                &wbuf,
                                1,
                                &recvd,
                                &flags,
                                (sockaddr*)out_addr,
                                (LPINT)&fromlen,
                                &ovl,
                                nullptr);
            if (r == 0) { // immediate
                nrd = (ssize_t)recvd;
                if (out_len)
                    *out_len = (socklen_t)fromlen;
                finish();
                return;
            }
            int e = WSAGetLastError();
            if (e == WSA_IO_PENDING) {
                inflight = true;
                return;
            }
            // error
            nrd = -1;
            finish();
        }
        static void completion_cb(worknode* w)
        {
            auto* self = static_cast<recvfrom_awaiter*>(w);
            if (self->finished)
                return;
            if (self->nrd < 0) {
                DWORD tr = 0, fl = 0;
                if (WSAGetOverlappedResult((SOCKET)self->owner._sock, &self->ovl, &tr, FALSE, &fl))
                    self->nrd = (ssize_t)tr;
                else
                    self->nrd = -1;
            }
            self->finish();
        }
        void finish()
        {
            if (finished)
                return;
            finished = true;
            serial_slot_awaiter<recvfrom_awaiter, udp_socket>::release(this);
            this->func = &io_waiter_base::resume_cb;
            if (this->h)
                this->h.resume();
        }
        bool    await_ready() noexcept { return false; }
        ssize_t await_resume() noexcept
        {
            if (nrd < 0 && this->owner.closed())
                return -1;
            return nrd;
        }
    };
    recvfrom_awaiter recv_from(void* buf, size_t len, sockaddr_in* addr, socklen_t* addrlen)
    {
        return recvfrom_awaiter(*this, buf, len, addr, addrlen);
    }

    /**
     * @brief UDP send_to awaiter：发送一个数据报；成功返回字节数，失败 -1。
     */
    struct sendto_awaiter : serial_slot_awaiter<sendto_awaiter, udp_socket> {
        const void*        buf;
        size_t             len;
        const sockaddr_in* dest;
        ssize_t            nsent { -1 };
        iocp_ovl           ovl;
        WSABUF             wbuf;
        bool               inflight { false };
        bool               finished { false };
        sendto_awaiter(udp_socket& s, const void* b, size_t l, const sockaddr_in* d)
            : serial_slot_awaiter<sendto_awaiter, udp_socket>(s, s._send_q), buf(b), len(l), dest(d)
        {
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.waiter = this;
            wbuf.len   = (ULONG)l;
            wbuf.buf   = (CHAR*)const_cast<void*>(b);
        }
        static void lock_acquired_cb(worknode* w)
        {
            auto* self = static_cast<sendto_awaiter*>(w);
            self->issue();
        }
        void issue()
        {
            DWORD sent = 0;
            this->func = &sendto_awaiter::completion_cb;
            int dlen   = sizeof(sockaddr_in);
            int r
                = WSASendTo((SOCKET)this->owner._sock, &wbuf, 1, &sent, 0, (const sockaddr*)dest, dlen, &ovl, nullptr);
            if (r == 0) { // immediate
                nsent = (ssize_t)sent;
                finish();
                return;
            }
            int e = WSAGetLastError();
            if (e == WSA_IO_PENDING) {
                inflight = true;
                return;
            }
            nsent = -1;
            finish();
        }
        static void completion_cb(worknode* w)
        {
            auto* self = static_cast<sendto_awaiter*>(w);
            if (self->finished)
                return;
            if (self->nsent < 0) {
                DWORD tr = 0, fl = 0;
                if (WSAGetOverlappedResult((SOCKET)self->owner._sock, &self->ovl, &tr, FALSE, &fl))
                    self->nsent = (ssize_t)tr;
                else
                    self->nsent = -1;
            }
            self->finish();
        }
        void finish()
        {
            if (finished)
                return;
            finished = true;
            serial_slot_awaiter<sendto_awaiter, udp_socket>::release(this);
            this->func = &io_waiter_base::resume_cb;
            if (this->h)
                this->h.resume();
        }
        bool    await_ready() noexcept { return false; }
        ssize_t await_resume() noexcept
        {
            if (nsent < 0 && this->owner.closed())
                return -1;
            return nsent;
        }
    };
    sendto_awaiter send_to(const void* buf, size_t len, const sockaddr_in& dest)
    {
        return sendto_awaiter(*this, buf, len, &dest);
    }

private:
    friend class fd_workqueue<lock, Reactor>;
    udp_socket(workqueue<lock>& e, Reactor<lock>& r) : _exec(e), _reactor(&r)
    {
        _sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (_sock == INVALID_SOCKET)
            throw std::runtime_error("udp socket failed");
        u_long m = 1;
        ioctlsocket(_sock, FIONBIO, &m);
        _reactor->add_fd((int)_sock);
    }
    workqueue<lock>& _exec;
    Reactor<lock>*   _reactor { nullptr };
    SOCKET           _sock { INVALID_SOCKET };
    lock             _io_serial_lock; // 串行锁
    serial_queue     _send_q;         // 发送串行队列
    serial_queue     _recv_q;         // 接收串行队列
    bool             _closed { false };
};

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
