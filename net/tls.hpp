#pragma once

#include "callback_wq.hpp"
#include "io_serial.hpp"
#include "tcp_socket.hpp"
#include "udp_socket.hpp"
#include "worker.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <cerrno>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace co_wq::net {

enum class tls_mode { Client, Server };
enum class dtls_mode { Client, Server };

using os::ssize_t;

namespace detail {

    struct ssl_ctx_deleter {
        void operator()(SSL_CTX* ctx) const noexcept
        {
            if (ctx)
                SSL_CTX_free(ctx);
        }
    };

    inline void ensure_global_init()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            SSL_load_error_strings();
            OpenSSL_add_ssl_algorithms();
        });
    }

    inline SSL_CTX* make_ctx(tls_mode mode)
    {
        const SSL_METHOD* method = (mode == tls_mode::Client) ? TLS_client_method() : TLS_server_method();
        SSL_CTX*          ctx    = SSL_CTX_new(method);
        if (!ctx)
            throw std::runtime_error("SSL_CTX_new failed");
        SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION);
        return ctx;
    }

    inline SSL_CTX* make_dtls_ctx(dtls_mode mode)
    {
        const SSL_METHOD* method = (mode == dtls_mode::Client) ? DTLS_client_method() : DTLS_server_method();
        SSL_CTX*          ctx    = SSL_CTX_new(method);
        if (!ctx)
            throw std::runtime_error("SSL_CTX_new (dtls) failed");
        SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION | SSL_OP_NO_QUERY_MTU);
        return ctx;
    }

    /**
     * @brief 将 OpenSSL 错误栈收集为字符串。
     */
    inline std::string collect_errors()
    {
        BIO* bio = BIO_new(BIO_s_mem());
        if (!bio)
            return "unknown";
        ERR_print_errors(bio);
        BUF_MEM* mem = nullptr;
        BIO_get_mem_ptr(bio, &mem);
        std::string out = (mem && mem->data) ? std::string(mem->data, mem->length) : std::string {};
        BIO_free(bio);
        return out;
    }

    /**
     * @brief 将 OpenSSL 错误码转换为统一的负 errno。
     */
    inline int translate_failure(int ssl_err)
    {
        switch (ssl_err) {
        case SSL_ERROR_SYSCALL:
            return (errno != 0) ? -errno : -EIO;
        case SSL_ERROR_SSL:
            return -EIO;
        case SSL_ERROR_ZERO_RETURN:
            return -ECONNRESET;
        default:
            return -ssl_err;
        }
    }

    /**
     * @brief 将跨平台 `os::fd_t` 转换为 OpenSSL 所需的 int fd。
     *
     * 在 Windows 上会检查 HANDLE 是否超出 int 范围，否则抛出异常；在类 Unix 平台直接
     * 静态转换。
     */
    inline int to_ssl_fd(os::fd_t fd)
    {
#if defined(_WIN32)
        auto value = static_cast<std::uintptr_t>(fd);
        if (value > static_cast<std::uintptr_t>(std::numeric_limits<int>::max()))
            throw std::runtime_error("socket handle exceeds SSL fd range");
        return static_cast<int>(value);
#else
        return static_cast<int>(fd);
#endif
    }

} // namespace detail

/**
 * @brief TLS 上下文封装，管理 `SSL_CTX` 生命周期。
 */
class tls_context {
public:
    tls_context() = default;

    explicit tls_context(std::shared_ptr<SSL_CTX> ctx) : _ctx(std::move(ctx)) { }

    SSL_CTX* native_handle() const noexcept { return _ctx.get(); }

    bool valid() const noexcept { return static_cast<bool>(_ctx); }

    /**
     * @brief 根据模式创建内置 TLS 上下文。
     */
    static tls_context make(tls_mode mode)
    {
        detail::ensure_global_init();
        return tls_context { std::shared_ptr<SSL_CTX>(detail::make_ctx(mode), detail::ssl_ctx_deleter {}) };
    }

    /**
     * @brief 创建服务器上下文并加载 PEM 证书/私钥。
     *
     * @param cert_path 证书路径。
     * @param key_path  私钥路径。
     */
    static tls_context make_server_with_pem(const std::string& cert_path, const std::string& key_path)
    {
        auto ctx = make(tls_mode::Server);
        if (SSL_CTX_use_certificate_file(ctx.native_handle(), cert_path.c_str(), SSL_FILETYPE_PEM) <= 0)
            throw std::runtime_error("failed to load certificate: " + cert_path + " " + detail::collect_errors());
        if (SSL_CTX_use_PrivateKey_file(ctx.native_handle(), key_path.c_str(), SSL_FILETYPE_PEM) <= 0)
            throw std::runtime_error("failed to load private key: " + key_path + " " + detail::collect_errors());
        if (!SSL_CTX_check_private_key(ctx.native_handle()))
            throw std::runtime_error("TLS private key mismatch");
        return ctx;
    }

private:
    std::shared_ptr<SSL_CTX> _ctx;
};

/**
 * @brief DTLS 上下文封装。
 */
class dtls_context {
public:
    dtls_context() = default;

    explicit dtls_context(std::shared_ptr<SSL_CTX> ctx) : _ctx(std::move(ctx)) { }

    SSL_CTX* native_handle() const noexcept { return _ctx.get(); }

    bool valid() const noexcept { return static_cast<bool>(_ctx); }

    /**
     * @brief 根据模式创建内置 DTLS 上下文。
     */
    static dtls_context make(dtls_mode mode)
    {
        detail::ensure_global_init();
        return dtls_context { std::shared_ptr<SSL_CTX>(detail::make_dtls_ctx(mode), detail::ssl_ctx_deleter {}) };
    }

    /**
     * @brief 创建 DTLS 服务器上下文并加载 PEM 证书/私钥。
     */
    static dtls_context make_server_with_pem(const std::string& cert_path, const std::string& key_path)
    {
        auto ctx = make(dtls_mode::Server);
        if (SSL_CTX_use_certificate_file(ctx.native_handle(), cert_path.c_str(), SSL_FILETYPE_PEM) <= 0)
            throw std::runtime_error("failed to load certificate: " + cert_path + " " + detail::collect_errors());
        if (SSL_CTX_use_PrivateKey_file(ctx.native_handle(), key_path.c_str(), SSL_FILETYPE_PEM) <= 0)
            throw std::runtime_error("failed to load private key: " + key_path + " " + detail::collect_errors());
        if (!SSL_CTX_check_private_key(ctx.native_handle()))
            throw std::runtime_error("DTLS private key mismatch");
        return ctx;
    }

private:
    std::shared_ptr<SSL_CTX> _ctx;
};

/**
 * @brief 通用 TLS 套接字封装，组合底层传输 socket 与 OpenSSL `SSL` 对象。
 *
 * 模板参数允许在 TCP、UDP (DTLS) 等不同传输之上复用同一套 awaiter/缓冲逻辑。
 */
template <lockable lock, template <class> class Reactor, class TransportSocket, class Context> class basic_tls_socket {
public:
    using transport_type = TransportSocket;
    using context_type   = Context;
    using reactor_type   = Reactor<lock>;
    using workqueue_type = workqueue<lock>;

    template <class D> using tp_base = two_phase_drain_awaiter<D, basic_tls_socket>;

    basic_tls_socket(workqueue<lock>&  exec,
                     Reactor<lock>&    reactor,
                     TransportSocket&& transport,
                     Context           ctx,
                     const char*       ssl_new_error)
        : _exec(&exec)
        , _reactor(&reactor)
        , _transport(std::move(transport))
        , _ctx(std::move(ctx))
        , _ssl(nullptr, &SSL_free)
        , _cbq(std::make_unique<callback_wq<lock>>(exec))
    {
        detail::ensure_global_init();
        _ssl.reset(SSL_new(_ctx.native_handle()));
        if (!_ssl)
            throw std::runtime_error(ssl_new_error);
        SSL_set_mode(_ssl.get(), SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        serial_queue_init(_send_q);
        serial_queue_init(_recv_q);
    }

    basic_tls_socket(const basic_tls_socket&)            = delete;
    basic_tls_socket& operator=(const basic_tls_socket&) = delete;

    basic_tls_socket(basic_tls_socket&& other) noexcept
        : _exec(other._exec)
        , _reactor(other._reactor)
        , _transport(std::move(other._transport))
        , _ctx(std::move(other._ctx))
        , _ssl(std::move(other._ssl))
        , _handshake_done(other._handshake_done)
        , _rx_eof(other._rx_eof)
        , _tx_shutdown(other._tx_shutdown)
        , _cbq(std::move(other._cbq))
        , _active(other._active)
    {
        serial_queue_init(_send_q);
        serial_queue_init(_recv_q);
        other.reset_after_move();
    }

    basic_tls_socket& operator=(basic_tls_socket&& other) noexcept
    {
        if (this != &other) {
            close();
            _exec           = other._exec;
            _reactor        = other._reactor;
            _transport      = std::move(other._transport);
            _ctx            = std::move(other._ctx);
            _ssl            = std::move(other._ssl);
            _handshake_done = other._handshake_done;
            _rx_eof         = other._rx_eof;
            _tx_shutdown    = other._tx_shutdown;
            _cbq            = std::move(other._cbq);
            _active         = other._active;
            serial_queue_init(_send_q);
            serial_queue_init(_recv_q);
            other.reset_after_move();
        }
        return *this;
    }

    ~basic_tls_socket() { close(); }

    void close()
    {
        if (!_active)
            return;
        if (_ssl) {
            SSL_shutdown(_ssl.get());
            _ssl.reset();
        }
        close_transport();
        _handshake_done = false;
        _rx_eof         = true;
        _tx_shutdown    = true;
    }

    void shutdown_tx()
    {
        if (!_active)
            return;
        if (_ssl && !_tx_shutdown) {
            SSL_shutdown(_ssl.get());
            _tx_shutdown = true;
        }
        shutdown_transport();
    }

    bool rx_eof() const noexcept { return _rx_eof; }
    bool tx_shutdown() const noexcept { return _tx_shutdown; }

    workqueue<lock>&      exec() { return *_exec; }
    lock&                 serial_lock() { return _io_serial_lock; }
    Reactor<lock>*        reactor() { return _reactor; }
    transport_type&       underlying() { return _transport; }
    const transport_type& underlying() const { return _transport; }
    bool                  handshake_done() const noexcept { return _handshake_done; }
    SSL*                  ssl_handle() const noexcept { return _ssl.get(); }

    struct handshake_awaiter : io_waiter_base {
        basic_tls_socket& owner;
        int               result { 0 };

        explicit handshake_awaiter(basic_tls_socket& s) : owner(s) { }

        bool await_ready() const noexcept { return owner._handshake_done; }

        void await_suspend(std::coroutine_handle<> coro)
        {
            this->h = coro;
            this->store_route_guard(owner._cbq->retain_guard());
            this->route_ctx  = owner._cbq->context();
            this->route_post = &callback_wq<lock>::post_adapter;
            INIT_LIST_HEAD(&this->ws_node);
            drive();
        }

        void drive()
        {
            while (!owner._handshake_done) {
                int rc = SSL_do_handshake(owner._ssl.get());
                if (rc == 1) {
                    owner._handshake_done = true;
                    if (this->h)
                        this->h.resume();
                    return;
                }
                int ssl_err = SSL_get_error(owner._ssl.get(), rc);
                if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                    uint32_t mask = 0;
                    if (ssl_err == SSL_ERROR_WANT_READ)
                        mask |= EPOLLIN;
                    if (ssl_err == SSL_ERROR_WANT_WRITE)
                        mask |= EPOLLOUT;
                    this->func = &handshake_awaiter::resume_cb;
                    owner._reactor->add_waiter_custom(owner.io_handle(), mask, this);
                    return;
                }
                result = detail::translate_failure(ssl_err);
                break;
            }
            if (this->h)
                this->h.resume();
        }

        static void resume_cb(worknode* w)
        {
            auto* self = static_cast<handshake_awaiter*>(w);
            self->drive();
        }

        int await_resume() const noexcept
        {
            if (!owner._handshake_done)
                return result == 0 ? -ECONNRESET : result;
            return 0;
        }
    };

    handshake_awaiter handshake() { return handshake_awaiter(*this); }

    struct recv_awaiter : tp_base<recv_awaiter> {
        basic_tls_socket& owner;
        void*             buf;
        size_t            len;
        size_t            recvd { 0 };
        ssize_t           err { 0 };
        bool              full { false };
        uint32_t          wait_mask { 0 };

        recv_awaiter(basic_tls_socket& s, void* b, size_t l, bool f)
            : tp_base<recv_awaiter>(s, s._recv_q), owner(s), buf(b), len(l), full(f)
        {
            this->store_route_guard(owner._cbq->retain_guard());
            this->route_ctx  = owner._cbq->context();
            this->route_post = &callback_wq<lock>::post_adapter;
        }

        static void register_wait(recv_awaiter* self, bool)
        {
            uint32_t mask = self->wait_mask ? self->wait_mask : EPOLLIN;
            self->owner._reactor->add_waiter_custom(self->owner.io_handle(), mask, self);
        }

        int attempt_once()
        {
            if (recvd >= len)
                return 0;
            int rc = SSL_read(owner._ssl.get(), static_cast<char*>(buf) + recvd, static_cast<int>(len - recvd));
            if (rc > 0) {
                recvd += static_cast<size_t>(rc);
                if (!full)
                    return 0;
                return recvd == len ? 0 : 1;
            }
            if (rc == 0) {
                owner._rx_eof = true;
                return 0;
            }
            int ssl_err = SSL_get_error(owner._ssl.get(), rc);
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                wait_mask = 0;
                if (ssl_err == SSL_ERROR_WANT_READ)
                    wait_mask |= EPOLLIN;
                if (ssl_err == SSL_ERROR_WANT_WRITE)
                    wait_mask |= EPOLLOUT;
                return -1;
            }
            if (ssl_err == SSL_ERROR_ZERO_RETURN) {
                owner._rx_eof = true;
                return 0;
            }
            err = detail::translate_failure(ssl_err);
            return 0;
        }

        ssize_t await_resume() const noexcept
        {
            if (err < 0 && recvd == 0)
                return err;
            return static_cast<ssize_t>(recvd);
        }
    };

    recv_awaiter recv(void* buf, size_t len) { return recv_awaiter(*this, buf, len, false); }
    recv_awaiter recv_all(void* buf, size_t len) { return recv_awaiter(*this, buf, len, true); }

    struct send_awaiter : tp_base<send_awaiter> {
        basic_tls_socket& owner;
        const void*       buf;
        size_t            len;
        size_t            sent { 0 };
        ssize_t           err { 0 };
        bool              full { false };
        uint32_t          wait_mask { 0 };

        send_awaiter(basic_tls_socket& s, const void* b, size_t l, bool f)
            : tp_base<send_awaiter>(s, s._send_q), owner(s), buf(b), len(l), full(f)
        {
            this->store_route_guard(owner._cbq->retain_guard());
            this->route_ctx  = owner._cbq->context();
            this->route_post = &callback_wq<lock>::post_adapter;
        }

        static void register_wait(send_awaiter* self, bool)
        {
            uint32_t mask = self->wait_mask ? self->wait_mask : EPOLLOUT;
            self->owner._reactor->add_waiter_custom(self->owner.io_handle(), mask, self);
        }

        int attempt_once()
        {
            if (sent >= len)
                return 0;
            int rc = SSL_write(owner._ssl.get(), static_cast<const char*>(buf) + sent, static_cast<int>(len - sent));
            if (rc > 0) {
                sent += static_cast<size_t>(rc);
                if (!full)
                    return 0;
                return sent == len ? 0 : 1;
            }
            if (rc == 0) {
                owner._tx_shutdown = true;
                return 0;
            }
            int ssl_err = SSL_get_error(owner._ssl.get(), rc);
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                wait_mask = 0;
                if (ssl_err == SSL_ERROR_WANT_READ)
                    wait_mask |= EPOLLIN;
                if (ssl_err == SSL_ERROR_WANT_WRITE)
                    wait_mask |= EPOLLOUT;
                return -1;
            }
            if (ssl_err == SSL_ERROR_ZERO_RETURN) {
                owner._tx_shutdown = true;
                return 0;
            }
            err = detail::translate_failure(ssl_err);
            return 0;
        }

        ssize_t await_resume() const noexcept
        {
            if (err < 0 && sent == 0)
                return err;
            return static_cast<ssize_t>(sent);
        }
    };

    send_awaiter send(const void* buf, size_t len) { return send_awaiter(*this, buf, len, false); }
    send_awaiter send_all(const void* buf, size_t len) { return send_awaiter(*this, buf, len, true); }

protected:
    os::fd_t io_handle() const { return _transport.native_handle(); }

    void close_transport()
    {
        if constexpr (requires(transport_type& t) { t.close(); }) {
            _transport.close();
        }
    }

    void shutdown_transport()
    {
        if constexpr (requires(transport_type& t) { t.shutdown_tx(); }) {
            _transport.shutdown_tx();
        } else {
            close_transport();
        }
    }

    void reset_after_move()
    {
        _exec           = nullptr;
        _reactor        = nullptr;
        _handshake_done = false;
        _rx_eof         = false;
        _tx_shutdown    = false;
        _active         = false;
        serial_queue_init(_send_q);
        serial_queue_init(_recv_q);
    }

    workqueue<lock>*                          _exec { nullptr };
    Reactor<lock>*                            _reactor { nullptr };
    transport_type                            _transport;
    Context                                   _ctx;
    std::unique_ptr<SSL, decltype(&SSL_free)> _ssl;
    bool                                      _handshake_done { false };
    bool                                      _rx_eof { false };
    bool                                      _tx_shutdown { false };
    std::unique_ptr<callback_wq<lock>>        _cbq;
    bool                                      _active { true };
    lock                                      _io_serial_lock;
    serial_queue                              _send_q;
    serial_queue                              _recv_q;
};

template <lockable lock, template <class> class Reactor = epoll_reactor>
class tls_socket : public basic_tls_socket<lock, Reactor, tcp_socket<lock, Reactor>, tls_context> {
    using base_type = basic_tls_socket<lock, Reactor, tcp_socket<lock, Reactor>, tls_context>;

public:
    using typename base_type::handshake_awaiter;
    using typename base_type::recv_awaiter;
    using typename base_type::send_awaiter;

    template <class D> using tp_base = typename base_type::template tp_base<D>;

    tls_socket(workqueue<lock>&            exec,
               Reactor<lock>&              reactor,
               tcp_socket<lock, Reactor>&& tcp,
               tls_context                 ctx,
               tls_mode                    mode)
        : base_type(exec, reactor, std::move(tcp), std::move(ctx), "SSL_new failed")
    {
        if (SSL_set_fd(this->ssl_handle(), detail::to_ssl_fd(this->underlying().native_handle())) != 1)
            throw std::runtime_error("SSL_set_fd failed");
        if (mode == tls_mode::Client)
            SSL_set_connect_state(this->ssl_handle());
        else
            SSL_set_accept_state(this->ssl_handle());
    }

    tls_socket(const tls_socket&)                = delete;
    tls_socket& operator=(const tls_socket&)     = delete;
    tls_socket(tls_socket&&) noexcept            = default;
    tls_socket& operator=(tls_socket&&) noexcept = default;
    ~tls_socket()                                = default;
};

template <lockable lock, template <class> class Reactor = epoll_reactor>
class dtls_socket : public basic_tls_socket<lock, Reactor, udp_socket<lock, Reactor>, dtls_context> {
    using base_type = basic_tls_socket<lock, Reactor, udp_socket<lock, Reactor>, dtls_context>;

public:
    using typename base_type::handshake_awaiter;
    using typename base_type::recv_awaiter;
    using typename base_type::send_awaiter;

    template <class D> using tp_base = typename base_type::template tp_base<D>;

    dtls_socket(workqueue<lock>&            exec,
                Reactor<lock>&              reactor,
                udp_socket<lock, Reactor>&& udp,
                dtls_context                ctx,
                dtls_mode                   mode)
        : base_type(exec, reactor, std::move(udp), std::move(ctx), "SSL_new (dtls) failed")
    {
        BIO* bio = BIO_new_dgram(detail::to_ssl_fd(this->underlying().native_handle()), BIO_NOCLOSE);
        if (!bio)
            throw std::runtime_error("BIO_new_dgram failed");
        SSL_set_bio(this->ssl_handle(), bio, bio);
        SSL_set_read_ahead(this->ssl_handle(), 1);
        if (mode == dtls_mode::Client)
            SSL_set_connect_state(this->ssl_handle());
        else {
            SSL_set_accept_state(this->ssl_handle());
            SSL_set_options(this->ssl_handle(), SSL_OP_COOKIE_EXCHANGE);
        }
    }

    dtls_socket(const dtls_socket&)                = delete;
    dtls_socket& operator=(const dtls_socket&)     = delete;
    dtls_socket(dtls_socket&&) noexcept            = default;
    dtls_socket& operator=(dtls_socket&&) noexcept = default;
    ~dtls_socket()                                 = default;
};

} // namespace co_wq::net
