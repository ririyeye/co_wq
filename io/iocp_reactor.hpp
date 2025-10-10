// iocp_reactor.hpp - Windows reactor combining IOCP completions with wepoll readiness
#pragma once
#ifdef _WIN32
#include "io_waiter.hpp"
#include "wepoll/wepoll.h"
#include "workqueue.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifndef EPOLLET
#define EPOLLET 0u
#endif

struct iocp_ovl : OVERLAPPED {
    co_wq::net::io_waiter_base* waiter { nullptr };
    std::atomic<std::uint32_t>  generation { 0 };
    std::atomic<bool>           cancelled { false };
};

namespace co_wq::net {

static bool is_pointer_plausible(const void* ptr) noexcept
{
    if (!ptr)
        return false;
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION mbi {};
    SIZE_T                   queried = VirtualQuery(ptr, &mbi, sizeof(mbi));
    if (queried == 0)
        return false;
    if (!(mbi.State & MEM_COMMIT))
        return false;
    DWORD protect = mbi.Protect;
    if (protect & PAGE_NOACCESS)
        return false;
    if (protect & PAGE_GUARD)
        return false;
#else
    (void)ptr;
#endif
    return true;
}

namespace detail {
    inline HANDLE fd_to_handle(int fd) noexcept
    {
        return reinterpret_cast<HANDLE>(static_cast<intptr_t>(fd));
    }

    inline SOCKET fd_to_socket(int fd) noexcept
    {
        return static_cast<SOCKET>(static_cast<uintptr_t>(static_cast<intptr_t>(fd)));
    }

    inline bool is_socket_fd(int fd) noexcept
    {
        SOCKET s = fd_to_socket(fd);
        if (s == INVALID_SOCKET)
            return false;
        int opt    = 0;
        int optlen = static_cast<int>(sizeof(opt));
        int rc     = ::getsockopt(s, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&opt), &optlen);
        if (rc == 0)
            return true;
        int werr = WSAGetLastError();
        if (werr == WSAENOTSOCK)
            return false;
        return false;
    }
} // namespace detail

/**
 * @brief Windows 平台下的统一 reactor：
 *  - 使用 IOCP 完成异步 I/O 事件 (文件/USB 等 Overlapped 操作)。
 *  - 使用 wepoll 提供 epoll 风格的套接字就绪事件，从而与 Linux 版本高度复用等待逻辑。
 */
template <lockable lock> class iocp_reactor {
public:
    explicit iocp_reactor(workqueue<lock>& exec) : _exec(exec)
    {
        ensure_wsa();
        _iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
        if (!_iocp)
            throw std::runtime_error("CreateIoCompletionPort failed");
        _epoll = epoll_create1(0);
        if (!_epoll) {
            CloseHandle(_iocp);
            throw std::runtime_error("epoll_create1 failed");
        }
        _running.store(true, std::memory_order_relaxed);
        _thr = std::thread([this] { this->run_loop(); });
    }
    iocp_reactor(const iocp_reactor&)            = delete;
    iocp_reactor& operator=(const iocp_reactor&) = delete;
    ~iocp_reactor()
    {
        _running.store(false, std::memory_order_relaxed);
        if (_iocp)
            PostQueuedCompletionStatus(_iocp, 0, 0, nullptr);
        if (_thr.joinable())
            _thr.join();
        if (_epoll)
            epoll_close(_epoll);
        if (_iocp)
            CloseHandle(_iocp);
    }

    void add_fd(int fd)
    {
        HANDLE h = detail::fd_to_handle(fd);
        CreateIoCompletionPort(h, _iocp, static_cast<ULONG_PTR>(fd), 0);
        if (!detail::is_socket_fd(fd))
            return;
        std::scoped_lock<lock> lk(_lk);
        if (_fds.find(fd) == _fds.end())
            _fds.emplace(fd, fd_state {});
    }

    void add_waiter(int fd, uint32_t evmask, io_waiter_base* waiter) { register_waiter(fd, evmask, waiter, false); }

    void add_waiter_custom(int fd, uint32_t evmask, io_waiter_base* waiter)
    {
        register_waiter(fd, evmask, waiter, true);
    }

    void add_waiter_with_timeout(int fd, uint32_t evmask, io_waiter_base* waiter, std::chrono::milliseconds timeout)
    {
        (void)timeout;
        register_waiter(fd, evmask, waiter, false);
    }

    void remove_fd(int fd)
    {
        std::vector<waiter_item> to_resume;
        bool                     had_socket = false;
        {
            std::scoped_lock<lock> lk(_lk);
            auto                   it = _fds.find(fd);
            if (it != _fds.end()) {
                had_socket = true;
                to_resume.swap(it->second.waiters);
                _fds.erase(it);
            }
        }
        if (had_socket)
            epoll_ctl(_epoll, EPOLL_CTL_DEL, detail::fd_to_socket(fd), nullptr);
        for (auto& wi : to_resume)
            post_via_route(_exec, *wi.waiter);
    }

    void post_completion(io_waiter_base* w) { post_via_route(_exec, *w); }

private:
    struct waiter_item {
        uint32_t        mask { 0 };
        io_waiter_base* waiter { nullptr };
    };
    struct fd_state {
        std::vector<waiter_item> waiters;
        uint32_t                 interest { 0 };
        bool                     registered { false };
    };

    void register_waiter(int fd, uint32_t evmask, io_waiter_base* waiter, bool preserve_func)
    {
        if (!waiter)
            return;
        if (!detail::is_socket_fd(fd)) {
            post_via_route(_exec, *waiter);
            return;
        }
        std::scoped_lock<lock> lk(_lk);
        auto&                  st = _fds[fd];
        if (!preserve_func)
            waiter->func = &io_waiter_base::resume_cb;
        INIT_LIST_HEAD(&waiter->ws_node);
        st.waiters.push_back(waiter_item { evmask, waiter });
        update_fd_interest_unlocked(fd, st);
    }

    void update_fd_interest_unlocked(int fd, fd_state& st)
    {
        uint32_t new_interest = 0;
        for (auto& wi : st.waiters)
            new_interest |= wi.mask;
        if (new_interest == st.interest)
            return;
        st.interest = new_interest;
        SOCKET s    = detail::fd_to_socket(fd);
        if (st.interest == 0) {
            if (st.registered)
                epoll_ctl(_epoll, EPOLL_CTL_DEL, s, nullptr);
            st.registered = false;
            return;
        }
        epoll_event ev {};
        ev.data.fd = fd;
        ev.events  = st.interest | EPOLLERR | EPOLLHUP;
        int rc     = epoll_ctl(_epoll, st.registered ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, s, &ev);
        if (rc == 0) {
            st.registered = true;
            return;
        }
        if (errno == ENOENT) {
            if (epoll_ctl(_epoll, EPOLL_CTL_ADD, s, &ev) == 0)
                st.registered = true;
        } else if (errno == EEXIST) {
            st.registered = true;
        }
    }

    void dispatch_ready(epoll_event* evs, int count)
    {
        for (int i = 0; i < count; ++i) {
            int                      fd    = evs[i].data.fd;
            uint32_t                 flags = evs[i].events;
            std::vector<waiter_item> to_resume;
            {
                std::scoped_lock<lock> lk(_lk);
                auto                   it = _fds.find(fd);
                if (it != _fds.end()) {
                    auto& st      = it->second;
                    auto& vec     = st.waiters;
                    auto  new_end = std::remove_if(vec.begin(), vec.end(), [&](waiter_item& wi) {
                        if ((flags & wi.mask) || (flags & (EPOLLERR | EPOLLHUP))) {
                            to_resume.push_back(wi);
                            return true;
                        }
                        return false;
                    });
                    vec.erase(new_end, vec.end());
                    update_fd_interest_unlocked(fd, st);
                }
            }
            for (auto& wi : to_resume)
                post_via_route(_exec, *wi.waiter);
        }
    }

    bool process_completions(DWORD timeout_ms)
    {
        bool         processed = false;
        DWORD        bytes     = 0;
        ULONG_PTR    key       = 0;
        LPOVERLAPPED povl      = nullptr;
        while (_running.load(std::memory_order_relaxed)) {
            BOOL ok = GetQueuedCompletionStatus(_iocp, &bytes, &key, &povl, timeout_ms);
            if (!ok && povl == nullptr)
                break;
            timeout_ms = 0; // drain remaining completions without blocking
            if (!povl) {
                if (!_running.load(std::memory_order_relaxed))
                    break;
                continue;
            }
            processed = true;
            handle_completion(povl, bytes, key);
        }
        return processed;
    }

    void handle_completion(LPOVERLAPPED povl, DWORD bytes, ULONG_PTR key)
    {
        auto* ovl        = reinterpret_cast<iocp_ovl*>(povl);
        auto  generation = ovl->generation.load(std::memory_order_acquire);
        auto* waiter     = ovl->waiter;
        if (!waiter) {
            if (ovl->cancelled.load(std::memory_order_acquire)) {
                CO_WQ_CBQ_TRACE("[iocp] info: completion after cancellation povl=%p bytes=%lu key=%p gen=%lu\n",
                                static_cast<void*>(povl),
                                static_cast<unsigned long>(bytes),
                                reinterpret_cast<void*>(key),
                                static_cast<unsigned long>(generation));
                return;
            }
            CO_WQ_CBQ_WARN("[iocp] warning: completion without waiter povl=%p bytes=%lu key=%p gen=%lu\n",
                           static_cast<void*>(povl),
                           static_cast<unsigned long>(bytes),
                           reinterpret_cast<void*>(key),
                           static_cast<unsigned long>(generation));
            return;
        }

        if (!is_pointer_plausible(waiter)) {
            CO_WQ_CBQ_WARN(
                "[iocp] warning: completion with invalid waiter pointer waiter=%p povl=%p bytes=%lu key=%p gen=%lu\n",
                static_cast<void*>(waiter),
                static_cast<void*>(povl),
                static_cast<unsigned long>(bytes),
                reinterpret_cast<void*>(key),
                static_cast<unsigned long>(generation));
            ovl->waiter = nullptr;
            ovl->cancelled.store(true, std::memory_order_release);
            return;
        }

        io_waiter_base::route_guard_ptr guard            = waiter->load_route_guard();
        long                            guard_use        = io_waiter_base::route_guard_use_count(guard);
        bool                            magic_ok         = waiter->has_valid_magic();
        bool                            already_enqueued = waiter->callback_enqueued.load(std::memory_order_acquire);
        void*                           func_ptr         = reinterpret_cast<void*>(waiter->func);
        if (!magic_ok) {
            CO_WQ_CBQ_WARN(
                "[iocp] warning: waiter magic invalid waiter=%p func=%p guard_use=%ld bytes=%lu key=%p gen=%lu\n",
                static_cast<void*>(waiter),
                func_ptr,
                static_cast<long>(guard_use),
                static_cast<unsigned long>(bytes),
                reinterpret_cast<void*>(key),
                static_cast<unsigned long>(generation));
            waiter->exchange_route_guard();
            return;
        }
        if (!waiter->func) {
            CO_WQ_CBQ_WARN("[iocp] warning: waiter func null waiter=%p guard_use=%ld bytes=%lu key=%p\n",
                           static_cast<void*>(waiter),
                           static_cast<long>(guard_use),
                           static_cast<unsigned long>(bytes),
                           reinterpret_cast<void*>(key));
            return;
        }
        if (already_enqueued) {
            CO_WQ_CBQ_TRACE("[iocp] info: skip repost waiter=%p func=%p guard_use=%ld bytes=%lu key=%p\n",
                            static_cast<void*>(waiter),
                            func_ptr,
                            static_cast<long>(guard_use),
                            static_cast<unsigned long>(bytes),
                            reinterpret_cast<void*>(key));
            return;
        }
        CO_WQ_CBQ_TRACE("[iocp] dispatch waiter=%p func=%p guard_use=%ld bytes=%lu key=%p\n",
                        static_cast<void*>(waiter),
                        func_ptr,
                        static_cast<long>(guard_use),
                        static_cast<unsigned long>(bytes),
                        reinterpret_cast<void*>(key));
        post_via_route(_exec, *waiter);
    }

    void run_loop()
    {
        constexpr int            MAX_EVENTS = 32;
        std::vector<epoll_event> evs(MAX_EVENTS);
        while (_running.load(std::memory_order_relaxed)) {
            process_completions(0);
            int n = epoll_wait(_epoll, evs.data(), MAX_EVENTS, 10);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                CO_WQ_CBQ_WARN("[iocp] warning: epoll_wait failed errno=%d\n", errno);
                break;
            }
            if (n > 0)
                dispatch_ready(evs.data(), n);
            if (!_running.load(std::memory_order_relaxed))
                break;
            if (n == 0)
                process_completions(10);
        }
        process_completions(0);
    }

    static void ensure_wsa()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            WSADATA wsa {};
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
                throw std::runtime_error("WSAStartup failed");
        });
    }

    HANDLE                            _iocp { nullptr };
    HANDLE                            _epoll { nullptr };
    std::atomic_bool                  _running { false };
    std::thread                       _thr;
    workqueue<lock>&                  _exec;
    lock                              _lk;
    std::unordered_map<int, fd_state> _fds;
};

} // namespace co_wq::net
#endif // _WIN32
