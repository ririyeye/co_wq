// fd_wait.hpp - generic awaiters for fd readiness (read/write)
#pragma once
#ifdef __linux__
#include "epoll_reactor.hpp" // default reactor
#include "io_waiter.hpp"
#include <sys/epoll.h>

namespace co_wq::net {

enum class wait_event : uint32_t { read = EPOLLIN, write = EPOLLOUT };
inline wait_event operator|(wait_event a, wait_event b)
{
    return static_cast<wait_event>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

template <lockable lock, template <class> class Reactor = epoll_reactor> struct fd_wait_awaiter : io_waiter_base {
    workqueue<lock>& exec;
    int              fd;
    uint32_t         mask;
    bool             ready_immediate { false };
    fd_wait_awaiter(workqueue<lock>& e, int f, uint32_t m) : exec(e), fd(f), mask(m) { }
    bool await_ready() noexcept { return ready_immediate; }
    void await_suspend(std::coroutine_handle<> h)
    {
        this->h = h;
        INIT_LIST_HEAD(&this->ws_node);
        Reactor<lock>::instance(exec).add_waiter(fd, mask, this);
    }
    uint32_t await_resume() const noexcept { return mask; }
};

template <lockable lock, template <class> class Reactor = epoll_reactor>
inline fd_wait_awaiter<lock, Reactor> fd_wait_read(workqueue<lock>& exec, int fd)
{
    return fd_wait_awaiter<lock, Reactor>(exec, fd, EPOLLIN);
}
template <lockable lock, template <class> class Reactor = epoll_reactor>
inline fd_wait_awaiter<lock, Reactor> fd_wait_write(workqueue<lock>& exec, int fd)
{
    return fd_wait_awaiter<lock, Reactor>(exec, fd, EPOLLOUT);
}

} // namespace co_wq::net
#endif
