// tcp_listener.hpp - TCP 异步 accept (Windows IOCP)
#pragma once

#ifdef _WIN32

#include "reactor_default.hpp"
#include "stream_listener_base.hpp"
#include "tcp_socket.hpp"
#include "worker.hpp"
#include <stdexcept>
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace co_wq::net {

/** @brief Windows 端 TCP accept 协程失败时默认返回的错误码。 */
inline constexpr int k_accept_fatal = -2;

/**
 * @brief Windows 平台的 TCP 监听器封装。
 *
 * 基于 `detail::stream_listener_base`，提供：
 *  - 监听 socket 的创建与 IOCP 注册；
 *  - `bind_listen` 辅助方法设置地址/端口；
 *  - `accept()` Awaiter（继承自基类）。
 *
 * @tparam lock 对应 `workqueue<lock>` 的锁类型。
 * @tparam Reactor Reactor 模板（默认 `CO_WQ_DEFAULT_REACTOR`）。
 */
template <lockable lock, template <class> class Reactor = CO_WQ_DEFAULT_REACTOR>
class tcp_listener : public detail::stream_listener_base<tcp_listener<lock, Reactor>, lock, Reactor> {
    using base = detail::stream_listener_base<tcp_listener<lock, Reactor>, lock, Reactor>;

public:
    static constexpr int k_accept_fatal = net::k_accept_fatal;

    /** @brief 创建 TCP 监听 socket 并注册至 Reactor。 */
    explicit tcp_listener(workqueue<lock>& exec, Reactor<lock>& reactor, int family = AF_INET)
        : base(exec, reactor, family, SOCK_STREAM, IPPROTO_TCP)
    {
    }

    /**
     * @brief 绑定指定主机和端口并开始监听。
     *
     * @param host IPv4 字符串，留空或 "0.0.0.0" 表示 INADDR_ANY。
     * @param port 监听端口（主机字节序）。
     * @param backlog listen backlog，默认 128。
     * @throws std::runtime_error 绑定或监听失败时抛出。
     */
    void bind_listen(const std::string& host, uint16_t port, int backlog = 128, bool dual_stack = false)
    {
        SOCKET sock  = static_cast<SOCKET>(this->native_handle());
        BOOL   reuse = TRUE;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        if (this->family() == AF_INET6) {
            std::string view = host;
            if (!view.empty() && view.front() == '[' && view.back() == ']')
                view = view.substr(1, view.size() - 2);

            sockaddr_in6 addr6 {};
            addr6.sin6_family = AF_INET6;
            addr6.sin6_port   = htons(port);
            if (view.empty() || view == "::" || view == "*") {
                IN6_ADDR any    = IN6ADDR_ANY_INIT;
                addr6.sin6_addr = any;
            } else {
                if (InetPtonA(AF_INET6, view.c_str(), &addr6.sin6_addr) <= 0)
                    throw std::runtime_error("InetPton failed");
            }

            BOOL v6only = dual_stack ? FALSE : TRUE;
            setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only));

            if (::bind(sock, reinterpret_cast<const sockaddr*>(&addr6), sizeof(addr6)) == SOCKET_ERROR)
                throw std::runtime_error("bind failed");
        } else {
            sockaddr_in addr {};
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(port);
            if (host.empty() || host == "0.0.0.0" || host == "*")
                addr.sin_addr.s_addr = INADDR_ANY;
            else if (InetPtonA(AF_INET, host.c_str(), &addr.sin_addr) <= 0)
                throw std::runtime_error("InetPton failed");
            if (::bind(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
                throw std::runtime_error("bind failed");
        }

        if (::listen(sock, backlog) == SOCKET_ERROR)
            throw std::runtime_error("listen failed");
    }

    using base::accept;
};

/**
 * @brief `co_await` 形式的 Accept 原语，返回原生 fd。
 */
template <lockable lock, template <class> class Reactor = CO_WQ_DEFAULT_REACTOR>
inline Task<int, Work_Promise<lock, int>> async_accept(tcp_listener<lock, Reactor>& lst)
{
    co_return co_await lst.accept();
}

/**
 * @brief Accept 并接管成 `tcp_socket` 对象。
 *
 * 当 accept 失败（返回负值）时，构造一个临时 socket 并立即关闭，确保返回对象处于“无效已关闭”状态，方便上层判断。
 */
template <lockable lock, template <class> class Reactor = CO_WQ_DEFAULT_REACTOR>
inline Task<tcp_socket<lock, Reactor>, Work_Promise<lock, tcp_socket<lock, Reactor>>>
async_accept_socket(fd_workqueue<lock, Reactor>& fwq, tcp_listener<lock, Reactor>& lst)
{
    int fd = co_await lst.accept();
    if (fd < 0) {
        auto tmp = fwq.make_tcp_socket();
        tmp.close();
        co_return std::move(tmp);
    }
    co_return fwq.adopt_tcp_socket(fd);
}

} // namespace co_wq::net

#endif // _WIN32
