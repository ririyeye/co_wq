#include "http_easy_server.hpp"

#include "http1_parser.hpp"
#include "http2_server_session.hpp"
#include "http_common.hpp"

#if defined(USING_SSL)
#include "../tls.hpp"
#endif

#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#if !defined(_WIN32)
#include <netdb.h>
#include <sys/socket.h>
#endif
#if !defined(USING_SSL)
#include <stdexcept>
#endif
#include <string>
#include <string_view>
#include <utility>

namespace co_wq::net::http {
namespace {

    std::string format_sockaddr(const sockaddr* addr, socklen_t len)
    {
        if (!addr || len <= 0)
            return {};

        char host[NI_MAXHOST] = {};
        char serv[NI_MAXSERV] = {};
        int  rc = ::getnameinfo(addr, len, host, sizeof(host), serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV);
        if (rc != 0)
            return {};

        std::string result;
        if (addr->sa_family == AF_INET6) {
            result.push_back('[');
            result.append(host);
            result.push_back(']');
        } else {
            result.append(host);
        }
        result.push_back(':');
        result.append(serv);
        return result;
    }

} // namespace

HttpEasyServer::HttpEasyServer(HttpServerWorkqueue& wq) : server_wq_(wq) { }

bool HttpEasyServer::start(const Config& config)
{
    std::lock_guard lock(mutex_);
    if (running_.load(std::memory_order_acquire))
        return false;

    config_ = config;
    stop_requested_.store(false, std::memory_order_release);
    finished_ = false;

#if defined(USING_SSL)
    if (config_.tls) {
        tls_context_.emplace(net::tls_context::make_server_with_pem(config_.tls->cert_file, config_.tls->key_file));
        if (config_.enable_http2)
            SSL_CTX_set_alpn_select_cb(tls_context_->native_handle(), &HttpEasyServer::select_http_alpn, this);
        CO_WQ_LOG_INFO("[http] TLS certificate loaded: cert=%s key=%s",
                       config_.tls->cert_file.c_str(),
                       config_.tls->key_file.c_str());
    } else {
        tls_context_.reset();
    }
#else
    if (config_.tls)
        throw std::runtime_error("TLS support requires USING_SSL");
#endif

    server_task_         = run_server(config_);
    coroutine_           = server_task_.get();
    auto& promise        = coroutine_.promise();
    promise.mUserData    = this;
    promise.mOnCompleted = &HttpEasyServer::on_server_completed;

    running_.store(true, std::memory_order_release);
    post_to(server_task_, server_wq_.base());
    return true;
}

void HttpEasyServer::request_stop()
{
    stop_requested_.store(true, std::memory_order_release);
    auto fd = listener_fd_.exchange(net::os::invalid_fd(), std::memory_order_acq_rel);
    if (fd != net::os::invalid_fd()) {
        server_wq_.reactor().remove_fd(fd);
        net::os::close_fd(fd);
    }
}

void HttpEasyServer::wait()
{
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return finished_; });
}

void HttpEasyServer::on_server_completed(Promise_base& promise)
{
    auto* self = static_cast<HttpEasyServer*>(promise.mUserData);
    if (!self)
        return;
    {
        std::lock_guard lock(self->mutex_);
        self->running_.store(false, std::memory_order_release);
        self->finished_ = true;
    }
    self->cv_.notify_all();
}

#if defined(USING_SSL)
int HttpEasyServer::select_http_alpn(SSL*,
                                     const unsigned char** out,
                                     unsigned char*        outlen,
                                     const unsigned char*  in,
                                     unsigned int          inlen,
                                     void*)
{
    unsigned int pos = 0;
    while (pos < inlen) {
        unsigned int         len   = in[pos];
        const unsigned char* proto = in + pos + 1;
        if (len == 2 && std::memcmp(proto, "h2", 2) == 0) {
            *out    = proto;
            *outlen = static_cast<unsigned char>(len);
            return SSL_TLSEXT_ERR_OK;
        }
        pos += 1 + len;
    }
    pos = 0;
    while (pos < inlen) {
        unsigned int         len   = in[pos];
        const unsigned char* proto = in + pos + 1;
        if (len == 8 && std::memcmp(proto, "http/1.1", 8) == 0) {
            *out    = proto;
            *outlen = static_cast<unsigned char>(len);
            return SSL_TLSEXT_ERR_OK;
        }
        pos += 1 + len;
    }
    return SSL_TLSEXT_ERR_NOACK;
}
#endif

HttpEasyServer::TaskType HttpEasyServer::run_server(Config config_copy)
{
    Config config = std::move(config_copy);
    stop_requested_.store(false, std::memory_order_release);

    net::tcp_listener<SpinLock> listener(server_wq_.base(), server_wq_.reactor());
    try {
        listener.bind_listen(config.host, config.port, static_cast<int>(config.backlog));
    } catch (const std::exception& ex) {
        CO_WQ_LOG_ERROR("[http] failed to bind %s:%u: %s",
                        config.host.c_str(),
                        static_cast<unsigned>(config.port),
                        ex.what());
        listener_fd_.store(net::os::invalid_fd(), std::memory_order_release);
        running_.store(false, std::memory_order_release);
        finished_ = true;
        cv_.notify_all();
        co_return;
    }

    listener_fd_.store(listener.native_handle(), std::memory_order_release);
    CO_WQ_LOG_INFO("[http] listening on %s:%u", config.host.c_str(), static_cast<unsigned>(config.port));

#if defined(USING_SSL)
    const net::tls_context* tls_ctx_ptr = tls_context_ ? &*tls_context_ : nullptr;
#else
    const net::tls_context* tls_ctx_ptr = nullptr;
#endif

    while (!stop_requested_.load(std::memory_order_acquire)) {
        int fd = co_await listener.accept();
        if (stop_requested_.load(std::memory_order_acquire))
            break;
        if (fd == net::k_accept_fatal) {
            CO_WQ_LOG_ERROR("[http] accept fatal error, shutting down");
            break;
        }
        if (fd < 0)
            continue;

        auto peer_desc = describe_peer(fd);
#if defined(USING_SSL)
        if (tls_ctx_ptr) {
            try {
                auto tls_sock = server_wq_.adopt_tls_socket(fd, *tls_ctx_ptr, net::tls_mode::Server);
                auto task     = handle_http_connection(std::move(tls_sock), true, peer_desc);
                post_to(task, server_wq_.base());
            } catch (const std::exception& ex) {
                CO_WQ_LOG_ERROR("[http] tls socket setup failed: %s", ex.what());
            }
            continue;
        }
#endif
        auto socket = server_wq_.adopt_tcp_socket(fd);
        auto task   = handle_http_connection(std::move(socket), false, std::move(peer_desc));
        post_to(task, server_wq_.base());
    }

    listener.close();
    listener_fd_.store(net::os::invalid_fd(), std::memory_order_release);
    co_return;
}

std::string HttpEasyServer::describe_peer(int fd) const
{
    sockaddr_storage addr {};
    socklen_t        len = sizeof(addr);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        return {};
    return format_sockaddr(reinterpret_cast<sockaddr*>(&addr), len);
}

namespace {
    static constexpr std::string_view k_http2_preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
}

template <typename Socket>
HttpEasyServer::TaskType HttpEasyServer::handle_http_connection(Socket sock, bool is_tls, std::string peer_desc)
{
    bool        use_http2 = false;
    std::string initial_data;

    if constexpr (requires(Socket& s) { s.handshake(); }) {
        int hs = co_await sock.handshake();
        if (hs != 0) {
            CO_WQ_LOG_ERROR("[http] tls handshake failed, code=%d", hs);
            sock.close();
            co_return;
        }
#if defined(USING_SSL)
        if (config_.enable_http2) {
            const unsigned char* alpn     = nullptr;
            unsigned int         alpn_len = 0;
            SSL_get0_alpn_selected(sock.ssl_handle(), &alpn, &alpn_len);
            if (alpn_len == 2 && alpn && std::memcmp(alpn, "h2", 2) == 0)
                use_http2 = true;
        }
#endif
    }

    if (config_.enable_http2 && !use_http2) {
        std::array<char, 4096> probe {};
        size_t                 read_total = 0;
        bool                   mismatch   = false;
        while (read_total < k_http2_preface.size()) {
            ssize_t n = co_await sock.recv(probe.data() + read_total, probe.size() - read_total);
            if (n <= 0) {
                sock.close();
                co_return;
            }
            read_total += static_cast<size_t>(n);
            initial_data.assign(probe.data(), read_total);
            size_t compare_len = std::min(initial_data.size(), k_http2_preface.size());
            if (initial_data.compare(0, compare_len, k_http2_preface.substr(0, compare_len)) != 0) {
                mismatch = true;
                break;
            }
            if (read_total >= probe.size() || read_total >= k_http2_preface.size())
                break;
        }
        if (!mismatch && initial_data.size() >= k_http2_preface.size())
            use_http2 = true;
    }

    if (config_.verbose_logging) {
        if (peer_desc.empty())
            peer_desc = "unknown";
        CO_WQ_LOG_INFO("[http] client connected: %s", peer_desc.c_str());
    }

    if (use_http2) {
        co_await handle_http2_connection(std::move(sock), std::move(initial_data));
    } else {
        co_await handle_http1_connection(std::move(sock), std::move(initial_data), is_tls, peer_desc);
    }

    if (config_.verbose_logging)
        CO_WQ_LOG_INFO("[http] client disconnected: %s", peer_desc.empty() ? "unknown" : peer_desc.c_str());

    co_return;
}

template <typename Socket>
HttpEasyServer::TaskType HttpEasyServer::handle_http1_connection(Socket             sock,
                                                                 std::string        initial_data,
                                                                 bool               is_tls,
                                                                 const std::string& peer_desc)
{
    auto                   parser = Http1Parser::make_request();
    std::array<char, 4096> buffer {};
    bool                   parse_error  = false;
    std::string            error_reason = "";
    bool                   received_any_data { false };
    bool                   client_disconnected_early { false };

    if (!initial_data.empty()) {
        received_any_data = true;
        if (!parser.feed(std::string_view(initial_data.data(), initial_data.size()), &error_reason))
            parse_error = true;
    }

    while (!parser.is_message_complete() && !parse_error) {
        ssize_t n = co_await sock.recv(buffer.data(), buffer.size());
        if (n < 0) {
            parse_error  = true;
            error_reason = "socket read error";
            break;
        }
        if (n == 0) {
            if (!received_any_data) {
                client_disconnected_early = true;
            } else {
                if (!parser.finish(&error_reason))
                    parse_error = true;
            }
            break;
        }
        received_any_data = true;
        if (!parser.feed(std::string_view(buffer.data(), static_cast<size_t>(n)), &error_reason))
            parse_error = true;
    }

    if (client_disconnected_early) {
        sock.close();
        co_return;
    }

    if (!parser.is_message_complete() && !parse_error) {
        if (!parser.finish(&error_reason))
            parse_error = true;
    }

    std::string response;

    if (parse_error) {
        HttpResponse error_resp;
        error_resp.status_code             = 400;
        error_resp.reason                  = "Bad Request";
        error_resp.body                    = "Bad Request\n";
        error_resp.headers["content-type"] = "text/plain; charset=utf-8";
        response                           = build_http1_response(error_resp);
        CO_WQ_LOG_ERROR("[http] parse error: %s",
                        error_reason.empty() ? parser.last_error().c_str() : error_reason.c_str());
    } else {
        HttpRequest req = parser.request();

        if (req.path.empty()) {
            std::string_view target_view = req.target.empty() ? std::string_view("/") : std::string_view(req.target);
            if (target_view.rfind("http://", 0) == 0 || target_view.rfind("https://", 0) == 0) {
                auto scheme_pos = target_view.find("//");
                if (scheme_pos != std::string_view::npos) {
                    std::string_view host_and_path = target_view.substr(scheme_pos + 2);
                    if (auto slash_pos = host_and_path.find('/'); slash_pos != std::string_view::npos)
                        target_view = host_and_path.substr(slash_pos);
                    else
                        target_view = std::string_view("/");
                }
            }

            size_t qpos = target_view.find('?');
            size_t fpos = target_view.find('#');
            size_t cut  = std::min(qpos == std::string_view::npos ? target_view.size() : qpos,
                                  fpos == std::string_view::npos ? target_view.size() : fpos);
            req.path.assign(target_view.substr(0, cut));
            if (req.path.empty())
                req.path = "/";

            if (qpos != std::string_view::npos) {
                size_t query_end = (fpos != std::string_view::npos && fpos > qpos) ? fpos : target_view.size();
                req.query.assign(target_view.substr(qpos + 1, query_end - qpos - 1));
            } else {
                req.query.clear();
            }
        }

        if (req.scheme.empty())
            req.scheme = is_tls ? "https" : "http";

        if (req.host.empty()) {
            if (auto host_hdr = req.header("host")) {
                std::string host_value = *host_hdr;
                std::string parsed_host;
                uint16_t    parsed_port = 0;
                if (split_host_port(host_value, is_tls ? 443 : 80, parsed_host, parsed_port)) {
                    req.host = std::move(parsed_host);
                    req.port = static_cast<int>(parsed_port);
                } else {
                    req.host = std::move(host_value);
                    req.port = is_tls ? 443 : 80;
                }
            }
        }
        auto build_forwarded_for = [](const std::string& peer) -> std::string {
            if (peer.empty())
                return {};
            if (peer.front() == '[') {
                const auto pos = peer.find(']');
                if (pos != std::string::npos)
                    return peer.substr(1, pos - 1);
                return peer;
            }
            const auto pos = peer.find_last_of(':');
            if (pos == std::string::npos)
                return peer;
            return peer.substr(0, pos);
        };

        auto build_upstream_target = [&](const HttpRouter::ReverseProxyConfig& cfg,
                                         const HttpRouter::MatchResult&        match_result,
                                         std::string_view                      target) {
            std::string_view suffix = target;
            if (cfg.strip_prefix && match_result.allow_prefix && suffix.size() >= match_result.route_path.size())
                suffix.remove_prefix(match_result.route_path.size());

            if (suffix.empty())
                return cfg.upstream_path.empty() ? std::string("/") : cfg.upstream_path;

            std::string upstream_path = cfg.upstream_path.empty() ? std::string("/") : cfg.upstream_path;

            if (!upstream_path.empty() && upstream_path.back() == '/' && suffix.front() == '/')
                upstream_path.pop_back();
            if (upstream_path.empty())
                upstream_path = "/";

            if (upstream_path.back() != '/' && suffix.front() != '?' && suffix.front() != '#'
                && suffix.front() != '/') {
                upstream_path.push_back('/');
            }

            upstream_path.append(suffix.data(), suffix.size());
            return upstream_path;
        };

        auto host_header_for_upstream = [](const HttpRouter::ReverseProxyConfig& cfg) {
            std::string host_value   = cfg.host;
            const bool  is_https     = cfg.scheme == "https";
            const bool  default_port = (is_https && cfg.port == 443) || (!is_https && cfg.port == 80) || cfg.port == 0;
            if (!default_port && cfg.port != 0) {
                host_value.push_back(':');
                host_value.append(std::to_string(cfg.port));
            }
            return host_value;
        };

        auto is_hop_by_hop = [](std::string_view name_lower) {
            return name_lower == "connection" || name_lower == "proxy-connection" || name_lower == "keep-alive"
                || name_lower == "transfer-encoding" || name_lower == "upgrade";
        };

        HttpResponse app;
        bool         handled = false;
        auto         match   = router_.match(req);

        if (match.matched && match.proxy != nullptr) {
            const auto&      cfg           = *match.proxy;
            std::string_view target_view   = req.target.empty() ? std::string_view("/") : std::string_view(req.target);
            std::string      upstream_path = build_upstream_target(cfg, match, target_view);
            std::string      host_value    = host_header_for_upstream(cfg);
            std::string      upstream_url  = cfg.scheme;
            upstream_url.append("//");
            upstream_url.append(host_value);
            upstream_url.append(upstream_path);

            EasyRequest upstream_request;
            upstream_request.url                  = std::move(upstream_url);
            upstream_request.method               = req.method.empty() ? std::string("GET") : req.method;
            upstream_request.body                 = req.body;
            upstream_request.drop_content_headers = true;
            upstream_request.buffer_body          = true;
            upstream_request.prefer_http2         = cfg.scheme == "https";
            upstream_request.verbose              = config_.verbose_logging;

            std::vector<HeaderEntry> forwarded_headers;
            forwarded_headers.reserve(req.headers.size() + 4);

            std::string client_host;
            for (const auto& kv : req.headers) {
                const std::string lower_name = to_lower(kv.first);
                if (is_hop_by_hop(lower_name))
                    continue;
                if (!cfg.preserve_host && lower_name == "host")
                    continue;
                forwarded_headers.push_back({ kv.first, kv.second });
                if (lower_name == "host" && client_host.empty())
                    client_host = kv.second;
            }

            if (!cfg.preserve_host)
                forwarded_headers.push_back({ "Host", host_value });

            std::string forwarded_proto = req.scheme.empty() ? (is_tls ? "https" : "http") : req.scheme;
            forwarded_headers.push_back({ "X-Forwarded-Proto", forwarded_proto });

            if (!client_host.empty())
                forwarded_headers.push_back({ "X-Forwarded-Host", client_host });

            const std::string peer_only = build_forwarded_for(peer_desc);
            if (!peer_only.empty()) {
                auto header_it = std::find_if(
                    forwarded_headers.begin(),
                    forwarded_headers.end(),
                    [](const HeaderEntry& hdr) { return iequals(hdr.name, "x-forwarded-for"); });
                if (header_it != forwarded_headers.end()) {
                    header_it->value.push_back(',');
                    header_it->value.push_back(' ');
                    header_it->value.append(peer_only);
                } else {
                    forwarded_headers.push_back({ "X-Forwarded-For", peer_only });
                }
            }

            upstream_request.headers = std::move(forwarded_headers);

            HttpEasyClient client(server_wq_);
            EasyResponse   upstream_response;
            int            rc = co_await client.perform(upstream_request, upstream_response);

            if (rc != 0 || !upstream_response.message_complete) {
                app.set_status(502, "Bad Gateway");
                app.set_header("content-type", "text/plain; charset=utf-8");
                app.set_body("Bad Gateway\n");
            } else {
                if (upstream_response.status_code == 0)
                    upstream_response.status_code = 502;
                app.set_status(upstream_response.status_code, upstream_response.reason);
                for (const auto& header : upstream_response.headers) {
                    if (is_hop_by_hop(to_lower(header.name)))
                        continue;
                    app.set_header(header.name, header.value);
                }
                app.set_body(upstream_response.body);
            }

            router_.apply_middlewares(req, app);
            handled = true;
        } else if (match.matched && match.handler) {
            app = match.handler(req);
            router_.apply_middlewares(req, app);
            handled = true;
        }

        if (!handled)
            app = router_.handle(req);

        response = build_http1_response(app);
    }

    (void)co_await sock.send_all(response.data(), response.size());
    sock.close();
    co_return;
}

template <typename Socket>
HttpEasyServer::TaskType HttpEasyServer::handle_http2_connection(Socket sock, std::string initial_data)
{
    Http2ServerSession session([&](const HttpRequest& req) { return router_.handle(req); });

    if (!session.init()) {
        CO_WQ_LOG_ERROR("[http] http/2 session init failed: %s", session.last_error().c_str());
        sock.close();
        co_return;
    }

    std::array<nghttp2_settings_entry, 1> settings { { { NGHTTP2_SETTINGS_ENABLE_PUSH, 0 } } };
    if (session.submit_settings(settings) != 0) {
        CO_WQ_LOG_ERROR("[http] submit settings failed: %s", session.last_error().c_str());
        sock.close();
        co_return;
    }

    while (!session.pending_send_data().empty()) {
        auto    pending = session.pending_send_data();
        ssize_t sent    = co_await sock.send_all(reinterpret_cast<const char*>(pending.data()), pending.size());
        if (sent <= 0) {
            sock.close();
            co_return;
        }
        session.consume_send_data(static_cast<size_t>(sent));
    }

    if (!initial_data.empty()) {
        std::string error_reason;
        if (!session.feed(std::string_view(initial_data.data(), initial_data.size()), &error_reason)) {
            CO_WQ_LOG_ERROR("[http] http/2 preface error: %s",
                            error_reason.empty() ? session.last_error().c_str() : error_reason.c_str());
            sock.close();
            co_return;
        }
        while (!session.pending_send_data().empty()) {
            auto    pending = session.pending_send_data();
            ssize_t sent    = co_await sock.send_all(reinterpret_cast<const char*>(pending.data()), pending.size());
            if (sent <= 0) {
                sock.close();
                co_return;
            }
            session.consume_send_data(static_cast<size_t>(sent));
        }
    }

    std::array<char, 8192> buffer {};
    while (session.want_read() || session.want_write()) {
        ssize_t n = co_await sock.recv(buffer.data(), buffer.size());
        if (n <= 0)
            break;

        std::string error_reason;
        if (!session.feed(std::string_view(buffer.data(), static_cast<size_t>(n)), &error_reason)) {
            CO_WQ_LOG_ERROR("[http] http/2 decode error: %s",
                            error_reason.empty() ? session.last_error().c_str() : error_reason.c_str());
            break;
        }

        while (!session.pending_send_data().empty()) {
            auto    pending = session.pending_send_data();
            ssize_t sent    = co_await sock.send_all(reinterpret_cast<const char*>(pending.data()), pending.size());
            if (sent <= 0) {
                sock.close();
                co_return;
            }
            session.consume_send_data(static_cast<size_t>(sent));
        }
    }

    sock.close();
    co_return;
}

using TcpSocket = net::tcp_socket<SpinLock, epoll_reactor>;

template HttpEasyServer::TaskType HttpEasyServer::handle_http_connection<TcpSocket>(TcpSocket, bool, std::string);
template HttpEasyServer::TaskType
HttpEasyServer::handle_http1_connection<TcpSocket>(TcpSocket, std::string, bool, const std::string&);
template HttpEasyServer::TaskType HttpEasyServer::handle_http2_connection<TcpSocket>(TcpSocket, std::string);

#if defined(USING_SSL)
using TlsSocket = net::tls_socket<SpinLock, epoll_reactor>;

template HttpEasyServer::TaskType HttpEasyServer::handle_http_connection<TlsSocket>(TlsSocket, bool, std::string);
template HttpEasyServer::TaskType
HttpEasyServer::handle_http1_connection<TlsSocket>(TlsSocket, std::string, bool, const std::string&);
template HttpEasyServer::TaskType HttpEasyServer::handle_http2_connection<TlsSocket>(TlsSocket, std::string);
#endif

} // namespace co_wq::net::http
