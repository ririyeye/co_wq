// tcp_listener.hpp - TCP 异步 accept (Windows IOCP)
#pragma once

#ifdef _WIN32

#include "fd_base.hpp"
#include "io_waiter.hpp"
#include "iocp_reactor.hpp"
#include "tcp_socket.hpp"
#include <mswsock.h>
#include <stdexcept>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace co_wq::net {

// 统一 accept 语义常量（与 Linux 对齐）
inline constexpr int k_accept_fatal = -2;

template <lockable lock, template <class> class Reactor = iocp_reactor> class tcp_listener {
public:
    explicit tcp_listener(workqueue<lock>& exec, Reactor<lock>& reactor) : _exec(exec), _reactor(reactor)
    {
        _sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (_sock == INVALID_SOCKET)
            throw std::runtime_error("listener socket failed");
        u_long m = 1;
        ioctlsocket(_sock, FIONBIO, &m);
        _reactor.add_fd((int)_sock);
        load_acceptex();
    }
    ~tcp_listener() { close(); }
    void bind_listen(const std::string& host, uint16_t port, int backlog = 128)
    {
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        if (host.empty() || host == "0.0.0.0")
            addr.sin_addr.s_addr = INADDR_ANY;
        else if (InetPtonA(AF_INET, host.c_str(), &addr.sin_addr) <= 0)
            throw std::runtime_error("InetPton failed");
        BOOL opt = TRUE;
        setsockopt(_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
        if (::bind(_sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
            throw std::runtime_error("bind failed");
        if (::listen(_sock, backlog) == SOCKET_ERROR)
            throw std::runtime_error("listen failed");
    }
    void close()
    {
        if (_sock != INVALID_SOCKET) {
            _reactor.remove_fd((int)_sock);
            ::closesocket(_sock);
            _sock = INVALID_SOCKET;
        }
    }
    int native_handle() const { return (int)_sock; }
    struct accept_awaiter : io_waiter_base {
        tcp_listener& lst;
        int           newfd { -1 };
        iocp_ovl      ovl;
        char          buffer[(sizeof(sockaddr_in) + 16) * 2];
        DWORD         bytes = 0; // local addr + remote addr
        accept_awaiter(tcp_listener& l) : lst(l)
        {
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.waiter = this;
        }
        // fast path: 在 await_ready 里直接发起 AcceptEx，若立即完成则不挂起。
        bool await_ready() noexcept
        {
            // 准备异步结构
            INIT_LIST_HEAD(&this->ws_node);
            if (!lst._acceptex) {
                newfd = k_accept_fatal;
                return true;
            }
            SOCKET as = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (as == INVALID_SOCKET) {
                newfd = k_accept_fatal;
                return true;
            }
            u_long m = 1;
            ioctlsocket(as, FIONBIO, &m);
            // 发起 AcceptEx
            BOOL ok = lst._acceptex(lst._sock,
                                    as,
                                    buffer,
                                    0,
                                    sizeof(sockaddr_in) + 16,
                                    sizeof(sockaddr_in) + 16,
                                    &bytes,
                                    &ovl);
            if (ok) { // 立即完成 fast path
                newfd      = (int)as;
                _accepted  = newfd;
                update_context(newfd);
                return true; // 不挂起
            }
            int err = WSAGetLastError();
            if (err != ERROR_IO_PENDING) {
                ::closesocket(as);
                newfd = k_accept_fatal;
                return true; // 同样不挂起，直接返回错误
            }
            _accepted = (int)as; // pending, 将在 await_suspend 后由 IOCP 完成
            return false;        // 挂起等待完成
        }
        void await_suspend(std::coroutine_handle<> awaiting)
        {
            // 仅在 pending 情况下来到这里
            this->h    = awaiting;
            this->func = &io_waiter_base::resume_cb;
            // ws_node 已在 await_ready 初始化
        }
        int await_resume() noexcept
        {
            if (newfd == k_accept_fatal)
                return k_accept_fatal;
            if (newfd == -1) { // pending 完成路径
                DWORD transferred = 0;
                if (GetOverlappedResult((HANDLE)lst._reactor.iocp_handle(), &ovl, &transferred, FALSE)) {
                    newfd = _accepted;
                    update_context(newfd);
                } else {
                    newfd = k_accept_fatal;
                }
            }
            return newfd;
        }

    private:
        int  _accepted { -1 };
        bool _context_updated { false };
        void update_context(int fd)
        {
            if (_context_updated || fd < 0)
                return;
            setsockopt((SOCKET)fd,
                       SOL_SOCKET,
                       SO_UPDATE_ACCEPT_CONTEXT,
                       (char*)&lst._sock,
                       sizeof(lst._sock));
            _context_updated = true;
        }
    };
    accept_awaiter accept() { return accept_awaiter(*this); }

private:
    void load_acceptex()
    {
        if (_acceptex)
            return;
        GUID   guid  = WSAID_ACCEPTEX;
        DWORD  bytes = 0;
        SOCKET s     = _sock;
        if (WSAIoctl(s,
                     SIO_GET_EXTENSION_FUNCTION_POINTER,
                     &guid,
                     sizeof(guid),
                     &_acceptex,
                     sizeof(_acceptex),
                     &bytes,
                     NULL,
                     NULL)
            == SOCKET_ERROR)
            _acceptex = nullptr; // will fallback to error
    }
    workqueue<lock>& _exec;
    Reactor<lock>&   _reactor;
    SOCKET           _sock { INVALID_SOCKET };
    LPFN_ACCEPTEX    _acceptex { nullptr };
};

template <lockable lock, template <class> class Reactor = iocp_reactor>
inline Task<int, Work_Promise<lock, int>> async_accept(tcp_listener<lock, Reactor>& lst)
{
    int fd = co_await lst.accept();
    co_return fd;
}

template <lockable lock, template <class> class Reactor = iocp_reactor>
inline Task<tcp_socket<lock, Reactor>, Work_Promise<lock, tcp_socket<lock, Reactor>>>
async_accept_socket(fd_workqueue<lock, Reactor>& fwq, tcp_listener<lock, Reactor>& lst)
{
    int fd = co_await lst.accept();
    if (fd < 0) {
        auto tmp = fwq.make_tcp_socket();
        tmp.close();
        co_return std::move(tmp);
    }
    co_return fwq.adopt_tcp_socket(fd);
}

} // namespace co_wq::net

#endif // _WIN32
