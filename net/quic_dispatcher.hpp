#pragma once

#include "io_waiter.hpp"
#include "os_compat.hpp"
#include "udp_socket.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#if !defined(OPENSSL_NO_QUIC)
#include <openssl/quic.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
namespace co_wq::net {

namespace quic_detail {

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

} // namespace quic_detail

/**
 * @brief QUIC I/O 调度器，负责为 OpenSSL QUIC 对象注册与驱动底层 UDP 事件。
 */
template <lockable lock, template <class> class Reactor> class quic_dispatcher {
public:
    using lock_type       = lock;
    using reactor_type    = Reactor<lock>;
    using workqueue_type  = workqueue<lock>;
    using udp_socket_type = udp_socket<lock, Reactor>;

    struct datagram {
        sockaddr_storage     addr {};
        socklen_t            addr_len { 0 };
        std::vector<uint8_t> payload;
    };

    class channel {
    public:
        channel(quic_dispatcher& owner, SSL* ssl) noexcept : _owner(owner), _ssl(ssl)
        {
            std::memset(&_peer, 0, sizeof(_peer));
        }

        channel(const channel&)                = delete;
        channel& operator=(const channel&)     = delete;
        channel(channel&&) noexcept            = default;
        channel& operator=(channel&&) noexcept = default;
        ~channel()                             = default;

        SSL* ssl() const noexcept
        {
            std::lock_guard<lock> guard { _mutex };
            return _ssl;
        }

        void clear_ssl() noexcept
        {
            std::lock_guard<lock> guard { _mutex };
            _ssl = nullptr;
        }

        void set_peer(const sockaddr* addr, socklen_t len) noexcept
        {
            if (!addr || len <= 0 || len > static_cast<socklen_t>(sizeof(sockaddr_storage)))
                return;
            std::lock_guard<lock> guard { _mutex };
            std::memcpy(&_peer, addr, static_cast<size_t>(len));
            _peer_len = len;
        }

        bool peer_address(sockaddr_storage& out, socklen_t& len) const noexcept
        {
            std::lock_guard<lock> guard { _mutex };
            if (_peer_len == 0)
                return false;
            std::memcpy(&out, &_peer, static_cast<size_t>(_peer_len));
            len = _peer_len;
            return true;
        }

        void push_inbound(datagram&& packet)
        {
            {
                std::lock_guard<lock> guard { _mutex };
                _rx_queue.emplace_back(std::move(packet));
            }
            _owner.wake();
        }

        std::optional<datagram> pop_inbound()
        {
            std::lock_guard<lock> guard { _mutex };
            if (_rx_queue.empty())
                return std::nullopt;
            datagram packet = std::move(_rx_queue.front());
            _rx_queue.pop_front();
            return packet;
        }

        void push_outbound(datagram&& packet)
        {
            {
                std::lock_guard<lock> guard { _mutex };
                _tx_queue.emplace_back(std::move(packet));
            }
            _owner.wake();
        }

        void push_outbound_front(datagram&& packet)
        {
            {
                std::lock_guard<lock> guard { _mutex };
                _tx_queue.emplace_front(std::move(packet));
            }
            _owner.wake();
        }

        std::optional<datagram> pop_outbound()
        {
            std::lock_guard<lock> guard { _mutex };
            if (_tx_queue.empty())
                return std::nullopt;
            datagram packet = std::move(_tx_queue.front());
            _tx_queue.pop_front();
            return packet;
        }

        size_t pending_inbound() const noexcept
        {
            std::lock_guard<lock> guard { _mutex };
            return _rx_queue.size();
        }

        size_t pending_outbound() const noexcept
        {
            std::lock_guard<lock> guard { _mutex };
            return _tx_queue.size();
        }

        size_t pending_inbound_bytes() const noexcept
        {
            std::lock_guard<lock> guard { _mutex };
            size_t                total = 0;
            for (const auto& packet : _rx_queue)
                total += packet.payload.size();
            return total;
        }

        size_t pending_outbound_bytes() const noexcept
        {
            std::lock_guard<lock> guard { _mutex };
            size_t                total = 0;
            for (const auto& packet : _tx_queue)
                total += packet.payload.size();
            return total;
        }

    private:
        quic_dispatcher&     _owner;
        mutable lock         _mutex;
        SSL*                 _ssl { nullptr };
        sockaddr_storage     _peer {};
        socklen_t            _peer_len { 0 };
        std::deque<datagram> _rx_queue;
        std::deque<datagram> _tx_queue;
    };

    using channel_ptr = std::shared_ptr<channel>;

    class channel_bio {
    public:
        using dispatcher_type  = quic_dispatcher;
        using channel_weak_ptr = std::weak_ptr<channel>;

        static std::string format_payload_preview(const uint8_t* data, size_t len, size_t max_bytes = 16)
        {
            if (!data || len == 0)
                return "<empty>";
            size_t      count = std::min(len, max_bytes);
            std::string text;
            text.reserve(count * 3 + 4);
            for (size_t i = 0; i < count; ++i) {
                char buf[4] {};
                std::snprintf(buf, sizeof(buf), "%02x", static_cast<unsigned>(data[i]));
                if (i != 0)
                    text.push_back(' ');
                text.append(buf);
            }
            if (len > count)
                text.append(" ...");
            return text;
        }

        struct context {
            dispatcher_type* dispatcher { nullptr };
            channel_weak_ptr channel;
            datagram         current_rx;
            size_t           rx_offset { 0 };
            bool             has_pending { false };
            size_t           mtu { 1252 };
            bool             no_trunc { true };
            bool             local_addr_enabled { false };
        };

        static BIO* create(dispatcher_type& dispatcher, channel_ptr channel_handle)
        {
            if (!channel_handle)
                return nullptr;
            BIO_METHOD* method = get_method();
            if (!method)
                return nullptr;
            BIO* bio = BIO_new(method);
            if (!bio)
                return nullptr;

            try {
                auto* ctx       = new context();
                ctx->dispatcher = &dispatcher;
                ctx->channel    = channel_handle;
                BIO_set_data(bio, ctx);
                BIO_set_init(bio, 1);
                BIO_set_shutdown(bio, 0);
                return bio;
            } catch (...) {
                BIO_free(bio);
                return nullptr;
            }
        }

        static void destroy_ctx(BIO* bio)
        {
            if (!bio)
                return;
            auto* ctx = static_cast<context*>(BIO_get_data(bio));
            delete ctx;
            BIO_set_data(bio, nullptr);
            BIO_set_init(bio, 0);
        }

    private:
        static BIO_METHOD* get_method()
        {
            static BIO_METHOD* method = []() -> BIO_METHOD* {
                BIO_METHOD* m = BIO_meth_new(BIO_TYPE_SOURCE_SINK | BIO_TYPE_DESCRIPTOR, "co_wq.quic_dispatcher");
                if (!m)
                    return nullptr;
                if (BIO_meth_set_write(m, &channel_bio::bio_write) != 1
                    || BIO_meth_set_read(m, &channel_bio::bio_read) != 1
                    || BIO_meth_set_sendmmsg(m, &channel_bio::bio_sendmmsg) != 1
                    || BIO_meth_set_recvmmsg(m, &channel_bio::bio_recvmmsg) != 1
                    || BIO_meth_set_puts(m, &channel_bio::bio_puts) != 1
                    || BIO_meth_set_gets(m, &channel_bio::bio_gets) != 1
                    || BIO_meth_set_ctrl(m, &channel_bio::bio_ctrl) != 1
                    || BIO_meth_set_create(m, &channel_bio::bio_create) != 1
                    || BIO_meth_set_destroy(m, &channel_bio::bio_destroy) != 1
                    || BIO_meth_set_callback_ctrl(m, &channel_bio::bio_callback_ctrl) != 1) {
                    BIO_meth_free(m);
                    return nullptr;
                }
                return m;
            }();
            return method;
        }

        static context* get_context(BIO* bio) { return static_cast<context*>(BIO_get_data(bio)); }

        static channel_ptr lock_channel(context* ctx)
        {
            if (!ctx)
                return {};
            return ctx->channel.lock();
        }

        static int bio_create(BIO* bio)
        {
            if (!bio)
                return 0;
            BIO_set_init(bio, 0);
            BIO_set_data(bio, nullptr);
            BIO_set_flags(bio, 0);
            return 1;
        }

        static int bio_destroy(BIO* bio)
        {
            destroy_ctx(bio);
            return 1;
        }

        static int bio_write(BIO* bio, const char* data, int len)
        {
            if (!bio || len <= 0)
                return 0;

            BIO_clear_retry_flags(bio);
            auto* ctx = get_context(bio);
            auto  ch  = lock_channel(ctx);
            if (!ctx || !ch)
                return 0;

            datagram packet;
            packet.payload.assign(reinterpret_cast<const uint8_t*>(data), reinterpret_cast<const uint8_t*>(data) + len);
            packet.addr_len = 0;
            sockaddr_storage peer {};
            socklen_t        peer_len = 0;
            if (ch->peer_address(peer, peer_len)) {
                std::memcpy(&packet.addr, &peer, static_cast<size_t>(peer_len));
                packet.addr_len = peer_len;
            }
            ch->push_outbound(std::move(packet));
            return len;
        }

        static int bio_read(BIO* bio, char* data, int len)
        {
            if (!bio || !data || len <= 0)
                return 0;

            BIO_clear_retry_flags(bio);
            auto* ctx = get_context(bio);
            auto  ch  = lock_channel(ctx);
            if (!ctx || !ch)
                return 0;

            if (!ctx->has_pending) {
                if (auto packet = ch->pop_inbound()) {
                    ctx->current_rx  = std::move(*packet);
                    ctx->rx_offset   = 0;
                    ctx->has_pending = true;
                } else {
                    BIO_set_retry_read(bio);
                    return -1;
                }
            }

            const auto remaining = ctx->current_rx.payload.size() - ctx->rx_offset;
            if (remaining == 0) {
                ctx->has_pending = false;
                ctx->current_rx  = datagram {};
                ctx->rx_offset   = 0;
                BIO_set_retry_read(bio);
                return -1;
            }

            const size_t to_copy = std::min(static_cast<size_t>(len), remaining);
            std::memcpy(data, ctx->current_rx.payload.data() + ctx->rx_offset, to_copy);
            ctx->rx_offset += to_copy;
            if (ctx->rx_offset >= ctx->current_rx.payload.size()) {
                ctx->has_pending = false;
                ctx->current_rx  = datagram {};
                ctx->rx_offset   = 0;
            }
            return static_cast<int>(to_copy);
        }

        static BIO_MSG* msg_at(BIO_MSG* base, size_t stride, size_t index)
        {
            if (!base)
                return nullptr;
            auto* raw = reinterpret_cast<unsigned char*>(base);
            return reinterpret_cast<BIO_MSG*>(raw + index * stride);
        }

        static int bio_sendmmsg(BIO* bio, BIO_MSG* msg, size_t stride, size_t num_msg, uint64_t, size_t* processed)
        {
            if (!bio)
                return 0;

            BIO_clear_retry_flags(bio);
            auto* ctx = get_context(bio);
            auto  ch  = lock_channel(ctx);
            if (!ctx || !ch) {
                if (processed)
                    *processed = 0;
                return 0;
            }

            size_t sent = 0;
            for (size_t i = 0; i < num_msg; ++i) {
                auto* entry = msg_at(msg, stride, i);
                if (!entry || !entry->data || entry->data_len == 0)
                    continue;

                datagram     packet;
                const auto*  bytes = static_cast<const uint8_t*>(entry->data);
                const size_t len   = entry->data_len;
                packet.payload.assign(bytes, bytes + len);
                packet.addr_len = 0;

                if (entry->peer) {
                    sockaddr_storage peer {};
                    socklen_t        peer_len = 0;
                    if (bioaddr_to_sockaddr(entry->peer, peer, peer_len)) {
                        packet.addr     = peer;
                        packet.addr_len = peer_len;
                    }
                }

                ch->push_outbound(std::move(packet));
                ++sent;
            }

            CO_WQ_LOG_INFO("[quic dispatcher] bio_sendmmsg processed=%zu", sent);
            if (processed)
                *processed = sent;
            return 1;
        }

        static int bio_recvmmsg(BIO* bio, BIO_MSG* msg, size_t stride, size_t num_msg, uint64_t, size_t* processed)
        {
            if (!bio)
                return 0;

            BIO_clear_retry_flags(bio);
            auto* ctx = get_context(bio);
            auto  ch  = lock_channel(ctx);
            if (!ctx || !ch) {
                if (processed)
                    *processed = 0;
                return 0;
            }

            size_t received = 0;
            for (size_t i = 0; i < num_msg; ++i) {
                auto* entry = msg_at(msg, stride, i);
                if (!entry || !entry->data || entry->data_len == 0)
                    continue;

                std::optional<datagram> packet_opt;
                if (ctx->has_pending) {
                    packet_opt.emplace(std::move(ctx->current_rx));
                    ctx->current_rx  = datagram {};
                    ctx->rx_offset   = 0;
                    ctx->has_pending = false;
                } else {
                    packet_opt = ch->pop_inbound();
                }

                if (!packet_opt)
                    break;

                auto&        packet  = *packet_opt;
                const size_t to_copy = std::min(entry->data_len, packet.payload.size());
                std::memcpy(entry->data, packet.payload.data(), to_copy);
                entry->data_len = to_copy;
                entry->flags    = 0;

                if (entry->peer)
                    sockaddr_to_bioaddr(packet.addr, packet.addr_len, entry->peer);

                const std::string peer_text = packet.addr_len
                    ? dispatcher_type::make_address_key(reinterpret_cast<const sockaddr*>(&packet.addr),
                                                        packet.addr_len)
                    : std::string { "<unknown>" };
                const std::string preview   = format_payload_preview(packet.payload.data(), packet.payload.size(), 12);
                CO_WQ_LOG_INFO("[quic dispatcher] bio_recvmmsg msg[%zu] len=%zu peer=%s preview=%s",
                               i,
                               to_copy,
                               peer_text.c_str(),
                               preview.c_str());

                ++received;
            }

            if (processed)
                *processed = received;

            CO_WQ_LOG_INFO("[quic dispatcher] bio_recvmmsg received=%zu", received);
            if (received == 0) {
                BIO_set_retry_read(bio);
                return -1;
            }

            return 1;
        }

        static int bio_puts(BIO* bio, const char* str)
        {
            if (!str)
                return 0;
            return bio_write(bio, str, static_cast<int>(std::strlen(str)));
        }

        static int bio_gets(BIO*, char*, int) { return -1; }

        static bool bioaddr_to_sockaddr(const BIO_ADDR* addr, sockaddr_storage& out, socklen_t& len)
        {
            if (!addr)
                return false;
            const int family = BIO_ADDR_family(addr);
            if (family != AF_INET && family != AF_INET6)
                return false;

            size_t        raw_len = 0;
            unsigned char buffer[sizeof(in6_addr)] {};
            if (BIO_ADDR_rawaddress(addr, buffer, &raw_len) != 1)
                return false;
            const unsigned short net_port = BIO_ADDR_rawport(addr);

            if (family == AF_INET) {
                if (raw_len != sizeof(in_addr))
                    return false;
                auto* sin = reinterpret_cast<sockaddr_in*>(&out);
                std::memset(sin, 0, sizeof(*sin));
                sin->sin_family = AF_INET;
                std::memcpy(&sin->sin_addr, buffer, sizeof(in_addr));
                sin->sin_port = htons(net_port);
                len           = sizeof(sockaddr_in);
                return true;
            }

            auto* sin6 = reinterpret_cast<sockaddr_in6*>(&out);
            std::memset(sin6, 0, sizeof(*sin6));
            sin6->sin6_family = AF_INET6;
            std::memcpy(&sin6->sin6_addr, buffer, std::min(raw_len, sizeof(in6_addr)));
            sin6->sin6_port = htons(net_port);
            len             = sizeof(sockaddr_in6);
            return true;
        }

        static bool sockaddr_to_bioaddr(const sockaddr_storage& addr, socklen_t len, BIO_ADDR* out)
        {
            if (!out)
                return false;
            if (len == sizeof(sockaddr_in)) {
                const auto* sin = reinterpret_cast<const sockaddr_in*>(&addr);
                return BIO_ADDR_rawmake(out, AF_INET, &sin->sin_addr, sizeof(in_addr), ntohs(sin->sin_port)) == 1;
            }
            if (len == sizeof(sockaddr_in6)) {
                const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(&addr);
                return BIO_ADDR_rawmake(out, AF_INET6, &sin6->sin6_addr, sizeof(in6_addr), ntohs(sin6->sin6_port)) == 1;
            }
            return false;
        }

        static long bio_ctrl(BIO* bio, int cmd, long num, void* ptr)
        {
            auto* ctx = get_context(bio);
            auto  ch  = lock_channel(ctx);

            switch (cmd) {
            case BIO_CTRL_RESET:
                if (ctx) {
                    ctx->current_rx  = datagram {};
                    ctx->rx_offset   = 0;
                    ctx->has_pending = false;
                }
                return 1;
            case BIO_CTRL_EOF:
                return 0;
            case BIO_CTRL_SET_CLOSE:
                BIO_set_shutdown(bio, static_cast<int>(num));
                return 1;
            case BIO_CTRL_GET_CLOSE:
                return BIO_get_shutdown(bio);
            case BIO_CTRL_FLUSH:
                return 1;
            case BIO_CTRL_PENDING:
                if (!ctx)
                    return 0;
                {
                    size_t pending = 0;
                    if (ctx->has_pending && ctx->current_rx.payload.size() >= ctx->rx_offset)
                        pending += ctx->current_rx.payload.size() - ctx->rx_offset;
                    if (ch)
                        pending += ch->pending_inbound_bytes();
                    return static_cast<long>(pending);
                }
            case BIO_CTRL_WPENDING:
                if (!ch)
                    return 0;
                return static_cast<long>(ch->pending_outbound_bytes());
            case BIO_CTRL_DGRAM_SET_MTU:
                if (ctx && num > 0)
                    ctx->mtu = static_cast<size_t>(num);
                return 1;
            case BIO_CTRL_DGRAM_GET_MTU:
                return ctx ? static_cast<long>(ctx->mtu) : 0;
            case BIO_CTRL_DGRAM_GET_MTU_OVERHEAD:
                return 28; // IPv4 UDP header size by default
            case BIO_CTRL_DGRAM_SET_NO_TRUNC:
                if (ctx)
                    ctx->no_trunc = (num != 0);
                return 1;
            case BIO_CTRL_DGRAM_GET_NO_TRUNC:
                return ctx && ctx->no_trunc;
            case BIO_CTRL_DGRAM_SET_PEER:
            case BIO_CTRL_DGRAM_SET_CONNECTED:
            case BIO_CTRL_DGRAM_CONNECT:
                if (ctx && ptr) {
                    sockaddr_storage peer {};
                    socklen_t        peer_len = 0;
                    if (bioaddr_to_sockaddr(static_cast<const BIO_ADDR*>(ptr), peer, peer_len) && ch) {
                        const std::string addr_text = dispatcher_type::make_address_key(
                            reinterpret_cast<const sockaddr*>(&peer),
                            peer_len);
                        CO_WQ_LOG_INFO("[quic dispatcher] BIO set peer channel=%p addr=%s",
                                       static_cast<void*>(ch.get()),
                                       addr_text.c_str());
                        ch->set_peer(reinterpret_cast<const sockaddr*>(&peer), peer_len);
                        if (ctx->dispatcher)
                            ctx->dispatcher->bind_channel_peer(ch, reinterpret_cast<const sockaddr*>(&peer), peer_len);
                        return 1;
                    }
                    return 0;
                }
                return 0;
            case BIO_CTRL_DGRAM_GET_PEER:
                if (ctx && ptr && ch) {
                    sockaddr_storage peer {};
                    socklen_t        peer_len = 0;
                    if (ch->peer_address(peer, peer_len))
                        return sockaddr_to_bioaddr(peer, peer_len, static_cast<BIO_ADDR*>(ptr)) ? 1 : 0;
                }
                return 0;
            case BIO_CTRL_DGRAM_GET_RECV_TIMER_EXP:
            case BIO_CTRL_DGRAM_GET_SEND_TIMER_EXP:
                return 0;
            case BIO_CTRL_DGRAM_GET_LOCAL_ADDR_ENABLE:
                if (ctx && ptr) {
                    *static_cast<int*>(ptr) = ctx->local_addr_enabled ? 1 : 0;
                    return 1;
                }
                return 0;
            case BIO_CTRL_DGRAM_SET_LOCAL_ADDR_ENABLE:
                if (ctx) {
                    ctx->local_addr_enabled = (num != 0);
                    return 1;
                }
                return 0;
            case BIO_CTRL_DGRAM_GET_CAPS:
            case BIO_CTRL_DGRAM_GET_EFFECTIVE_CAPS:
            case BIO_CTRL_DGRAM_GET_LOCAL_ADDR_CAP:
                return 0;
            case BIO_CTRL_DGRAM_SET_CAPS:
                return 1;
            default:
                return 0;
            }
        }

        static long bio_callback_ctrl(BIO*, int, BIO_info_cb*) { return 0; }
    };

    channel_ptr register_session(SSL* ssl, const sockaddr* peer = nullptr, socklen_t peer_len = 0)
    {
        if (!ssl)
            return {};
        std::lock_guard<lock> guard { _channels_lock };
        auto                  it = _channels.find(ssl);
        if (it != _channels.end()) {
            if (auto& ch = it->second; ch && peer && peer_len > 0) {
                ch->set_peer(peer, peer_len);
                bind_channel_peer(ch, peer, peer_len);
            }
            return it->second;
        }
        auto channel_instance = std::make_shared<channel>(*this, ssl);
        if (peer && peer_len > 0) {
            channel_instance->set_peer(peer, peer_len);
            bind_channel_peer(channel_instance, peer, peer_len);
        }
        std::string peer_text;
        if (peer && peer_len > 0)
            peer_text = make_address_key(peer, peer_len);
        CO_WQ_LOG_INFO("[quic dispatcher] register session ssl=%p channel=%p peer=%s",
                       static_cast<void*>(ssl),
                       static_cast<void*>(channel_instance.get()),
                       peer_text.empty() ? "<unset>" : peer_text.c_str());
        _channels.emplace(ssl, channel_instance);
        return channel_instance;
    }

    channel_ptr attach_session_bio(SSL* ssl, const sockaddr* peer = nullptr, socklen_t peer_len = 0)
    {
        if (!ssl)
            return {};
        auto channel_handle = register_session(ssl, peer, peer_len);
        if (!channel_handle)
            return {};
        if (!assign_channel_bio(ssl, channel_handle)) {
            unregister_session(ssl);
            return {};
        }
        return channel_handle;
    }

    bool attach_listener_bio(SSL* listener)
    {
        if (!listener)
            return false;
        auto channel_handle = attach_session_bio(listener);
        if (!channel_handle)
            return false;
        {
            std::lock_guard<lock> guard { _listener_lock };
            _listener_channel = channel_handle;
        }
        _listener = listener;
        return true;
    }

    bool promote_listener_peer(SSL* ssl)
    {
        if (!ssl)
            return false;
        auto session_channel  = find_channel(ssl);
        auto listener_channel = this->listener_channel();
        if (!session_channel || !listener_channel)
            return false;

        sockaddr_storage peer {};
        socklen_t        peer_len = 0;
        if (!listener_channel->peer_address(peer, peer_len))
            return false;

        std::vector<datagram> backlog;
        while (auto packet = listener_channel->pop_inbound())
            backlog.emplace_back(std::move(*packet));

        unbind_channel(listener_channel);
        CO_WQ_LOG_INFO("[quic dispatcher] promote listener peer backlog=%zu target=%p peer=%s",
                       backlog.size(),
                       static_cast<void*>(session_channel.get()),
                       make_address_key(reinterpret_cast<const sockaddr*>(&peer), peer_len).c_str());
        session_channel->set_peer(reinterpret_cast<const sockaddr*>(&peer), peer_len);
        bind_channel_peer(session_channel, reinterpret_cast<const sockaddr*>(&peer), peer_len);

        for (auto& packet : backlog)
            session_channel->push_inbound(std::move(packet));
        return true;
    }

    void unregister_session(SSL* ssl)
    {
        if (!ssl)
            return;
        channel_ptr removed;
        {
            std::lock_guard<lock> guard { _channels_lock };
            auto                  it = _channels.find(ssl);
            if (it == _channels.end())
                return;
            removed = std::move(it->second);
            _channels.erase(it);
        }
        if (removed)
            removed->clear_ssl();
        unbind_channel(removed);
    }

    channel_ptr find_channel(SSL* ssl) const
    {
        if (!ssl)
            return {};
        std::lock_guard<lock> guard { _channels_lock };
        auto                  it = _channels.find(ssl);
        if (it == _channels.end())
            return {};
        return it->second;
    }

    BIO* create_channel_bio(channel_ptr channel_handle)
    {
        return channel_bio::create(*this, std::move(channel_handle));
    }

    quic_dispatcher(workqueue_type& exec, reactor_type& reactor, udp_socket_type& udp, SSL* listener = nullptr) noexcept
        : _exec(exec), _reactor(reactor), _udp(udp), _listener(listener)
    {
#if !defined(_WIN32)
        _wake_waiter.owner      = this;
        _wake_waiter.debug_name = "quic_wake";
        setup_waker();
#endif
    }

    ~quic_dispatcher()
    {
#if !defined(_WIN32)
        resume_pending();
        teardown_waker();
#endif
    }

    void set_listener(SSL* listener) noexcept { _listener = listener; }

    SSL* listener() const noexcept { return _listener; }

    workqueue_type& exec() noexcept { return _exec; }

    const workqueue_type& exec() const noexcept { return _exec; }

    udp_socket_type& udp() noexcept { return _udp; }

    const udp_socket_type& udp() const noexcept { return _udp; }

    reactor_type& reactor() noexcept { return _reactor; }

    const reactor_type& reactor() const noexcept { return _reactor; }

    void process_io()
    {
        pump_inbound();
        pump_outbound();
    }

    void wake() noexcept
    {
#if !defined(_WIN32)
        if (_wake_pipe[1] == os::invalid_fd()) {
            resume_pending();
            return;
        }
        uint8_t byte = 1;
        ssize_t rc   = ::write(_wake_pipe[1], &byte, sizeof(byte));
        (void)rc;
#endif
    }

    static void post_adapter(void* ctx, worknode* node)
    {
        if (!ctx || !node)
            return;
        auto* exec = static_cast<workqueue_type*>(ctx);
        exec->post(*node);
    }

    uint32_t wait_mask(SSL* ssl) const noexcept
    {
        uint32_t mask = 0;
        if (ssl) {
            if (SSL_net_read_desired(ssl))
                mask |= EPOLLIN;
            if (SSL_net_write_desired(ssl))
                mask |= EPOLLOUT;
        }
        if (mask == 0)
            mask = EPOLLIN | EPOLLOUT;
        return mask;
    }

    void wait(io_waiter_base* waiter, uint32_t mask)
    {
        if (!waiter)
            return;
        if (!waiter->func)
            waiter->func = &io_waiter_base::resume_cb;
        INIT_LIST_HEAD(&waiter->ws_node);
        _reactor.add_waiter_custom(_udp.native_handle(), mask ? mask : (EPOLLIN | EPOLLOUT), waiter);
    }

    template <class Rep, class Period>
    void wait_with_timeout(io_waiter_base* waiter, uint32_t mask, const std::chrono::duration<Rep, Period>& timeout)
    {
        if (!waiter)
            return;
        auto original_func = waiter->func;
        CO_WQ_LOG_DEBUG("[quic dispatcher] wait_with_timeout start waiter=%p func=%p mask=0x%x timeout_ms=%lld",
                        static_cast<void*>(waiter),
                        reinterpret_cast<void*>(original_func),
                        mask,
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count()));
        if (!original_func)
            waiter->func = &io_waiter_base::resume_cb;
        INIT_LIST_HEAD(&waiter->ws_node);
        auto events = mask ? mask : (EPOLLIN | EPOLLOUT);
        if constexpr (requires(reactor_type&                      r,
                               decltype(_udp.native_handle())     fd,
                               io_waiter_base*                    w,
                               std::chrono::duration<Rep, Period> t) { r.add_waiter_with_timeout(fd, events, w, t); }) {
            register_waiter_timeout(waiter);
            _reactor.add_waiter_with_timeout(_udp.native_handle(), events, waiter, timeout);
        } else {
            _reactor.add_waiter_custom(_udp.native_handle(), events, waiter);
        }
        if (original_func)
            waiter->func = original_func;
        CO_WQ_LOG_DEBUG("[quic dispatcher] wait_with_timeout done waiter=%p func=%p",
                        static_cast<void*>(waiter),
                        reinterpret_cast<void*>(waiter->func));
    }

    void drive_listener()
    {
        process_io();
        if (_listener)
            SSL_handle_events(_listener);
        pump_outbound();
    }

    void drive_session(SSL* ssl)
    {
        process_io();
        if (ssl)
            SSL_handle_events(ssl);
        pump_outbound();
    }

    void track_waiter(io_waiter_base* waiter)
    {
#if !defined(_WIN32)
        if (!waiter)
            return;
        std::scoped_lock<lock> lk(_waiters_lock);
        if (std::find(_waiters.begin(), _waiters.end(), waiter) == _waiters.end())
            _waiters.push_back(waiter);
#else
        (void)waiter;
#endif
    }

    void untrack_waiter(io_waiter_base* waiter)
    {
        if (!waiter)
            return;
        clear_waiter_timeout(waiter);
#if !defined(_WIN32)
        std::scoped_lock<lock> lk(_waiters_lock);
        auto                   it = std::find(_waiters.begin(), _waiters.end(), waiter);
        if (it != _waiters.end()) {
            if (it + 1 != _waiters.end())
                *it = _waiters.back();
            _waiters.pop_back();
        }
#endif
    }

private:
#if !defined(_WIN32)
    struct wake_waiter : io_waiter_base {
        quic_dispatcher* owner { nullptr };

        static void resume_cb(worknode* node)
        {
            auto* self = static_cast<wake_waiter*>(node);
            if (!self || !self->owner)
                return;
            self->owner->handle_wakeup();
        }
    };

    void handle_wakeup()
    {
        if (_wake_pipe[0] == os::invalid_fd())
            return;
        uint8_t buffer[64];
        while (true) {
            ssize_t rc = ::read(_wake_pipe[0], buffer, sizeof(buffer));
            if (rc <= 0) {
                if (rc < 0 && errno == EINTR)
                    continue;
                break;
            }
            if (rc < static_cast<ssize_t>(sizeof(buffer)))
                break;
        }
        resume_pending();
    }

    void arm_waker()
    {
        if (_wake_pipe[0] == os::invalid_fd())
            return;
        _wake_waiter.func = &wake_waiter::resume_cb;
        INIT_LIST_HEAD(&_wake_waiter.ws_node);
        _reactor.cancel_waiter(_wake_pipe[0], &_wake_waiter);
        _reactor.add_waiter_custom(_wake_pipe[0], EPOLLIN, &_wake_waiter);
    }

    void setup_waker()
    {
        if (_wake_pipe[0] != os::invalid_fd() || _wake_pipe[1] != os::invalid_fd())
            return;
        int pipefd[2] { -1, -1 };
#if defined(__linux__)
        if (::pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) != 0) {
#endif
            if (::pipe(pipefd) != 0) {
                pipefd[0] = pipefd[1] = -1;
            } else {
                ::fcntl(pipefd[0], F_SETFL, ::fcntl(pipefd[0], F_GETFL, 0) | O_NONBLOCK);
                ::fcntl(pipefd[1], F_SETFL, ::fcntl(pipefd[1], F_GETFL, 0) | O_NONBLOCK);
#ifdef FD_CLOEXEC
                ::fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
                ::fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
#endif
            }
#if defined(__linux__)
        }
#endif
        if (pipefd[0] < 0 || pipefd[1] < 0) {
            if (pipefd[0] >= 0)
                ::close(pipefd[0]);
            if (pipefd[1] >= 0)
                ::close(pipefd[1]);
            return;
        }
        _wake_pipe[0] = static_cast<os::fd_t>(pipefd[0]);
        _wake_pipe[1] = static_cast<os::fd_t>(pipefd[1]);
        arm_waker();
    }

    void teardown_waker()
    {
        if (_wake_pipe[0] != os::invalid_fd()) {
            _reactor.cancel_waiter(_wake_pipe[0], &_wake_waiter);
            ::close(_wake_pipe[0]);
            _wake_pipe[0] = os::invalid_fd();
        }
        if (_wake_pipe[1] != os::invalid_fd()) {
            ::close(_wake_pipe[1]);
            _wake_pipe[1] = os::invalid_fd();
        }
    }
#endif

    channel_ptr listener_channel() const
    {
        std::lock_guard<lock> guard { _listener_lock };
        return _listener_channel.lock();
    }

    static std::string make_address_key(const sockaddr* addr, socklen_t len)
    {
        if (!addr || len <= 0)
            return {};
        char           buffer[INET6_ADDRSTRLEN] {};
        unsigned short port = 0;

        if (addr->sa_family == AF_INET && len >= static_cast<socklen_t>(sizeof(sockaddr_in))) {
            const auto* sin = reinterpret_cast<const sockaddr_in*>(addr);
            if (!inet_ntop(AF_INET, &sin->sin_addr, buffer, sizeof(buffer)))
                return {};
            port = ntohs(sin->sin_port);
            char key[INET6_ADDRSTRLEN + 8] {};
            std::snprintf(key, sizeof(key), "%s:%u", buffer, static_cast<unsigned>(port));
            return std::string { key };
        }

        if (addr->sa_family == AF_INET6 && len >= static_cast<socklen_t>(sizeof(sockaddr_in6))) {
            const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(addr);
            if (!inet_ntop(AF_INET6, &sin6->sin6_addr, buffer, sizeof(buffer)))
                return {};
            port = ntohs(sin6->sin6_port);
            char key[INET6_ADDRSTRLEN + 10] {};
            std::snprintf(key, sizeof(key), "[%s]:%u", buffer, static_cast<unsigned>(port));
            return std::string { key };
        }
        return {};
    }

    void bind_channel_peer(const channel_ptr& ch, const sockaddr* addr, socklen_t len)
    {
        if (!ch || !addr || len <= 0)
            return;
        auto key = make_address_key(addr, len);
        if (key.empty())
            return;
        CO_WQ_LOG_INFO("[quic dispatcher] bind channel %p to peer %s", static_cast<const void*>(ch.get()), key.c_str());
        std::lock_guard<lock> guard { _addr_lock };
        if (auto it = _channel_to_addr.find(ch.get()); it != _channel_to_addr.end()) {
            _addr_to_channel.erase(it->second);
            it->second = key;
        } else {
            _channel_to_addr.emplace(ch.get(), key);
        }
        _addr_to_channel[key] = ch;
    }

    void unbind_channel(const channel_ptr& ch)
    {
        if (!ch)
            return;
        std::lock_guard<lock> guard { _addr_lock };
        auto                  it = _channel_to_addr.find(ch.get());
        if (it == _channel_to_addr.end())
            return;
        _addr_to_channel.erase(it->second);
        _channel_to_addr.erase(it);
    }

    channel_ptr find_channel_by_addr(const sockaddr* addr, socklen_t len)
    {
        auto key = make_address_key(addr, len);
        if (key.empty())
            return {};
        std::lock_guard<lock> guard { _addr_lock };
        auto                  it = _addr_to_channel.find(key);
        if (it == _addr_to_channel.end()) {
            CO_WQ_LOG_INFO("[quic dispatcher] find channel miss %s (tracked=%zu)",
                           key.c_str(),
                           _addr_to_channel.size());
            return {};
        }
        CO_WQ_LOG_INFO("[quic dispatcher] find channel by %s -> %p",
                       key.c_str(),
                       static_cast<void*>(it->second.lock().get()));
        return it->second.lock();
    }

    bool assign_channel_bio(SSL* ssl, const channel_ptr& channel_handle)
    {
        if (!ssl || !channel_handle)
            return false;

        BIO* bio = create_channel_bio(channel_handle);
        if (!bio)
            return false;

        if (BIO_up_ref(bio) != 1) {
            BIO_free(bio);
            return false;
        }

        SSL_set0_rbio(ssl, bio);
        SSL_set0_wbio(ssl, bio);
        return true;
    }

    void pump_inbound()
    {
        std::array<uint8_t, 2048> buffer {};
        sockaddr_storage          addr {};
        socklen_t                 addr_len = sizeof(addr);

        while (true) {
            addr_len          = sizeof(addr);
            const auto result = os::recvfrom(_udp.native_handle(),
                                             buffer.data(),
                                             buffer.size(),
                                             MSG_DONTWAIT,
                                             reinterpret_cast<sockaddr*>(&addr),
                                             &addr_len);
            if (result < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                break;
            }
            if (result == 0)
                continue;

            const auto bytes = static_cast<size_t>(result);
            datagram   packet;
            packet.addr     = addr;
            packet.addr_len = addr_len;
            packet.payload.resize(bytes);
            std::memcpy(packet.payload.data(), buffer.data(), bytes);
            const std::string preview     = channel_bio::format_payload_preview(packet.payload.data(), bytes, 12);
            const std::string source_text = make_address_key(reinterpret_cast<const sockaddr*>(&packet.addr),
                                                             packet.addr_len);

            auto target = find_channel_by_addr(reinterpret_cast<const sockaddr*>(&packet.addr), packet.addr_len);
            auto listener_handle    = listener_channel();
            bool target_is_listener = false;
            if (!target) {
                CO_WQ_LOG_INFO("[quic dispatcher] inbound datagram %zu bytes from %s preview=%s not matched",
                               bytes,
                               source_text.c_str(),
                               preview.c_str());
                target             = listener_handle;
                target_is_listener = static_cast<bool>(target);
            } else if (listener_handle && target.get() == listener_handle.get()) {
                target_is_listener = true;
            }
            if (!target)
                continue;
            size_t backlog_before = target->pending_inbound();
            if (target_is_listener) {
                target->set_peer(reinterpret_cast<const sockaddr*>(&packet.addr), packet.addr_len);
                bind_channel_peer(target, reinterpret_cast<const sockaddr*>(&packet.addr), packet.addr_len);
            }
            CO_WQ_LOG_INFO("[quic dispatcher] route inbound %zu bytes to channel=%p listener=%d backlog_before=%zu",
                           bytes,
                           static_cast<void*>(target.get()),
                           target_is_listener ? 1 : 0,
                           backlog_before);
            target->push_inbound(std::move(packet));
        }
    }

    void pump_outbound()
    {
        std::vector<channel_ptr> snapshot;
        {
            std::lock_guard<lock> guard { _channels_lock };
            snapshot.reserve(_channels.size());
            for (auto& [ssl, ch] : _channels) {
                (void)ssl;
                if (ch)
                    snapshot.push_back(ch);
            }
        }

        for (auto& ch : snapshot)
            flush_channel_outbound(ch);
    }

    void flush_channel_outbound(const channel_ptr& ch)
    {
        if (!ch)
            return;

        while (true) {
            auto packet_opt = ch->pop_outbound();
            if (!packet_opt)
                break;

            auto& packet = *packet_opt;
            if (packet.payload.empty())
                continue;

            sockaddr_storage dest     = packet.addr;
            socklen_t        dest_len = packet.addr_len;
            if (dest_len == 0) {
                if (!ch->peer_address(dest, dest_len))
                    continue;
            }

            const auto bytes = packet.payload.size();
            const auto sent  = os::sendto(_udp.native_handle(),
                                         packet.payload.data(),
                                         bytes,
                                         MSG_DONTWAIT | MSG_NOSIGNAL,
                                         reinterpret_cast<const sockaddr*>(&dest),
                                         dest_len);
            if (sent < 0) {
                CO_WQ_LOG_ERROR("[quic dispatcher] sendto failed: %s", std::strerror(errno));
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    ch->push_outbound_front(std::move(packet));
                    break;
                }
                CO_WQ_LOG_ERROR("[quic dispatcher] dropping outbound datagram (%zu bytes)", bytes);
                continue;
            }
            CO_WQ_LOG_INFO("[quic dispatcher] sent %zu bytes", static_cast<size_t>(sent));
        }
    }

    void resume_pending()
    {
#if !defined(_WIN32)
        std::vector<io_waiter_base*> pending;
        {
            std::scoped_lock<lock> lk(_waiters_lock);
            if (_waiters.empty()) {
                arm_waker();
                return;
            }
            pending.swap(_waiters);
        }
        for (auto* waiter : pending) {
            if (!waiter)
                continue;
            clear_waiter_timeout(waiter);
            _reactor.cancel_waiter(_udp.native_handle(), waiter);
            post_via_route(_exec, *waiter);
        }
        arm_waker();
#endif
    }

#if !defined(_WIN32)
    wake_waiter                  _wake_waiter {};
    os::fd_t                     _wake_pipe[2] { os::invalid_fd(), os::invalid_fd() };
    std::vector<io_waiter_base*> _waiters;
    lock                         _waiters_lock;
#endif

private:
    struct timeout_entry {
        quic_dispatcher*      owner { nullptr };
        worknode::work_func_t previous { nullptr };
    };

    static void waiter_timeout_adapter(worknode* node)
    {
        auto* waiter = static_cast<io_waiter_base*>(node);
        if (!waiter)
            return;

        timeout_entry entry {};
        {
            std::lock_guard<std::mutex> guard { s_timeout_mutex };
            auto                        it = s_timeout_entries.find(waiter);
            if (it != s_timeout_entries.end()) {
                entry = it->second;
                s_timeout_entries.erase(it);
                if (waiter->timeout_func == &waiter_timeout_adapter)
                    waiter->set_timeout_callback(entry.previous);
            }
        }

        if (entry.owner)
            entry.owner->handle_waiter_timeout(waiter);

        if (entry.previous)
            entry.previous(node);
    }

    void register_waiter_timeout(io_waiter_base* waiter)
    {
        if (!waiter)
            return;
        timeout_entry entry { this, waiter->timeout_func };
        {
            std::lock_guard<std::mutex> guard { s_timeout_mutex };
            s_timeout_entries[waiter] = entry;
        }
        waiter->set_timeout_callback(&waiter_timeout_adapter);
    }

    void clear_waiter_timeout(io_waiter_base* waiter)
    {
        if (!waiter)
            return;

        timeout_entry entry {};
        bool          restore_previous = false;
        {
            std::lock_guard<std::mutex> guard { s_timeout_mutex };
            auto                        it = s_timeout_entries.find(waiter);
            if (it == s_timeout_entries.end())
                return;
            entry            = it->second;
            restore_previous = (waiter->timeout_func == &waiter_timeout_adapter);
            s_timeout_entries.erase(it);
        }

        if (restore_previous)
            waiter->set_timeout_callback(entry.previous);
    }

    void handle_waiter_timeout(io_waiter_base* waiter)
    {
        CO_WQ_LOG_DEBUG("[quic dispatcher] waiter timeout waiter=%p", static_cast<void*>(waiter));
        untrack_waiter(waiter);
    }

    static inline std::mutex                                         s_timeout_mutex {};
    static inline std::unordered_map<io_waiter_base*, timeout_entry> s_timeout_entries {};

    mutable lock                          _channels_lock;
    std::unordered_map<SSL*, channel_ptr> _channels;

    mutable lock                                            _listener_lock;
    std::weak_ptr<channel>                                  _listener_channel;
    mutable lock                                            _addr_lock;
    std::unordered_map<std::string, std::weak_ptr<channel>> _addr_to_channel;
    std::unordered_map<const channel*, std::string>         _channel_to_addr;

    workqueue_type&  _exec;
    reactor_type&    _reactor;
    udp_socket_type& _udp;
    SSL*             _listener { nullptr };
};

struct quic_accept_result {
    int  rc { 0 };
    SSL* ssl { nullptr };
};

/**
 * @brief Awaiter：用于等待 OpenSSL QUIC listener 有新的连接可接受。
 */
template <lockable lock, template <class> class Reactor> class quic_accept_awaiter : public io_waiter_base {
public:
    using dispatcher_type = quic_dispatcher<lock, Reactor>;

    quic_accept_awaiter(dispatcher_type& dispatcher, SSL* listener = nullptr, std::atomic_bool* cancel = nullptr)
        : _dispatcher(dispatcher), _listener(listener ? listener : dispatcher.listener()), _cancel(cancel)
    {
        this->route_ctx  = &_dispatcher.exec();
        this->route_post = &dispatcher_type::post_adapter;
        this->debug_name = "quic_accept";
    }

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h)
    {
        this->h = h;
        drive();
    }

    quic_accept_result await_resume() noexcept
    {
        _dispatcher.untrack_waiter(this);
        return _result;
    }

private:
    void drive()
    {
        if (!_listener) {
            _result.rc  = -EINVAL;
            _result.ssl = nullptr;
            if (this->h)
                this->h.resume();
            return;
        }

        if (_cancel && _cancel->load(std::memory_order_acquire)) {
            _result.rc  = -ECANCELED;
            _result.ssl = nullptr;
            if (this->h)
                this->h.resume();
            return;
        }

        _dispatcher.drive_listener();
        while (true) {
            SSL* accepted = SSL_accept_connection(_listener, SSL_ACCEPT_CONNECTION_NO_BLOCK);
            if (accepted) {
                _result.rc  = 0;
                _result.ssl = accepted;
                if (this->h)
                    this->h.resume();
                return;
            }

            int rc = SSL_handle_events(_listener);
            if (rc > 0) {
                uint32_t mask = _dispatcher.wait_mask(_listener);
                this->func    = &quic_accept_awaiter::resume_cb;
                ERR_clear_error();
                _dispatcher.track_waiter(this);
                _dispatcher.wait_with_timeout(this, mask, std::chrono::milliseconds(200));
                return;
            }

            int ssl_err = SSL_get_error(_listener, rc);
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE
                || (rc == 0 && ssl_err == SSL_ERROR_SYSCALL && errno == 0)) {
                uint32_t mask = _dispatcher.wait_mask(_listener);
                this->func    = &quic_accept_awaiter::resume_cb;
                ERR_clear_error();
                _dispatcher.track_waiter(this);
                _dispatcher.wait_with_timeout(this, mask, std::chrono::milliseconds(200));
                return;
            }

            _result.rc  = quic_detail::translate_failure(ssl_err);
            _result.ssl = nullptr;
            if (this->h)
                this->h.resume();
            return;
        }
    }

    static void resume_cb(worknode* node)
    {
        auto* self = static_cast<quic_accept_awaiter*>(node);
        if (!self)
            return;
        self->_dispatcher.untrack_waiter(self);
        self->drive();
    }

    dispatcher_type&   _dispatcher;
    SSL*               _listener { nullptr };
    std::atomic_bool*  _cancel { nullptr };
    quic_accept_result _result {};
};

} // namespace co_wq::net
