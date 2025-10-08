/**
 * @file tcp_listener.hpp
 * @brief 基于 `stream_listener_base` 的 TCP 监听器封装，提供协程化 accept。
 */
#pragma once

#include "epoll_reactor.hpp" // 默认 reactor
#include "fd_base.hpp"
#include "stream_listener_base.hpp"
#include "tcp_socket.hpp"
#include "worker.hpp"
#include <cstring>
#include <string>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace co_wq::net {

/**
 * @brief 统一的 accept 致命错误返回值，跨平台保持一致。
 */
inline constexpr int k_accept_fatal = -2; // fatal error permanent for this try

/**
 * @brief TCP 监听器，提供地址绑定、监听与协程化 `accept` 能力。
 *
 * 典型流程为：
 * 1. 构造监听器并传入系统工作队列与 reactor；
 * 2. 调用 `bind_listen` 绑定地址并开始监听；
 * 3. `co_await listener.accept()` 获取新连接 fd，然后交给 `tcp_socket` 继续处理。
 */
template <lockable lock, template <class> class Reactor = epoll_reactor>
class tcp_listener : public detail::stream_listener_base<tcp_listener<lock, Reactor>, lock, Reactor> {
    using base = detail::stream_listener_base<tcp_listener<lock, Reactor>, lock, Reactor>;

public:
    /**
     * @brief 创建一个 TCP 监听器并立即生成底层非阻塞 socket。
     *
     * @param exec   绑定的系统工作队列。
     * @param reactor 事件反应器实例。
     * @param family 地址族，默认为 `AF_INET`，可选 `AF_INET6`。
     */
    explicit tcp_listener(workqueue<lock>& exec, Reactor<lock>& reactor, int family = AF_INET)
        : base(exec, reactor, family, SOCK_STREAM)
    {
    }

    /**
     * @brief 绑定地址并开始监听。
     *
     * @param host       监听地址。IPv4 下传入空串/`0.0.0.0` 表示 `INADDR_ANY`；IPv6 下可传
     *                   入如 `[::1]` 的文本形式，函数会自动去除方括号。
     * @param port       监听端口。
     * @param backlog    `listen` 的 backlog 参数，决定半连接队列大小。
     * @param dual_stack 当监听 IPv6 时是否允许双栈。true 表示允许同时接受 IPv4/IPv6。
     *
     * @throws std::runtime_error 绑定或监听失败时抛出。
     */
    void bind_listen(const std::string& host, uint16_t port, int backlog = 128, bool dual_stack = false)
    {
        os::fd_t fd  = this->native_handle();
        int      opt = 1;
        os::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        if (this->family() == AF_INET6) {
            std::string view = host;
            if (!view.empty() && view.front() == '[' && view.back() == ']')
                view = view.substr(1, view.size() - 2);

            sockaddr_in6 addr6 {};
            addr6.sin6_family = AF_INET6;
            addr6.sin6_port   = htons(port);
            if (view.empty() || view == "::" || view == "*")
                std::memset(&addr6.sin6_addr, 0, sizeof(addr6.sin6_addr));
            else if (inet_pton_wrapper(AF_INET6, view, &addr6.sin6_addr) <= 0)
                throw std::runtime_error("inet_pton failed");

            int v6only = dual_stack ? 0 : 1;
            os::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

            if (os::bind(fd, reinterpret_cast<sockaddr*>(&addr6), sizeof(addr6)) < 0)
                throw std::runtime_error("bind failed");
        } else {
            sockaddr_in addr {};
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(port);
            if (host.empty() || host == "0.0.0.0" || host == "*")
                addr.sin_addr.s_addr = INADDR_ANY;
            else if (inet_pton_wrapper(AF_INET, host, &addr.sin_addr) <= 0)
                throw std::runtime_error("inet_pton failed");
            if (os::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
                throw std::runtime_error("bind failed");
        }

        if (os::listen(fd, backlog) < 0)
            throw std::runtime_error("listen failed");
    }
    using base::accept;

private:
    static int inet_pton_wrapper(int family, const std::string& text, void* dst)
    {
#if defined(_WIN32)
        return ::InetPtonA(family, text.c_str(), dst);
#else
        return ::inet_pton(family, text.c_str(), dst);
#endif
    }
};

/**
 * @brief 异步等待一个新的 TCP 连接。
 *
 * @param lst 监听器实例。
 * @return 成功时返回新连接 fd；若遇到致命错误则返回 `k_accept_fatal`。
 */
template <lockable lock, template <class> class Reactor = epoll_reactor>
inline Task<int, Work_Promise<lock, int>> async_accept(tcp_listener<lock, Reactor>& lst)
{
    int fd = co_await lst.accept();
    co_return fd;
}

// 辅助: 若成功返回已创建的 tcp_socket，否则返回一个已关闭的占位 socket
/**
 * @brief 接受连接并自动封装为 `tcp_socket`。
 *
 * @param fwq  fd 工作队列，用于创建/接管 `tcp_socket` 对象。
 * @param lst 监听器实例。
 * @return 成功返回已就绪的 socket；若遇到致命错误，则返回一个 `close()` 后的占位实例。
 */
template <lockable lock, template <class> class Reactor = epoll_reactor>
inline Task<tcp_socket<lock, Reactor>, Work_Promise<lock, tcp_socket<lock, Reactor>>>
async_accept_socket(fd_workqueue<lock, Reactor>& fwq, tcp_listener<lock, Reactor>& lst)
{
    int fd = co_await lst.accept();
    if (fd < 0) { // -2 fatal => 返回一个已关闭 socket 占位
        auto tmp = fwq.make_tcp_socket();
        tmp.close();
        co_return std::move(tmp);
    }
    co_return fwq.adopt_tcp_socket(fd);
}

} // namespace co_wq::net
