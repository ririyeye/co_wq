/**
 * @file unix_listener.hpp
 * @brief Unix Domain socket 监听器封装，支持抽象命名空间。
 */
#pragma once

#include "epoll_reactor.hpp"
#include "stream_listener_base.hpp"
#include "unix_socket.hpp"
#include "worker.hpp"
#include <string>

#if defined(_WIN32)
#include <winsock2.h>
using mode_t = unsigned int;
#else
#include <cstddef>
#include <cstring>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace co_wq::net {

template <lockable lock, template <class> class Reactor> class fd_workqueue;

/** @brief 统一的 Unix Domain accept 致命错误返回值。 */
inline constexpr int k_accept_fatal = -2;

/**
 * @brief Unix Domain socket 监听器，自动处理路径/抽象命名空间。
 *
 * 支持以 `@` 前缀创建抽象命名空间套接字，并在关闭时自动清理物理路径，避免留下陈旧的
 * socket 文件。
 */
template <lockable lock, template <class> class Reactor = epoll_reactor>
class unix_listener : public detail::stream_listener_base<unix_listener<lock, Reactor>, lock, Reactor> {
    using base = detail::stream_listener_base<unix_listener<lock, Reactor>, lock, Reactor>;

public:
    static constexpr int k_accept_fatal = -2;
    /**
     * @brief 创建 Unix Domain 监听器。
     *
     * @param exec    系统工作队列。
     * @param reactor 事件反应器实例。
     */
    explicit unix_listener(workqueue<lock>& exec, Reactor<lock>& reactor)
        : base(exec, reactor, AF_UNIX, SOCK_STREAM) { }

    /**
     * @brief 绑定路径并开始监听。
     *
     * @param path             UDS 路径，若以 `@` 开头则视为抽象命名空间。
     * @param backlog          `listen` backlog。
     * @param unlink_existing  是否在绑定前删除已有同名文件。
     * @param mode             创建物理路径时使用的权限。
     *
     * @throws std::runtime_error 绑定或监听失败时抛出。
     */
    void bind_listen(const std::string& path, int backlog = 128, bool unlink_existing = true, mode_t mode = 0666)
    {
#if defined(_WIN32)
        (void)path;
        (void)backlog;
        (void)unlink_existing;
        (void)mode;
        throw std::runtime_error("unix_listener is not supported on Windows");
#else
        if (_bound)
            throw std::runtime_error("listener already bound");
        sockaddr_un addr {};
        addr.sun_family    = AF_UNIX;
        bool      abstract = !path.empty() && path[0] == '@';
        socklen_t slen     = 0;
        if (abstract) {
            size_t len = path.size();
            if (len <= 1 || len - 1 >= sizeof(addr.sun_path))
                throw std::runtime_error("unix path too long");
            addr.sun_path[0] = '\0';
            std::memcpy(addr.sun_path + 1, path.data() + 1, len - 1);
            slen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + len);
        } else {
            if (path.size() >= sizeof(addr.sun_path))
                throw std::runtime_error("unix path too long");
            std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
            addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
            slen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + std::strlen(addr.sun_path) + 1);
            if (unlink_existing)
                ::unlink(addr.sun_path);
        }
        if (::bind(this->native_handle(), reinterpret_cast<sockaddr*>(&addr), slen) < 0)
            throw std::runtime_error("bind failed");
        if (!abstract)
            ::chmod(addr.sun_path, mode);
        if (::listen(this->native_handle(), backlog) < 0)
            throw std::runtime_error("listen failed");
        _path     = path;
        _abstract = abstract;
        _bound    = true;
#endif
    }

    /**
     * @brief 关闭监听器并清理文件路径。
     *
     * 抽象命名空间不需要额外清理；普通路径会自动 `unlink`。
     */
    void close()
    {
        base::close();
#if !defined(_WIN32)
        if (_bound) {
            if (!_abstract && !_path.empty()) {
                ::unlink(_path.c_str());
                _path.clear();
            }
            _bound = false;
        }
#else
        _path.clear();
        _abstract = false;
        _bound    = false;
#endif
    }

    using base::accept;

private:
    std::string _path;
    bool        _abstract { false };
    bool        _bound { false };
};

/**
 * @brief 异步等待 Unix Domain 连接。
 *
 * @param lst 监听器实例。
 * @return 新连接 fd，或 `k_accept_fatal` 表示不可恢复错误。
 */
template <lockable lock, template <class> class Reactor = epoll_reactor>
inline Task<int, Work_Promise<lock, int>> async_accept(unix_listener<lock, Reactor>& lst)
{
    int fd = co_await lst.accept();
    co_return fd;
}

/**
 * @brief 接受连接并封装为 `unix_socket`。
 *
 * @param fwq fd 工作队列，用于生成 `unix_socket`。
 * @param lst 监听器实例。
 * @return 成功返回可用 socket；失败返回已关闭的占位对象。
 */
template <lockable lock, template <class> class Reactor = epoll_reactor>
inline Task<unix_socket<lock, Reactor>, Work_Promise<lock, unix_socket<lock, Reactor>>>
async_accept_socket(fd_workqueue<lock, Reactor>& fwq, unix_listener<lock, Reactor>& lst)
{
    int fd = co_await lst.accept();
    if (fd < 0) {
        auto tmp = fwq.make_unix_socket();
        tmp.close();
        co_return std::move(tmp);
    }
    co_return fwq.adopt_unix_socket(fd);
}

} // namespace co_wq::net
