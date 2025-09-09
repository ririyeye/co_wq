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

/**
 * @brief 基于 epoll 的反应器 (Reactor)，管理 fd 上挂起的 IO waiter。
 *
 * 特性:
 *  - 使用独立线程 run_loop 调用 epoll_wait。
 *  - 所有事件统一投递回外部提供的 workqueue (执行协程恢复)。
 *  - 采用 EPOLLET (边缘触发) + 聚合 interest mask；waiter 被触发后即从列表移除。
 *  - remove_fd 会把仍未唤醒的 waiter 全部投递回执行队列，避免悬挂。
 *
 * 线程安全: 通过模板 lock 对 _fds 访问上锁；对外 add_fd/add_waiter/remove_fd 可并发调用。
 */
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
    /**
     * @brief 注册 fd 进入内部表，但不立即添加 epoll interest（需后续 add_waiter）。
     */
    void add_fd(int fd)
    {
        std::scoped_lock<lock> lk(_lk);
        if (_fds.find(fd) == _fds.end())
            _fds.emplace(fd, fd_state {});
    }
    /**
     * @brief 为指定 fd 添加一个等待给定事件掩码的 waiter。
     * @param fd 文件描述符（必须先 add_fd）。
     * @param evmask EPOLLIN / EPOLLOUT 等组合。
     * @param waiter 挂起协程的等待节点；触发后会投递到 workqueue。
     */
    void add_waiter(int fd, uint32_t evmask, io_waiter_base* waiter)
    {
        register_waiter(fd, evmask, waiter, /*preserve_func=*/false);
    }
    /**
     * @brief 自定义回调版本：不强制重置 waiter->func，允许调用者提前设置 (例如内部多次驱动状态机)。
     */
    void add_waiter_custom(int fd, uint32_t evmask, io_waiter_base* waiter)
    {
        register_waiter(fd, evmask, waiter, /*preserve_func=*/true);
    }
    /**
     * @brief 移除 fd，并唤醒所有挂起 waiter（以便协程得到取消/错误处理机会）。
     */
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
            post_via_route(_exec, *wi.waiter); // 回调通过路由队列，保证顺序
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
    void register_waiter(int fd, uint32_t evmask, io_waiter_base* waiter, bool preserve_func)
    {
        std::scoped_lock<lock> lk(_lk);
        if (!preserve_func)
            waiter->func = &io_waiter_base::resume_cb;
        INIT_LIST_HEAD(&waiter->ws_node);
        auto& st = _fds[fd];
        st.waiters.push_back({ evmask, waiter });
        update_fd_interest_unlocked(fd, st);
    }
    /**
     * @brief 重新计算 fd 的事件关注并更新 epoll，若 mask 为 0 则 DEL。
     * @note 调用者需已持锁。
     */
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
    /**
     * @brief epoll 主循环：等待事件 -> 收集满足条件的 waiter -> 投递执行。
     * @note 使用 50ms 超时时间来保证析构快速退出（无显式唤醒机制）。
     */
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
                    post_via_route(_exec, *wi.waiter);
            }
        }
    }
    int                               _epfd { -1 };
    std::atomic_bool                  _running { false };
    std::thread                       _thr;  // 绑定的 IO 线程
    workqueue<lock>&                  _exec; // 主 workqueue，用于恢复协程
    lock                              _lk;   // 使用模板锁
    std::unordered_map<int, fd_state> _fds;
};

} // namespace co_wq::net
#endif
