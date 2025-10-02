#pragma once

#include "callback_wq.hpp"
#include "io_serial.hpp"
#include "tcp_socket.hpp"
#include "worker.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <cerrno>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace co_wq::net {

enum class tls_mode { Client, Server };

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

} // namespace detail

class tls_context {
public:
    tls_context() = default;

    explicit tls_context(std::shared_ptr<SSL_CTX> ctx) : _ctx(std::move(ctx)) { }

    SSL_CTX* native_handle() const noexcept { return _ctx.get(); }

    bool valid() const noexcept { return static_cast<bool>(_ctx); }

    static tls_context make(tls_mode mode)
    {
        detail::ensure_global_init();
        return tls_context { std::shared_ptr<SSL_CTX>(detail::make_ctx(mode), detail::ssl_ctx_deleter {}) };
    }

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

template <lockable lock, template <class> class Reactor = epoll_reactor> class tls_socket {
public:
    template <class D> using tp_base = two_phase_drain_awaiter<D, tls_socket>;

    tls_socket(workqueue<lock>&            exec,
               Reactor<lock>&              reactor,
               tcp_socket<lock, Reactor>&& tcp,
               tls_context                 ctx,
               tls_mode                    mode)
        : _exec(&exec)
        , _reactor(&reactor)
        , _tcp(std::move(tcp))
        , _ctx(std::move(ctx))
        , _ssl(nullptr, &SSL_free)
        , _cbq(std::make_unique<callback_wq<lock>>(exec))
    {
        detail::ensure_global_init();
        _ssl.reset(SSL_new(_ctx.native_handle()));
        if (!_ssl)
            throw std::runtime_error("SSL_new failed");
        SSL_set_mode(_ssl.get(), SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        if (SSL_set_fd(_ssl.get(), _tcp.native_handle()) != 1)
            throw std::runtime_error("SSL_set_fd failed");
        if (mode == tls_mode::Client)
            SSL_set_connect_state(_ssl.get());
        else
            SSL_set_accept_state(_ssl.get());
        serial_queue_init(_send_q);
        serial_queue_init(_recv_q);
    }

    tls_socket(const tls_socket&)            = delete;
    tls_socket& operator=(const tls_socket&) = delete;

    tls_socket(tls_socket&& other) noexcept
        : _exec(other._exec)
        , _reactor(other._reactor)
        , _tcp(std::move(other._tcp))
        , _ctx(std::move(other._ctx))
        , _ssl(std::move(other._ssl))
        , _handshake_done(other._handshake_done)
        , _rx_eof(other._rx_eof)
        , _tx_shutdown(other._tx_shutdown)
        , _cbq(std::move(other._cbq))
    {
        serial_queue_init(_send_q);
        serial_queue_init(_recv_q);
        other._exec           = nullptr;
        other._reactor        = nullptr;
        other._handshake_done = false;
        other._rx_eof         = false;
        other._tx_shutdown    = false;
        serial_queue_init(other._send_q);
        serial_queue_init(other._recv_q);
    }

    tls_socket& operator=(tls_socket&& other) noexcept
    {
        if (this != &other) {
            close();
            _exec           = other._exec;
            _reactor        = other._reactor;
            _tcp            = std::move(other._tcp);
            _ctx            = std::move(other._ctx);
            _ssl            = std::move(other._ssl);
            _handshake_done = other._handshake_done;
            _rx_eof         = other._rx_eof;
            _tx_shutdown    = other._tx_shutdown;
            _cbq            = std::move(other._cbq);
            serial_queue_init(_send_q);
            serial_queue_init(_recv_q);
            other._exec           = nullptr;
            other._reactor        = nullptr;
            other._handshake_done = false;
            other._rx_eof         = false;
            other._tx_shutdown    = false;
            serial_queue_init(other._send_q);
            serial_queue_init(other._recv_q);
        }
        return *this;
    }

    ~tls_socket() { close(); }

    void close()
    {
        if (_ssl) {
            SSL_shutdown(_ssl.get());
            _ssl.reset();
        }
        _tcp.close();
        _handshake_done = false;
        _rx_eof         = true;
        _tx_shutdown    = true;
    }

    void shutdown_tx()
    {
        if (_ssl && !_tx_shutdown) {
            SSL_shutdown(_ssl.get());
            _tx_shutdown = true;
        }
        _tcp.shutdown_tx();
    }

    bool rx_eof() const noexcept { return _rx_eof; }
    bool tx_shutdown() const noexcept { return _tx_shutdown; }

    workqueue<lock>&           exec() { return *_exec; }
    lock&                      serial_lock() { return _io_serial_lock; }
    Reactor<lock>*             reactor() { return _reactor; }
    tcp_socket<lock, Reactor>& underlying() { return _tcp; }
    bool                       handshake_done() const noexcept { return _handshake_done; }
    SSL*                       ssl_handle() const noexcept { return _ssl.get(); }

    struct handshake_awaiter : io_waiter_base {
        tls_socket& owner;
        int         result { 0 };

        explicit handshake_awaiter(tls_socket& s) : owner(s) { }

        bool await_ready() const noexcept { return owner._handshake_done; }

        void await_suspend(std::coroutine_handle<> h)
        {
            this->h          = h;
            this->route_ctx  = owner._cbq.get();
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
                    owner._reactor->add_waiter_custom(owner._tcp.native_handle(), mask, this);
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
        tls_socket& owner;
        void*       buf;
        size_t      len;
        size_t      recvd { 0 };
        ssize_t     err { 0 };
        bool        full { false };
        uint32_t    wait_mask { 0 };

        recv_awaiter(tls_socket& s, void* b, size_t l, bool f)
            : tp_base<recv_awaiter>(s, s._recv_q), owner(s), buf(b), len(l), full(f)
        {
            this->route_ctx  = owner._cbq.get();
            this->route_post = &callback_wq<lock>::post_adapter;
        }

        static void register_wait(recv_awaiter* self, bool)
        {
            uint32_t mask = self->wait_mask ? self->wait_mask : EPOLLIN;
            self->owner._reactor->add_waiter_custom(self->owner._tcp.native_handle(), mask, self);
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
        tls_socket& owner;
        const void* buf;
        size_t      len;
        size_t      sent { 0 };
        ssize_t     err { 0 };
        bool        full { false };
        uint32_t    wait_mask { 0 };

        send_awaiter(tls_socket& s, const void* b, size_t l, bool f)
            : tp_base<send_awaiter>(s, s._send_q), owner(s), buf(b), len(l), full(f)
        {
            this->route_ctx  = owner._cbq.get();
            this->route_post = &callback_wq<lock>::post_adapter;
        }

        static void register_wait(send_awaiter* self, bool)
        {
            uint32_t mask = self->wait_mask ? self->wait_mask : EPOLLOUT;
            self->owner._reactor->add_waiter_custom(self->owner._tcp.native_handle(), mask, self);
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

private:
    workqueue<lock>*                          _exec { nullptr };
    Reactor<lock>*                            _reactor { nullptr };
    tcp_socket<lock, Reactor>                 _tcp;
    tls_context                               _ctx;
    std::unique_ptr<SSL, decltype(&SSL_free)> _ssl;
    bool                                      _handshake_done { false };
    bool                                      _rx_eof { false };
    bool                                      _tx_shutdown { false };
    std::unique_ptr<callback_wq<lock>>        _cbq;
    lock                                      _io_serial_lock;
    serial_queue                              _send_q;
    serial_queue                              _recv_q;
};

} // namespace co_wq::net
