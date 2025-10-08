// epoll_reactor.hpp - epoll/wepoll based reactor managing fd waiters
#pragma once

#include "../net/os_compat.hpp"
#include "io_waiter.hpp"
#include "workqueue.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include "wepoll/wepoll.h"
#else
#include <sys/epoll.h>
#include <unistd.h>
#endif

#ifndef EPOLLET
#define EPOLLET (1u << 31)
#endif

namespace co_wq::net {

using co_wq::lockable;

template <lockable lock> class epoll_reactor {
    using fd_t = os::fd_t;

#if defined(_WIN32)
    using epoll_handle = HANDLE;
    static constexpr epoll_handle invalid_epoll_handle() noexcept { return nullptr; }
    static SOCKET                 to_native_fd(fd_t fd) noexcept { return static_cast<SOCKET>(fd); }
    static void set_event_data(epoll_event& ev, fd_t fd) noexcept { ev.data.sock = static_cast<SOCKET>(fd); }
    static fd_t get_event_fd(const epoll_event& ev) noexcept { return static_cast<fd_t>(ev.data.sock); }
#else
    using epoll_handle = int;
    static constexpr epoll_handle invalid_epoll_handle() noexcept { return -1; }
    static int                    to_native_fd(fd_t fd) noexcept { return static_cast<int>(fd); }
    static void set_event_data(epoll_event& ev, fd_t fd) noexcept { ev.data.fd = static_cast<int>(fd); }
    static fd_t get_event_fd(const epoll_event& ev) noexcept { return static_cast<fd_t>(ev.data.fd); }
#endif

public:
    explicit epoll_reactor(workqueue<lock>& exec) : _exec(exec)
    {
#if defined(_WIN32)
        _epfd = ::epoll_create1(0);
#else
        _epfd = ::epoll_create1(EPOLL_CLOEXEC);
#endif
        if (_epfd == invalid_epoll_handle())
            throw std::runtime_error("epoll_create1 failed");
        _running.store(true, std::memory_order_relaxed);
        _thr = std::thread([this] { this->run_loop(); });
    }
    epoll_reactor(const epoll_reactor&)            = delete;
    epoll_reactor& operator=(const epoll_reactor&) = delete;
    ~epoll_reactor()
    {
        _running.store(false, std::memory_order_relaxed);
        if (_thr.joinable())
            _thr.join();
        if (_epfd != invalid_epoll_handle()) {
#if defined(_WIN32)
            ::epoll_close(_epfd);
#else
            ::close(_epfd);
#endif
        }
    }

    void add_fd(fd_t fd)
    {
        std::scoped_lock<lock> lk(_lk);
        if (_fds.find(fd) == _fds.end())
            _fds.emplace(fd, fd_state {});
    }

    void add_waiter(fd_t fd, uint32_t evmask, io_waiter_base* waiter) { register_waiter(fd, evmask, waiter, false); }

    void add_waiter_custom(fd_t fd, uint32_t evmask, io_waiter_base* waiter)
    {
        register_waiter(fd, evmask, waiter, true);
    }

    void remove_fd(fd_t fd)
    {
        std::vector<waiter_item> to_resume;
        {
            std::scoped_lock<lock> lk(_lk);
            auto                   it = _fds.find(fd);
            if (it != _fds.end()) {
                to_resume.swap(it->second.waiters);
                _fds.erase(it);
            }
        }
        if (_epfd != invalid_epoll_handle())
            ::epoll_ctl(_epfd, EPOLL_CTL_DEL, to_native_fd(fd), nullptr);
        for (auto& wi : to_resume)
            post_via_route(_exec, *wi.waiter);
    }

private:
    struct waiter_item {
        uint32_t        mask;
        io_waiter_base* waiter;
    };
    struct fd_state {
        std::vector<waiter_item> waiters;
        uint32_t                 interest { 0 };
    };

    void register_waiter(fd_t fd, uint32_t evmask, io_waiter_base* waiter, bool preserve_func)
    {
        std::scoped_lock<lock> lk(_lk);
        if (!preserve_func)
            waiter->func = &io_waiter_base::resume_cb;
        INIT_LIST_HEAD(&waiter->ws_node);
        auto& st = _fds[fd];
        st.waiters.push_back({ evmask, waiter });
        update_fd_interest_unlocked(fd, st);
    }

    void update_fd_interest_unlocked(fd_t fd, fd_state& st)
    {
        uint32_t new_interest = 0;
        for (auto& wi : st.waiters)
            new_interest |= wi.mask;
        if (new_interest == st.interest)
            return;
        st.interest = new_interest;
        if (st.interest == 0) {
            ::epoll_ctl(_epfd, EPOLL_CTL_DEL, to_native_fd(fd), nullptr);
            return;
        }
        epoll_event ev {};
        set_event_data(ev, fd);
        ev.events = st.interest | EPOLLERR | EPOLLHUP | EPOLLET;
        if (::epoll_ctl(_epfd, EPOLL_CTL_MOD, to_native_fd(fd), &ev) < 0) {
            if (errno == ENOENT)
                ::epoll_ctl(_epfd, EPOLL_CTL_ADD, to_native_fd(fd), &ev);
        }
    }

    void run_loop()
    {
        constexpr int            MAX_EVENTS = 32;
        std::vector<epoll_event> evs(MAX_EVENTS);
        while (_running.load(std::memory_order_relaxed)) {
            int n = ::epoll_wait(_epfd, evs.data(), MAX_EVENTS, 50);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (n == 0)
                continue;
            for (int i = 0; i < n; ++i) {
                fd_t                     fd    = get_event_fd(evs[i]);
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
    }

    epoll_handle                       _epfd { invalid_epoll_handle() };
    std::atomic_bool                   _running { false };
    std::thread                        _thr;
    workqueue<lock>&                   _exec;
    lock                               _lk;
    std::unordered_map<fd_t, fd_state> _fds;
};

} // namespace co_wq::net
