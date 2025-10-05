#pragma once

/**
 * @file stream_socket_base.hpp
 * @brief Windows 平台流式 socket 公共 CRTP 基类，复用串行化队列、回调队列及 fd 生命周期管理。
 */

#ifdef _WIN32

#include "callback_wq.hpp"
#include "io_serial.hpp"
#include "io_waiter.hpp"
#include "workqueue.hpp"
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <utility>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace co_wq::net::detail {

/**
 * @brief Windows socket 共享核心：统一管理 SOCKET 生命周期、Reactor 注册与串行化资源。
 *
 * 提供：
 *  - RAII 式 SOCKET 创建/关闭；
 *  - send/recv 双串行队列（用于同一 socket 的串行化 IO）；
 *  - 每个 socket 独立的 `callback_wq`，确保回调在单线程语义下执行；
 *  - Reactor 句柄管理，支持在移动构造/赋值时平稳接管资源。
 *
 * @tparam lock 与 `workqueue<lock>` 搭配的锁类型；
 * @tparam Reactor Reactor 模板（通常为 `iocp_reactor`）。
 */
template <lockable lock, template <class> class Reactor> class socket_core {
public:
    socket_core(const socket_core&)            = delete;
    socket_core& operator=(const socket_core&) = delete;

    /**
     * @brief 移动构造：接管对方的 SOCKET、Reactor 等核心资源。
     *
     * 串行队列会重新初始化（等待者需由上层保证不存在），回调队列重新绑定至 `_exec`。
     */
    socket_core(socket_core&& o) noexcept
        : _exec(o._exec)
        , _reactor(o._reactor)
        , _sock(std::exchange(o._sock, INVALID_SOCKET))
        , _closed(std::exchange(o._closed, false))
        , _cbq(_exec)
    {
        serial_queue_init(_send_q);
        serial_queue_init(_recv_q);
        void* new_state = _cbq.context();
        void* old_state = o._cbq.context();
        std::fprintf(
            stderr,
            "[socket_core] move_ctor this=%p from=%p sock=%llu cbq=%p state_new=%p state_old=%p reactor=%p exec=%p\n",
            static_cast<void*>(this),
            static_cast<void*>(&o),
            static_cast<unsigned long long>(static_cast<std::uintptr_t>(_sock)),
            static_cast<void*>(&_cbq),
            new_state,
            old_state,
            static_cast<void*>(_reactor),
            static_cast<void*>(&_exec));
    }

    /**
     * @brief 移动赋值：先关闭自身，再接管来源对象的 SOCKET。
     */
    socket_core& operator=(socket_core&& o) noexcept
    {
        if (this != &o) {
            close_socket();
            _reactor = o._reactor;
            _sock    = std::exchange(o._sock, INVALID_SOCKET);
            _closed  = std::exchange(o._closed, false);
            std::destroy_at(&_cbq);
            std::construct_at(&_cbq, _exec);
            serial_queue_init(_send_q);
            serial_queue_init(_recv_q);
            std::fprintf(stderr,
                         "[socket_core] move_assign this=%p from=%p sock=%llu cbq=%p state=%p reactor=%p exec=%p\n",
                         static_cast<void*>(this),
                         static_cast<void*>(&o),
                         static_cast<unsigned long long>(static_cast<std::uintptr_t>(_sock)),
                         static_cast<void*>(&_cbq),
                         _cbq.context(),
                         static_cast<void*>(_reactor),
                         static_cast<void*>(&_exec));
        }
        return *this;
    }

    ~socket_core() { close_socket(); }

    /** @brief 返回底层 SOCKET 句柄（以 int 形式）。 */
    int native_handle() const { return static_cast<int>(_sock); }

    /** @brief 以 SOCKET 类型返回原生句柄。 */
    SOCKET socket_handle() const { return _sock; }

    /** @brief 获取绑定的工作队列。 */
    workqueue<lock>& exec() { return _exec; }

    /** @brief 获取串行化锁对象。 */
    lock& serial_lock() { return _io_serial_lock; }

    /** @brief 获取关联 reactor 指针。 */
    Reactor<lock>* reactor() { return _reactor; }

    /** @brief 访问串行化发送队列。 */
    serial_queue& send_queue() { return _send_q; }

    /** @brief 访问串行化接收队列。 */
    serial_queue& recv_queue() { return _recv_q; }

    /** @brief 每个 socket 绑定的回调路由队列。 */
    callback_wq<lock>& callback_queue() { return _cbq; }

    /** @brief 是否已关闭。 */
    bool closed() const noexcept { return _closed; }

protected:
    /**
     * @brief 构造并创建新的 SOCKET。
     * @param exec 执行器引用。
     * @param reactor Reactor 引用。
     * @param family 地址族。
     * @param type 套接字类型。
     * @param protocol 协议号。
     */
    socket_core(workqueue<lock>& exec, Reactor<lock>& reactor, int family, int type, int protocol = 0)
        : _exec(exec), _reactor(&reactor), _cbq(exec)
    {
        std::fprintf(
            stderr,
            "[socket_core] ctor this=%p sock=%llu cbq=%p state=%p reactor=%p exec=%p family=%d type=%d protocol=%d\n",
            static_cast<void*>(this),
            static_cast<unsigned long long>(static_cast<std::uintptr_t>(_sock)),
            static_cast<void*>(&_cbq),
            _cbq.context(),
            static_cast<void*>(_reactor),
            static_cast<void*>(&_exec),
            family,
            type,
            protocol);
        create_socket(family, type, protocol);
    }

    /**
     * @brief 使用现有 SOCKET 构造，常用于 accept 后接管句柄。
     * @param sock 已创建的 SOCKET。
     * @throws std::runtime_error 当 SOCKET 无效时抛出。
     */
    socket_core(SOCKET sock, workqueue<lock>& exec, Reactor<lock>& reactor)
        : _exec(exec), _reactor(&reactor), _sock(sock), _cbq(exec)
    {
        if (_sock == INVALID_SOCKET)
            throw std::runtime_error("invalid socket handle");
        std::fprintf(stderr,
                     "[socket_core] adopt_ctor this=%p sock=%llu cbq=%p state=%p reactor=%p exec=%p\n",
                     static_cast<void*>(this),
                     static_cast<unsigned long long>(static_cast<std::uintptr_t>(_sock)),
                     static_cast<void*>(&_cbq),
                     _cbq.context(),
                     static_cast<void*>(_reactor),
                     static_cast<void*>(&_exec));
        init_socket();
    }

    /**
     * @brief 关闭 SOCKET 并释放串行队列中的等待者。
     *
     * 若 Reactor 可用，则先调用 `remove_fd`（内部会关闭 socket），否则直接 `closesocket`。
     * 随后收集 send/recv 队列的等待节点，统一投递 resume，避免永远挂起。
     */
    void close_socket()
    {
        if (_sock != INVALID_SOCKET) {
            std::fprintf(stderr,
                         "[socket_core] close_socket this=%p sock=%llu cbq=%p state=%p reactor=%p exec=%p\n",
                         static_cast<void*>(this),
                         static_cast<unsigned long long>(static_cast<std::uintptr_t>(_sock)),
                         static_cast<void*>(&_cbq),
                         _cbq.context(),
                         static_cast<void*>(_reactor),
                         static_cast<void*>(&_exec));
            _closed            = true;
            HANDLE sock_handle = reinterpret_cast<HANDLE>(_sock);
            if (sock_handle) {
                BOOL  cancel_ok  = ::CancelIoEx(sock_handle, nullptr);
                DWORD cancel_err = cancel_ok ? ERROR_SUCCESS : ::GetLastError();
                if (!cancel_ok && cancel_err != ERROR_NOT_FOUND && cancel_err != ERROR_INVALID_HANDLE) {
                    std::fprintf(stderr,
                                 "[socket_core] close_socket CancelIoEx failed this=%p sock=%llu err=%lu\n",
                                 static_cast<void*>(this),
                                 static_cast<unsigned long long>(static_cast<std::uintptr_t>(_sock)),
                                 static_cast<unsigned long>(cancel_err));
                } else {
                    std::fprintf(stderr,
                                 "[socket_core] close_socket CancelIoEx status this=%p sock=%llu err=%lu\n",
                                 static_cast<void*>(this),
                                 static_cast<unsigned long long>(static_cast<std::uintptr_t>(_sock)),
                                 static_cast<unsigned long>(cancel_err));
                }
            }
            if (_reactor) {
                _reactor->remove_fd(native_handle());
            } else {
                ::closesocket(_sock);
            }
            list_head pending;
            INIT_LIST_HEAD(&pending);
            serial_collect_waiters(_io_serial_lock, { &_send_q, &_recv_q }, pending);
            serial_post_pending(_exec, pending);
            _sock = INVALID_SOCKET;
            std::fprintf(stderr,
                         "[socket_core] close_socket done this=%p cbq=%p state=%p pending_empty=%d\n",
                         static_cast<void*>(this),
                         static_cast<void*>(&_cbq),
                         _cbq.context(),
                         list_empty(&pending) ? 1 : 0);
        }
    }

private:
    /** @brief 根据指定协议参数创建新 SOCKET，并执行初始化。 */
    void create_socket(int family, int type, int protocol)
    {
        _sock = ::socket(family, type, protocol);
        if (_sock == INVALID_SOCKET)
            throw std::runtime_error("socket failed");
        std::fprintf(stderr,
                     "[socket_core] create_socket this=%p sock=%llu family=%d type=%d protocol=%d\n",
                     static_cast<void*>(this),
                     static_cast<unsigned long long>(static_cast<std::uintptr_t>(_sock)),
                     family,
                     type,
                     protocol);
        init_socket();
    }

    /** @brief 将 SOCKET 设置为非阻塞并注册到 Reactor，同时初始化串行队列状态。 */
    void init_socket()
    {
        set_non_block();
        if (_reactor)
            _reactor->add_fd(native_handle());
        serial_queue_init(_send_q);
        serial_queue_init(_recv_q);
        _closed = false;
        std::fprintf(stderr,
                     "[socket_core] init_socket this=%p sock=%llu cbq=%p state=%p reactor=%p exec=%p\n",
                     static_cast<void*>(this),
                     static_cast<unsigned long long>(static_cast<std::uintptr_t>(_sock)),
                     static_cast<void*>(&_cbq),
                     _cbq.context(),
                     static_cast<void*>(_reactor),
                     static_cast<void*>(&_exec));
    }

    /** @brief 调用 `ioctlsocket` 将 SOCKET 切换为非阻塞模式。 */
    void set_non_block()
    {
        u_long mode = 1;
        ::ioctlsocket(_sock, FIONBIO, &mode);
    }

    workqueue<lock>&  _exec;
    Reactor<lock>*    _reactor { nullptr };
    SOCKET            _sock { INVALID_SOCKET };
    bool              _closed { false };
    lock              _io_serial_lock;
    serial_queue      _send_q;
    serial_queue      _recv_q;
    callback_wq<lock> _cbq;
};

/**
 * @brief Windows 流式 socket 公共基类。
 *
 * 在 `socket_core` 基础上补充：
 *  - 半关闭状态记录（TX 侧）；
 *  - 接收 EOF 标记（RX 侧）；
 *  - 统一的 `close()`、`shutdown_tx()` 辅助。
 */
template <class Derived, lockable lock, template <class> class Reactor>
class stream_socket_base : public socket_core<lock, Reactor> {
    using core = socket_core<lock, Reactor>;

public:
    stream_socket_base(const stream_socket_base&)                = delete;
    stream_socket_base& operator=(const stream_socket_base&)     = delete;
    stream_socket_base(stream_socket_base&&) noexcept            = default;
    stream_socket_base& operator=(stream_socket_base&&) noexcept = default;
    ~stream_socket_base()                                        = default;

    /**
     * @brief 关闭底层 SOCKET 并重置 EOF/Shutdown 状态。
     */
    void close()
    {
        core::close_socket();
        _rx_eof      = false;
        _tx_shutdown = false;
    }

    /**
     * @brief 半关闭发送方向（调用 `shutdown(SD_SEND)`）。
     *
     * 仅在尚未调用过 `shutdown_tx` 且 SOCKET 有效时实际执行。
     */
    void shutdown_tx()
    {
        if (core::socket_handle() != INVALID_SOCKET && !_tx_shutdown) {
            ::shutdown(core::socket_handle(), SD_SEND);
            _tx_shutdown = true;
        }
    }

    /** @brief 查询发送方向是否已半关闭。 */
    bool tx_shutdown() const noexcept { return _tx_shutdown; }
    /** @brief 查询接收方向是否收到 EOF。 */
    bool rx_eof() const noexcept { return _rx_eof; }

    /** @brief 标记接收方向 EOF。 */
    void mark_rx_eof() { _rx_eof = true; }
    /** @brief 标记发送方向已半关闭（用于错误处理时同步状态）。 */
    void mark_tx_shutdown() { _tx_shutdown = true; }

protected:
    /**
     * @brief 使用 `socket_core` 构造流式 socket。
     */
    stream_socket_base(workqueue<lock>& exec, Reactor<lock>& reactor, int family, int type, int protocol = 0)
        : core(exec, reactor, family, type, protocol)
    {
    }

    /**
     * @brief 接管现有 SOCKET 构造。
     */
    stream_socket_base(SOCKET sock, workqueue<lock>& exec, Reactor<lock>& reactor) : core(sock, exec, reactor) { }

private:
    bool _rx_eof { false };
    bool _tx_shutdown { false };
};

} // namespace co_wq::net::detail

#endif // _WIN32
