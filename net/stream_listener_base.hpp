/**
 * @file stream_listener_base.hpp
 * @brief 跨平台流式 listener 的 CRTP 基类，封装 socket 创建、关闭与异步 accept。
 */
#pragma once

#include "io_waiter.hpp"
#include "os_compat.hpp"
#include "workqueue.hpp"
#include <cerrno>
#include <stdexcept>

#if defined(_WIN32)
#include "../io/wepoll/wepoll.h"
#else
#include <sys/epoll.h>
#endif

namespace co_wq::net::detail {

/**
 * @brief 提供通用监听 socket 能力的 CRTP 基类。
 *
 * @tparam Derived 派生类类型，需要提供 `accept()` 的具体行为，可选定义 `k_accept_fatal` 常量。
 * @tparam lock 工作队列锁类型，需满足 `lockable` 概念。
 * @tparam Reactor 反应器模板，负责事件注册与派发。
 */
template <class Derived, lockable lock, template <class> class Reactor> class stream_listener_base {

public:
    /**
     * @brief 获取致命错误返回值。
     * @return 返回派生类自定义的常量，缺省为 -2，表示 accept 无法恢复。
     */
    static constexpr int accept_fatal()
    {
        if constexpr (requires { Derived::k_accept_fatal; }) {
            return Derived::k_accept_fatal;
        } else {
            return -2;
        }
    }

    stream_listener_base(const stream_listener_base&)            = delete;
    stream_listener_base& operator=(const stream_listener_base&) = delete;

    stream_listener_base(stream_listener_base&& o) noexcept
        : _exec(o._exec), _reactor(o._reactor), _fd(o._fd), _family(o._family)
    {
        o._fd = os::invalid_fd();
    }
    stream_listener_base& operator=(stream_listener_base&& o) noexcept
    {
        if (this != &o) {
            close();
            _exec    = o._exec;
            _reactor = o._reactor;
            _fd      = o._fd;
            _family  = o._family;
            o._fd    = os::invalid_fd();
        }
        return *this;
    }

    /**
     * @brief 析构时自动关闭监听 socket。
     */
    ~stream_listener_base() { close(); }

    /**
     * @brief 关闭监听 socket 并移除 reactor 注册。
     */
    void close()
    {
        if (_fd != os::invalid_fd()) {
            if (_reactor)
                _reactor->remove_fd(_fd);
            os::close_fd(_fd);
            _fd = os::invalid_fd();
        }
    }

    /** @brief 获取底层监听 fd。 */
    os::fd_t native_handle() const { return _fd; }
    /** @brief 返回绑定的工作队列。 */
    workqueue<lock>& exec() { return _exec; }
    int              family() const noexcept { return _family; }
    /** @brief 返回关联的 reactor。 */
    Reactor<lock>* reactor() { return _reactor; }

    /**
     * @brief 异步 accept 的 awaiter。
     */
    struct accept_awaiter : io_waiter_base {
        stream_listener_base& lst;
        int                   newfd { -1 };
        explicit accept_awaiter(stream_listener_base& l) : lst(l) { }
        bool await_ready() noexcept
        {
            newfd = try_accept();
            return newfd >= 0 || newfd == accept_fatal();
        }
        void await_suspend(std::coroutine_handle<> resume_handle)
        {
            this->h = resume_handle;
            INIT_LIST_HEAD(&this->ws_node);
            if (lst._reactor)
                lst._reactor->add_waiter(lst._fd, EPOLLIN, this);
        }
        int await_resume() noexcept
        {
            if (newfd >= 0 || newfd == accept_fatal())
                return newfd;
            return try_accept();
        }
        int try_accept() noexcept
        {
            sockaddr_storage addr;
            socklen_t        alen = sizeof(addr);
            os::fd_t fd = os::accept(lst._fd, reinterpret_cast<sockaddr*>(&addr), &alen, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (fd != os::invalid_fd())
                return static_cast<int>(fd);
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return -1;
            return accept_fatal();
        }
    };

    /**
     * @brief 返回一次性的异步 accept awaiter。
     */
    accept_awaiter accept() { return accept_awaiter(*this); }

protected:
    /**
     * @brief 直接创建新的监听 socket。
     */
    stream_listener_base(workqueue<lock>& exec, Reactor<lock>& reactor, int domain, int type, int protocol = 0)
        : _exec(exec), _reactor(&reactor), _family(domain)
    {
        _fd = os::create_socket(domain, type | SOCK_CLOEXEC, protocol);
        if (_fd == os::invalid_fd())
            throw std::runtime_error("listener socket failed");
        set_non_block();
        _reactor->add_fd(_fd);
    }

    /**
     * @brief 将监听 fd 设置为非阻塞。
     */
    void set_non_block() { os::set_non_block(_fd); }

    workqueue<lock>& _exec;
    Reactor<lock>*   _reactor { nullptr };
    os::fd_t         _fd { os::invalid_fd() };
    int              _family { AF_INET };
};

} // namespace co_wq::net::detail
