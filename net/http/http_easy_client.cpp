#include "http_easy_client.hpp"

#include "http_common.hpp"
#if defined(USING_SSL)
#include "../tls_utils.hpp"
#include <openssl/x509_vfy.h>
#endif

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace co_wq::net::http {
namespace {

    int verbose_printf(bool enabled, const char* fmt, ...)
    {
        if (!enabled)
            return 0;
        va_list args;
        va_start(args, fmt);
        const int rc = std::vfprintf(stderr, fmt, args);
        va_end(args);
        return rc;
    }

    struct BuiltRequest {
        std::string              payload;
        std::vector<HeaderEntry> headers;
    };

    BuiltRequest build_http_request(const EasyRequest& base, const UrlParts& url)
    {
        BuiltRequest result;
        result.headers = base.headers;

        if (base.drop_content_headers)
            remove_content_headers(result.headers);

        const bool has_body  = !base.body.empty();
        const bool send_body = has_body && !base.drop_content_headers;

        if (!header_exists(result.headers, "Host")) {
            std::string host_value   = url.host;
            const bool  is_https     = url.scheme == "https";
            const bool  default_port = (is_https && url.port == 443) || (!is_https && url.port == 80);
            if (!default_port) {
                host_value.push_back(':');
                host_value.append(std::to_string(url.port));
            }
            result.headers.push_back({ "Host", std::move(host_value) });
        }

        if (!header_exists(result.headers, "User-Agent"))
            result.headers.push_back({ "User-Agent", base.user_agent });

        if (!header_exists(result.headers, "Accept"))
            result.headers.push_back({ "Accept", "*/*" });

        if (!header_exists(result.headers, "Connection"))
            result.headers.push_back({ "Connection", "close" });

        if (send_body && !header_exists(result.headers, "Content-Length")
            && !header_exists(result.headers, "Transfer-Encoding"))
            result.headers.push_back({ "Content-Length", std::to_string(base.body.size()) });

        std::string_view path = url.path.empty() ? std::string_view { "/" } : std::string_view { url.path };
        result.payload.reserve(base.method.size() + path.size() + 32 + (send_body ? base.body.size() : 0));
        result.payload.append(base.method);
        result.payload.push_back(' ');
        result.payload.append(path.data(), path.size());
        result.payload.append(" HTTP/1.1\r\n");
        for (const auto& header : result.headers) {
            result.payload.append(header.name);
            result.payload.append(": ");
            result.payload.append(header.value);
            result.payload.append("\r\n");
        }
        result.payload.append("\r\n");
        if (send_body)
            result.payload.append(base.body);
        return result;
    }

    void log_request_verbose(const UrlParts& url, const EasyRequest& req, const BuiltRequest& built)
    {
        verbose_printf(req.verbose,
                       "> %s %s HTTP/1.1\n",
                       req.method.c_str(),
                       url.path.empty() ? "/" : url.path.c_str());
        if (!req.verbose)
            return;
        for (const auto& header : built.headers)
            std::fprintf(stderr, "> %s: %s\n", header.name.c_str(), header.value.c_str());
        std::fprintf(stderr, ">\n");
        if (!req.body.empty())
            std::fprintf(stderr, "> [%zu bytes of body]\n", req.body.size());
    }

    std::vector<net::dns::resolve_result::endpoint_entry> gather_endpoints(const net::dns::resolve_result& resolved)
    {
        auto endpoints = resolved.endpoints;
        if (endpoints.empty() && resolved.success && resolved.length > 0) {
            net::dns::resolve_result::endpoint_entry entry {};
            entry.addr = resolved.storage;
            entry.len  = resolved.length;
            endpoints.push_back(entry);
        }
        return endpoints;
    }

} // namespace

void EasyResponse::reset()
{
    status_code = 0;
    reason.clear();
    headers.clear();
    header_lookup.clear();
    used_http2       = false;
    headers_complete = false;
    message_complete = false;
    body.clear();
}

std::optional<std::string> EasyResponse::header(std::string_view name) const
{
    auto it = header_lookup.find(to_lower(name));
    if (it == header_lookup.end())
        return std::nullopt;
    return it->second;
}

HttpEasyClient::HttpEasyClient(HttpClientWorkqueue& fdwq) : fdwq_(fdwq) { }

HttpEasyClient::TaskInt HttpEasyClient::perform(const EasyRequest& request, EasyResponse& response)
{
    response.reset();

    auto parsed_url = parse_url(request.url);
    if (!parsed_url)
        co_return -EINVAL;

    BuiltRequest built = build_http_request(request, *parsed_url);
    log_request_verbose(*parsed_url, request, built);

    auto perform_http1_request = [&](auto& socket) -> HttpEasyClient::TaskInt {
        Http1Parser parser   = Http1Parser::make_response();
        auto&       resp     = parser.response_parser();
        resp.verbose         = request.verbose;
        resp.include_headers = request.include_headers;
        resp.buffer_body     = request.buffer_body;
        resp.output          = request.output;

        ssize_t sent = co_await socket.send_all(built.payload.data(), built.payload.size());
        if (sent < 0)
            co_return -errno;

        std::array<char, 8192> buffer {};
        while (!parser.is_message_complete()) {
            ssize_t n = co_await socket.recv(buffer.data(), buffer.size());
            if (n < 0)
                co_return -errno;
            if (n == 0)
                break;
            std::string parse_error;
            if (!parser.feed(std::string_view(buffer.data(), static_cast<size_t>(n)), &parse_error))
                co_return -EIO;
        }

        if (!parser.is_message_complete()) {
            std::string parse_error;
            if (!parser.finish(&parse_error))
                co_return -EIO;
        }

        response.status_code      = resp.status_code;
        response.reason           = resp.status_text;
        response.headers          = resp.header_sequence;
        response.header_lookup    = resp.header_lookup;
        response.headers_complete = resp.headers_complete;
        response.message_complete = resp.message_complete;
        if (request.buffer_body)
            response.body = resp.body_buffer;
        response.used_http2 = false;
        co_return 0;
    };

    auto perform_http2_request = [&](auto& socket) -> HttpEasyClient::TaskInt {
#if !defined(USING_SSL)
        (void)socket;
        co_return -ENOTSUP;
#else
        Http2ClientSession session;
        if (!session.init())
            co_return -EIO;

        bool        stream_failed = false;
        std::string header_block;

        auto append_header = [&](std::string name, std::string value) {
            std::string lower             = to_lower(name);
            response.header_lookup[lower] = value;
            response.headers.push_back({ std::move(name), std::move(value) });
        };

        session.set_header_handler([&](std::string_view name, std::string_view value) {
            if (name == ":status") {
                response.status_code = session.response().status_code;
                response.reason.assign(value.begin(), value.end());
            } else if (!name.empty() && name.front() != ':') {
                append_header(std::string(name), std::string(value));
            }
        });

        session.set_headers_complete_handler([&](bool end_stream) {
            response.headers_complete = true;
            if (request.verbose) {
                std::fprintf(stderr, "< HTTP/2 %d %s\n", response.status_code, response.reason.c_str());
                for (const auto& header : response.headers)
                    std::fprintf(stderr, "< %s: %s\n", header.name.c_str(), header.value.c_str());
                std::fprintf(stderr, "<\n");
            }
            if (request.include_headers && request.output) {
                header_block.clear();
                header_block.append("HTTP/2 ");
                if (!response.reason.empty())
                    header_block.append(response.reason);
                else
                    header_block.append(std::to_string(response.status_code));
                header_block.append("\r\n");
                for (const auto& header : response.headers) {
                    header_block.append(header.name);
                    header_block.append(": ");
                    header_block.append(header.value);
                    header_block.append("\r\n");
                }
                header_block.append("\r\n");
                std::fwrite(header_block.data(), 1, header_block.size(), request.output);
            }
            if (end_stream) {
                response.message_complete = true;
                if (request.output)
                    std::fflush(request.output);
            }
        });

        session.set_data_handler([&](std::string_view data) {
            if (request.buffer_body)
                response.body.append(data.data(), data.size());
            else if (request.output)
                std::fwrite(data.data(), 1, data.size(), request.output);
        });

        session.set_stream_close_handler([&](uint32_t error_code) {
            if (error_code != NGHTTP2_NO_ERROR)
                stream_failed = true;
            if (!response.message_complete)
                response.message_complete = true;
        });

        std::array<nghttp2_settings_entry, 1> settings { { { NGHTTP2_SETTINGS_ENABLE_PUSH, 0 } } };
        if (session.submit_settings(settings) != 0)
            co_return -EIO;

        auto flush_pending = [&]() -> HttpEasyClient::TaskInt {
            while (!session.pending_send_data().empty()) {
                auto    pending = session.pending_send_data();
                ssize_t sent = co_await socket.send_all(reinterpret_cast<const char*>(pending.data()), pending.size());
                if (sent < 0)
                    co_return -errno;
                session.consume_send_data(static_cast<size_t>(sent));
            }
            co_return 0;
        };

        if (int rc = co_await flush_pending(); rc != 0)
            co_return rc;

        Http2ClientSession::Request h2_req;
        h2_req.method     = request.method;
        h2_req.scheme     = parsed_url->scheme;
        h2_req.path       = parsed_url->path.empty() ? std::string("/") : parsed_url->path;
        h2_req.authority  = parsed_url->host;
        bool default_port = (parsed_url->scheme == "https" && parsed_url->port == 443)
            || (parsed_url->scheme == "http" && parsed_url->port == 80);
        if (!default_port) {
            h2_req.authority.push_back(':');
            h2_req.authority.append(std::to_string(parsed_url->port));
        }

        for (const auto& header : built.headers) {
            std::string lower = to_lower(header.name);
            if (lower == "host" || lower == "connection" || lower == "proxy-connection" || lower == "upgrade"
                || lower == "keep-alive" || lower == "transfer-encoding")
                continue;
            h2_req.headers.emplace_back(std::move(lower), header.value);
        }

        if (!request.body.empty())
            h2_req.body = &request.body;

        if (int32_t stream_id = session.start_request(h2_req); stream_id < 0)
            co_return -EIO;

        if (int rc = co_await flush_pending(); rc != 0)
            co_return rc;

        std::array<char, 8192> recv_buffer {};
        while (!session.message_complete()) {
            ssize_t n = co_await socket.recv(recv_buffer.data(), recv_buffer.size());
            if (n < 0)
                co_return -errno;
            if (n == 0)
                break;

            std::string error_reason;
            if (!session.feed(std::string_view(recv_buffer.data(), static_cast<size_t>(n)), &error_reason)) {
                stream_failed = true;
                break;
            }

            if (int rc = co_await flush_pending(); rc != 0) {
                stream_failed = true;
                break;
            }
        }

        if (!session.message_complete())
            stream_failed = true;

        if (int rc = co_await flush_pending(); rc != 0)
            stream_failed = true;

        if (request.buffer_body && request.output)
            std::fflush(request.output);

        if (stream_failed)
            co_return -EIO;

        response.status_code      = session.response().status_code;
        response.reason           = session.response().reason;
        response.headers_complete = session.headers_complete();
        response.message_complete = session.message_complete();
        response.used_http2       = true;
        co_return 0;
#endif
    };

    auto resolve_family = [](auto& sock) {
        if constexpr (requires { sock.family(); })
            return sock.family();
        else
            return sock.underlying().family();
    };

    auto resolve_dual_stack = [](auto& sock) {
        if constexpr (requires { sock.dual_stack(); })
            return sock.dual_stack();
        else
            return sock.underlying().dual_stack();
    };

    auto connect_socket = [&](auto& sock, const sockaddr* addr, socklen_t len) -> HttpEasyClient::TaskInt {
        if constexpr (requires { sock.connect(addr, len); }) {
            int rc = co_await sock.connect(addr, len);
            co_return rc;
        } else {
            int rc = co_await sock.underlying().connect(addr, len);
            co_return rc;
        }
    };

    auto connect_and_request = [&](auto create_socket, bool allow_http2) -> HttpEasyClient::TaskInt {
        auto socket = create_socket();

        net::dns::resolve_options dns_opts;
        dns_opts.family           = resolve_family(socket);
        dns_opts.allow_dual_stack = resolve_dual_stack(socket);

        auto resolved = net::dns::resolve_sync(parsed_url->host, parsed_url->port, dns_opts);
        if (!resolved.success)
            co_return resolved.error_code == 0 ? -EHOSTUNREACH : -resolved.error_code;

        auto endpoints = gather_endpoints(resolved);
        if (endpoints.empty())
            co_return -EHOSTUNREACH;

        bool connected = false;

        for (size_t attempt = 0; attempt < endpoints.size(); ++attempt) {
            if (attempt > 0)
                socket = create_socket();

            auto& ep = endpoints[attempt];
            int   rc = co_await connect_socket(socket, reinterpret_cast<const sockaddr*>(&ep.addr), ep.len);
            if (rc != 0)
                continue;
            connected = true;
            break;
        }

        if (!connected)
            co_return -ECONNREFUSED;

        if constexpr (requires { socket.handshake(); }) {
            int handshake_rc = co_await socket.handshake();
            if (handshake_rc != 0) {
#if defined(USING_SSL)
                if (request.verbose) {
                    long verify_res = SSL_get_verify_result(socket.ssl_handle());
                    if (verify_res != X509_V_OK) {
                        verbose_printf(request.verbose,
                                       "* TLS verify result: %ld (%s)\n",
                                       verify_res,
                                       X509_verify_cert_error_string(verify_res));
                    }
                    auto error_stack = co_wq::net::tls_utils::collect_error_stack();
                    if (!error_stack.empty())
                        verbose_printf(request.verbose, "* TLS error stack:\n%s", error_stack.c_str());
                }
#endif
                co_return handshake_rc;
            }

            bool negotiated_http2 = false;
            if (allow_http2) {
                const unsigned char* alpn     = nullptr;
                unsigned int         alpn_len = 0;
                SSL_get0_alpn_selected(socket.ssl_handle(), &alpn, &alpn_len);
                if (alpn && alpn_len == 2 && std::memcmp(alpn, "h2", 2) == 0)
                    negotiated_http2 = true;
            }

            if (negotiated_http2) {
                response.used_http2 = true;
                co_return co_await perform_http2_request(socket);
            }

            response.used_http2 = false;
            co_return co_await perform_http1_request(socket);
        } else {
            response.used_http2 = false;
            co_return co_await perform_http1_request(socket);
        }
    };

    if (parsed_url->scheme == "https") {
#if defined(USING_SSL)
        auto tls_ctx = net::tls_context::make(net::tls_mode::Client);
        if (SSL_CTX* ctx_handle = tls_ctx.native_handle()) {
            if (request.insecure) {
                SSL_CTX_set_verify(ctx_handle, SSL_VERIFY_NONE, nullptr);
                verbose_printf(request.verbose, "* TLS verification disabled by --insecure\n");
            } else {
                bool default_paths    = (SSL_CTX_set_default_verify_paths(ctx_handle) == 1);
                bool roots_loaded     = co_wq::net::tls_utils::load_system_root_certificates(ctx_handle);
                bool trust_configured = default_paths || roots_loaded;
                if (trust_configured)
                    SSL_CTX_set_verify(ctx_handle, SSL_VERIFY_PEER, nullptr);
                else
                    SSL_CTX_set_verify(ctx_handle, SSL_VERIFY_NONE, nullptr);

                if (request.verbose) {
                    if (trust_configured) {
                        const char* source = nullptr;
                        if (default_paths && roots_loaded)
                            source = "default-paths+windows-root";
                        else if (roots_loaded)
                            source = "windows-root";
                        else
                            source = "default-paths";
                        verbose_printf(request.verbose, "* TLS trust store configured (%s)\n", source);
                    } else {
                        verbose_printf(request.verbose,
                                       "* TLS trust store unavailable; continuing without verification\n");
                    }
                }
            }
        }

        auto create_tls_socket = [&](bool advertise_http2) {
            auto sock = fdwq_.make_tls_socket(tls_ctx, net::tls_mode::Client);
            if (SSL* ssl = sock.ssl_handle()) {
                SSL_set_tlsext_host_name(ssl, parsed_url->host.c_str());
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
                if (!request.insecure)
                    SSL_set1_host(ssl, parsed_url->host.c_str());
#endif
                if (advertise_http2) {
                    static const unsigned char alpn_protos[] = {
                        2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'
                    };
                    SSL_set_alpn_protos(ssl, alpn_protos, sizeof(alpn_protos));
                } else {
                    static const unsigned char alpn_http1[] = { 8, 'h', 't', 't', 'p', '/', '1', '.', '1' };
                    SSL_set_alpn_protos(ssl, alpn_http1, sizeof(alpn_http1));
                }
            }
            return sock;
        };

        auto http1_factory = [&]() { return create_tls_socket(false); };
        if (request.prefer_http2) {
            auto http2_factory = [&]() { return create_tls_socket(true); };
            int  rc            = co_await connect_and_request(http2_factory, true);
            if (rc == 0)
                co_return 0;

            verbose_printf(request.verbose, "* HTTP/2 attempt failed (%d); falling back to HTTP/1.1\n", rc);

            response.reset();
            rc = co_await connect_and_request(http1_factory, false);
            co_return rc;
        }

        int rc = co_await connect_and_request(http1_factory, false);
        co_return rc;
#else
        co_return -ENOTSUP;
#endif
    }

    auto create_tcp_socket = [&]() { return fdwq_.make_tcp_socket(AF_INET6, true); };
    int  rc                = co_await connect_and_request(create_tcp_socket, false);
    co_return rc;
}

} // namespace co_wq::net::http
