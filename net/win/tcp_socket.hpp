// tcp_socket.hpp - TCP socket 协程原语
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

namespace co_wq::net {

template <lockable lock, template <class> class Reactor> class fd_workqueue; // fwd

// Overlapped-based tcp_socket (minimal: connect sync, recv/send async via IOCP)
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
    void close()
    {
        if (_fd != -1) {
            if (_reactor)
                _reactor->remove_fd(_fd);
            ::closesocket((SOCKET)_fd);
            _fd = -1;
        }
    }
    void shutdown_tx()
    {
        if (_fd != -1 && !_tx_shutdown) {
            ::shutdown((SOCKET)_fd, SD_SEND);
            _tx_shutdown = true;
        }
    }
    bool             tx_shutdown() const noexcept { return _tx_shutdown; }
    bool             rx_eof() const noexcept { return _rx_eof; }
    int              native_handle() const { return _fd; }
    workqueue<lock>& exec() { return _exec; }
    lock&            serial_lock() { return _dummy_serial_lock; }
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
    struct recv_awaiter : io_waiter_base {
        tcp_socket& sock;
        void*       buf;
        size_t      len;
        ssize_t     nread { -1 };
        iocp_ovl    ovl;
        recv_awaiter(tcp_socket& s, void* b, size_t l) : sock(s), buf(b), len(l)
        {
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.waiter = this;
        }
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> awaiting)
        {
            this->h    = awaiting;
            this->func = &io_waiter_base::resume_cb;
            INIT_LIST_HEAD(&this->ws_node);
            WSABUF wbuf { (ULONG)len, (CHAR*)buf };
            DWORD  flags = 0;
            DWORD  recvd = 0;
            int    r     = WSARecv((SOCKET)sock._fd, &wbuf, 1, &recvd, &flags, &ovl, nullptr);
            if (r == 0) { // completed immediately
                nread = (ssize_t)recvd;
                sock._reactor->post_completion(this);
                return;
            }
            int err = WSAGetLastError();
            if (r == SOCKET_ERROR && err == WSA_IO_PENDING) {
                return; // will complete
            }
            nread = -1;
            sock._reactor->post_completion(this);
        }
        ssize_t await_resume() noexcept
        {
            if (nread == -1) {
                // try get from overlapped
                DWORD transferred = 0;
                DWORD flags       = 0;
                if (WSAGetOverlappedResult((SOCKET)sock._fd, &ovl, &transferred, FALSE, &flags))
                    nread = (ssize_t)transferred;
            }
            if (nread == 0)
                sock.mark_rx_eof();
            return nread;
        }
    };
    recv_awaiter recv(void* buf, size_t len) { return recv_awaiter(*this, buf, len); }
    struct send_awaiter : io_waiter_base {
        tcp_socket& sock;
        const void* buf;
        size_t      len;
        ssize_t     nsent { -1 };
        iocp_ovl    ovl;
        send_awaiter(tcp_socket& s, const void* b, size_t l) : sock(s), buf(b), len(l)
        {
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.waiter = this;
        }
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> awaiting)
        {
            this->h    = awaiting;
            this->func = &io_waiter_base::resume_cb;
            INIT_LIST_HEAD(&this->ws_node);
            WSABUF wbuf { (ULONG)len, (CHAR*)const_cast<void*>(buf) };
            DWORD  sent  = 0;
            DWORD  flags = 0;
            int    r     = WSASend((SOCKET)sock._fd, &wbuf, 1, &sent, flags, &ovl, nullptr);
            if (r == 0) {
                nsent = (ssize_t)sent;
                sock._reactor->post_completion(this);
                return;
            }
            int err = WSAGetLastError();
            if (r == SOCKET_ERROR && err == WSA_IO_PENDING) {
                return;
            }
            if (err == WSAECONNRESET)
                sock._tx_shutdown = true;
            nsent = -1;
            sock._reactor->post_completion(this);
        }
        ssize_t await_resume() noexcept
        {
            if (nsent == -1) {
                DWORD transferred = 0;
                DWORD flags       = 0;
                if (WSAGetOverlappedResult((SOCKET)sock._fd, &ovl, &transferred, FALSE, &flags))
                    nsent = (ssize_t)transferred;
            }
            return nsent;
        }
    };
    send_awaiter send(const void* buf, size_t len) { return send_awaiter(*this, buf, len); }
    // send_all: reuse single overlapped send for echo test scope
    send_awaiter send_all(const void* buf, size_t len) { return send_awaiter(*this, buf, len); }

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
    lock             _dummy_serial_lock;
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

} // namespace co_wq::net
