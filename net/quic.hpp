#pragma once

#include "callback_wq.hpp"
#include "io_serial.hpp"
#include "msquic_loader.hpp"
#include "workqueue.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace co_wq::net {

template <lockable lock> class quic_socket;
template <lockable lock> class quic_listener;

template <lockable lock> struct quic_connection_state;

struct quic_context : std::enable_shared_from_this<quic_context> {
    std::shared_ptr<MsquicApi> api;
    MsquicRegistrationHandle   registration {};
    MsquicConfigurationHandle  configuration {};

    quic_context(std::shared_ptr<MsquicApi> api_ptr, MsquicRegistrationHandle reg, MsquicConfigurationHandle cfg)
        : api(std::move(api_ptr)), registration(reg), configuration(cfg)
    {
    }

    ~quic_context()
    {
        if (api) {
            if (configuration.value) {
                api->configuration_close(configuration);
                configuration = {};
            }
            if (registration.value) {
                api->registration_close(registration);
                registration = {};
            }
        }
    }

    static std::shared_ptr<quic_context> create(std::shared_ptr<MsquicApi>      api_ptr,
                                                const MsquicRegistrationConfig& reg_cfg,
                                                const MsquicSettings&           settings,
                                                const MsquicConstBuffer*        alpns,
                                                std::uint32_t                   alpn_count)
    {
        if (!api_ptr) {
            throw std::invalid_argument("MsQuic api pointer is null");
        }
        MsquicRegistrationHandle reg {};
        MsquicStatus             status = api_ptr->registration_open(reg_cfg, reg);
        if (quic_status_failed(status)) {
            throw std::runtime_error("MsQuic registration_open failed");
        }

        MsquicConfigurationHandle cfg {};
        status = api_ptr->configuration_open(reg, alpns, alpn_count, settings, cfg);
        if (quic_status_failed(status)) {
            api_ptr->registration_close(reg);
            throw std::runtime_error("MsQuic configuration_open failed");
        }

        return std::shared_ptr<quic_context>(new quic_context(std::move(api_ptr), reg, cfg));
    }

    MsquicStatus load_credential(const MsquicCredentialConfig& credential)
    {
        if (!api || !configuration.value) {
            return 1;
        }
        return api->configuration_load_credential(configuration, credential);
    }
};

template <lockable lock> struct quic_connection_state : std::enable_shared_from_this<quic_connection_state<lock>> {
    std::shared_ptr<quic_context> ctx;
    MsquicConnectionHandle        handle {};
    quic_listener<lock>*          owner { nullptr };

    std::mutex                                    mutex;
    bool                                          connected { false };
    std::vector<std::weak_ptr<quic_socket<lock>>> pending_handshake;

    explicit quic_connection_state(std::shared_ptr<quic_context> context, MsquicConnectionHandle h)
        : ctx(std::move(context)), handle(h)
    {
    }

    void register_pending(const std::shared_ptr<quic_socket<lock>>& sock)
    {
        bool resume_now = false;
        {
            std::lock_guard<std::mutex> guard(mutex);
            if (connected) {
                resume_now = true;
            } else {
                pending_handshake.emplace_back(sock);
            }
        }
        if (resume_now && sock)
            sock->mark_handshake_done(0);
    }

    void on_connected()
    {
        std::vector<std::weak_ptr<quic_socket<lock>>> waiters;
        {
            std::lock_guard<std::mutex> guard(mutex);
            connected = true;
            waiters   = pending_handshake;
            pending_handshake.clear();
        }
        for (auto& weak : waiters) {
            if (auto s = weak.lock())
                s->mark_handshake_done(0);
        }
    }

    void on_failure(int err)
    {
        std::vector<std::weak_ptr<quic_socket<lock>>> waiters;
        {
            std::lock_guard<std::mutex> guard(mutex);
            waiters = pending_handshake;
            pending_handshake.clear();
        }
        for (auto& weak : waiters) {
            if (auto s = weak.lock())
                s->mark_handshake_done(err);
        }
    }
};

template <lockable lock> class quic_socket : public std::enable_shared_from_this<quic_socket<lock>> {
public:
    using connection_state_type = quic_connection_state<lock>;

    quic_socket(const quic_socket&)            = delete;
    quic_socket& operator=(const quic_socket&) = delete;

    struct handshake_awaiter : io_waiter_base {
        quic_socket& owner;
        explicit handshake_awaiter(quic_socket& s) : owner(s) { this->set_debug_name("quic_handshake"); }

        bool await_ready() const noexcept { return owner._handshake_done; }

        void await_suspend(std::coroutine_handle<> h)
        {
            this->h = h;
            this->store_route_guard(owner._cbq->retain_guard());
            this->route_ctx  = owner._cbq->context();
            this->route_post = &callback_wq<lock>::post_adapter;
            this->func       = &io_waiter_base::resume_cb;
            owner.register_handshake_waiter(this);
        }

        int await_resume() const noexcept { return owner._handshake_result; }
    };

    struct recv_awaiter : two_phase_drain_awaiter<recv_awaiter, quic_socket> {
        quic_socket& owner;
        void*        buffer;
        size_t       length;
        size_t       received { 0 };
        ssize_t      err { 0 };
        bool         full { false };

        recv_awaiter(quic_socket& s, void* buf, size_t len, bool require_full)
            : two_phase_drain_awaiter<recv_awaiter, quic_socket>(s, s._recv_q)
            , owner(s)
            , buffer(buf)
            , length(len)
            , full(require_full)
        {
            this->set_debug_name("quic_recv");
            this->store_route_guard(owner._cbq->retain_guard());
            this->route_ctx  = owner._cbq->context();
            this->route_post = &callback_wq<lock>::post_adapter;
        }

        static void register_wait(recv_awaiter* self, bool /*first*/) { self->owner.register_recv_waiter(self); }

        int attempt_once()
        {
            if (length == 0)
                return 0;

            size_t                     copied          = 0;
            bool                       fin             = false;
            ssize_t                    pending_err     = 0;
            size_t                     queue_snapshot  = 0;
            size_t                     offset_snapshot = 0;
            bool                       enable_receive  = false;
            MsquicStreamHandle         enable_handle {};
            std::shared_ptr<MsquicApi> enable_api;
            {
                std::lock_guard<std::mutex> guard(owner._state_mutex);
                copied = owner.copy_received_to(static_cast<std::uint8_t*>(buffer) + received, length - received);
                received += copied;
                fin             = owner._recv_fin && owner._recv_queue.empty();
                pending_err     = owner._recv_error;
                queue_snapshot  = owner._recv_queue.size();
                offset_snapshot = owner._recv_offset;
                if (owner._recv_queue.empty() && !owner._recv_fin && owner._recv_disabled && !owner._stream_closed
                    && owner._stream.value && owner._ctx && owner._ctx->api) {
                    enable_receive       = true;
                    enable_handle        = owner._stream;
                    enable_api           = owner._ctx->api;
                    owner._recv_disabled = false;
                }
            }

            if (enable_receive && enable_api && enable_handle.value) {
                enable_api->stream_receive_set_enabled(enable_handle, true);
                CO_WQ_LOG_DEBUG("[quic_socket] receive re-enabled");
            }

            if (copied > 0 && msquic_debug_enabled()) {
                CO_WQ_LOG_DEBUG("[quic_socket] drain copied=%zu queue=%zu offset=%zu",
                                copied,
                                queue_snapshot,
                                offset_snapshot);
            }

            if (copied > 0) {
                if (!full || received >= length)
                    return 0;
                return 1;
            }

            if (pending_err < 0) {
                if (received == 0)
                    err = pending_err;
                return 0;
            }

            if (fin)
                return 0;

            return -1;
        }

        ssize_t await_resume() const noexcept
        {
            if (err < 0 && received == 0)
                return err;
            return static_cast<ssize_t>(received);
        }
    };

    struct send_awaiter : two_phase_drain_awaiter<send_awaiter, quic_socket> {
        quic_socket&    owner;
        const void*     buffer;
        size_t          length;
        size_t          sent { 0 };
        ssize_t         err { 0 };
        bool            full { false };
        MsquicSendFlags flags { MsquicSendFlags::None };
        MsquicBuffer    descriptor {};

        send_awaiter(quic_socket& s, const void* buf, size_t len, bool require_full, MsquicSendFlags send_flags)
            : two_phase_drain_awaiter<send_awaiter, quic_socket>(s, s._send_q)
            , owner(s)
            , buffer(buf)
            , length(len)
            , full(require_full)
            , flags(send_flags)
        {
            this->set_debug_name("quic_send");
            this->store_route_guard(owner._cbq->retain_guard());
            this->route_ctx  = owner._cbq->context();
            this->route_post = &callback_wq<lock>::post_adapter;
        }

        static void register_wait(send_awaiter* self, bool /*first*/) { self->owner.register_send_waiter(self); }

        int attempt_once()
        {
            if (length == 0)
                return 0;

            const std::uint8_t* data_ptr   = nullptr;
            size_t              chunk      = 0;
            MsquicSendFlags     send_flags = MsquicSendFlags::None;

            {
                std::lock_guard<std::mutex> guard(owner._state_mutex);
                if (owner._stream_closed) {
                    err = owner._send_error < 0 ? owner._send_error : -ECONNRESET;
                    return 0;
                }

                if (!owner._send_inflight) {
                    chunk = length - sent;
                    if (chunk == 0)
                        return 0;
                    if (chunk > static_cast<size_t>(std::numeric_limits<std::uint32_t>::max())) {
                        err = -EMSGSIZE;
                        return 0;
                    }
                    data_ptr                = static_cast<const std::uint8_t*>(buffer) + sent;
                    owner._send_inflight    = true;
                    owner._send_completed   = false;
                    owner._send_error       = 0;
                    owner._send_pending_len = chunk;
                    send_flags              = ((sent + chunk) == length) ? flags : MsquicSendFlags::None;
                    if (!owner._send_started) {
                        send_flags          = send_flags | MsquicSendFlags::Start;
                        owner._send_started = true;
                    }
                    owner._send_pending_flags = send_flags;
                    if (msquic_debug_enabled()) {
                        CO_WQ_LOG_DEBUG("[quic_socket] stream_send schedule chunk=%zu flags=0x%x",
                                        chunk,
                                        static_cast<unsigned>(send_flags));
                    }
                } else {
                    if (!owner._send_completed)
                        return -1;
                    sent += owner._send_pending_len;
                    bool fin_sent = (owner._send_pending_flags & MsquicSendFlags::Fin) == MsquicSendFlags::Fin;
                    owner._send_pending_len   = 0;
                    owner._send_pending_flags = MsquicSendFlags::None;
                    owner._send_inflight      = false;
                    owner._send_completed     = false;
                    if (owner._send_error < 0) {
                        err = owner._send_error;
                        return 0;
                    }
                    if (msquic_debug_enabled()) {
                        CO_WQ_LOG_DEBUG("[quic_socket] send advance sent=%zu/%zu fin=%d",
                                        sent,
                                        length,
                                        fin_sent ? 1 : 0);
                    }
                    if (fin_sent)
                        owner._tx_fin = true;
                    if (!full || sent >= length)
                        return 0;
                    return 1;
                }
            }
            descriptor.length = static_cast<std::uint32_t>(chunk);
            descriptor.data   = const_cast<std::uint8_t*>(data_ptr);
            auto status       = owner._ctx && owner._ctx->api
                      ? owner._ctx->api->stream_send(owner._stream, &descriptor, 1, send_flags, this)
                      : static_cast<MsquicStatus>(1);
            if (quic_status_failed(status)) {
                std::lock_guard<std::mutex> guard(owner._state_mutex);
                owner._send_inflight      = false;
                owner._send_pending_len   = 0;
                owner._send_pending_flags = MsquicSendFlags::None;
                owner._send_error         = translate_status(status);
                err                       = owner._send_error;
                return 0;
            }
            return -1;
        }

        ssize_t await_resume() const noexcept
        {
            if (err < 0 && sent == 0)
                return err;
            return static_cast<ssize_t>(sent);
        }
    };

    static std::shared_ptr<quic_socket> create(workqueue<lock>&                             exec,
                                               std::shared_ptr<quic_context>                ctx,
                                               std::shared_ptr<quic_connection_state<lock>> connection,
                                               MsquicStreamHandle                           stream)
    {
        struct enable_make_shared : quic_socket {
            enable_make_shared(workqueue<lock>&                             exec,
                               std::shared_ptr<quic_context>                ctx,
                               std::shared_ptr<quic_connection_state<lock>> conn,
                               MsquicStreamHandle                           stream)
                : quic_socket(exec, std::move(ctx), std::move(conn), stream)
            {
            }
        };
        auto ptr = std::make_shared<enable_make_shared>(exec, std::move(ctx), std::move(connection), stream);
        ptr->after_construct(ptr);
        return ptr;
    }

    ~quic_socket() { close(); }

    handshake_awaiter handshake() { return handshake_awaiter(*this); }
    recv_awaiter      recv(void* buffer, size_t len) { return recv_awaiter(*this, buffer, len, false); }
    recv_awaiter      recv_all(void* buffer, size_t len) { return recv_awaiter(*this, buffer, len, true); }
    send_awaiter      send(const void* buffer, size_t len, MsquicSendFlags flags = MsquicSendFlags::None)
    {
        return send_awaiter(*this, buffer, len, false, flags);
    }
    send_awaiter send_all(const void* buffer, size_t len, MsquicSendFlags flags = MsquicSendFlags::None)
    {
        return send_awaiter(*this, buffer, len, true, flags);
    }

    void shutdown_tx(bool graceful = true, std::uint64_t error_code = 0)
    {
        MsquicStreamHandle handle {};
        bool               do_graceful = graceful;
        {
            std::lock_guard<std::mutex> guard(_state_mutex);
            if (_stream_closed || !_stream.value)
                return;
            handle = _stream;
            if (do_graceful) {
                _graceful_shutdown_requested = true;
                _shutdown_pending            = true;
            } else {
                _graceful_shutdown_requested = false;
                _shutdown_pending            = false;
            }
        }
        if (_ctx && _ctx->api && handle.value) {
            auto flag = do_graceful ? MsquicStreamShutdownFlags::Graceful : MsquicStreamShutdownFlags::AbortSend;
            if (msquic_debug_enabled()) {
                CO_WQ_LOG_DEBUG("[quic_socket] stream shutdown flag=0x%x error=%llu",
                                static_cast<unsigned>(flag),
                                static_cast<unsigned long long>(error_code));
            }
            _ctx->api->stream_shutdown(handle, flag, error_code);
        }
    }

    void close()
    {
        MsquicStreamHandle handle {};
        bool               should_abort  = false;
        bool               wait_shutdown = false;
        {
            std::lock_guard<std::mutex> guard(_state_mutex);
            if (!_stream.value)
                return;
            if (_stream_closed)
                return;
            if (_shutdown_pending && _graceful_shutdown_requested) {
                _close_pending = true;
                wait_shutdown  = true;
            } else {
                _stream_closed               = true;
                handle                       = _stream;
                _stream                      = {};
                should_abort                 = !_graceful_shutdown_requested;
                _graceful_shutdown_requested = false;
                _shutdown_pending            = false;
            }
        }
        if (wait_shutdown)
            return;
        if (_ctx && _ctx->api && handle.value) {
            if (should_abort) {
                if (msquic_debug_enabled()) {
                    CO_WQ_LOG_DEBUG("[quic_socket] stream abort on close");
                }
                _ctx->api->stream_shutdown(handle,
                                           MsquicStreamShutdownFlags::AbortSend
                                               | MsquicStreamShutdownFlags::AbortReceive,
                                           0);
            }
            if (msquic_debug_enabled()) {
                CO_WQ_LOG_DEBUG("[quic_socket] stream close handle");
            }
            _ctx->api->stream_close(handle);
        }
        if (should_abort && _connection)
            _connection->on_failure(-ECONNRESET);
        notify_recv_ready();
        notify_send_ready();
        notify_handshake_ready();
        release_self_reference();
    }

    bool rx_fin() const noexcept { return _recv_fin; }
    bool tx_fin() const noexcept { return _tx_fin; }
    bool closed() const noexcept { return _stream_closed; }

    workqueue<lock>&   exec() { return *_exec; }
    lock&              serial_lock() { return _serial_lock; }
    callback_wq<lock>& callback_queue() { return *_cbq; }

    void mark_handshake_done(int result)
    {
        bool notify = false;
        {
            std::lock_guard<std::mutex> guard(_state_mutex);
            if (!_handshake_done) {
                _handshake_done   = true;
                _handshake_result = result;
                notify            = true;
            }
        }
        if (notify)
            notify_handshake_ready();
    }

private:
    quic_socket(workqueue<lock>&                             exec,
                std::shared_ptr<quic_context>                ctx,
                std::shared_ptr<quic_connection_state<lock>> connection,
                MsquicStreamHandle                           stream)
        : _exec(&exec)
        , _ctx(std::move(ctx))
        , _connection(std::move(connection))
        , _stream(stream)
        , _cbq(std::make_unique<callback_wq<lock>>(exec))
    {
        serial_queue_init(_send_q);
        serial_queue_init(_recv_q);
        if (_ctx && _ctx->api && _stream.value)
            _ctx->api->set_stream_callback(_stream, &quic_socket::stream_callback, this);
    }

    void after_construct(const std::shared_ptr<quic_socket>& self)
    {
        {
            std::lock_guard<std::mutex> guard(_state_mutex);
            _self_ref = self;
        }
        if (_connection)
            _connection->register_pending(self);
        else
            mark_handshake_done(0);
    }

    static MsquicStatus stream_callback(MsquicStreamHandle, void* ctx, const MsquicStreamEvent& event)
    {
        auto* self = static_cast<quic_socket*>(ctx);
        if (!self)
            return 0;
        return self->handle_stream_event(event);
    }

    MsquicStatus handle_stream_event(const MsquicStreamEvent& event)
    {
        switch (event.type) {
        case MsquicStreamEventType::Receive:
            handle_receive(event.receive);
            break;
        case MsquicStreamEventType::SendComplete:
            handle_send_complete(event.send_complete);
            break;
        case MsquicStreamEventType::PeerSendShutdown:
            handle_peer_send_shutdown();
            break;
        case MsquicStreamEventType::PeerSendAborted:
            handle_peer_send_aborted(event.peer_send_aborted);
            break;
        case MsquicStreamEventType::PeerReceiveAborted:
            handle_peer_receive_aborted(event.peer_receive_aborted);
            break;
        case MsquicStreamEventType::SendShutdownComplete:
            handle_send_shutdown_complete(event.send_shutdown_complete);
            break;
        case MsquicStreamEventType::ShutdownComplete:
            handle_shutdown_complete(event.shutdown_complete);
            break;
        default:
            break;
        }
        return 0;
    }

    void handle_receive(const MsquicStreamReceiveEvent& ev)
    {
        std::vector<std::vector<std::uint8_t>> copied_chunks;
        copied_chunks.reserve(ev.buffers.size());
        std::size_t                newly_appended = 0;
        std::size_t                queue_size     = 0;
        MsquicStreamHandle         handle_disable {};
        std::shared_ptr<MsquicApi> api_disable;
        std::uint64_t              event_length = ev.total_length;
        std::uint64_t              skip         = 0;
        {
            std::lock_guard<std::mutex> guard(_state_mutex);
            if (ev.absolute_offset < _recv_next_offset)
                skip = std::min<std::uint64_t>(_recv_next_offset - ev.absolute_offset, ev.total_length);
        }

        std::uint64_t remaining_skip = skip;
        for (const auto& buf : ev.buffers) {
            if (!buf.data || buf.length == 0)
                continue;
            const std::uint8_t* data = buf.data;
            std::uint32_t       len  = buf.length;
            if (remaining_skip > 0) {
                if (remaining_skip >= len) {
                    remaining_skip -= len;
                    continue;
                }
                data += remaining_skip;
                len -= static_cast<std::uint32_t>(remaining_skip);
                remaining_skip = 0;
            }
            if (len == 0)
                continue;
            copied_chunks.emplace_back(data, data + len);
            newly_appended += len;
        }

        {
            std::lock_guard<std::mutex> guard(_state_mutex);
            for (auto& chunk : copied_chunks)
                _recv_queue.push_back(std::move(chunk));
            if (newly_appended > 0)
                _recv_next_offset += newly_appended;
            if (ev.fin)
                _recv_fin = true;
            queue_size = _recv_queue.size();
            if (!_recv_queue.empty() && !_recv_fin && !_recv_disabled && !_stream_closed && _stream.value && _ctx
                && _ctx->api) {
                _recv_disabled = true;
                handle_disable = _stream;
                api_disable    = _ctx->api;
            }
        }
        CO_WQ_LOG_DEBUG("[quic_socket] receive event off=%llu total=%llu fin=%d flags=0x%x buffers=%zu queue=%zu "
                        "skip=%llu appended=%zu",
                        static_cast<unsigned long long>(ev.absolute_offset),
                        static_cast<unsigned long long>(event_length),
                        ev.fin ? 1 : 0,
                        static_cast<unsigned>(ev.flags),
                        ev.buffers.size(),
                        queue_size,
                        static_cast<unsigned long long>(skip),
                        newly_appended);
        if (msquic_debug_enabled()) {
            CO_WQ_LOG_DEBUG("[quic_socket] receive event off=%llu total=%llu fin=%d queue=%zu skip=%llu appended=%zu",
                            static_cast<unsigned long long>(ev.absolute_offset),
                            static_cast<unsigned long long>(event_length),
                            ev.fin ? 1 : 0,
                            queue_size,
                            static_cast<unsigned long long>(skip),
                            newly_appended);
        }
        if (api_disable && handle_disable.value) {
            api_disable->stream_receive_set_enabled(handle_disable, false);
            CO_WQ_LOG_DEBUG("[quic_socket] receive disabled");
        }
        if (newly_appended > 0 || ev.fin)
            notify_recv_ready();
    }

    void handle_send_complete(const MsquicStreamSendCompleteEvent& ev)
    {
        ssize_t error_snapshot = 0;
        bool    inflight       = false;
        size_t  pending_len    = 0;
        {
            std::lock_guard<std::mutex> guard(_state_mutex);
            _send_error     = ev.canceled ? -ECANCELED : 0;
            _send_completed = true;
            error_snapshot  = _send_error;
            inflight        = _send_inflight;
            pending_len     = _send_pending_len;
        }
        if (msquic_debug_enabled()) {
            CO_WQ_LOG_DEBUG("[quic_socket] send complete canceled=%d err=%zd inflight=%d pending_len=%zu",
                            ev.canceled ? 1 : 0,
                            error_snapshot,
                            inflight ? 1 : 0,
                            pending_len);
        }
        notify_send_ready();
    }

    void handle_peer_send_shutdown()
    {
        bool notify = false;
        {
            std::lock_guard<std::mutex> guard(_state_mutex);
            if (!_recv_fin) {
                _recv_fin = true;
                notify    = true;
            }
        }
        CO_WQ_LOG_DEBUG("[quic_socket] peer send shutdown");
        if (notify)
            notify_recv_ready();
    }

    void handle_peer_send_aborted(const MsquicStreamPeerSendAbortedEvent& ev)
    {
        bool notify = false;
        {
            std::lock_guard<std::mutex> guard(_state_mutex);
            _recv_error = -ECONNRESET;
            _recv_fin   = true;
            notify      = true;
        }
        CO_WQ_LOG_DEBUG("[quic_socket] peer send aborted error=%llu", static_cast<unsigned long long>(ev.error_code));
        if (notify)
            notify_recv_ready();
    }

    void handle_peer_receive_aborted(const MsquicStreamPeerReceiveAbortedEvent& ev)
    {
        bool notify = false;
        {
            std::lock_guard<std::mutex> guard(_state_mutex);
            if (_send_error == 0)
                _send_error = -EPIPE;
            if (_send_inflight && !_send_completed) {
                _send_completed     = true;
                _send_inflight      = false;
                _send_pending_len   = 0;
                _send_pending_flags = MsquicSendFlags::None;
            }
            notify = true;
        }
        if (msquic_debug_enabled()) {
            CO_WQ_LOG_DEBUG("[quic_socket] peer receive aborted error=%llu",
                            static_cast<unsigned long long>(ev.error_code));
        }
        if (notify)
            notify_send_ready();
    }

    void handle_send_shutdown_complete(const MsquicStreamSendShutdownCompleteEvent& ev)
    {
        bool notify = false;
        {
            std::lock_guard<std::mutex> guard(_state_mutex);
            _tx_fin = true;
            notify  = true;
        }
        if (msquic_debug_enabled()) {
            CO_WQ_LOG_DEBUG("[quic_socket] send shutdown complete graceful=%d", ev.graceful ? 1 : 0);
        }
        if (notify)
            notify_send_ready();
    }

    void handle_shutdown_complete(const MsquicStreamShutdownCompleteEvent&)
    {
        MsquicStreamHandle to_close {};
        bool               release_self = false;
        {
            std::lock_guard<std::mutex> guard(_state_mutex);
            _recv_fin                    = true;
            _tx_fin                      = true;
            _shutdown_pending            = false;
            _graceful_shutdown_requested = false;
            if (!_stream_closed || _close_pending) {
                _stream_closed = true;
                to_close       = _stream;
                _stream        = {};
                release_self   = true;
            }
            _close_pending = false;
        }
        CO_WQ_LOG_DEBUG("[quic_socket] shutdown complete");
        notify_recv_ready();
        notify_send_ready();
        notify_handshake_ready();
        if (to_close.value && _ctx && _ctx->api)
            _ctx->api->stream_close(to_close);
        if (release_self)
            release_self_reference();
    }

    size_t copy_received_to(std::uint8_t* dest, size_t max_len)
    {
        size_t total = 0;
        while (max_len > 0 && !_recv_queue.empty()) {
            auto&  front  = _recv_queue.front();
            size_t offset = _recv_offset;
            size_t avail  = front.size() - offset;
            size_t take   = std::min(max_len, avail);
            std::copy_n(front.data() + offset, take, dest + total);
            total += take;
            max_len -= take;
            _recv_offset += take;
            if (_recv_offset >= front.size()) {
                _recv_queue.pop_front();
                _recv_offset = 0;
            }
        }
        if (total > 0) {
            CO_WQ_LOG_DEBUG("[quic_socket] copy total=%zu queue=%zu offset=%zu",
                            total,
                            _recv_queue.size(),
                            _recv_offset);
        }
        return total;
    }

    static int translate_status(MsquicStatus status)
    {
        if (status == 0)
            return 0;
        return -EIO;
    }

    void register_recv_waiter(io_waiter_base* waiter)
    {
        if (!waiter)
            return;
        {
            std::lock_guard<std::mutex> guard(_waiter_mutex);
            _pending_recv_waiter = waiter;
        }
        if (has_pending_recv())
            resume_recv_waiter(waiter);
    }

    void register_send_waiter(io_waiter_base* waiter)
    {
        if (!waiter)
            return;
        {
            std::lock_guard<std::mutex> guard(_waiter_mutex);
            _pending_send_waiter = waiter;
        }
        if (send_ready())
            resume_send_waiter(waiter);
    }

    void register_handshake_waiter(io_waiter_base* waiter)
    {
        if (!waiter)
            return;
        {
            std::lock_guard<std::mutex> guard(_waiter_mutex);
            _pending_handshake_waiter = waiter;
        }
        {
            std::lock_guard<std::mutex> guard(_state_mutex);
            if (_handshake_done)
                resume_handshake_waiter(waiter);
        }
    }

    bool has_pending_recv()
    {
        std::lock_guard<std::mutex> guard(_state_mutex);
        return !_recv_queue.empty() || _recv_fin || _recv_error < 0;
    }

    bool send_ready()
    {
        std::lock_guard<std::mutex> guard(_state_mutex);
        return !_send_inflight || _send_completed || _stream_closed;
    }

    void resume_recv_waiter(io_waiter_base* expected)
    {
        io_waiter_base* waiter = nullptr;
        {
            std::lock_guard<std::mutex> guard(_waiter_mutex);
            if (_pending_recv_waiter == expected)
                waiter = std::exchange(_pending_recv_waiter, nullptr);
        }
        if (waiter)
            post_via_route(*_exec, *waiter);
    }

    void notify_recv_ready()
    {
        io_waiter_base* waiter = nullptr;
        {
            std::lock_guard<std::mutex> guard(_waiter_mutex);
            waiter = std::exchange(_pending_recv_waiter, nullptr);
        }
        if (waiter)
            post_via_route(*_exec, *waiter);
    }

    void notify_send_ready()
    {
        io_waiter_base* waiter = nullptr;
        {
            std::lock_guard<std::mutex> guard(_waiter_mutex);
            waiter = std::exchange(_pending_send_waiter, nullptr);
        }
        if (waiter)
            post_via_route(*_exec, *waiter);
    }

    void notify_handshake_ready()
    {
        io_waiter_base* waiter = nullptr;
        {
            std::lock_guard<std::mutex> guard(_waiter_mutex);
            waiter = std::exchange(_pending_handshake_waiter, nullptr);
        }
        if (waiter)
            post_via_route(*_exec, *waiter);
    }

    void resume_send_waiter(io_waiter_base* expected)
    {
        io_waiter_base* waiter = nullptr;
        {
            std::lock_guard<std::mutex> guard(_waiter_mutex);
            if (_pending_send_waiter == expected)
                waiter = std::exchange(_pending_send_waiter, nullptr);
        }
        if (waiter)
            post_via_route(*_exec, *waiter);
    }

    void resume_handshake_waiter(io_waiter_base* expected)
    {
        io_waiter_base* waiter = nullptr;
        {
            std::lock_guard<std::mutex> guard(_waiter_mutex);
            if (_pending_handshake_waiter == expected)
                waiter = std::exchange(_pending_handshake_waiter, nullptr);
        }
        if (waiter)
            post_via_route(*_exec, *waiter);
    }

    workqueue<lock>*                             _exec { nullptr };
    std::shared_ptr<quic_context>                _ctx;
    std::shared_ptr<quic_connection_state<lock>> _connection;
    MsquicStreamHandle                           _stream {};
    std::unique_ptr<callback_wq<lock>>           _cbq;

    lock         _serial_lock;
    serial_queue _send_q;
    serial_queue _recv_q;

    std::mutex                            _state_mutex;
    std::deque<std::vector<std::uint8_t>> _recv_queue;
    size_t                                _recv_offset { 0 };
    std::uint64_t                         _recv_next_offset { 0 };
    bool                                  _recv_fin { false };
    bool                                  _recv_disabled { false };
    bool                                  _tx_fin { false };
    bool                                  _stream_closed { false };
    bool                                  _graceful_shutdown_requested { false };
    bool                                  _shutdown_pending { false };
    bool                                  _close_pending { false };
    ssize_t                               _recv_error { 0 };

    bool            _send_inflight { false };
    bool            _send_completed { false };
    bool            _send_started { false };
    size_t          _send_pending_len { 0 };
    MsquicSendFlags _send_pending_flags { MsquicSendFlags::None };
    ssize_t         _send_error { 0 };

    bool _handshake_done { false };
    int  _handshake_result { 0 };

    std::mutex      _waiter_mutex;
    io_waiter_base* _pending_recv_waiter { nullptr };
    io_waiter_base* _pending_send_waiter { nullptr };
    io_waiter_base* _pending_handshake_waiter { nullptr };

    std::shared_ptr<quic_socket> _self_ref;

    void release_self_reference()
    {
        std::shared_ptr<quic_socket> tmp;
        {
            std::lock_guard<std::mutex> guard(_state_mutex);
            tmp = std::move(_self_ref);
        }
    }
};

template <lockable lock> class quic_listener {
public:
    using socket_type = quic_socket<lock>;

    quic_listener(workqueue<lock>& exec, std::shared_ptr<quic_context> ctx)
        : _exec(&exec), _ctx(std::move(ctx)), _cbq(std::make_unique<callback_wq<lock>>(exec))
    {
    }

    quic_listener(const quic_listener&)            = delete;
    quic_listener& operator=(const quic_listener&) = delete;

    ~quic_listener()
    {
        stop();
        if (_ctx && _ctx->api && _listener.value) {
            _ctx->api->listener_close(_listener);
            _listener = {};
        }
    }

    MsquicStatus start(std::uint16_t port, const MsquicConstBuffer* alpns, std::uint32_t alpn_count)
    {
        if (!_ctx || !_ctx->api)
            return 1;
        if (!_listener.value) {
            auto status = _ctx->api->listener_open(_ctx->registration,
                                                   &quic_listener::listener_callback,
                                                   this,
                                                   _listener);
            if (quic_status_failed(status))
                return status;
        }
        auto status = _ctx->api->listener_start_any(_listener, alpns, alpn_count, port);
        if (quic_status_failed(status))
            return status;
        {
            std::lock_guard<std::mutex> guard(_mutex);
            _stopped = false;
        }
        return status;
    }

    void stop()
    {
        bool need_stop = false;
        {
            std::lock_guard<std::mutex> guard(_mutex);
            if (_stopped)
                return;
            _stopped  = true;
            need_stop = true;
        }
        if (need_stop && _ctx && _ctx->api && _listener.value)
            _ctx->api->listener_stop(_listener);
        drain_accept_waiter();
    }

    workqueue<lock>& exec() { return *_exec; }

    struct accept_awaiter : io_waiter_base {
        quic_listener&               owner;
        std::shared_ptr<socket_type> result;

        explicit accept_awaiter(quic_listener& lst) : owner(lst) { this->set_debug_name("quic_accept"); }

        bool await_ready() { return owner.try_dequeue(result); }

        void await_suspend(std::coroutine_handle<> h)
        {
            this->h = h;
            this->store_route_guard(owner._cbq->retain_guard());
            this->route_ctx  = owner._cbq->context();
            this->route_post = &callback_wq<lock>::post_adapter;
            this->func       = &io_waiter_base::resume_cb;
            owner.register_accept_waiter(this);
        }

        std::shared_ptr<socket_type> await_resume() { return std::move(result); }
    };

    accept_awaiter accept() { return accept_awaiter(*this); }

private:
    static MsquicStatus listener_callback(MsquicListenerHandle listener, void* ctx, const MsquicListenerEvent& event)
    {
        auto* self = static_cast<quic_listener*>(ctx);
        if (!self)
            return 0;
        return self->on_listener_event(listener, event);
    }

    static MsquicStatus
    connection_callback(MsquicConnectionHandle connection, void* ctx, const MsquicConnectionEvent& event)
    {
        auto* state = static_cast<quic_connection_state<lock>*>(ctx);
        if (!state)
            return 0;
        auto holder = state->shared_from_this();
        if (!holder->owner)
            return 0;
        return holder->owner->on_connection_event(std::move(holder), connection, event);
    }

    MsquicStatus on_listener_event(MsquicListenerHandle, const MsquicListenerEvent& event)
    {
        switch (event.type) {
        case MsquicListenerEventType::NewConnection: {
            auto state   = std::make_shared<quic_connection_state<lock>>(_ctx, event.connection);
            state->owner = this;
            {
                std::lock_guard<std::mutex> guard(_conn_mutex);
                _connections[event.connection.value] = state;
            }
            auto status = _ctx->api->connection_set_configuration(event.connection, _ctx->configuration);
            if (quic_status_failed(status))
                return status;
            _ctx->api->set_connection_callback(event.connection, &quic_listener::connection_callback, state.get());
            break;
        }
        case MsquicListenerEventType::StopComplete:
            break;
        default:
            break;
        }
        return 0;
    }

    MsquicStatus on_connection_event(std::shared_ptr<quic_connection_state<lock>> state,
                                     MsquicConnectionHandle                       connection,
                                     const MsquicConnectionEvent&                 event)
    {
        switch (event.type) {
        case MsquicConnectionEventType::Connected:
            state->on_connected();
            break;
        case MsquicConnectionEventType::PeerStreamStarted: {
            auto socket = quic_socket<lock>::create(*_exec, _ctx, state, event.stream);
            enqueue_socket(std::move(socket));
            break;
        }
        case MsquicConnectionEventType::ShutdownComplete:
            if (_ctx && _ctx->api)
                _ctx->api->connection_close(connection);
            state->on_failure(-ECONNRESET);
            {
                std::lock_guard<std::mutex> guard(_conn_mutex);
                _connections.erase(connection.value);
            }
            break;
        case MsquicConnectionEventType::ShutdownByTransport:
        case MsquicConnectionEventType::ShutdownByPeer:
            state->on_failure(-ECONNRESET);
            break;
        default:
            break;
        }
        return 0;
    }

    bool try_dequeue(std::shared_ptr<socket_type>& out)
    {
        std::lock_guard<std::mutex> guard(_mutex);
        if (_pending.empty())
            return false;
        out = std::move(_pending.front());
        _pending.pop_front();
        return true;
    }

    void register_accept_waiter(accept_awaiter* waiter)
    {
        std::shared_ptr<socket_type> ready;
        {
            std::lock_guard<std::mutex> guard(_mutex);
            if (!_pending.empty()) {
                ready = std::move(_pending.front());
                _pending.pop_front();
            } else {
                _accept_waiter = waiter;
                return;
            }
        }
        waiter->result = std::move(ready);
        post_via_route(*_exec, *waiter);
    }

    void enqueue_socket(std::shared_ptr<socket_type> socket)
    {
        accept_awaiter* waiter = nullptr;
        {
            std::lock_guard<std::mutex> guard(_mutex);
            if (_accept_waiter) {
                waiter         = _accept_waiter;
                _accept_waiter = nullptr;
            } else {
                _pending.push_back(std::move(socket));
                return;
            }
        }
        if (waiter) {
            waiter->result = std::move(socket);
            post_via_route(*_exec, *waiter);
        }
    }

    void drain_accept_waiter()
    {
        accept_awaiter* waiter = nullptr;
        {
            std::lock_guard<std::mutex> guard(_mutex);
            waiter         = _accept_waiter;
            _accept_waiter = nullptr;
            _pending.clear();
        }
        if (waiter) {
            waiter->result.reset();
            post_via_route(*_exec, *waiter);
        }
    }

    workqueue<lock>*                   _exec { nullptr };
    std::shared_ptr<quic_context>      _ctx;
    MsquicListenerHandle               _listener {};
    std::unique_ptr<callback_wq<lock>> _cbq;

    std::mutex                               _mutex;
    std::deque<std::shared_ptr<socket_type>> _pending;
    accept_awaiter*                          _accept_waiter { nullptr };
    bool                                     _stopped { false };

    std::mutex                                                              _conn_mutex;
    std::unordered_map<void*, std::shared_ptr<quic_connection_state<lock>>> _connections;
};

} // namespace co_wq::net
