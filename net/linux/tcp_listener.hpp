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
#include <arpa/inet.h>
#include <string>

namespace co_wq::net {

/**
 * @brief 统一的 accept 致命错误返回值，跨平台保持一致。
 */
inline constexpr int k_accept_fatal = -2; // fatal error permanent for this try

/**
 * @brief TCP 监听器，提供 bind+listen 及异步 accept 能力。
 */
template <lockable lock, template <class> class Reactor = epoll_reactor>
class tcp_listener : public detail::stream_listener_base<tcp_listener<lock, Reactor>, lock, Reactor> {
    using base = detail::stream_listener_base<tcp_listener<lock, Reactor>, lock, Reactor>;

public:
    explicit tcp_listener(workqueue<lock>& exec, Reactor<lock>& reactor) : base(exec, reactor, AF_INET, SOCK_STREAM) { }

    /**
     * @brief 绑定并监听。
     * @param host 监听地址("0.0.0.0" 或 空 字符串 表示 INADDR_ANY)。
     * @param port 端口。
     * @param backlog listen backlog。
     */
    void bind_listen(const std::string& host, uint16_t port, int backlog = 128)
    {
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        if (host.empty() || host == "0.0.0.0")
            addr.sin_addr.s_addr = INADDR_ANY;
        else if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0)
            throw std::runtime_error("inet_pton failed");
        int opt = 1;
        ::setsockopt(this->native_handle(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (::bind(this->native_handle(), (sockaddr*)&addr, sizeof(addr)) < 0)
            throw std::runtime_error("bind failed");
        if (::listen(this->native_handle(), backlog) < 0)
            throw std::runtime_error("listen failed");
    }
    using base::accept;
};

/**
 * @brief 异步等待一个新的 TCP 连接。
 * @return 成功时返回新 fd，致命错误返回 `k_accept_fatal`。
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
 * @return 成功返回已就绪的 socket，否则返回一个已关闭的占位实例。
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
