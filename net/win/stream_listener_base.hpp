#pragma once

#ifdef _WIN32

/**
 * @file stream_listener_base.hpp
 * @brief Windows 平台流式监听 socket 的通用基类，封装 socket 生命周期与异步 AcceptEx awaiter。
 */

#include "io_waiter.hpp"
#include "iocp_reactor.hpp"
#include "workqueue.hpp"
#include <mswsock.h>
#include <stdexcept>
#include <utility>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>


namespace co_wq::net::detail {

/**
 * @brief Windows 平台流式监听器通用 CRTP 基类。
 *
 * 负责：
 *  - 为派生类创建/关闭监听 SOCKET；
 *  - 将监听句柄注册到 IOCP Reactor；
 *  - 暴露 Awaiter，用于协程化的 AcceptEx。
 *
 * @tparam Derived 实际派生类型，用于自定义常量（如 k_accept_fatal）。
 * @tparam lock 绑定执行器所用的锁类型，与 `workqueue<lock>` 对应。
 * @tparam Reactor Reactor 模板（默认 `iocp_reactor`），需提供 `add_fd/remove_fd/post_completion` 等接口。
 */
template <class Derived, lockable lock, template <class> class Reactor> class stream_listener_base {
public:
    static constexpr int k_accept_fatal_default = -2;

    /**
     * @brief 获取 accept 失败时返回给上层的错误码。
     *
     * 若派生类定义 `Derived::k_accept_fatal` 则优先使用该值，否则退回默认值 -2。
     */
    static constexpr int accept_fatal()
    {
        if constexpr (requires { Derived::k_accept_fatal; }) {
            return Derived::k_accept_fatal;
        } else {
            return k_accept_fatal_default;
        }
    }

    stream_listener_base(const stream_listener_base&)            = delete;
    stream_listener_base& operator=(const stream_listener_base&) = delete;

    stream_listener_base(stream_listener_base&& o) noexcept
        : _exec(o._exec), _reactor(o._reactor), _sock(std::exchange(o._sock, INVALID_SOCKET)), _acceptex(o._acceptex)
    {
    }

    stream_listener_base& operator=(stream_listener_base&& o) noexcept
    {
        if (this != &o) {
            close();
            _exec     = o._exec;
            _reactor  = o._reactor;
            _sock     = std::exchange(o._sock, INVALID_SOCKET);
            _acceptex = o._acceptex;
        }
        return *this;
    }

    /** @brief 析构时自动关闭监听 socket，并解除 Reactor 注册。 */
    ~stream_listener_base() { close(); }

    /**
     * @brief 主动关闭监听 socket。
     *
     * 会先尝试从 Reactor 移除 fd，若 Reactor 不存在则直接 closesocket。
     * 关闭后 `_acceptex` 缓存指针会被重置，避免悬空指针。
     */
    void close()
    {
        if (_sock != INVALID_SOCKET) {
            if (_reactor)
                _reactor->remove_fd(static_cast<int>(_sock));
            else
                ::closesocket(_sock);
            _sock     = INVALID_SOCKET;
            _acceptex = nullptr;
        }
    }

    /** @brief 返回监听 socket 的原生句柄（int 形式）。 */
    int native_handle() const { return static_cast<int>(_sock); }
    /** @brief 访问绑定的执行器。 */
    workqueue<lock>& exec() { return _exec; }
    /** @brief 返回关联的 Reactor 指针，可能为空。 */
    Reactor<lock>* reactor() { return _reactor; }

    /**
     * @brief Awaiter：封装单次 AcceptEx 调用。
     *
     * - 在 await_ready 中创建子 socket 并投递 AcceptEx；
     * - 完成回调通过 IOCP 投递回工作队列；
     * - await_resume 返回新连接的 fd，失败时返回 `accept_fatal()` 指定值。
     */
    struct accept_awaiter : io_waiter_base {
        stream_listener_base& owner;
        int                   newfd { -1 };
        iocp_ovl              ovl {};
        SOCKET                accepted { INVALID_SOCKET };
        std::vector<char>     addrbuf;

        /**
         * @brief 构造 Awaiter，预先分配地址缓冲区。
         * @param o 所属监听器实例。
         */
        explicit accept_awaiter(stream_listener_base& o) : owner(o), addrbuf((sizeof(sockaddr_in) + 16) * 2)
        {
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.waiter = this;
        }
        /** @brief 若 AcceptEx 能立即完成则返回 true，否则挂起等待 IOCP 完成。 */
        bool await_ready() noexcept
        {
            INIT_LIST_HEAD(&this->ws_node);
            SOCKET child = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (child == INVALID_SOCKET) {
                newfd = accept_fatal();
                return true;
            }
            u_long m = 1;
            ::ioctlsocket(child, FIONBIO, &m);
            if (!owner._acceptex) {
                ::closesocket(child);
                newfd = accept_fatal();
                return true;
            }
            BOOL ok = owner._acceptex(owner._sock,
                                      child,
                                      addrbuf.data(),
                                      0,
                                      static_cast<DWORD>(addrbuf.size() / 2),
                                      static_cast<DWORD>(addrbuf.size() / 2),
                                      nullptr,
                                      &ovl);
            if (ok) {
                setsockopt(child,
                           SOL_SOCKET,
                           SO_UPDATE_ACCEPT_CONTEXT,
                           reinterpret_cast<char*>(&owner._sock),
                           sizeof(owner._sock));
                newfd = static_cast<int>(child);
                return true;
            }
            int err = WSAGetLastError();
            if (err != ERROR_IO_PENDING) {
                ::closesocket(child);
                newfd = accept_fatal();
                return true;
            }
            accepted         = child;
            this->func       = &io_waiter_base::resume_cb;
            this->route_ctx  = nullptr;
            this->route_post = nullptr;
            return false;
        }
        /** @brief 记录等待中的协程句柄。 */
        void await_suspend(std::coroutine_handle<> awaiting) { this->h = awaiting; }
        /** @brief 取得 AcceptEx 结果，失败时负责关闭子 socket。 */
        int await_resume() noexcept
        {
            if (newfd != -1)
                return newfd;
            DWORD bytes = 0;
            DWORD flags = 0;
            if (WSAGetOverlappedResult(owner._sock, &ovl, &bytes, FALSE, &flags)) {
                setsockopt(accepted,
                           SOL_SOCKET,
                           SO_UPDATE_ACCEPT_CONTEXT,
                           reinterpret_cast<char*>(&owner._sock),
                           sizeof(owner._sock));
                return static_cast<int>(accepted);
            }
            if (accepted != INVALID_SOCKET)
                ::closesocket(accepted);
            return accept_fatal();
        }
    };

    /**
     * @brief 异步等待一个新连接。
     * @return `accept_awaiter`，可 `co_await` 得到新 fd 或错误码。
     */
    accept_awaiter accept() { return accept_awaiter(*this); }

protected:
    /**
     * @brief 构造监听器并创建新 socket。
     * @param exec 绑定的工作队列。
     * @param reactor 负责 IOCP 事件派发的 Reactor。
     * @param family 地址族（AF_INET/AF_INET6）。
     * @param type socket 类型（SOCK_STREAM 等）。
     * @param protocol 协议号（如 IPPROTO_TCP）。
     */
    stream_listener_base(workqueue<lock>& exec, Reactor<lock>& reactor, int family, int type, int protocol = 0)
        : _exec(exec), _reactor(&reactor)
    {
        _sock = ::socket(family, type, protocol);
        if (_sock == INVALID_SOCKET)
            throw std::runtime_error("listener socket failed");
        u_long m = 1;
        ::ioctlsocket(_sock, FIONBIO, &m);
        _reactor->add_fd(static_cast<int>(_sock));
        load_acceptex();
    }

    /**
     * @brief 动态查询 AcceptEx 扩展函数指针。
     *
     * Windows 上需要通过 `WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER)` 获取，失败时将 `_acceptex` 置空，
     * 这样 `accept()` 会返回 `accept_fatal()`。
     */
    void load_acceptex()
    {
        GUID  guid  = WSAID_ACCEPTEX;
        DWORD bytes = 0;
        if (WSAIoctl(_sock,
                     SIO_GET_EXTENSION_FUNCTION_POINTER,
                     &guid,
                     sizeof(guid),
                     &_acceptex,
                     sizeof(_acceptex),
                     &bytes,
                     NULL,
                     NULL)
            == SOCKET_ERROR) {
            _acceptex = nullptr;
        }
    }

    workqueue<lock>& _exec;
    Reactor<lock>*   _reactor { nullptr };
    SOCKET           _sock { INVALID_SOCKET };
    LPFN_ACCEPTEX    _acceptex { nullptr };
};

} // namespace co_wq::net::detail

#endif // _WIN32
