/**
 * @file tcp_socket.hpp
 * @brief 基于 Windows Overlapped/IOCP 的 TCP 协程原语，实现与 Linux 版本尽量一致的接口。
 */
#pragma once

#ifdef _WIN32

#include "callback_wq.hpp"
#include "io_serial.hpp"
#include "io_waiter.hpp"
#include "iocp_reactor.hpp"
#include "reactor_default.hpp"
#include "stream_socket_base.hpp"
#include "worker.hpp"
#include <atomic>
#include <basetsd.h>
#include <mswsock.h>
#include <string>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>

#ifndef _SSIZE_T_DEFINED
using ssize_t = SSIZE_T;
#define _SSIZE_T_DEFINED
#endif

#ifndef IOVEC_DEFINED_CO_WQ
struct iovec {
    void*  iov_base;
    size_t iov_len;
};
#define IOVEC_DEFINED_CO_WQ 1
#endif

namespace co_wq::net {

template <lockable lock, template <class> class Reactor> class fd_workqueue;

/**
 * @brief Windows 平台 TCP 协程 socket。
 *
 * 使用 IOCP 驱动异步 IO，并通过串行队列保证 send/recv 的单通道顺序，接口与 Linux 版本尽量保持一致。
 *
 * @tparam lock 配合 `workqueue<lock>` 的锁类型。
 * @tparam Reactor Reactor 模板（默认 `CO_WQ_DEFAULT_REACTOR`）。
 */
template <lockable lock, template <class> class Reactor = CO_WQ_DEFAULT_REACTOR>
class tcp_socket : public detail::stream_socket_base<tcp_socket<lock, Reactor>, lock, Reactor> {
    using base = detail::stream_socket_base<tcp_socket<lock, Reactor>, lock, Reactor>;

public:
    tcp_socket()                                 = delete;
    tcp_socket(const tcp_socket&)                = delete;
    tcp_socket& operator=(const tcp_socket&)     = delete;
    tcp_socket(tcp_socket&&) noexcept            = default;
    tcp_socket& operator=(tcp_socket&&) noexcept = default;
    ~tcp_socket()                                = default;

    using base::callback_queue;
    using base::close;
    using base::exec;
    using base::family;
    using base::mark_rx_eof;
    using base::mark_tx_shutdown;
    using base::native_handle;
    using base::reactor;
    using base::recv_queue;
    using base::send_queue;
    using base::serial_lock;
    using base::shutdown_tx;
    using base::socket_handle;
    using base::tx_shutdown;

    bool dual_stack() const noexcept { return _dual_stack; }

    static void
    log_waiter_state(const char* tag, const io_waiter_base* waiter, [[maybe_unused]] const char* detail = nullptr)
    {
        if (!tag)
            tag = "<null-tag>";
        [[maybe_unused]] const char*   name  = (waiter && waiter->debug_name) ? waiter->debug_name : "<null-name>";
        [[maybe_unused]] std::uint32_t magic = waiter ? waiter->debug_magic : 0;
        [[maybe_unused]] void*         func  = waiter ? reinterpret_cast<void*>(waiter->func) : nullptr;
        auto                           guard = waiter ? waiter->load_route_guard() : io_waiter_base::route_guard_ptr {};
        [[maybe_unused]] long          guard_use = io_waiter_base::route_guard_use_count(guard);
        [[maybe_unused]] void*         guard_ptr = guard ? guard.get() : nullptr;
        CO_WQ_CBQ_TRACE("[tcp_socket] %s waiter=%p func=%p name=%s magic=%08x guard_use=%ld guard_ptr=%p detail=%s\n",
                        tag,
                        static_cast<const void*>(waiter),
                        func,
                        name,
                        magic,
                        guard_use,
                        guard_ptr,
                        detail ? detail : "");
    }

    /**
     * @brief ConnectEx Awaiter，负责协程化的 TCP 建连。
     */
    struct connect_awaiter : io_waiter_base {
        tcp_socket&      sock;
        std::string      host;
        uint16_t         port;
        int              ret { -1 };
        iocp_ovl         ovl {};
        LPFN_CONNECTEX   _connectex { nullptr };
        sockaddr_storage remote {};
        int              remote_len { 0 };
        bool             remote_ready { false };
        bool             issued { false };
        std::uint32_t    ovl_generation { 0 };
        int              family { AF_INET };
        bool             dual_stack { false };
        int              selected_family { AF_UNSPEC };
        bool             pre_resolved { false };

        connect_awaiter(tcp_socket& s, std::string h, uint16_t p) : sock(s), host(std::move(h)), port(p)
        {
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.cancelled.store(true, std::memory_order_relaxed);
            ovl.waiter = this;
            auto& cbq  = sock.callback_queue();
            this->store_route_guard(cbq.retain_guard());
            this->route_ctx  = cbq.context();
            this->route_post = &callback_wq<lock>::post_adapter;
            this->set_debug_name("tcp_socket::connect");
            family     = sock.family();
            dual_stack = sock.dual_stack();
            log_waiter_state("connect ctor", this, host.c_str());
        }
        connect_awaiter(tcp_socket& s, const sockaddr* addr, int len) : sock(s)
        {
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.cancelled.store(true, std::memory_order_relaxed);
            ovl.waiter = this;
            auto& cbq  = sock.callback_queue();
            this->store_route_guard(cbq.retain_guard());
            this->route_ctx  = cbq.context();
            this->route_post = &callback_wq<lock>::post_adapter;
            this->set_debug_name("tcp_socket::connect");
            family       = sock.family();
            dual_stack   = sock.dual_stack();
            pre_resolved = true;
            remote_ready = (addr != nullptr && len > 0);
            remote_len   = remote_ready ? len : 0;
            if (remote_ready) {
                if (remote_len > static_cast<int>(sizeof(remote)))
                    remote_len = static_cast<int>(sizeof(remote));
                std::memcpy(&remote, addr, static_cast<size_t>(remote_len));
            }
            host = format_pre_resolved(addr, len);
            port = extract_port(addr, len);
            log_waiter_state("connect ctor", this, host.c_str());
        }
        ~connect_awaiter()
        {
            if (ovl.waiter == this) {
                ovl.cancelled.store(true);
                CO_WQ_CBQ_TRACE("[tcp_socket] connect dtor clearing ovl waiter=%p\n", static_cast<void*>(this));
                ovl.waiter = nullptr;
            }
            if (auto guard = this->exchange_route_guard()) {
                [[maybe_unused]] long  guard_use = io_waiter_base::route_guard_use_count(guard);
                [[maybe_unused]] void* guard_ptr = guard.get();
                CO_WQ_CBQ_TRACE("[tcp_socket] connect guard release waiter=%p guard_use=%ld guard_ptr=%p\n",
                                static_cast<void*>(this),
                                guard_use,
                                guard_ptr);
                CO_WQ_CBQ_TRACE("[tcp_socket] connect guard released waiter=%p\n", static_cast<void*>(this));
            }
            if (issued && ret != 0 && this->func == &io_waiter_base::resume_cb && !this->h) {
                CO_WQ_CBQ_WARN("[tcp_socket] connect dtor warning: issued without coroutine handle waiter=%p\n",
                               static_cast<void*>(this));
            }
            log_waiter_state("connect dtor", this, issued ? "issued" : "not-issued");
        }
        static uint16_t extract_port(const sockaddr* addr, int len)
        {
            if (!addr || len <= 0)
                return 0;
            if (addr->sa_family == AF_INET) {
                const auto* in = reinterpret_cast<const sockaddr_in*>(addr);
                return ntohs(in->sin_port);
            }
            if (addr->sa_family == AF_INET6) {
                const auto* in6 = reinterpret_cast<const sockaddr_in6*>(addr);
                return ntohs(in6->sin6_port);
            }
            return 0;
        }
        static std::string format_pre_resolved(const sockaddr* addr, int len)
        {
            if (!addr || len <= 0)
                return {};
            char host_buf[NI_MAXHOST] = {};
            char serv_buf[NI_MAXSERV] = {};
            if (::getnameinfo(addr,
                              len,
                              host_buf,
                              sizeof(host_buf),
                              serv_buf,
                              sizeof(serv_buf),
                              NI_NUMERICHOST | NI_NUMERICSERV)
                == 0) {
                std::string host_str(host_buf);
                if (addr->sa_family == AF_INET6)
                    host_str = '[' + host_str + ']';
                if (serv_buf[0] != '\0') {
                    host_str.push_back(':');
                    host_str.append(serv_buf);
                }
                return host_str;
            }
            return {};
        }
        bool load_connectex()
        {
            if (_connectex)
                return true;
            GUID  guid  = WSAID_CONNECTEX;
            DWORD bytes = 0;
            if (WSAIoctl(sock.socket_handle(),
                         SIO_GET_EXTENSION_FUNCTION_POINTER,
                         &guid,
                         sizeof(guid),
                         &_connectex,
                         sizeof(_connectex),
                         &bytes,
                         NULL,
                         NULL)
                == SOCKET_ERROR) {
                int err = WSAGetLastError();
                ret     = err ? -static_cast<int>(err) : -1;
                CO_WQ_CBQ_WARN("[tcp_socket::connect] load_connectex failed host=%s port=%u err=%d\n",
                               host.c_str(),
                               static_cast<unsigned>(port),
                               err);
                _connectex = nullptr;
                return false;
            }
            return true;
        }

        static std::string strip_brackets(const std::string& input)
        {
            if (input.size() >= 2 && input.front() == '[' && input.back() == ']')
                return input.substr(1, input.size() - 2);
            return input;
        }

        bool prepare_remote()
        {
            if (pre_resolved)
                return remote_ready;
            std::string node = strip_brackets(host);
            if (node.empty()) {
                ret = -WSAEINVAL;
                CO_WQ_CBQ_WARN("[tcp_socket::connect] empty host provided port=%u\n", static_cast<unsigned>(port));
                return false;
            }

            ADDRINFOA hints {};
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;
            hints.ai_family   = family;
            if (family == AF_INET6 && dual_stack)
                hints.ai_family = AF_UNSPEC;

            char service[16] {};
            _snprintf_s(service, sizeof(service), _TRUNCATE, "%u", static_cast<unsigned>(port));

            ADDRINFOA* result = nullptr;
            int        rc     = ::getaddrinfo(node.c_str(), service, &hints, &result);
            if (rc != 0 || !result) {
                ret = rc ? -rc : -1;
                CO_WQ_CBQ_WARN("[tcp_socket::connect] getaddrinfo failed host=%s port=%u rc=%d\n",
                               node.c_str(),
                               static_cast<unsigned>(port),
                               rc);
                if (result)
                    ::freeaddrinfo(result);
                return false;
            }

            std::unique_ptr<ADDRINFOA, decltype(&::freeaddrinfo)> guard(result, ::freeaddrinfo);
            struct candidate_entry {
                sockaddr_storage addr {};
                int              len { 0 };
                int              family { AF_UNSPEC };
            };
            std::vector<candidate_entry> primary;
            std::vector<candidate_entry> fallback;
            primary.reserve(4);
            fallback.reserve(4);
            for (auto* ai = result; ai; ai = ai->ai_next) {
                candidate_entry entry;
                if (ai->ai_family == family) {
                    if (ai->ai_addrlen > sizeof(entry.addr))
                        continue;
                    std::memcpy(&entry.addr, ai->ai_addr, static_cast<size_t>(ai->ai_addrlen));
                    entry.len    = static_cast<int>(ai->ai_addrlen);
                    entry.family = ai->ai_family;
                    primary.push_back(entry);
                } else if (family == AF_INET6 && dual_stack && ai->ai_family == AF_INET) {
                    auto* v4 = reinterpret_cast<sockaddr_in*>(ai->ai_addr);
                    auto* v6 = reinterpret_cast<sockaddr_in6*>(&entry.addr);
                    ZeroMemory(v6, sizeof(sockaddr_in6));
                    v6->sin6_family    = AF_INET6;
                    v6->sin6_port      = v4->sin_port;
                    unsigned char* dst = reinterpret_cast<unsigned char*>(&v6->sin6_addr);
                    dst[10]            = 0xFF;
                    dst[11]            = 0xFF;
                    std::memcpy(dst + 12, &v4->sin_addr, sizeof(v4->sin_addr));
                    entry.len    = static_cast<int>(sizeof(sockaddr_in6));
                    entry.family = AF_INET;
                    fallback.push_back(entry);
                }
            }

            auto* source_vector = &primary;
            if (source_vector->empty())
                source_vector = &fallback;

            if (source_vector->empty()) {
                ret = -WSAEAFNOSUPPORT;
                CO_WQ_CBQ_WARN("[tcp_socket::connect] no compatible address host=%s port=%u\n",
                               node.c_str(),
                               static_cast<unsigned>(port));
            } else {
                static std::atomic_uint32_t round_robin { 0 };
                std::uint32_t               index  = round_robin.fetch_add(1, std::memory_order_relaxed);
                const auto&                 chosen = (*source_vector)[index % source_vector->size()];
                remote                             = chosen.addr;
                remote_len                         = chosen.len;
                remote_ready                       = true;
                selected_family                    = chosen.family;
            }
            return remote_ready;
        }

        bool bind_local()
        {
            if (family == AF_INET) {
                sockaddr_in local {};
                local.sin_family      = AF_INET;
                local.sin_addr.s_addr = INADDR_ANY;
                local.sin_port        = 0;
                if (::bind(sock.socket_handle(), reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
                    int err = WSAGetLastError();
                    ret     = err ? -static_cast<int>(err) : -1;
                    CO_WQ_CBQ_WARN("[tcp_socket::connect] bind IPv4 failed host=%s port=%u err=%d\n",
                                   host.c_str(),
                                   static_cast<unsigned>(port),
                                   err);
                    return false;
                }
                return true;
            }

            sockaddr_in6 local6 {};
            local6.sin6_family = AF_INET6;
            local6.sin6_port   = 0;
            IN6_ADDR any       = IN6ADDR_ANY_INIT;
            local6.sin6_addr   = any;
            if (::bind(sock.socket_handle(), reinterpret_cast<sockaddr*>(&local6), sizeof(local6)) == SOCKET_ERROR) {
                int err = WSAGetLastError();
                ret     = err ? -static_cast<int>(err) : -1;
                CO_WQ_CBQ_WARN("[tcp_socket::connect] bind IPv6 failed host=%s port=%u err=%d\n",
                               host.c_str(),
                               static_cast<unsigned>(port),
                               err);
                return false;
            }
            return true;
        }

        bool await_ready() noexcept
        {
            INIT_LIST_HEAD(&this->ws_node);
            if (!prepare_remote())
                return true;
            if (!load_connectex()) {
                return true;
            }
            if (!bind_local())
                return true;
            this->func = &io_waiter_base::resume_cb;
            ovl.cancelled.store(false, std::memory_order_release);
            ovl.waiter     = this;
            ovl_generation = ovl.generation.fetch_add(1) + 1;
            ovl.generation.store(ovl_generation, std::memory_order_release);
            BOOL ok = _connectex(sock.socket_handle(),
                                 reinterpret_cast<sockaddr*>(&remote),
                                 static_cast<int>(remote_len),
                                 nullptr,
                                 0,
                                 nullptr,
                                 &ovl);
            issued  = true;
            if (ok) {
                setsockopt(sock.socket_handle(), SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);
                ret = 0;
                return true;
            }
            int err = WSAGetLastError();
            if (err != ERROR_IO_PENDING) {
                ret = err ? -static_cast<int>(err) : -1;
                CO_WQ_CBQ_WARN("[tcp_socket::connect] ConnectEx immediate failure host=%s port=%u err=%d\n",
                               host.c_str(),
                               static_cast<unsigned>(port),
                               err);
                return true;
            }
            return false;
        }
        void await_suspend(std::coroutine_handle<> awaiting)
        {
            [[maybe_unused]] void* haddr = awaiting ? awaiting.address() : nullptr;
            CO_WQ_CBQ_TRACE("[tcp_socket::connect] await_suspend this=%p awaiting=%p host=%s port=%u issued=%d\n",
                            static_cast<void*>(this),
                            haddr,
                            host.c_str(),
                            static_cast<unsigned>(port),
                            issued ? 1 : 0);
            this->h = awaiting;
            log_waiter_state("connect suspend", this, issued ? "issued" : "not-issued");
        }
        int await_resume() noexcept
        {
            CO_WQ_CBQ_TRACE("[tcp_socket::connect] await_resume this=%p h=%p host=%s port=%u ret=%d issued=%d\n",
                            static_cast<void*>(this),
                            this->h ? this->h.address() : nullptr,
                            host.c_str(),
                            static_cast<unsigned>(port),
                            ret,
                            issued ? 1 : 0);
            if (ret == 0)
                return 0;
            if (issued && ret != 0) {
                DWORD transferred = 0;
                DWORD flags       = 0;
                if (WSAGetOverlappedResult(sock.socket_handle(), &ovl, &transferred, FALSE, &flags)) {
                    setsockopt(sock.socket_handle(), SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);
                    ret = 0;
                } else {
                    int err = WSAGetLastError();
                    ret     = err ? -static_cast<int>(err) : -1;
                    CO_WQ_CBQ_WARN("[tcp_socket::connect] overlapped completion failed host=%s port=%u err=%d\n",
                                   host.c_str(),
                                   static_cast<unsigned>(port),
                                   err);
                }
            }
            log_waiter_state("connect resume", this, ret == 0 ? "ok" : "error");
            return ret;
        }
    };

    /** @brief 创建 Connect Awaiter。 */
    connect_awaiter connect(const std::string& host, uint16_t port) { return connect_awaiter(*this, host, port); }
    connect_awaiter connect(const sockaddr* addr, int len) { return connect_awaiter(*this, addr, len); }

    /**
     * @brief 单次接收 Awaiter：读取任意字节即返回。
     */
    struct recv_awaiter : serial_slot_awaiter<recv_awaiter, tcp_socket> {
        void*    buf { nullptr };
        size_t   len { 0 };
        ssize_t  nread { -1 };
        iocp_ovl ovl {};
        bool     started { false };
        bool     inflight { false };
        bool     finished { false };
        bool     result_ready { false };
        recv_awaiter(tcp_socket& owner, void* b, size_t l)
            : serial_slot_awaiter<recv_awaiter, tcp_socket>(owner, owner.recv_queue()), buf(b), len(l)
        {
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.waiter = this;
            ovl.cancelled.store(true, std::memory_order_relaxed);
            auto& cbq = owner.callback_queue();
            this->store_route_guard(cbq.retain_guard());
            this->route_ctx  = cbq.context();
            this->route_post = &callback_wq<lock>::post_adapter;
            this->set_debug_name("tcp_socket::recv");
        }
        static void lock_acquired_cb(worknode* w)
        {
            auto* self    = static_cast<recv_awaiter*>(w);
            self->started = true;
            self->issue();
        }
        void issue()
        {
            inflight     = false;
            finished     = false;
            result_ready = false;
            nread        = -1;
            WSABUF wbuf { static_cast<ULONG>(len), static_cast<CHAR*>(buf) };
            DWORD  flags = 0;
            DWORD  recvd = 0;
            ovl.waiter   = this;
            ovl.cancelled.store(false, std::memory_order_release);
            this->func = &recv_awaiter::completion_cb;
            int r      = WSARecv(socket_handle(), &wbuf, 1, &recvd, &flags, &ovl, nullptr);
            if (r == 0) {
                nread        = static_cast<ssize_t>(recvd);
                result_ready = true;
                if (recvd == 0)
                    this->owner.mark_rx_eof();
                finish();
                return;
            }
            int err = WSAGetLastError();
            if (r == SOCKET_ERROR && err == WSA_IO_PENDING) {
                inflight = true;
                return;
            }
            nread        = err ? -static_cast<ssize_t>(err) : -1;
            result_ready = true;
            finish();
        }
        static void completion_cb(worknode* w)
        {
            auto* self = static_cast<recv_awaiter*>(w);
            if (self->finished)
                return;
            DWORD transferred = 0;
            DWORD flags       = 0;
            if (WSAGetOverlappedResult(self->socket_handle(), &self->ovl, &transferred, FALSE, &flags)) {
                self->nread = static_cast<ssize_t>(transferred);
                if (transferred == 0)
                    self->owner.mark_rx_eof();
            } else {
                int err     = WSAGetLastError();
                self->nread = err ? -static_cast<ssize_t>(err) : -1;
            }
            self->result_ready = true;
            self->inflight     = false;
            self->finish();
        }
        void finish()
        {
            if (finished)
                return;
            finished = true;
            ovl.cancelled.store(true, std::memory_order_release);
            ovl.waiter = nullptr;
            this->func = &io_waiter_base::resume_cb;
            serial_slot_awaiter<recv_awaiter, tcp_socket>::release(this);
            post_via_route(this->owner.exec(), *this);
        }
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> awaiting)
        {
            this->h    = awaiting;
            this->func = &recv_awaiter::lock_acquired_cb;
            INIT_LIST_HEAD(&this->ws_node);
            serial_acquire_or_enqueue(this->q, this->owner.serial_lock(), this->owner.exec(), *this);
        }
        ssize_t await_resume() noexcept
        {
            if (!result_ready) {
                DWORD transferred = 0;
                DWORD flags       = 0;
                if (WSAGetOverlappedResult(socket_handle(), &ovl, &transferred, FALSE, &flags)) {
                    nread = static_cast<ssize_t>(transferred);
                    if (transferred == 0)
                        this->owner.mark_rx_eof();
                } else {
                    int err = WSAGetLastError();
                    nread   = err ? -static_cast<ssize_t>(err) : -1;
                }
                result_ready = true;
            }
            if (nread == 0)
                this->owner.mark_rx_eof();
            return nread;
        }

    private:
        SOCKET socket_handle() const { return this->owner.socket_handle(); }
    };

    recv_awaiter recv(void* buf, size_t len) { return recv_awaiter(*this, buf, len); }

    /**
     * @brief 完整接收 Awaiter：循环读取直到缓冲区写满或遇到 EOF。
     */
    struct recv_all_awaiter : serial_slot_awaiter<recv_all_awaiter, tcp_socket> {
        char*    buf { nullptr };
        size_t   len { 0 };
        size_t   recvd { 0 };
        ssize_t  err { 0 };
        iocp_ovl ovl {};
        bool     inflight { false };
        bool     finished { false };
        recv_all_awaiter(tcp_socket& owner, void* b, size_t l)
            : serial_slot_awaiter<recv_all_awaiter, tcp_socket>(owner, owner.recv_queue())
            , buf(static_cast<char*>(b))
            , len(l)
        {
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.waiter = this;
            ovl.cancelled.store(true, std::memory_order_relaxed);
            auto& cbq = owner.callback_queue();
            this->store_route_guard(cbq.retain_guard());
            this->route_ctx  = cbq.context();
            this->route_post = &callback_wq<lock>::post_adapter;
            this->set_debug_name("tcp_socket::recv_all");
        }
        static void lock_acquired_cb(worknode* w)
        {
            auto* self = static_cast<recv_all_awaiter*>(w);
            self->drive();
        }
        void issue()
        {
            WSABUF wbuf { static_cast<ULONG>(len - recvd), buf + recvd };
            DWORD  flags = 0;
            DWORD  got   = 0;
            ovl.waiter   = this;
            ovl.cancelled.store(false, std::memory_order_release);
            this->func = &recv_all_awaiter::drive_cb;
            int r      = WSARecv(socket_handle(), &wbuf, 1, &got, &flags, &ovl, nullptr);
            if (r == 0) {
                if (got == 0) {
                    this->owner.mark_rx_eof();
                    finish();
                    return;
                }
                recvd += got;
                if (recvd >= len) {
                    finish();
                    return;
                }
                return;
            }
            int e = WSAGetLastError();
            if (r == SOCKET_ERROR && e == WSA_IO_PENDING) {
                inflight = true;
                return;
            }
            if (recvd == 0)
                err = e ? -static_cast<ssize_t>(e) : -1;
            finish();
        }
        void drive()
        {
            while (!finished && recvd < len) {
                issue();
                if (inflight)
                    return;
            }
            if (!finished && recvd >= len)
                finish();
        }
        static void drive_cb(worknode* w)
        {
            auto* self = static_cast<recv_all_awaiter*>(w);
            if (self->finished)
                return;
            DWORD transferred = 0;
            DWORD flags       = 0;
            if (WSAGetOverlappedResult(self->socket_handle(), &self->ovl, &transferred, FALSE, &flags)) {
                if (transferred == 0) {
                    self->owner.mark_rx_eof();
                    self->finish();
                    return;
                }
                self->recvd += transferred;
                if (self->recvd >= self->len) {
                    self->finish();
                    return;
                }
                self->inflight = false;
                self->drive();
                return;
            }
            if (self->recvd == 0) {
                int e     = WSAGetLastError();
                self->err = e ? -static_cast<ssize_t>(e) : -1;
            }
            self->finish();
        }
        void finish()
        {
            if (finished)
                return;
            finished = true;
            ovl.cancelled.store(true, std::memory_order_release);
            ovl.waiter = nullptr;
            serial_slot_awaiter<recv_all_awaiter, tcp_socket>::release(this);
            this->func = &io_waiter_base::resume_cb;
            post_via_route(this->owner.exec(), *this);
        }
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> awaiting)
        {
            this->h    = awaiting;
            this->func = &recv_all_awaiter::lock_acquired_cb;
            INIT_LIST_HEAD(&this->ws_node);
            serial_acquire_or_enqueue(this->q, this->owner.serial_lock(), this->owner.exec(), *this);
        }
        ssize_t await_resume() noexcept { return (err < 0 && recvd == 0) ? err : static_cast<ssize_t>(recvd); }

    private:
        SOCKET socket_handle() const { return this->owner.socket_handle(); }
    };

    recv_all_awaiter recv_all(void* buf, size_t len) { return recv_all_awaiter(*this, buf, len); }

    /**
     * @brief 发送 Awaiter：支持缓冲区及 iovec，按需多次投递 WSASend。
     */
    struct send_awaiter : serial_slot_awaiter<send_awaiter, tcp_socket> {
        bool                use_vec { false };
        bool                full { false };
        const char*         buf { nullptr };
        size_t              len { 0 };
        std::vector<WSABUF> bufs;
        size_t              total { 0 };
        size_t              sent { 0 };
        ssize_t             err { 0 };
        iocp_ovl            ovl {};
        bool                inflight { false };
        bool                finished { false };
        send_awaiter(tcp_socket& owner, const void* b, size_t l, bool full_mode)
            : serial_slot_awaiter<send_awaiter, tcp_socket>(owner, owner.send_queue())
            , use_vec(false)
            , full(full_mode)
            , buf(static_cast<const char*>(b))
            , len(l)
            , total(l)
        {
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.waiter = this;
            ovl.cancelled.store(true, std::memory_order_relaxed);
            auto& cbq = owner.callback_queue();
            this->store_route_guard(cbq.retain_guard());
            this->route_ctx  = cbq.context();
            this->route_post = &callback_wq<lock>::post_adapter;
            this->set_debug_name(full_mode ? "tcp_socket::send_all" : "tcp_socket::send");
        }
        send_awaiter(tcp_socket& owner, const struct iovec* iov, int iovcnt, bool full_mode)
            : serial_slot_awaiter<send_awaiter, tcp_socket>(owner, owner.send_queue()), use_vec(true), full(full_mode)
        {
            bufs.reserve(static_cast<size_t>(iovcnt));
            for (int i = 0; i < iovcnt; ++i) {
                WSABUF b;
                b.len = static_cast<ULONG>(iov[i].iov_len);
                b.buf = static_cast<CHAR*>(iov[i].iov_base);
                bufs.push_back(b);
                total += b.len;
            }
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.waiter = this;
            ovl.cancelled.store(true, std::memory_order_relaxed);
            auto& cbq = owner.callback_queue();
            this->store_route_guard(cbq.retain_guard());
            this->route_ctx  = cbq.context();
            this->route_post = &callback_wq<lock>::post_adapter;
            this->set_debug_name(full_mode ? "tcp_socket::send_all" : "tcp_socket::sendv");
        }
        static void lock_acquired_cb(worknode* w)
        {
            auto* self = static_cast<send_awaiter*>(w);
            self->drive();
        }
        void compact()
        {
            while (!bufs.empty() && bufs.front().len == 0)
                bufs.erase(bufs.begin());
        }
        void advance(DWORD done)
        {
            sent += done;
            if (!use_vec)
                return;
            DWORD remain = done;
            for (auto& b : bufs) {
                if (remain >= b.len) {
                    remain -= b.len;
                    b.len = 0;
                    b.buf = nullptr;
                } else {
                    b.buf += remain;
                    b.len -= remain;
                    remain = 0;
                    break;
                }
            }
        }
        void issue()
        {
            DWORD sent_now = 0;
            DWORD flags    = 0;
            ovl.waiter     = this;
            ovl.cancelled.store(false, std::memory_order_release);
            this->func = &send_awaiter::drive_cb;
            if (!use_vec) {
                size_t remain = len - sent;
                if (remain == 0) {
                    finish();
                    return;
                }
                WSABUF wb { static_cast<ULONG>(remain), const_cast<CHAR*>(buf + sent) };
                int    r = WSASend(socket_handle(), &wb, 1, &sent_now, flags, &ovl, nullptr);
                if (r == 0) {
                    advance(sent_now);
                    if (!full || sent >= total) {
                        finish();
                        return;
                    }
                    return;
                }
            } else {
                compact();
                if (bufs.empty()) {
                    finish();
                    return;
                }
                int r = WSASend(socket_handle(),
                                bufs.data(),
                                static_cast<DWORD>(bufs.size()),
                                &sent_now,
                                flags,
                                &ovl,
                                nullptr);
                if (r == 0) {
                    if (sent_now)
                        advance(sent_now);
                    if (!full || sent >= total) {
                        finish();
                        return;
                    }
                    return;
                }
            }
            int e = WSAGetLastError();
            if (e == WSA_IO_PENDING) {
                inflight = true;
                return;
            }
            if (e == WSAECONNRESET || e == WSAENOTCONN)
                this->owner.mark_tx_shutdown();
            if (sent == 0)
                err = e ? -static_cast<ssize_t>(e) : -1;
            finish();
        }
        void drive()
        {
            if (!full) {
                issue();
                if (!inflight && !finished)
                    finish();
                return;
            }
            while (!finished && sent < total && err == 0) {
                issue();
                if (inflight)
                    return;
            }
            if (!finished && (err < 0 || sent >= total))
                finish();
        }
        static void drive_cb(worknode* w)
        {
            auto* self = static_cast<send_awaiter*>(w);
            if (self->finished)
                return;
            DWORD transferred = 0;
            DWORD flags       = 0;
            if (WSAGetOverlappedResult(self->socket_handle(), &self->ovl, &transferred, FALSE, &flags)) {
                if (transferred)
                    self->advance(transferred);
                self->inflight = false;
                if (!self->full || self->sent >= self->total) {
                    self->finish();
                    return;
                }
                self->drive();
                return;
            }
            if (self->sent == 0) {
                int e     = WSAGetLastError();
                self->err = e ? -static_cast<ssize_t>(e) : -1;
            }
            self->finish();
        }
        void finish()
        {
            if (finished)
                return;
            finished = true;
            ovl.cancelled.store(true, std::memory_order_release);
            ovl.waiter = nullptr;
            serial_slot_awaiter<send_awaiter, tcp_socket>::release(this);
            this->func = &io_waiter_base::resume_cb;
            post_via_route(this->owner.exec(), *this);
        }
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> awaiting)
        {
            this->h    = awaiting;
            this->func = &send_awaiter::lock_acquired_cb;
            INIT_LIST_HEAD(&this->ws_node);
            serial_acquire_or_enqueue(this->q, this->owner.serial_lock(), this->owner.exec(), *this);
        }
        ssize_t await_resume() noexcept { return err < 0 ? err : static_cast<ssize_t>(sent); }

    private:
        SOCKET socket_handle() const { return this->owner.socket_handle(); }
    };

    send_awaiter send(const void* buf, size_t len) { return send_awaiter(*this, buf, len, false); }
    send_awaiter send_all(const void* buf, size_t len) { return send_awaiter(*this, buf, len, true); }
    send_awaiter sendv(const struct iovec* iov, int iovcnt) { return send_awaiter(*this, iov, iovcnt, false); }
    send_awaiter send_all(const struct iovec* iov, int iovcnt) { return send_awaiter(*this, iov, iovcnt, true); }

private:
    friend class fd_workqueue<lock, Reactor>;
    explicit tcp_socket(workqueue<lock>& exec,
                        Reactor<lock>&   reactor,
                        int              fam               = AF_INET,
                        bool             enable_dual_stack = false)
        : base(exec, reactor, fam, SOCK_STREAM, IPPROTO_TCP), _dual_stack(fam == AF_INET6 ? enable_dual_stack : false)
    {
        if (fam == AF_INET6) {
            BOOL v6only = _dual_stack ? FALSE : TRUE;
            if (setsockopt(this->socket_handle(),
                           IPPROTO_IPV6,
                           IPV6_V6ONLY,
                           reinterpret_cast<const char*>(&v6only),
                           sizeof(v6only))
                == SOCKET_ERROR) {
                int err = WSAGetLastError();
                CO_WQ_CBQ_WARN("[tcp_socket] IPV6_V6ONLY setsockopt failed err=%d\n", err);
            }
        }
    }
    tcp_socket(int fd, workqueue<lock>& exec, Reactor<lock>& reactor) : base(static_cast<SOCKET>(fd), exec, reactor)
    {
        if (family() == AF_INET6) {
            _dual_stack = query_dual_stack_flag();
        }
    }

    bool query_dual_stack_flag() const
    {
        if (family() != AF_INET6)
            return false;
        DWORD v6only = 1;
        int   len    = static_cast<int>(sizeof(v6only));
        if (getsockopt(this->socket_handle(), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&v6only), &len)
            == SOCKET_ERROR)
            return false;
        return v6only == 0;
    }

    bool _dual_stack { false };
};

template <lockable lock>
inline Task<int, Work_Promise<lock, int>> async_connect(tcp_socket<lock>& s, const std::string host, uint16_t port)
{
    co_return co_await s.connect(host, port);
}

/** @brief 异步发送整个缓冲区。 */
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>> async_send_all(tcp_socket<lock>& s, const void* buf, size_t len)
{
    co_return co_await s.send_all(buf, len);
}

/** @brief 读取少量数据，返回实际读取的字节数。 */
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>> async_recv_some(tcp_socket<lock>& s, void* buf, size_t len)
{
    ssize_t n = co_await s.recv(buf, len);
    co_return n;
}

/** @brief 读取固定长度数据直至缓冲区填满。 */
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>> async_recv_all(tcp_socket<lock>& s, void* buf, size_t len)
{
    co_return co_await s.recv_all(buf, len);
}

/** @brief 发送多缓冲区数据并保证全量写出。 */
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>>
async_sendv_all(tcp_socket<lock>& s, const struct iovec* iov, int iovcnt)
{
    co_return co_await s.send_all(iov, iovcnt);
}

} // namespace co_wq::net

#endif // _WIN32
