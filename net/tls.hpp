#pragma once

#include "callback_wq.hpp"
#include "io_serial.hpp"
#include "quic_dispatcher.hpp"
#include "tcp_socket.hpp"
#include "tls_utils.hpp"
#include "udp_socket.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/sslerr.h>
#if !defined(OPENSSL_NO_QUIC)
#include <cstring>
#include <openssl/quic.h>
#endif

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace co_wq::net {

enum class tls_mode { Client, Server };
enum class dtls_mode { Client, Server };
enum class quic_mode { Client, Server };

using ssize_type = std::ptrdiff_t;

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

    inline void setup_ctx_message_logging(SSL_CTX* ctx);
    inline void setup_ssl_message_logging(SSL* ssl, void* owner_hint = nullptr);

    inline SSL_CTX* make_ctx(tls_mode mode, bool enable_tls_trace)
    {
        const SSL_METHOD* method = (mode == tls_mode::Client) ? TLS_client_method() : TLS_server_method();
        SSL_CTX*          ctx    = SSL_CTX_new(method);
        if (!ctx)
            throw std::runtime_error("SSL_CTX_new failed");
        SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION);
        if (enable_tls_trace)
            setup_ctx_message_logging(ctx);
        return ctx;
    }

    inline SSL_CTX* make_dtls_ctx(dtls_mode mode, bool enable_tls_trace)
    {
        const SSL_METHOD* method = (mode == dtls_mode::Client) ? DTLS_client_method() : DTLS_server_method();
        SSL_CTX*          ctx    = SSL_CTX_new(method);
        if (!ctx)
            throw std::runtime_error("SSL_CTX_new (dtls) failed");
        SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION | SSL_OP_NO_QUERY_MTU);
        if (enable_tls_trace)
            setup_ctx_message_logging(ctx);
        return ctx;
    }

    /**
     * @brief 将 OpenSSL 错误栈收集为字符串。
     */
    inline std::string collect_errors()
    {
        return tls_utils::collect_error_stack();
    }

    inline const char* ssl_error_name(int err)
    {
        switch (err) {
        case SSL_ERROR_NONE:
            return "SSL_ERROR_NONE";
        case SSL_ERROR_SSL:
            return "SSL_ERROR_SSL";
        case SSL_ERROR_WANT_READ:
            return "SSL_ERROR_WANT_READ";
        case SSL_ERROR_WANT_WRITE:
            return "SSL_ERROR_WANT_WRITE";
        case SSL_ERROR_WANT_CONNECT:
            return "SSL_ERROR_WANT_CONNECT";
        case SSL_ERROR_WANT_ACCEPT:
            return "SSL_ERROR_WANT_ACCEPT";
        case SSL_ERROR_WANT_X509_LOOKUP:
            return "SSL_ERROR_WANT_X509_LOOKUP";
        case SSL_ERROR_SYSCALL:
            return "SSL_ERROR_SYSCALL";
        case SSL_ERROR_ZERO_RETURN:
            return "SSL_ERROR_ZERO_RETURN";
#if defined(SSL_ERROR_WANT_READ_THEN_WRITE)
        case SSL_ERROR_WANT_READ_THEN_WRITE:
            return "SSL_ERROR_WANT_READ_THEN_WRITE";
#endif
#if defined(SSL_ERROR_WANT_WRITE_THEN_READ)
        case SSL_ERROR_WANT_WRITE_THEN_READ:
            return "SSL_ERROR_WANT_WRITE_THEN_READ";
#endif
#if defined(SSL_ERROR_WANT_ASYNC)
        case SSL_ERROR_WANT_ASYNC:
            return "SSL_ERROR_WANT_ASYNC";
#endif
#if defined(SSL_ERROR_WANT_ASYNC_JOB)
        case SSL_ERROR_WANT_ASYNC_JOB:
            return "SSL_ERROR_WANT_ASYNC_JOB";
#endif
#if defined(SSL_ERROR_WANT_CLIENT_HELLO_CB)
        case SSL_ERROR_WANT_CLIENT_HELLO_CB:
            return "SSL_ERROR_WANT_CLIENT_HELLO_CB";
#endif
        default:
            return "SSL_ERROR_UNKNOWN";
        }
    }

    inline std::string format_error_code(unsigned long code)
    {
        if (code == 0)
            return "0";
        char buf[256] = { 0 };
        ERR_error_string_n(code, buf, sizeof(buf));
        return std::string(buf);
    }

    inline const char* handshake_type_name(int type)
    {
        switch (type) {
        case SSL3_MT_HELLO_REQUEST:
            return "hello_request";
        case SSL3_MT_CLIENT_HELLO:
            return "client_hello";
        case SSL3_MT_SERVER_HELLO:
            return "server_hello";
#if defined(TLS1_MT_NEWSESSION_TICKET)
        case TLS1_MT_NEWSESSION_TICKET:
            return "new_session_ticket";
#endif
        case SSL3_MT_CERTIFICATE:
            return "certificate";
        case SSL3_MT_SERVER_KEY_EXCHANGE:
            return "server_key_exchange";
        case SSL3_MT_CERTIFICATE_REQUEST:
            return "certificate_request";
        case SSL3_MT_SERVER_DONE:
            return "server_hello_done";
        case SSL3_MT_CERTIFICATE_VERIFY:
            return "certificate_verify";
        case SSL3_MT_CLIENT_KEY_EXCHANGE:
            return "client_key_exchange";
        case SSL3_MT_FINISHED:
            return "finished";
#if defined(SSL3_MT_ENCRYPTED_EXTENSIONS)
        case SSL3_MT_ENCRYPTED_EXTENSIONS:
            return "encrypted_extensions";
#endif
#if defined(TLS1_MT_KEY_UPDATE)
        case TLS1_MT_KEY_UPDATE:
            return "key_update";
#endif
        default:
            return "unknown";
        }
    }

    inline const char* ssl_record_type_name(int type)
    {
        switch (type) {
        case SSL3_RT_CHANGE_CIPHER_SPEC:
            return "change_cipher_spec";
        case SSL3_RT_ALERT:
            return "alert";
        case SSL3_RT_HANDSHAKE:
            return "handshake";
        case SSL3_RT_APPLICATION_DATA:
            return "application_data";
#if defined(SSL3_RT_QUIC_APPLICATION_DATA)
        case SSL3_RT_QUIC_APPLICATION_DATA:
            return "quic_application_data";
#endif
        default:
            return "unknown";
        }
    }

    inline void
    ssl_message_callback(int write_p, int version, int content_type, const void* buf, size_t len, SSL* ssl, void* arg)
    {
        const char* direction = write_p ? "out" : "in";
        const char* type_name = ssl_record_type_name(content_type);
        const char* state     = ssl ? SSL_state_string_long(ssl) : "<null>";
        if (content_type == SSL3_RT_HANDSHAKE && len > 0) {
            int handshake_type = static_cast<int>(static_cast<const unsigned char*>(buf)[0]);
            CO_WQ_LOG_INFO("[tls trace] ssl=%p owner=%p dir=%s type=%s handshake=%s len=%zu version=0x%x state=%s",
                           static_cast<void*>(ssl),
                           arg,
                           direction,
                           type_name,
                           handshake_type_name(handshake_type),
                           len,
                           version,
                           state);
            return;
        }
        if (content_type == SSL3_RT_ALERT && len >= 2) {
            auto level = SSL_alert_type_string_long(static_cast<int>(static_cast<const unsigned char*>(buf)[0]));
            auto desc  = SSL_alert_desc_string_long(static_cast<int>(static_cast<const unsigned char*>(buf)[1]));
            CO_WQ_LOG_WARN(
                "[tls trace] ssl=%p owner=%p dir=%s type=alert level=%s desc=%s len=%zu version=0x%x state=%s",
                static_cast<void*>(ssl),
                arg,
                direction,
                level ? level : "<unknown>",
                desc ? desc : "<unknown>",
                len,
                version,
                state);
            return;
        }
        CO_WQ_LOG_DEBUG("[tls trace] ssl=%p owner=%p dir=%s type=%s len=%zu version=0x%x state=%s",
                        static_cast<void*>(ssl),
                        arg,
                        direction,
                        type_name,
                        len,
                        version,
                        state);
    }

    inline void setup_ctx_message_logging(SSL_CTX* ctx)
    {
        if (!ctx)
            return;
        SSL_CTX_set_msg_callback(ctx, &ssl_message_callback);
        SSL_CTX_set_msg_callback_arg(ctx, ctx);
    }

    inline void setup_ssl_message_logging(SSL* ssl, void* owner_hint)
    {
        if (!ssl)
            return;
        SSL_set_msg_callback(ssl, &ssl_message_callback);
        SSL_set_msg_callback_arg(ssl, owner_hint);
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

#if !defined(OPENSSL_NO_QUIC)
    inline constexpr unsigned char default_quic_alpn[] = { 5, 'c', 'o', '-', 'w', 'q' };

    inline int quic_select_alpn_cb(SSL*,
                                   const unsigned char** out,
                                   unsigned char*        outlen,
                                   const unsigned char*  in,
                                   unsigned int          inlen,
                                   void*)
    {
        unsigned int offset = 0;
        while (offset < inlen) {
            unsigned char len = in[offset];
            if (len == 0 || offset + 1U + len > inlen)
                break;
            if (len == default_quic_alpn[0] && std::memcmp(in + offset + 1, default_quic_alpn + 1, len) == 0) {
                *out    = in + offset + 1;
                *outlen = len;
                return SSL_TLSEXT_ERR_OK;
            }
            offset += 1U + len;
        }
        return SSL_TLSEXT_ERR_NOACK;
    }

    inline void configure_quic_client_alpn(SSL_CTX* ctx)
    {
        SSL_CTX_set_alpn_protos(ctx, default_quic_alpn, sizeof(default_quic_alpn));
    }

    inline void configure_quic_server_alpn(SSL_CTX* ctx)
    {
        SSL_CTX_set_alpn_protos(ctx, default_quic_alpn, sizeof(default_quic_alpn));
        SSL_CTX_set_alpn_select_cb(ctx, &quic_select_alpn_cb, nullptr);
    }
#endif

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

    explicit tls_context(std::shared_ptr<SSL_CTX> ctx, bool trace_enabled = false)
        : _ctx(std::move(ctx)), _trace_enabled(trace_enabled)
    {
    }

    SSL_CTX* native_handle() const noexcept { return _ctx.get(); }

    bool valid() const noexcept { return static_cast<bool>(_ctx); }

    bool trace_enabled() const noexcept { return _trace_enabled; }

    /**
     * @brief 根据模式创建内置 TLS 上下文。
     */
    static tls_context make(tls_mode mode, bool enable_tls_trace = false)
    {
        detail::ensure_global_init();
        return tls_context { std::shared_ptr<SSL_CTX>(detail::make_ctx(mode, enable_tls_trace),
                                                      detail::ssl_ctx_deleter {}),
                             enable_tls_trace };
    }

    /**
     * @brief 创建服务器上下文并加载 PEM 证书/私钥。
     *
     * @param cert_path 证书路径。
     * @param key_path  私钥路径。
     */
    static tls_context
    make_server_with_pem(const std::string& cert_path, const std::string& key_path, bool enable_tls_trace = false)
    {
        auto ctx = make(tls_mode::Server, enable_tls_trace);
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
    bool                     _trace_enabled { false };
};

/**
 * @brief DTLS 上下文封装。
 */
class dtls_context {
public:
    dtls_context() = default;

    explicit dtls_context(std::shared_ptr<SSL_CTX> ctx, bool trace_enabled = false)
        : _ctx(std::move(ctx)), _trace_enabled(trace_enabled)
    {
    }

    SSL_CTX* native_handle() const noexcept { return _ctx.get(); }

    bool valid() const noexcept { return static_cast<bool>(_ctx); }

    bool trace_enabled() const noexcept { return _trace_enabled; }

    /**
     * @brief 根据模式创建内置 DTLS 上下文。
     */
    static dtls_context make(dtls_mode mode, bool enable_tls_trace = false)
    {
        detail::ensure_global_init();
        return dtls_context { std::shared_ptr<SSL_CTX>(detail::make_dtls_ctx(mode, enable_tls_trace),
                                                       detail::ssl_ctx_deleter {}),
                              enable_tls_trace };
    }

    /**
     * @brief 创建 DTLS 服务器上下文并加载 PEM 证书/私钥。
     */
    static dtls_context
    make_server_with_pem(const std::string& cert_path, const std::string& key_path, bool enable_tls_trace = false)
    {
        auto ctx = make(dtls_mode::Server, enable_tls_trace);
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
    bool                     _trace_enabled { false };
};

#if !defined(OPENSSL_NO_QUIC)
/**
 * @brief QUIC 上下文封装。
 */
class quic_context {
public:
    quic_context() = default;

    explicit quic_context(std::shared_ptr<SSL_CTX> ctx, bool trace_enabled = false)
        : _ctx(std::move(ctx)), _trace_enabled(trace_enabled)
    {
    }

    SSL_CTX* native_handle() const noexcept { return _ctx.get(); }

    bool valid() const noexcept { return static_cast<bool>(_ctx); }

    bool trace_enabled() const noexcept { return _trace_enabled; }

    static quic_context make(quic_mode mode, bool thread_assisted = false, bool enable_tls_trace = false)
    {
        detail::ensure_global_init();
        const SSL_METHOD* method = nullptr;
        if (mode == quic_mode::Client) {
            if (thread_assisted)
                method = OSSL_QUIC_client_thread_method();
            if (!method)
                method = OSSL_QUIC_client_method();
        } else {
            method = OSSL_QUIC_server_method();
        }
        if (!method)
            throw std::runtime_error("QUIC method unavailable");
        SSL_CTX* ctx = SSL_CTX_new(method);
        if (!ctx)
            throw std::runtime_error("SSL_CTX_new (quic) failed");
        SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
        SSL_CTX_set_default_verify_paths(ctx);
        if (mode == quic_mode::Client)
            detail::configure_quic_client_alpn(ctx);
        else
            detail::configure_quic_server_alpn(ctx);
        if (enable_tls_trace)
            detail::setup_ctx_message_logging(ctx);
        return quic_context { std::shared_ptr<SSL_CTX>(ctx, detail::ssl_ctx_deleter {}), enable_tls_trace };
    }

    static quic_context make_server_with_pem(const std::string& cert_path,
                                             const std::string& key_path,
                                             bool               thread_assisted  = false,
                                             bool               enable_tls_trace = false)
    {
        auto ctx = make(quic_mode::Server, thread_assisted, enable_tls_trace);
        if (SSL_CTX_use_certificate_file(ctx.native_handle(), cert_path.c_str(), SSL_FILETYPE_PEM) <= 0)
            throw std::runtime_error("failed to load QUIC certificate: " + cert_path + " " + detail::collect_errors());
        if (SSL_CTX_use_PrivateKey_file(ctx.native_handle(), key_path.c_str(), SSL_FILETYPE_PEM) <= 0)
            throw std::runtime_error("failed to load QUIC private key: " + key_path + " " + detail::collect_errors());
        if (!SSL_CTX_check_private_key(ctx.native_handle()))
            throw std::runtime_error("QUIC private key mismatch");
        detail::configure_quic_server_alpn(ctx.native_handle());
        return ctx;
    }

private:
    std::shared_ptr<SSL_CTX> _ctx;
    bool                     _trace_enabled { false };
};
#endif

/**
 * @brief 通用 TLS 套接字封装，组合底层传输 socket 与 OpenSSL `SSL` 对象。
 *
 * 模板参数允许在 TCP、UDP (DTLS) 等不同传输之上复用同一套 awaiter/缓冲逻辑。
 */
template <lockable lock, template <class> class Reactor, class TransportSocket, class Context> class basic_tls_socket {
public:
    struct adopt_ssl_tag { };
    static constexpr adopt_ssl_tag adopt_ssl {};

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
        initialize_ssl_handle();
    }

    basic_tls_socket(workqueue<lock>&  exec,
                     Reactor<lock>&    reactor,
                     TransportSocket&& transport,
                     Context           ctx,
                     SSL*              adopted_ssl,
                     adopt_ssl_tag) noexcept(false)
        : _exec(&exec)
        , _reactor(&reactor)
        , _transport(std::move(transport))
        , _ctx(std::move(ctx))
        , _ssl(adopted_ssl, &SSL_free)
        , _cbq(std::make_unique<callback_wq<lock>>(exec))
    {
        detail::ensure_global_init();
        if (!_ssl)
            throw std::runtime_error("adopted SSL handle is null");
        initialize_ssl_handle();
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

    virtual ~basic_tls_socket() { close(); }

private:
    void initialize_ssl_handle()
    {
        if (!_ssl)
            throw std::runtime_error("invalid SSL handle");
        SSL_set_mode(_ssl.get(), SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        serial_queue_init(_send_q);
        serial_queue_init(_recv_q);
        _handshake_done   = SSL_is_init_finished(_ssl.get());
        bool enable_trace = false;
        if constexpr (requires(const Context& c) { c.trace_enabled(); })
            enable_trace = _ctx.trace_enabled();
        if (enable_trace)
            detail::setup_ssl_message_logging(_ssl.get(), this);
    }

public:
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

public:
    SSL* ssl_handle() const noexcept { return _ssl.get(); }

    struct handshake_awaiter : io_waiter_base {
        basic_tls_socket& owner;
        int               result { 0 };
        bool              armed { false };
        bool              first_wait { true };

        explicit handshake_awaiter(basic_tls_socket& s) : owner(s) { }

        bool await_ready() const noexcept { return owner._handshake_done; }

        void await_suspend(std::coroutine_handle<> coro)
        {
            this->h = coro;
            this->store_route_guard(owner._cbq->retain_guard());
            this->route_ctx  = owner._cbq->context();
            this->route_post = &callback_wq<lock>::post_adapter;
            INIT_LIST_HEAD(&this->ws_node);
            CO_WQ_LOG_INFO("[tls handshake] await_suspend start ssl=%p done=%d",
                           static_cast<void*>(owner._ssl.get()),
                           owner._handshake_done ? 1 : 0);
            drive();
        }

        void drive()
        {
            CO_WQ_LOG_INFO("[tls handshake] drive enter ssl=%p done=%d armed=%d result=%d",
                           static_cast<void*>(owner._ssl.get()),
                           owner._handshake_done ? 1 : 0,
                           armed ? 1 : 0,
                           result);
            while (!owner._handshake_done) {
                CO_WQ_LOG_INFO("[tls handshake] drive loop ssl=%p armed=%d result=%d first_wait=%d",
                               static_cast<void*>(owner._ssl.get()),
                               armed ? 1 : 0,
                               result,
                               first_wait ? 1 : 0);
                if constexpr (requires { owner.drive_events(); }) {
                    owner.drive_events();
                }
                int rc = SSL_do_handshake(owner._ssl.get());
                CO_WQ_LOG_INFO("[tls handshake] ssl=%p rc=%d state=%s done=%d",
                               static_cast<void*>(owner._ssl.get()),
                               rc,
                               SSL_state_string_long(owner._ssl.get()),
                               owner._handshake_done ? 1 : 0);
                if (rc == 1) {
                    owner._handshake_done = true;
                    armed                 = false;
                    if (this->h)
                        this->h.resume();
                    return;
                }
                int  ssl_err       = SSL_get_error(owner._ssl.get(), rc);
                int  sys_err       = errno;
                auto ssl_code      = ERR_peek_error();
                int  shutdown_flag = SSL_get_shutdown(owner._ssl.get());
                int  pending_err   = SSL_get_error(owner._ssl.get(), 0);
                auto ssl_code_desc = detail::format_error_code(ssl_code);
                CO_WQ_LOG_INFO("[tls handshake] error detail ssl=%p err=%d(%s) sys=%d shutdown=0x%x pending=%d lib=%u "
                               "reason=%u code=%s",
                               static_cast<void*>(owner._ssl.get()),
                               ssl_err,
                               detail::ssl_error_name(ssl_err),
                               sys_err,
                               shutdown_flag,
                               pending_err,
                               static_cast<unsigned>(ERR_GET_LIB(ssl_code)),
                               static_cast<unsigned>(ERR_GET_REASON(ssl_code)),
                               ssl_code_desc.c_str());
                if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                    if constexpr (requires { owner.drive_events(); })
                        owner.drive_events();
                    uint32_t mask = 0;
                    if (ssl_err == SSL_ERROR_WANT_READ)
                        mask |= EPOLLIN;
                    if (ssl_err == SSL_ERROR_WANT_WRITE)
                        mask |= EPOLLOUT;
                    CO_WQ_LOG_INFO("[tls handshake] wait events ssl=%p mask=%u err=%d(%s) sys=%d pending=%d",
                                   static_cast<void*>(owner._ssl.get()),
                                   mask,
                                   ssl_err,
                                   detail::ssl_error_name(ssl_err),
                                   sys_err,
                                   pending_err);
                    this->func = &handshake_awaiter::resume_cb;
                    CO_WQ_LOG_INFO("[tls handshake] set resume func before wait self=%p func=%p",
                                   static_cast<void*>(this),
                                   reinterpret_cast<void*>(this->func));
                    owner.wait_io(this, mask, first_wait);
                    CO_WQ_LOG_INFO("[tls handshake] resume func after wait self=%p func=%p",
                                   static_cast<void*>(this),
                                   reinterpret_cast<void*>(this->func));
                    armed      = true;
                    first_wait = false;
                    return;
                }
                CO_WQ_LOG_ERROR("[tls handshake] failure rc=%d err=%d sys=%d ssl=0x%lx state=%s stack=%s",
                                rc,
                                ssl_err,
                                sys_err,
                                static_cast<unsigned long>(ssl_code),
                                SSL_state_string_long(owner._ssl.get()),
                                detail::collect_errors().c_str());
                result = detail::translate_failure(ssl_err);
                armed  = false;
                break;
            }
            CO_WQ_LOG_INFO("[tls handshake] drive exit ssl=%p done=%d result=%d",
                           static_cast<void*>(owner._ssl.get()),
                           owner._handshake_done ? 1 : 0,
                           result);
            armed = false;
            if (this->h)
                this->h.resume();
        }

        static void resume_cb(worknode* w)
        {
            auto* self = static_cast<handshake_awaiter*>(w);
            CO_WQ_LOG_INFO("[tls handshake] resume_cb(handshake) self=%p ssl=%p armed=%d",
                           static_cast<void*>(self),
                           static_cast<void*>(self->owner._ssl.get()),
                           self->armed ? 1 : 0);
            self->owner.on_waiter_resumed(self);
            self->drive();
        }

        int await_resume() const noexcept
        {
            if (armed)
                const_cast<handshake_awaiter*>(this)->owner.cancel_wait(const_cast<handshake_awaiter*>(this));
            if (!owner._handshake_done) {
                if (result == 0) {
                    auto*       ssl           = owner._ssl.get();
                    int         shutdown_flag = ssl ? SSL_get_shutdown(ssl) : 0;
                    int         pending_err   = ssl ? SSL_get_error(ssl, 0) : 0;
                    int         sys_err       = errno;
                    const char* state         = ssl ? SSL_state_string_long(ssl) : "<null>";
                    auto        err_code      = ERR_peek_error();
                    auto        err_desc      = detail::format_error_code(err_code);
                    CO_WQ_LOG_WARN(
                        "[tls handshake] await_resume abort result=0 shutdown=0x%x pending_err=%d(%s) sys=%d "
                        "state=%s lib=%u reason=%u code=%s stack=%s",
                        shutdown_flag,
                        pending_err,
                        detail::ssl_error_name(pending_err),
                        sys_err,
                        state,
                        static_cast<unsigned>(ERR_GET_LIB(err_code)),
                        static_cast<unsigned>(ERR_GET_REASON(err_code)),
                        err_desc.c_str(),
                        detail::collect_errors().c_str());
                }
                return result == 0 ? -ECONNRESET : result;
            }
            return 0;
        }
    };

    handshake_awaiter handshake() { return handshake_awaiter(*this); }

    struct recv_awaiter : tp_base<recv_awaiter> {
        basic_tls_socket& owner;
        void*             buf;
        size_t            len;
        size_t            recvd { 0 };
        ssize_type        err { 0 };
        bool              full { false };
        uint32_t          wait_mask { 0 };

        recv_awaiter(basic_tls_socket& s, void* b, size_t l, bool f)
            : tp_base<recv_awaiter>(s, s._recv_q), owner(s), buf(b), len(l), full(f)
        {
            this->store_route_guard(owner._cbq->retain_guard());
            this->route_ctx  = owner._cbq->context();
            this->route_post = &callback_wq<lock>::post_adapter;
        }

        static void register_wait(recv_awaiter* self, bool first)
        {
            uint32_t mask = self->wait_mask ? self->wait_mask : EPOLLIN;
            self->owner.wait_io(self, mask, first);
        }

        int attempt_once()
        {
            if constexpr (requires { owner.drive_events(); })
                owner.drive_events();
            if (recvd >= len)
                return 0;
            int rc = SSL_read(owner._ssl.get(), static_cast<char*>(buf) + recvd, static_cast<int>(len - recvd));
            if (rc > 0) {
                recvd += static_cast<size_t>(rc);
                if (!full)
                    return 0;
                return recvd == len ? 0 : 1;
            }
            int ssl_err = SSL_get_error(owner._ssl.get(), rc);
            if (ssl_err == SSL_ERROR_SSL) {
                unsigned long err_code = ERR_peek_last_error();
                if (ERR_GET_LIB(err_code) == ERR_LIB_SSL && ERR_GET_REASON(err_code) == SSL_R_PROTOCOL_IS_SHUTDOWN) {
                    int shut = SSL_get_shutdown(owner._ssl.get());
                    ERR_clear_error();
                    if ((shut & SSL_RECEIVED_SHUTDOWN) != 0) {
                        owner._rx_eof = true;
                        return 0;
                    }
                    wait_mask = EPOLLIN;
                    return -1;
                }
            }
            if (ssl_err == SSL_ERROR_ZERO_RETURN) {
                int shut = SSL_get_shutdown(owner._ssl.get());
                if ((shut & SSL_RECEIVED_SHUTDOWN) == 0) {
                    wait_mask = EPOLLIN;
                    ERR_clear_error();
                    return -1;
                }
                owner._rx_eof = true;
                ERR_clear_error();
                return 0;
            }
            if (rc == 0 && ssl_err == SSL_ERROR_SYSCALL && errno == 0) {
                int shut = SSL_get_shutdown(owner._ssl.get());
                if ((shut & SSL_RECEIVED_SHUTDOWN) == 0) {
                    wait_mask = EPOLLIN;
                    ERR_clear_error();
                    return -1;
                }
                owner._rx_eof = true;
                ERR_clear_error();
                return 0;
            }
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                wait_mask = 0;
                if (ssl_err == SSL_ERROR_WANT_READ)
                    wait_mask |= EPOLLIN;
                if (ssl_err == SSL_ERROR_WANT_WRITE)
                    wait_mask |= EPOLLOUT;
                return -1;
            }
            err = detail::translate_failure(ssl_err);
            return 0;
        }

        ssize_type await_resume() const noexcept
        {
            if (err < 0 && recvd == 0)
                return err;
            return static_cast<ssize_type>(recvd);
        }
    };

    recv_awaiter recv(void* buf, size_t len) { return recv_awaiter(*this, buf, len, false); }
    recv_awaiter recv_all(void* buf, size_t len) { return recv_awaiter(*this, buf, len, true); }

    struct send_awaiter : tp_base<send_awaiter> {
        basic_tls_socket& owner;
        const void*       buf;
        size_t            len;
        size_t            sent { 0 };
        ssize_type        err { 0 };
        bool              full { false };
        uint32_t          wait_mask { 0 };

        send_awaiter(basic_tls_socket& s, const void* b, size_t l, bool f)
            : tp_base<send_awaiter>(s, s._send_q), owner(s), buf(b), len(l), full(f)
        {
            this->store_route_guard(owner._cbq->retain_guard());
            this->route_ctx  = owner._cbq->context();
            this->route_post = &callback_wq<lock>::post_adapter;
        }

        static void register_wait(send_awaiter* self, bool first)
        {
            uint32_t mask = self->wait_mask ? self->wait_mask : EPOLLOUT;
            self->owner.wait_io(self, mask, first);
        }

        int attempt_once()
        {
            if constexpr (requires { owner.drive_events(); })
                owner.drive_events();
            if (sent >= len)
                return 0;
            int rc = SSL_write(owner._ssl.get(), static_cast<const char*>(buf) + sent, static_cast<int>(len - sent));
            if (rc > 0) {
                sent += static_cast<size_t>(rc);
                if (!full)
                    return 0;
                return sent == len ? 0 : 1;
            }
            int ssl_err = SSL_get_error(owner._ssl.get(), rc);
            if (ssl_err == SSL_ERROR_SSL) {
                unsigned long err_code = ERR_peek_last_error();
                if (ERR_GET_LIB(err_code) == ERR_LIB_SSL && ERR_GET_REASON(err_code) == SSL_R_PROTOCOL_IS_SHUTDOWN) {
                    int shut = SSL_get_shutdown(owner._ssl.get());
                    ERR_clear_error();
                    if ((shut & SSL_RECEIVED_SHUTDOWN) != 0) {
                        owner._tx_shutdown = true;
                        return 0;
                    }
                    wait_mask = EPOLLOUT;
                    return -1;
                }
            }
            if (ssl_err == SSL_ERROR_ZERO_RETURN) {
                int shut = SSL_get_shutdown(owner._ssl.get());
                if ((shut & SSL_RECEIVED_SHUTDOWN) == 0) {
                    wait_mask = EPOLLOUT;
                    ERR_clear_error();
                    return -1;
                }
                owner._tx_shutdown = true;
                ERR_clear_error();
                return 0;
            }
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                wait_mask = 0;
                if (ssl_err == SSL_ERROR_WANT_READ)
                    wait_mask |= EPOLLIN;
                if (ssl_err == SSL_ERROR_WANT_WRITE)
                    wait_mask |= EPOLLOUT;
                return -1;
            }
            if (rc == 0 && ssl_err == SSL_ERROR_SYSCALL && errno == 0) {
                int shut = SSL_get_shutdown(owner._ssl.get());
                if ((shut & SSL_SENT_SHUTDOWN) == 0) {
                    wait_mask = EPOLLOUT;
                    ERR_clear_error();
                    return -1;
                }
                owner._tx_shutdown = true;
                ERR_clear_error();
                return 0;
            }
            err = detail::translate_failure(ssl_err);
            return 0;
        }

        ssize_type await_resume() const noexcept
        {
            if (err < 0 && sent == 0)
                return err;
            return static_cast<ssize_type>(sent);
        }
    };

    send_awaiter send(const void* buf, size_t len) { return send_awaiter(*this, buf, len, false); }
    send_awaiter send_all(const void* buf, size_t len) { return send_awaiter(*this, buf, len, true); }

protected:
    virtual void drive_events() { }

    virtual void wait_io(io_waiter_base* waiter, uint32_t mask, bool)
    {
        uint32_t events = mask ? mask : (EPOLLIN | EPOLLOUT);
        _reactor->add_waiter_custom(io_handle(), events, waiter);
    }

    virtual void on_waiter_resumed(io_waiter_base*) { }

    virtual void cancel_wait(io_waiter_base* waiter) { _reactor->cancel_waiter(io_handle(), waiter); }

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

#if !defined(OPENSSL_NO_QUIC)
template <lockable lock, template <class> class Reactor = epoll_reactor>
class quic_socket : public basic_tls_socket<lock, Reactor, udp_socket<lock, Reactor>, quic_context> {
    using base_type = basic_tls_socket<lock, Reactor, udp_socket<lock, Reactor>, quic_context>;

public:
    using typename base_type::handshake_awaiter;
    using typename base_type::recv_awaiter;
    using typename base_type::send_awaiter;
    using adopt_ssl_tag                      = typename base_type::adopt_ssl_tag;
    static constexpr adopt_ssl_tag adopt_ssl = base_type::adopt_ssl;
    using dispatcher_type                    = quic_dispatcher<lock, Reactor>;
    using channel_ptr                        = typename dispatcher_type::channel_ptr;

    template <class D> using tp_base = typename base_type::template tp_base<D>;

    quic_socket(workqueue<lock>&            exec,
                Reactor<lock>&              reactor,
                udp_socket<lock, Reactor>&& udp,
                quic_context                ctx,
                quic_mode                   mode)
        : base_type(exec, reactor, std::move(udp), std::move(ctx), "SSL_new (quic) failed")
    {
        setup_quic_handle(mode, false);
        if (!ensure_default_dispatcher())
            throw std::runtime_error("failed to initialize QUIC dispatcher");
    }

    quic_socket(workqueue<lock>&            exec,
                Reactor<lock>&              reactor,
                udp_socket<lock, Reactor>&& udp,
                quic_context                ctx,
                SSL*                        adopted_ssl,
                quic_mode                   mode,
                adopt_ssl_tag)
        : base_type(exec, reactor, std::move(udp), std::move(ctx), adopted_ssl, adopt_ssl)
    {
        setup_quic_handle(mode, true);
        _allow_embedded_dispatcher = false;
    }

    quic_socket(const quic_socket&)            = delete;
    quic_socket& operator=(const quic_socket&) = delete;

    quic_socket(quic_socket&& other) noexcept
        : base_type(std::move(other))
        , _owned_dispatcher(std::move(other._owned_dispatcher))
        , _channel(std::move(other._channel))
        , _allow_embedded_dispatcher(other._allow_embedded_dispatcher)
        , _has_initial_peer(other._has_initial_peer)
        , _initial_peer_len(other._initial_peer_len)
    {
        if (_has_initial_peer)
            std::memcpy(&_initial_peer, &other._initial_peer, static_cast<size_t>(_initial_peer_len));
        else
            std::memset(&_initial_peer, 0, sizeof(_initial_peer));
        if (_owned_dispatcher)
            _dispatcher = _owned_dispatcher.get();
        else
            _dispatcher = other._dispatcher;
        other._dispatcher       = nullptr;
        other._has_initial_peer = false;
        other._initial_peer_len = 0;
        std::memset(&other._initial_peer, 0, sizeof(other._initial_peer));
    }

    quic_socket& operator=(quic_socket&& other) noexcept
    {
        if (this != &other) {
            detach_dispatcher();
            base_type::operator=(std::move(other));
            _owned_dispatcher          = std::move(other._owned_dispatcher);
            _channel                   = std::move(other._channel);
            _allow_embedded_dispatcher = other._allow_embedded_dispatcher;
            _has_initial_peer          = other._has_initial_peer;
            _initial_peer_len          = other._initial_peer_len;
            if (_has_initial_peer)
                std::memcpy(&_initial_peer, &other._initial_peer, static_cast<size_t>(_initial_peer_len));
            else
                std::memset(&_initial_peer, 0, sizeof(_initial_peer));
            if (_owned_dispatcher)
                _dispatcher = _owned_dispatcher.get();
            else
                _dispatcher = other._dispatcher;
            other._dispatcher       = nullptr;
            other._has_initial_peer = false;
            other._initial_peer_len = 0;
            std::memset(&other._initial_peer, 0, sizeof(other._initial_peer));
        }
        return *this;
    }

    ~quic_socket() { detach_dispatcher(); }

    void drive_events() override
    {
        SSL* ssl = this->ssl_handle();
        if (!ssl)
            return;
        if (!_dispatcher) {
            CO_WQ_LOG_ERROR("[quic socket] dispatcher not attached for drive_events ssl=%p", static_cast<void*>(ssl));
            return;
        }
        _dispatcher->drive_session(ssl);
    }

    bool attach_dispatcher(dispatcher_type& dispatcher)
    {
        SSL* ssl = this->ssl_handle();
        if (!ssl)
            return false;
        if (_dispatcher && _dispatcher != &dispatcher)
            detach_dispatcher();
        if (_dispatcher == &dispatcher && _channel)
            return true;
        const sockaddr* peer     = _has_initial_peer ? reinterpret_cast<const sockaddr*>(&_initial_peer) : nullptr;
        socklen_t       peer_len = _has_initial_peer ? _initial_peer_len : 0;
        auto            channel  = dispatcher.attach_session_bio(ssl, peer, peer_len);
        if (!channel)
            return false;
        _dispatcher = &dispatcher;
        _channel    = std::move(channel);
        if (_owned_dispatcher && _owned_dispatcher.get() != &dispatcher)
            _owned_dispatcher.reset();
        return true;
    }

    void detach_dispatcher() noexcept
    {
        SSL*  ssl     = this->ssl_handle();
        auto* current = _dispatcher;
        if (current && ssl)
            current->unregister_session(ssl);
        _channel.reset();
        if (_owned_dispatcher && _owned_dispatcher.get() == current)
            _owned_dispatcher.reset();
        _dispatcher = nullptr;
    }

    dispatcher_type* dispatcher() const noexcept { return _dispatcher; }

    bool set_initial_peer(const sockaddr* addr, socklen_t len)
    {
        if (!addr || len <= 0)
            return false;
        BIO_ADDR* bio = BIO_ADDR_new();
        if (!bio)
            return false;
        bool success = false;
        switch (addr->sa_family) {
        case AF_INET: {
            const auto* in = reinterpret_cast<const sockaddr_in*>(addr);
            success        = BIO_ADDR_rawmake(bio, AF_INET, &in->sin_addr, sizeof(in->sin_addr), ntohs(in->sin_port));
            break;
        }
        case AF_INET6: {
            const auto* in6 = reinterpret_cast<const sockaddr_in6*>(addr);
            success = BIO_ADDR_rawmake(bio, AF_INET6, &in6->sin6_addr, sizeof(in6->sin6_addr), ntohs(in6->sin6_port));
            break;
        }
        default:
            break;
        }
        if (success)
            success = (SSL_set1_initial_peer_addr(this->ssl_handle(), bio) == 1);
        BIO_ADDR_free(bio);
        if (success) {
            if (len <= static_cast<socklen_t>(sizeof(_initial_peer))) {
                std::memcpy(&_initial_peer, addr, static_cast<size_t>(len));
                _initial_peer_len = len;
                _has_initial_peer = true;
            }
            if (_dispatcher) {
                _channel = _dispatcher->register_session(this->ssl_handle(), addr, len);
            }
        }
        return success;
    }

private:
    void setup_quic_handle(quic_mode mode, bool adopted)
    {
        SSL* ssl = this->ssl_handle();
        if (!ssl)
            throw std::runtime_error("invalid QUIC SSL handle");
        (void)adopted;
        SSL_set_blocking_mode(ssl, 0);
        SSL_set_event_handling_mode(ssl, SSL_VALUE_EVENT_HANDLING_MODE_IMPLICIT);
        SSL_set_default_stream_mode(ssl, SSL_DEFAULT_STREAM_MODE_AUTO_BIDI);
        if (mode == quic_mode::Client)
            SSL_set_connect_state(ssl);
        else
            SSL_set_accept_state(ssl);
    }

    bool ensure_default_dispatcher()
    {
        if (_dispatcher)
            return true;
        if (!_allow_embedded_dispatcher)
            return false;
        auto* reactor_ptr = this->reactor();
        if (!reactor_ptr)
            return false;
        auto owned = std::make_unique<dispatcher_type>(this->exec(), *reactor_ptr, this->underlying());
        if (!owned)
            return false;
        if (!attach_dispatcher(*owned))
            return false;
        _owned_dispatcher = std::move(owned);
        return true;
    }

protected:
    void wait_io(io_waiter_base* waiter, uint32_t mask, bool first) override
    {
        if (!_dispatcher) {
            CO_WQ_LOG_ERROR("[quic socket] dispatcher not attached for wait_io ssl=%p",
                            static_cast<void*>(this->ssl_handle()));
            base_type::wait_io(waiter, mask, first);
            return;
        }
        _dispatcher->track_waiter(waiter);
        auto timeout = std::chrono::milliseconds(200);
        (void)first;
        _dispatcher->wait_with_timeout(waiter, mask, timeout);
    }

    void on_waiter_resumed(io_waiter_base* waiter) override
    {
        if (!_dispatcher) {
            CO_WQ_LOG_ERROR("[quic socket] dispatcher not attached for on_waiter_resumed ssl=%p",
                            static_cast<void*>(this->ssl_handle()));
            return;
        }
        _dispatcher->untrack_waiter(waiter);
    }

    void cancel_wait(io_waiter_base* waiter) override
    {
        if (_dispatcher)
            _dispatcher->untrack_waiter(waiter);
        base_type::cancel_wait(waiter);
    }

private:
    std::unique_ptr<dispatcher_type> _owned_dispatcher;
    dispatcher_type*                 _dispatcher { nullptr };
    channel_ptr                      _channel;
    bool                             _allow_embedded_dispatcher { true };
    bool                             _has_initial_peer { false };
    sockaddr_storage                 _initial_peer {};
    socklen_t                        _initial_peer_len { 0 };
};
#endif

} // namespace co_wq::net
