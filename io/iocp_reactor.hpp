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
        _exec.post(*waiter);
    }
    void add_waiter_custom(int fd, uint32_t mask, io_waiter_base* waiter) { add_waiter(fd, mask, waiter); }
    void remove_fd(int fd)
    {
        // Nothing besides closing; active overlapped completions will deliver with failure.
        closesocket(_get_socket(fd));
    }
    // helper used by overlapped operations to enqueue completion
    void   post_completion(io_waiter_base* w) { _exec.post(*w); }
    HANDLE iocp_handle() const noexcept { return _iocp; }

private:
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
                    _exec.post(*ovl->waiter);
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
