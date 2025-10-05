// iocp_reactor.hpp - Windows IOCP-based reactor implementing epoll_reactor interface subset
#pragma once
#ifdef _WIN32
#include "io_waiter.hpp"
#include "workqueue.hpp"
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")

// Define epoll style flags (only symbolic for template compatibility)
#ifndef EPOLLIN
#define EPOLLIN  0x01
#define EPOLLOUT 0x02
#define EPOLLERR 0x04
#define EPOLLHUP 0x08
#define EPOLLET  0x00
#endif
// POSIX compatibility macros
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// simple overlapped header embedding waiter pointer
struct iocp_ovl : OVERLAPPED {
    co_wq::net::io_waiter_base* waiter { nullptr };
};

namespace co_wq::net {

// We reuse the name epoll_reactor so existing code works unchanged.
template <lockable lock> class epoll_reactor { // reuse name for templates
public:
    explicit epoll_reactor(workqueue<lock>& exec) : _exec(exec)
    {
        ensure_wsa();
        _iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
        if (!_iocp)
            throw std::runtime_error("CreateIoCompletionPort failed");
        _running.store(true, std::memory_order_relaxed);
        _thr = std::thread([this] { loop(); });
    }
    epoll_reactor(const epoll_reactor&)            = delete;
    epoll_reactor& operator=(const epoll_reactor&) = delete;
    ~epoll_reactor()
    {
        _running.store(false, std::memory_order_relaxed);
        if (_iocp)
            PostQueuedCompletionStatus(_iocp, 0, 0, nullptr);
        if (_thr.joinable())
            _thr.join();
        if (_iocp)
            CloseHandle(_iocp);
    }
    void add_fd(int fd) { CreateIoCompletionPort((HANDLE)_get_socket(fd), _iocp, (ULONG_PTR)fd, 0); }
    void add_waiter(int, uint32_t, io_waiter_base* waiter)
    { // immediate post (not readiness-based)
        post_via_route(_exec, *waiter);
    }
    void add_waiter_custom(int fd, uint32_t mask, io_waiter_base* waiter)
    {
        if (!waiter) {
            return;
        }
        if (mask == 0) {
            post_via_route(_exec, *waiter);
            return;
        }
        auto ctx = new custom_wait_context(*this, _get_socket(fd), mask, waiter);
        if (!ctx->start()) {
            delete ctx;
        }
    }
    void remove_fd(int fd)
    {
        // Nothing besides closing; active overlapped completions will deliver with failure.
        closesocket(_get_socket(fd));
    }
    // helper used by overlapped operations to enqueue completion
    void   post_completion(io_waiter_base* w) { post_via_route(_exec, *w); }
    HANDLE iocp_handle() const noexcept { return _iocp; }

private:
    struct custom_wait_context : io_waiter_base {
        epoll_reactor&  reactor;
        io_waiter_base* target { nullptr };
        SOCKET          sock { INVALID_SOCKET };
        bool            use_send { false };
        iocp_ovl        ovl {};
        char            dummy { 0 };

        custom_wait_context(epoll_reactor& r, SOCKET s, uint32_t mask, io_waiter_base* t)
            : reactor(r), target(t), sock(s)
        {
            this->route_ctx  = t->route_ctx;
            this->route_post = t->route_post;
            this->func       = &custom_wait_context::dispatch_cb;
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.waiter = this;
            if (mask & EPOLLIN)
                use_send = false;
            else if (mask & EPOLLOUT)
                use_send = true;
            else
                use_send = false;
        }

        bool start()
        {
            if (sock == INVALID_SOCKET || !target) {
                if (target)
                    post_via_route(reactor._exec, *target);
                return false;
            }
            DWORD  bytes = 0;
            DWORD  flags = 0;
            WSABUF buf;
            buf.buf = &dummy;
            if (!use_send) {
                buf.len = sizeof(dummy);
                flags   = MSG_PEEK;
            } else {
                buf.len = 0;
                flags   = 0;
            }
            int r;
            if (!use_send) {
                r = WSARecv(sock, &buf, 1, &bytes, &flags, &ovl, nullptr);
            } else {
                r = WSASend(sock, &buf, 1, &bytes, flags, &ovl, nullptr);
            }
            if (r == 0) {
                post_via_route(reactor._exec, *target);
                return false;
            }
            int err = WSAGetLastError();
            if (err != WSA_IO_PENDING) {
                post_via_route(reactor._exec, *target);
                return false;
            }
            return true; // waiting for IOCP callback
        }

        static void dispatch_cb(worknode* w)
        {
            auto* self = static_cast<custom_wait_context*>(w);
            if (self->target)
                post_via_route(self->reactor._exec, *self->target);
            delete self;
        }
    };

    static SOCKET _get_socket(int fd) { return (SOCKET)fd; }
    void          loop()
    {
        DWORD        bytes = 0;
        ULONG_PTR    key   = 0;
        LPOVERLAPPED povl  = nullptr;
        while (_running.load(std::memory_order_relaxed)) {
            BOOL ok = GetQueuedCompletionStatus(_iocp, &bytes, &key, &povl, 50); // 50ms timeout
            if (!ok && povl == nullptr) {                                        // timeout or wake
                continue;
            }
            if (povl) {
                auto* ovl = reinterpret_cast<iocp_ovl*>(povl);
                if (ovl->waiter) {
                    // stash byte count in waiter->ws_node.prev (hack) is messy; better store in derived awaiter
                    post_via_route(_exec, *ovl->waiter);
                }
            }
        }
    }
    static void ensure_wsa()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
                throw std::runtime_error("WSAStartup failed");
        });
    }
    HANDLE           _iocp { nullptr };
    std::atomic_bool _running { false };
    std::thread      _thr;
    workqueue<lock>& _exec;
};

} // namespace co_wq::net
#endif // _WIN32

// Provide explicit iocp_reactor class template (wrapper) for template-template compatibility.
#ifdef _WIN32
namespace co_wq::net {
template <lockable lock> class iocp_reactor : public epoll_reactor<lock> {
public:
    using epoll_reactor<lock>::epoll_reactor;
};
}
#endif // _WIN32
