/**
 * @file stream_listener_base.hpp
 * @brief 跨平台流式 listener 的 CRTP 基类，封装 socket 创建、关闭与异步 accept。
 */
#pragma once

#include "io_waiter.hpp"
#include "os_compat.hpp"
#include "workqueue.hpp"
#include <cerrno>

#if defined(_WIN32)
#include "../io/wepoll/wepoll.h"
#else
#include <sys/epoll.h>
#endif

namespace co_wq::net::detail {

/**
 * @brief 提供跨平台监听 socket 能力的 CRTP 基类。
 *
 * 该基类封装了监听 socket 的生命周期管理、与 reactor 的交互以及一个通用的 `accept`
 * Awaiter，供 TCP/Unix Domain 等上层监听器复用。派生类只需负责绑定地址、设置套接字
 * 选项等协议相关细节，即可获得统一的协程化接受逻辑。
 *
 * @tparam Derived 派生类类型，需要提供 `accept()` 的具体行为，可选定义 `k_accept_fatal` 常量
 *                以描述不可恢复错误时的返回值。
 * @tparam lock   工作队列锁类型，需满足 `lockable` 概念，通常为 `SpinLock` 或互斥量。
 * @tparam Reactor 反应器模板，负责事件注册与派发，例如 `epoll_reactor` 或 `iocp_reactor`。
 */
template <class Derived, lockable lock, template <class> class Reactor> class stream_listener_base {

public:
    /**
     * @brief 获取致命错误返回值。
     * @return 返回派生类自定义的常量，缺省为 -2，表示 accept 无法恢复。
     */
    /**
     * @brief 获取 awaiter 判定的致命错误返回值。
     *
     * 若派生类提供了 `Derived::k_accept_fatal` 常量，则优先返回该值；否则默认返回 -2。
     * 调用者可据此判断错误是否可重试。
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
    /**
     * @brief 关闭监听 socket 并解除 Reactor 注册。
     *
     * 当监听器正在被协程等待时，关闭操作会在下一次调度时唤醒 awaiter 并返回错误。
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
    /**
     * @brief 返回关联的 reactor。
     * @return 若监听器注册在 reactor 上，则返回其指针；否则返回 nullptr。
     */
    Reactor<lock>* reactor() { return _reactor; }

    /**
     * @brief 异步 accept 的 awaiter。
     *
     * awaiter 将在调用 `co_await` 时立即尝试非阻塞 `accept`，若未能完成则挂起当前协程并
     * 将监听 fd 注册到 reactor，等待下一次可读事件。协程恢复后会返回新连接的 fd，或在
     * 遇到不可恢复错误时返回 `accept_fatal()`。
     */
    struct accept_awaiter : io_waiter_base {
        /** @brief 对应的监听器引用，用于访问底层 fd 与 reactor。 */
        stream_listener_base& lst;
        /** @brief 本次 `accept` 的结果，成功时为新 fd，失败时为负值。 */
        int newfd { -1 };
        explicit accept_awaiter(stream_listener_base& l) : lst(l) { }
        /**
         * @brief 首次尝试接受连接。
         *
         * @retval true  `accept` 已经完成，协程可直接继续执行。
         * @retval false 需要等待 reactor 事件，协程将被挂起。
         */
        bool await_ready() noexcept
        {
            newfd = try_accept();
            return newfd >= 0 || newfd == accept_fatal();
        }
        /**
         * @brief 在 `accept` 尚未完成时挂起协程，并将 awaiter 注册到 reactor。
         *
         * @param resume_handle 协程恢复所用的句柄，由调度器在事件就绪后回调。
         */
        void await_suspend(std::coroutine_handle<> resume_handle)
        {
            this->h = resume_handle;
            INIT_LIST_HEAD(&this->ws_node);
            if (lst._reactor)
                lst._reactor->add_waiter(lst._fd, EPOLLIN, this);
        }
        /**
         * @brief 返回最终的 `accept` 结果。
         *
         * @return 成功时为新建立连接的 fd；若遇到可重试错误则再次尝试一次；若遇到致命
         *         错误则返回 `accept_fatal()`。
         */
        int await_resume() noexcept
        {
            if (newfd >= 0 || newfd == accept_fatal())
                return newfd;
            return try_accept();
        }
        /**
         * @brief 执行一次非阻塞 `accept`。
         *
         * @retval >=0  新连接 fd。
         * @retval -1   当前没有待接受的连接，可在 reactor 事件后重试。
         * @retval accept_fatal() 发生不可恢复错误。
         */
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
