// epoll_reactor.hpp - epoll based reactor managing fd waiters
#pragma once
#ifdef __linux__
#include "io_waiter.hpp"
#include "workqueue.hpp"
#include <algorithm>
#include <atomic>
#include <errno.h>
#include <mutex>
#include <sys/epoll.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace co_wq::net {

template <lockable lock> class epoll_reactor {
public:
    explicit epoll_reactor(workqueue<lock>& exec) : _exec(exec)
    {
        _epfd = ::epoll_create1(EPOLL_CLOEXEC);
        if (_epfd < 0)
            throw std::runtime_error("epoll_create1 failed");
        _running.store(true, std::memory_order_relaxed);
        _thr = std::thread([this] { this->run_loop(); });
    }
    epoll_reactor(const epoll_reactor&)            = delete;
    epoll_reactor& operator=(const epoll_reactor&) = delete;
    ~epoll_reactor()
    {
        _running.store(false, std::memory_order_relaxed);
        if (_epfd >= 0) {
            ::epoll_ctl(_epfd, EPOLL_CTL_ADD, -1, nullptr);
        }
        if (_thr.joinable())
            _thr.join();
        if (_epfd >= 0)
            ::close(_epfd);
    }
    void add_fd(int fd)
    {
        std::scoped_lock<lock> lk(_lk);
        if (_fds.find(fd) == _fds.end())
            _fds.emplace(fd, fd_state {});
    }
    void add_waiter(int fd, uint32_t evmask, io_waiter_base* waiter)
    {
        std::scoped_lock<lock> lk(_lk);
        waiter->func = &io_waiter_base::resume_cb;
        INIT_LIST_HEAD(&waiter->ws_node);
        auto& st = _fds[fd];
        st.waiters.push_back({ evmask, waiter });
        update_fd_interest_unlocked(fd, st);
    }
    void remove_fd(int fd)
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
        ::epoll_ctl(_epfd, EPOLL_CTL_DEL, fd, nullptr);
        for (auto& wi : to_resume) {
            _exec.post(*wi.waiter); // 把 waiter 投递回主 workqueue
        }
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
    void update_fd_interest_unlocked(int fd, fd_state& st)
    {
        uint32_t new_interest = 0;
        for (auto& wi : st.waiters)
            new_interest |= wi.mask;
        if (new_interest == st.interest)
            return;
        st.interest = new_interest;
        if (st.interest == 0) {
            ::epoll_ctl(_epfd, EPOLL_CTL_DEL, fd, nullptr);
            return;
        }
        epoll_event ev {};
        ev.data.fd = fd;
        ev.events  = st.interest | EPOLLERR | EPOLLHUP | EPOLLET;
        if (::epoll_ctl(_epfd, EPOLL_CTL_MOD, fd, &ev) < 0) {
            if (errno == ENOENT)
                ::epoll_ctl(_epfd, EPOLL_CTL_ADD, fd, &ev);
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
                    _exec.post(*wi.waiter);
            }
        }
    }
    int                               _epfd { -1 };
    std::atomic_bool                  _running { false };
    std::thread                       _thr;  // 绑定的 IO 线程
    workqueue<lock>&                  _exec; // 主 workqueue，用于恢复协程
    lock                              _lk;   // 使用模板锁代替 std::mutex
    std::unordered_map<int, fd_state> _fds;
};

// 不再需要全局 singleton 的 init_reactor

} // namespace co_wq::net
#endif
