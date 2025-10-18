#pragma once

#if !defined(OPENSSL_NO_QUIC)

#include "callback_wq.hpp"
#include "io_serial.hpp"
#include "quic_dispatcher.hpp"
#include "tls.hpp"

#include "task.hpp"
#include "worker.hpp"

#include <openssl/err.h>
#include <openssl/quic.h>
#include <openssl/ssl.h>

#include <cerrno>
#include <chrono>
#include <coroutine>
#include <memory>
#include <stdexcept>

namespace co_wq::net {

/**
 * @brief 基于 `quic_dispatcher` 驱动的 QUIC 会话封装，提供握手与流式读写协程。
 */
template <lockable lock, template <class> class Reactor> class quic_stream_session {
public:
    using dispatcher_type            = quic_dispatcher<lock, Reactor>;
    using channel_ptr                = typename dispatcher_type::channel_ptr;
    template <class D> using tp_base = two_phase_drain_awaiter<D, quic_stream_session>;

    quic_stream_session(dispatcher_type& dispatcher, quic_context ctx, SSL* ssl, quic_mode mode, bool insecure)
        : _dispatcher(dispatcher)
        , _exec(dispatcher.exec())
        , _ctx(std::move(ctx))
        , _ssl(ssl, &SSL_free)
        , _cbq(std::make_unique<callback_wq<lock>>(_exec))
    {
        if (!_ssl)
            throw std::runtime_error("invalid QUIC session handle");
        SSL_set_blocking_mode(_ssl.get(), 0);
        SSL_set_event_handling_mode(_ssl.get(), SSL_VALUE_EVENT_HANDLING_MODE_IMPLICIT);
        SSL_set_default_stream_mode(_ssl.get(), SSL_DEFAULT_STREAM_MODE_AUTO_BIDI);
        if (mode == quic_mode::Client)
            SSL_set_connect_state(_ssl.get());
        else
            SSL_set_accept_state(_ssl.get());
        if (insecure)
            SSL_set_verify(_ssl.get(), SSL_VERIFY_NONE, nullptr);
        if (_ctx.trace_enabled())
            detail::setup_ssl_message_logging(_ssl.get(), this);
        _channel = _dispatcher.attach_session_bio(_ssl.get());
        if (!_channel)
            throw std::runtime_error("failed to attach QUIC session BIO");
        _dispatcher.promote_listener_peer(_ssl.get());
        serial_queue_init(_recv_q);
        serial_queue_init(_send_q);
    }

    quic_stream_session(const quic_stream_session&)            = delete;
    quic_stream_session& operator=(const quic_stream_session&) = delete;
    ~quic_stream_session() { close(); }
    quic_stream_session(quic_stream_session&& other) noexcept
        : _dispatcher(other._dispatcher)
        , _exec(other._exec)
        , _ctx(std::move(other._ctx))
        , _ssl(std::move(other._ssl))
        , _cbq(std::move(other._cbq))
        , _channel(std::move(other._channel))
        , _handshake_done(other._handshake_done)
        , _rx_eof(other._rx_eof)
        , _tx_shutdown(other._tx_shutdown)
    {
        other._handshake_done = false;
        other._rx_eof         = false;
        other._tx_shutdown    = false;
        if (_ssl && !_channel) {
            _channel = _dispatcher.attach_session_bio(_ssl.get());
            if (!_channel)
                throw std::runtime_error("failed to attach QUIC session BIO");
        }
        serial_queue_init(_recv_q);
        serial_queue_init(_send_q);
    }

    quic_stream_session& operator=(quic_stream_session&& other) noexcept
    {
        if (this != &other) {
            close();
            _ctx                  = std::move(other._ctx);
            _ssl                  = std::move(other._ssl);
            _cbq                  = std::move(other._cbq);
            _channel              = std::move(other._channel);
            _handshake_done       = other._handshake_done;
            _rx_eof               = other._rx_eof;
            _tx_shutdown          = other._tx_shutdown;
            other._handshake_done = false;
            other._rx_eof         = false;
            other._tx_shutdown    = false;
            if (_ssl && !_channel) {
                _channel = _dispatcher.attach_session_bio(_ssl.get());
                if (!_channel)
                    throw std::runtime_error("failed to attach QUIC session BIO");
            }
        }
        serial_queue_init(_recv_q);
        serial_queue_init(_send_q);
        return *this;
    }

    workqueue<lock>& exec() noexcept { return _exec; }
    SSL*             ssl_handle() const noexcept { return _ssl.get(); }
    bool             rx_eof() const noexcept { return _rx_eof; }
    bool             tx_shutdown() const noexcept { return _tx_shutdown; }
    lock&            serial_lock() noexcept { return _serial_lock; }

    Task<int, Work_Promise<lock, int>> handshake()
    {
        while (!_handshake_done) {
            _dispatcher.drive_listener();
            _dispatcher.drive_session(_ssl.get());
            int rc = SSL_do_handshake(_ssl.get());
            if (rc == 1) {
                _handshake_done = true;
                co_return 0;
            }
            int ssl_err = SSL_get_error(_ssl.get(), rc);
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                uint32_t mask = _dispatcher.wait_mask(_ssl.get());
                co_await make_waiter(*this, mask);
                continue;
            }
            unsigned long err_code = ERR_peek_last_error();
            char          err_buffer[256] {};
            if (err_code != 0)
                ERR_error_string_n(err_code, err_buffer, sizeof(err_buffer));
            CO_WQ_LOG_ERROR("[quic session] handshake fatal ssl=%p ssl_err=%d errno=%d state=%s err=%s",
                            static_cast<void*>(_ssl.get()),
                            ssl_err,
                            errno,
                            SSL_state_string_long(_ssl.get()),
                            err_buffer);
            co_return detail::translate_failure(ssl_err);
        }
        co_return 0;
    }

    struct recv_awaiter : tp_base<recv_awaiter> {
        quic_stream_session& owner;
        void*                buf;
        size_t               len;
        size_t               recvd { 0 };
        ssize_type           err { 0 };
        uint32_t             wait_mask { 0 };

        recv_awaiter(quic_stream_session& s, void* b, size_t l)
            : tp_base<recv_awaiter>(s, s._recv_q), owner(s), buf(b), len(l)
        {
            this->store_route_guard(owner._cbq->retain_guard());
            this->route_ctx  = owner._cbq->context();
            this->route_post = &callback_wq<lock>::post_adapter;
            this->debug_name = "quic_session_recv";
        }

        static void register_wait(recv_awaiter* self, bool first)
        {
            uint32_t mask = self->wait_mask ? self->wait_mask
                                            : self->owner._dispatcher.wait_mask(self->owner._ssl.get());
            self->owner.wait_io(self, mask, first);
        }

        int attempt_once()
        {
            if (!buf || len == 0) {
                recvd = 0;
                return 0;
            }
            if (owner._rx_eof)
                return 0;
            wait_mask = 0;
            owner._dispatcher.drive_listener();
            owner._dispatcher.drive_session(owner._ssl.get());
            int rc = SSL_read(owner._ssl.get(), static_cast<char*>(buf), static_cast<int>(len));
            if (rc > 0) {
                recvd = static_cast<size_t>(rc);
                return 0;
            }
            int ssl_err = SSL_get_error(owner._ssl.get(), rc);
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                wait_mask = owner._dispatcher.wait_mask(owner._ssl.get());
                return -1;
            }
            if (ssl_err == SSL_ERROR_ZERO_RETURN || (rc == 0 && ssl_err == SSL_ERROR_SYSCALL && errno == 0)) {
                int shut_flags = SSL_get_shutdown(owner._ssl.get());
                if ((shut_flags & SSL_RECEIVED_SHUTDOWN) != 0) {
                    owner._rx_eof = true;
                    ERR_clear_error();
                    recvd = 0;
                    return 0;
                }
                ERR_clear_error();
                wait_mask = owner._dispatcher.wait_mask(owner._ssl.get());
                return -1;
            }
            if (ssl_err == SSL_ERROR_SSL) {
                unsigned long err_code = ERR_peek_last_error();
                if (ERR_GET_LIB(err_code) == ERR_LIB_SSL) {
                    int reason = ERR_GET_REASON(err_code);
                    if (reason == SSL_R_PROTOCOL_IS_SHUTDOWN) {
                        ERR_clear_error();
                        wait_mask = owner._dispatcher.wait_mask(owner._ssl.get());
                        return -1;
                    }
                    if (reason == SSL_R_QUIC_NETWORK_ERROR) {
                        owner._rx_eof = true;
                        ERR_clear_error();
                        recvd = 0;
                        return 0;
                    }
                }
            }
            auto translated = detail::translate_failure(ssl_err);
            if (translated == -EBADF || translated == -ECONNRESET || translated == -EPIPE || translated == -EIO
                || translated == -ENOENT) {
                owner._rx_eof = true;
                recvd         = 0;
                return 0;
            }
            err = translated;
            return 0;
        }

        ssize_type await_resume() const noexcept
        {
            if (err < 0 && recvd == 0)
                return err;
            return static_cast<ssize_type>(recvd);
        }
    };

    recv_awaiter recv(void* buf, size_t len) { return recv_awaiter(*this, buf, len); }

    struct send_awaiter : tp_base<send_awaiter> {
        quic_stream_session& owner;
        const void*          buf;
        size_t               len;
        size_t               sent { 0 };
        ssize_type           err { 0 };
        uint32_t             wait_mask { 0 };

        send_awaiter(quic_stream_session& s, const void* b, size_t l)
            : tp_base<send_awaiter>(s, s._send_q), owner(s), buf(b), len(l)
        {
            this->store_route_guard(owner._cbq->retain_guard());
            this->route_ctx  = owner._cbq->context();
            this->route_post = &callback_wq<lock>::post_adapter;
            this->debug_name = "quic_session_send";
        }

        static void register_wait(send_awaiter* self, bool first)
        {
            uint32_t mask = self->wait_mask ? self->wait_mask
                                            : self->owner._dispatcher.wait_mask(self->owner._ssl.get());
            self->owner.wait_io(self, mask, first);
        }

        int attempt_once()
        {
            if (!buf || len == 0)
                return 0;
            if (sent >= len)
                return 0;
            if (owner._tx_shutdown)
                return 0;
            wait_mask = 0;
            owner._dispatcher.drive_listener();
            owner._dispatcher.drive_session(owner._ssl.get());
            int rc = SSL_write(owner._ssl.get(), static_cast<const char*>(buf) + sent, static_cast<int>(len - sent));
            if (rc > 0) {
                sent += static_cast<size_t>(rc);
                return sent == len ? 0 : 1;
            }
            int ssl_err = SSL_get_error(owner._ssl.get(), rc);
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                wait_mask = owner._dispatcher.wait_mask(owner._ssl.get());
                return -1;
            }
            if (ssl_err == SSL_ERROR_ZERO_RETURN || (rc == 0 && ssl_err == SSL_ERROR_SYSCALL && errno == 0)) {
                int shut_flags = SSL_get_shutdown(owner._ssl.get());
                if ((shut_flags & SSL_SENT_SHUTDOWN) != 0) {
                    owner._tx_shutdown = true;
                    ERR_clear_error();
                    return 0;
                }
                ERR_clear_error();
                wait_mask = owner._dispatcher.wait_mask(owner._ssl.get());
                return -1;
            }
            if (ssl_err == SSL_ERROR_SSL) {
                unsigned long err_code = ERR_peek_last_error();
                if (ERR_GET_LIB(err_code) == ERR_LIB_SSL) {
                    int reason = ERR_GET_REASON(err_code);
                    if (reason == SSL_R_PROTOCOL_IS_SHUTDOWN) {
                        ERR_clear_error();
                        wait_mask = owner._dispatcher.wait_mask(owner._ssl.get());
                        return -1;
                    }
                    if (reason == SSL_R_QUIC_NETWORK_ERROR) {
                        owner._tx_shutdown = true;
                        ERR_clear_error();
                        return 0;
                    }
                }
            }
            auto translated = detail::translate_failure(ssl_err);
            if (translated == -EBADF || translated == -ECONNRESET || translated == -EPIPE || translated == -EIO
                || translated == -ENOENT) {
                owner._tx_shutdown = true;
                return 0;
            }
            err = translated;
            return 0;
        }

        ssize_type await_resume() const noexcept
        {
            if (err < 0 && sent == 0)
                return err;
            return static_cast<ssize_type>(sent);
        }
    };

    send_awaiter send_all(const void* buf, size_t len) { return send_awaiter(*this, buf, len); }

    void wait_io(io_waiter_base* waiter, uint32_t mask, bool first)
    {
        if (!waiter)
            return;
        auto events  = mask ? mask : _dispatcher.wait_mask(_ssl.get());
        auto timeout = std::chrono::milliseconds(200);
        (void)first;
        _dispatcher.track_waiter(waiter);
        _dispatcher.wait_with_timeout(waiter, events, timeout);
    }

    void on_waiter_resumed(io_waiter_base* waiter)
    {
        if (waiter)
            _dispatcher.untrack_waiter(waiter);
    }

    void shutdown_tx()
    {
        if (_ssl)
            SSL_shutdown(_ssl.get());
        _tx_shutdown = true;
    }

    void close()
    {
        if (_ssl) {
            _dispatcher.unregister_session(_ssl.get());
            _ssl.reset();
        }
        _channel.reset();
        _rx_eof      = true;
        _tx_shutdown = true;
    }

private:
    struct waiter : io_waiter_base {
        quic_stream_session& owner;
        uint32_t             mask;

        waiter(quic_stream_session& s, uint32_t m) : owner(s), mask(m)
        {
            this->store_route_guard(owner._cbq->retain_guard());
            this->route_ctx  = owner._cbq->context();
            this->route_post = &callback_wq<lock>::post_adapter;
            this->debug_name = "quic_session_wait";
        }

        bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<> handle)
        {
            this->h = handle;
            owner._dispatcher.track_waiter(this);
            owner._dispatcher.wait_with_timeout(this, mask, std::chrono::milliseconds(200));
        }

        void await_resume() noexcept { owner._dispatcher.untrack_waiter(this); }
    };

    static waiter make_waiter(quic_stream_session& session, uint32_t mask)
    {
        return waiter(session, mask ? mask : (EPOLLIN | EPOLLOUT));
    }

    dispatcher_type&                          _dispatcher;
    workqueue<lock>&                          _exec;
    quic_context                              _ctx;
    std::unique_ptr<SSL, decltype(&SSL_free)> _ssl;
    std::unique_ptr<callback_wq<lock>>        _cbq;
    channel_ptr                               _channel;
    bool                                      _handshake_done { false };
    bool                                      _rx_eof { false };
    bool                                      _tx_shutdown { false };
    lock                                      _serial_lock;
    serial_queue                              _recv_q;
    serial_queue                              _send_q;
};

} // namespace co_wq::net

#endif // !OPENSSL_NO_QUIC
