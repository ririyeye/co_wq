#include "syswork.hpp"
#include "test_sys_stats_logger.hpp"

#include "tcp_listener.hpp"
#include "tcp_socket.hpp"
#if defined(USING_SSL)
#include "tls.hpp"
#endif
#include "fd_base.hpp"
#include "worker.hpp"

#include <llhttp.h>
#include <nghttp2/nghttp2.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#if defined(_WIN32)
#include <basetsd.h>
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

using namespace co_wq;

using NetFdWorkqueue = net::fd_workqueue<SpinLock, net::epoll_reactor>;

namespace {

struct HttpRequestContext {
    std::string                                  method;
    std::string                                  url;
    std::unordered_map<std::string, std::string> headers;
    std::string                                  current_field;
    std::string                                  current_value;
    std::string                                  body;
    bool                                         headers_complete { false };
    bool                                         message_complete { false };

    void reset()
    {
        method.clear();
        url.clear();
        headers.clear();
        current_field.clear();
        current_value.clear();
        body.clear();
        headers_complete = false;
        message_complete = false;
    }
};

std::string to_lower(std::string_view input)
{
    std::string out;
    out.reserve(input.size());
    for (char ch : input) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

#if defined(USING_SSL)
int select_http_alpn(SSL*,
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

int on_message_begin(llhttp_t* parser)
{
    auto* ctx = static_cast<HttpRequestContext*>(parser->data);
    if (ctx)
        ctx->reset();
    return 0;
}

int on_method(llhttp_t* parser, const char* at, size_t length)
{
    auto* ctx = static_cast<HttpRequestContext*>(parser->data);
    ctx->method.append(at, length);
    return 0;
}

int on_url(llhttp_t* parser, const char* at, size_t length)
{
    auto* ctx = static_cast<HttpRequestContext*>(parser->data);
    ctx->url.append(at, length);
    return 0;
}

int on_header_field(llhttp_t* parser, const char* at, size_t length)
{
    auto* ctx = static_cast<HttpRequestContext*>(parser->data);
    ctx->current_field.append(at, length);
    return 0;
}

int on_header_value(llhttp_t* parser, const char* at, size_t length)
{
    auto* ctx = static_cast<HttpRequestContext*>(parser->data);
    ctx->current_value.append(at, length);
    return 0;
}

int on_header_value_complete(llhttp_t* parser)
{
    auto* ctx = static_cast<HttpRequestContext*>(parser->data);
    if (!ctx->current_field.empty()) {
        ctx->headers[to_lower(ctx->current_field)] = ctx->current_value;
    }
    ctx->current_field.clear();
    ctx->current_value.clear();
    return 0;
}

int on_headers_complete(llhttp_t* parser)
{
    auto* ctx             = static_cast<HttpRequestContext*>(parser->data);
    ctx->headers_complete = true;
    return 0;
}

int on_body(llhttp_t* parser, const char* at, size_t length)
{
    auto* ctx = static_cast<HttpRequestContext*>(parser->data);
    ctx->body.append(at, length);
    return 0;
}

int on_message_complete(llhttp_t* parser)
{
    auto* ctx             = static_cast<HttpRequestContext*>(parser->data);
    ctx->message_complete = true;
    return 0;
}

struct AppResponse {
    int         status_code { 0 };
    std::string reason;
    std::string body;
    std::string content_type { "text/plain; charset=utf-8" };
};

AppResponse make_app_response(const std::string&                                  method,
                              const std::string&                                  url,
                              const std::unordered_map<std::string, std::string>& headers,
                              const std::string&                                  body)
{
    AppResponse result;

    if (method == "GET" && (url == "/" || url == "/index" || url == "/index.html")) {
        result.status_code = 200;
        result.reason      = "OK";
        result.body        = "Hello from co_wq HTTP server!\n";
        result.body += "Method: " + method + "\n";
        result.body += "Path: " + url + "\n";
    } else if (method == "GET" && url == "/health") {
        result.status_code = 200;
        result.reason      = "OK";
        result.body        = "OK\n";
    } else if (method == "POST" && url == "/echo-json") {
        try {
            nlohmann::json request_json;
            if (!body.empty()) {
                request_json = nlohmann::json::parse(body);
            } else {
                request_json = nlohmann::json::object();
            }

            nlohmann::json response_json {
                { "status",  "ok"         },
                { "method",  method       },
                { "path",    url          },
                { "request", request_json }
            };

            if (auto it = headers.find("content-type"); it != headers.end())
                response_json["request_content_type"] = it->second;

            result.status_code  = 200;
            result.reason       = "OK";
            result.content_type = "application/json; charset=utf-8";
            result.body         = response_json.dump();
        } catch (const nlohmann::json::exception& ex) {
            nlohmann::json error_json {
                { "status", "error"   },
                { "reason", ex.what() }
            };
            result.status_code  = 400;
            result.reason       = "Bad Request";
            result.content_type = "application/json; charset=utf-8";
            result.body         = error_json.dump();
        }
    } else {
        result.status_code = 404;
        result.reason      = "Not Found";
        result.body        = "Not Found\n";
    }

    return result;
}

std::string build_http_response(int              status_code,
                                std::string_view reason,
                                std::string_view body,
                                std::string_view content_type = "text/plain; charset=utf-8")
{
    std::string response;
    response.reserve(128 + body.size());
    response.append("HTTP/1.1 ");
    response.append(std::to_string(status_code));
    response.push_back(' ');
    response.append(reason);
    response.append("\r\n");
    response.append("Content-Type: ");
    response.append(content_type);
    response.append("\r\n");
    response.append("Content-Length: ");
    response.append(std::to_string(body.size()));
    response.append("\r\nConnection: close\r\n\r\n");
    response.append(body);
    return response;
}

std::atomic_bool               g_stop { false };
std::atomic<net::os::fd_t>     g_listener_fd { net::os::invalid_fd() };
std::atomic<std::atomic_bool*> g_finished_ptr { nullptr };

#if defined(_WIN32)
static BOOL WINAPI console_ctrl_handler(DWORD type)
{
    if (type == CTRL_C_EVENT) {
        g_stop.store(true, std::memory_order_release);
        auto fd = g_listener_fd.exchange(net::os::invalid_fd(), std::memory_order_acq_rel);
        if (fd != net::os::invalid_fd())
            net::os::close_fd(fd);
        if (auto* flag = g_finished_ptr.load(std::memory_order_acquire))
            flag->store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}
#else
void sigint_handler(int)
{
    g_stop.store(true, std::memory_order_release);
    auto fd = g_listener_fd.exchange(net::os::invalid_fd(), std::memory_order_acq_rel);
    if (fd != net::os::invalid_fd())
        net::os::close_fd(fd);
    if (auto* flag = g_finished_ptr.load(std::memory_order_acquire))
        flag->store(true, std::memory_order_release);
}
#endif

template <typename Socket>
Task<void, Work_Promise<SpinLock, void>> handle_http1_connection(Socket sock, std::string initial_data)
{
    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_message_begin         = on_message_begin;
    settings.on_method                = on_method;
    settings.on_url                   = on_url;
    settings.on_header_field          = on_header_field;
    settings.on_header_value          = on_header_value;
    settings.on_header_value_complete = on_header_value_complete;
    settings.on_headers_complete      = on_headers_complete;
    settings.on_body                  = on_body;
    settings.on_message_complete      = on_message_complete;

    llhttp_t parser;
    llhttp_init(&parser, HTTP_REQUEST, &settings);

    HttpRequestContext ctx;
    parser.data = &ctx;

    std::array<char, 4096> buffer {};
    bool                   parse_error  = false;
    std::string            error_reason = "";
    bool                   received_any_data { false };
    bool                   client_disconnected_early { false };

    if (!initial_data.empty()) {
        received_any_data  = true;
        llhttp_errno_t err = llhttp_execute(&parser, initial_data.data(), initial_data.size());
        if (err != HPE_OK) {
            parse_error        = true;
            const char* reason = llhttp_get_error_reason(&parser);
            error_reason       = (reason && *reason) ? reason : llhttp_errno_name(err);
        }
    }

    while (!ctx.message_complete && !parse_error) {
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
                llhttp_errno_t finish_err = llhttp_finish(&parser);
                if (finish_err != HPE_OK) {
                    parse_error  = true;
                    error_reason = llhttp_errno_name(finish_err);
                }
            }
            break;
        }
        received_any_data  = true;
        llhttp_errno_t err = llhttp_execute(&parser, buffer.data(), static_cast<size_t>(n));
        if (err != HPE_OK) {
            parse_error        = true;
            const char* reason = llhttp_get_error_reason(&parser);
            if (reason && *reason)
                error_reason = reason;
            else
                error_reason = llhttp_errno_name(err);
            break;
        }
    }

    if (client_disconnected_early) {
        sock.close();
        co_return;
    }

    if (!ctx.message_complete && !parse_error) {
        parse_error  = true;
        error_reason = "incomplete request";
    }

    std::string response_body;
    std::string response;

    if (parse_error) {
        response_body = "Bad Request\n";
        response      = build_http_response(400, "Bad Request", response_body);
        CO_WQ_LOG_ERROR("[http] parse error: %s", error_reason.c_str());
    } else {
        AppResponse app = make_app_response(ctx.method, ctx.url, ctx.headers, ctx.body);
        response_body   = app.body;
        response        = build_http_response(app.status_code, app.reason, response_body, app.content_type);
    }

    (void)co_await sock.send_all(response.data(), response.size());
    sock.close();
    co_return;
}

struct Http2ServerResponseData {
    std::string body;
    size_t      offset { 0 };
};

struct Http2ServerStream {
    std::string                                  method;
    std::string                                  path;
    std::unordered_map<std::string, std::string> headers;
    std::string                                  body;
    bool                                         responded { false };
};

struct Http2ServerSession {
    std::vector<uint8_t>                                 send_buffer;
    std::unordered_map<int32_t, Http2ServerStream>       streams;
    std::unordered_map<int32_t, Http2ServerResponseData> responses;
};

[[maybe_unused]] static ssize_t
h2_server_send_callback(nghttp2_session*, const uint8_t* data, size_t length, int, void* user_data)
{
    auto* state = static_cast<Http2ServerSession*>(user_data);
    state->send_buffer.insert(state->send_buffer.end(), data, data + length);
    return static_cast<ssize_t>(length);
}

[[maybe_unused]] static int h2_server_on_begin_headers(nghttp2_session*, const nghttp2_frame* frame, void* user_data)
{
    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST)
        return 0;
    auto* state = static_cast<Http2ServerSession*>(user_data);
    state->streams.emplace(frame->hd.stream_id, Http2ServerStream {});
    return 0;
}

[[maybe_unused]] static int h2_server_on_header(nghttp2_session*,
                                                const nghttp2_frame* frame,
                                                const uint8_t*       name,
                                                size_t               namelen,
                                                const uint8_t*       value,
                                                size_t               valuelen,
                                                uint8_t,
                                                void* user_data)
{
    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST)
        return 0;
    auto* state = static_cast<Http2ServerSession*>(user_data);
    auto  it    = state->streams.find(frame->hd.stream_id);
    if (it == state->streams.end())
        return 0;
    auto&       stream = it->second;
    std::string key(reinterpret_cast<const char*>(name), namelen);
    std::string val(reinterpret_cast<const char*>(value), valuelen);
    if (key == ":method") {
        stream.method = std::move(val);
    } else if (key == ":path") {
        stream.path = std::move(val);
    } else if (key == ":scheme" || key == ":authority") {
        // ignore
    } else {
        stream.headers[to_lower(key)] = std::move(val);
    }
    return 0;
}

[[maybe_unused]] static int
h2_server_on_data_chunk(nghttp2_session*, uint8_t, int32_t stream_id, const uint8_t* data, size_t len, void* user_data)
{
    auto* state = static_cast<Http2ServerSession*>(user_data);
    auto  it    = state->streams.find(stream_id);
    if (it == state->streams.end())
        return 0;
    it->second.body.append(reinterpret_cast<const char*>(data), len);
    return 0;
}

[[maybe_unused]] static ssize_t h2_server_data_read(nghttp2_session*,
                                                    int32_t,
                                                    uint8_t*             buf,
                                                    size_t               length,
                                                    uint32_t*            data_flags,
                                                    nghttp2_data_source* source,
                                                    void*)
{
    auto* payload = static_cast<Http2ServerResponseData*>(source->ptr);
    if (!payload)
        return 0;
    size_t remaining = payload->body.size() > payload->offset ? payload->body.size() - payload->offset : 0;
    size_t to_copy   = std::min(length, remaining);
    if (to_copy > 0) {
        std::memcpy(buf, payload->body.data() + payload->offset, to_copy);
        payload->offset += to_copy;
    }
    if (payload->offset >= payload->body.size())
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    return static_cast<ssize_t>(to_copy);
}

static int h2_server_submit_response(nghttp2_session* session, Http2ServerSession& state, int32_t stream_id)
{
    auto it = state.streams.find(stream_id);
    if (it == state.streams.end())
        return 0;
    auto& stream = it->second;
    if (stream.responded)
        return 0;

    AppResponse app            = make_app_response(stream.method, stream.path, stream.headers, stream.body);
    std::string status         = std::to_string(app.status_code);
    std::string content_length = std::to_string(app.body.size());

    std::vector<std::pair<std::string, std::string>> header_pairs;
    header_pairs.emplace_back(":status", std::move(status));
    header_pairs.emplace_back("content-type", app.content_type);
    header_pairs.emplace_back("content-length", content_length);

    std::vector<nghttp2_nv> nva;
    nva.reserve(header_pairs.size());
    for (auto& kv : header_pairs) {
        nva.push_back(nghttp2_nv { reinterpret_cast<uint8_t*>(kv.first.data()),
                                   reinterpret_cast<uint8_t*>(kv.second.data()),
                                   kv.first.size(),
                                   kv.second.size(),
                                   NGHTTP2_NV_FLAG_NONE });
    }

    nghttp2_data_provider  data_provider {};
    nghttp2_data_provider* provider_ptr = nullptr;
    if (!app.body.empty()) {
        auto& payload               = state.responses[stream_id];
        payload.body                = std::move(app.body);
        payload.offset              = 0;
        data_provider.source.ptr    = &payload;
        data_provider.read_callback = h2_server_data_read;
        provider_ptr                = &data_provider;
    }

    int rv = nghttp2_submit_response(session, stream_id, nva.data(), nva.size(), provider_ptr);
    if (rv != 0)
        return rv;
    stream.responded = true;
    return 0;
}

[[maybe_unused]] static int
h2_server_on_frame_recv(nghttp2_session* session, const nghttp2_frame* frame, void* user_data)
{
    auto*   state     = static_cast<Http2ServerSession*>(user_data);
    int32_t stream_id = frame->hd.stream_id;
    if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
            int rv = h2_server_submit_response(session, *state, stream_id);
            if (rv != 0)
                return rv;
        }
    } else if (frame->hd.type == NGHTTP2_DATA) {
        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
            int rv = h2_server_submit_response(session, *state, stream_id);
            if (rv != 0)
                return rv;
        }
    }
    return 0;
}

[[maybe_unused]] static int h2_server_on_stream_close(nghttp2_session*, int32_t stream_id, uint32_t, void* user_data)
{
    auto* state = static_cast<Http2ServerSession*>(user_data);
    state->streams.erase(stream_id);
    state->responses.erase(stream_id);
    return 0;
}

template <typename Socket>
Task<void, Work_Promise<SpinLock, void>> handle_http2_connection(Socket sock, std::string initial_data)
{
    Http2ServerSession session_state;

    nghttp2_session_callbacks* callbacks = nullptr;
    if (nghttp2_session_callbacks_new(&callbacks) != 0) {
        sock.close();
        co_return;
    }
    nghttp2_session_callbacks_set_send_callback(callbacks, h2_server_send_callback);
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, h2_server_on_begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, h2_server_on_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, h2_server_on_data_chunk);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, h2_server_on_frame_recv);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, h2_server_on_stream_close);

    nghttp2_session* session = nullptr;
    int              rv      = nghttp2_session_server_new(&session, callbacks, &session_state);
    nghttp2_session_callbacks_del(callbacks);
    if (rv != 0) {
        sock.close();
        co_return;
    }

    nghttp2_settings_entry settings[] = {
        { NGHTTP2_SETTINGS_ENABLE_PUSH, 0 }
    };
    rv = nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, settings, std::size(settings));
    if (rv != 0) {
        nghttp2_session_del(session);
        sock.close();
        co_return;
    }

    rv = nghttp2_session_send(session);
    if (rv == 0 && !session_state.send_buffer.empty()) {
        ssize_t sent = co_await sock.send_all(session_state.send_buffer.data(), session_state.send_buffer.size());
        if (sent <= 0) {
            nghttp2_session_del(session);
            sock.close();
            co_return;
        }
        session_state.send_buffer.clear();
    }

    if (!initial_data.empty()) {
        ssize_t consumed = nghttp2_session_mem_recv(session,
                                                    reinterpret_cast<const uint8_t*>(initial_data.data()),
                                                    initial_data.size());
        if (consumed < 0) {
            CO_WQ_LOG_ERROR("[http] http/2 preface error: %s", nghttp2_strerror(static_cast<int>(consumed)));
            nghttp2_session_del(session);
            sock.close();
            co_return;
        }
        rv = nghttp2_session_send(session);
        if (rv != 0) {
            nghttp2_session_del(session);
            sock.close();
            co_return;
        }
        if (!session_state.send_buffer.empty()) {
            ssize_t sent = co_await sock.send_all(session_state.send_buffer.data(), session_state.send_buffer.size());
            if (sent <= 0) {
                nghttp2_session_del(session);
                sock.close();
                co_return;
            }
            session_state.send_buffer.clear();
        }
    }

    std::array<char, 8192> buffer {};
    while (nghttp2_session_want_read(session) || nghttp2_session_want_write(session)) {
        ssize_t n = co_await sock.recv(buffer.data(), buffer.size());
        if (n <= 0)
            break;
        ssize_t consumed = nghttp2_session_mem_recv(session,
                                                    reinterpret_cast<uint8_t*>(buffer.data()),
                                                    static_cast<size_t>(n));
        if (consumed < 0) {
            CO_WQ_LOG_ERROR("[http] http/2 decode error: %s", nghttp2_strerror(static_cast<int>(consumed)));
            break;
        }
        rv = nghttp2_session_send(session);
        if (rv != 0) {
            CO_WQ_LOG_ERROR("[http] http/2 send error: %s", nghttp2_strerror(rv));
            break;
        }
        if (!session_state.send_buffer.empty()) {
            ssize_t sent = co_await sock.send_all(session_state.send_buffer.data(), session_state.send_buffer.size());
            if (sent <= 0)
                break;
            session_state.send_buffer.clear();
        }
    }

    sock.close();
    nghttp2_session_del(session);
    co_return;
}

template <typename Socket>
Task<void, Work_Promise<SpinLock, void>> handle_http_connection(Socket sock, bool enable_http2)
{
    static constexpr std::string_view h2_preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

    bool        use_http2 = false;
    std::string initial_data;

    if constexpr (requires(Socket& s) { s.handshake(); }) {
        int hs = co_await sock.handshake();
        if (hs != 0) {
            CO_WQ_LOG_ERROR("[http] tls handshake failed, code=%d", hs);
            sock.close();
            co_return;
        }
        if (enable_http2) {
#if defined(USING_SSL)
            const unsigned char* alpn     = nullptr;
            unsigned int         alpn_len = 0;
            SSL_get0_alpn_selected(sock.ssl_handle(), &alpn, &alpn_len);
            if (alpn_len == 2 && alpn && std::memcmp(alpn, "h2", 2) == 0)
                use_http2 = true;
#endif
        }
    }

    if (enable_http2 && !use_http2) {
        std::array<char, 4096> probe {};
        size_t                 read_total = 0;
        bool                   mismatch   = false;
        while (read_total < h2_preface.size()) {
            ssize_t n = co_await sock.recv(probe.data() + read_total, probe.size() - read_total);
            if (n <= 0) {
                sock.close();
                co_return;
            }
            read_total += static_cast<size_t>(n);
            initial_data.assign(probe.data(), read_total);
            size_t compare_len = std::min(initial_data.size(), h2_preface.size());
            if (initial_data.compare(0, compare_len, h2_preface.substr(0, compare_len)) != 0) {
                mismatch = true;
                break;
            }
            if (read_total >= probe.size() || read_total >= h2_preface.size())
                break;
        }
        if (!mismatch && initial_data.size() >= h2_preface.size())
            use_http2 = true;
    }

    if (use_http2) {
        co_await handle_http2_connection(std::move(sock), std::move(initial_data));
        co_return;
    }

    co_await handle_http1_connection(std::move(sock), std::move(initial_data));
}

Task<void, Work_Promise<SpinLock, void>> http_server(NetFdWorkqueue& fdwq,
                                                     std::string     host,
                                                     uint16_t        port
#if defined(USING_SSL)
                                                     ,
                                                     const net::tls_context* tls_ctx
#endif
                                                     ,
                                                     bool enable_http2)
{
    net::tcp_listener<SpinLock> listener(fdwq.base(), fdwq.reactor());
    listener.bind_listen(host, port, 128);
    g_listener_fd.store(listener.native_handle(), std::memory_order_release);

    CO_WQ_LOG_INFO("[http] listening on %s:%u", host.c_str(), static_cast<unsigned>(port));

    while (!g_stop.load(std::memory_order_acquire)) {
        int fd = co_await listener.accept();
        if (fd == net::k_accept_fatal) {
            CO_WQ_LOG_ERROR("[http] accept fatal error, shutting down");
            break;
        }
        if (fd < 0)
            continue;
#if defined(USING_SSL)
        if (tls_ctx) {
            try {
                auto tls_sock = fdwq.adopt_tls_socket(fd, *tls_ctx, net::tls_mode::Server);
                auto task     = handle_http_connection(std::move(tls_sock), enable_http2);
                post_to(task, fdwq.base());
            } catch (const std::exception& ex) {
                CO_WQ_LOG_ERROR("[http] tls socket setup failed: %s", ex.what());
            }
        } else
#endif
        {
            auto socket = fdwq.adopt_tcp_socket(fd);
            auto task   = handle_http_connection(std::move(socket), enable_http2);
            post_to(task, fdwq.base());
        }
    }

    listener.close();
    g_listener_fd.store(net::os::invalid_fd(), std::memory_order_release);
    co_return;
}

} // namespace

int main(int argc, char* argv[])
{
    std::string host = "0.0.0.0";
    uint16_t    port = 8080;
    std::string cert_path;
    std::string key_path;
    bool        use_tls      = false;
    bool        enable_http2 = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--cert" && i + 1 < argc) {
            cert_path = argv[++i];
            use_tls   = true;
        } else if (arg == "--key" && i + 1 < argc) {
            key_path = argv[++i];
            use_tls  = true;
        } else if (arg == "--http2") {
            enable_http2 = true;
        }
    }

#if defined(_WIN32)
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    std::signal(SIGINT, sigint_handler);
#endif

    co_wq::test::SysStatsLogger stats_logger("http_server");
    auto&                       wq = get_sys_workqueue(0);
    NetFdWorkqueue              fdwq(wq);

    const net::tls_context* tls_ctx_ptr = nullptr;
#if defined(USING_SSL)
    std::optional<net::tls_context> tls_ctx;
    if (use_tls) {
        if (cert_path.empty() || key_path.empty()) {
            CO_WQ_LOG_ERROR("[http] missing --cert/--key when TLS enabled");
            return 1;
        }
        try {
            tls_ctx.emplace(net::tls_context::make_server_with_pem(cert_path, key_path));
            if (enable_http2)
                SSL_CTX_set_alpn_select_cb(tls_ctx->native_handle(), &select_http_alpn, nullptr);
            tls_ctx_ptr = &(*tls_ctx);
            CO_WQ_LOG_INFO("[http] TLS enabled, cert=%s", cert_path.c_str());
        } catch (const std::exception& ex) {
            CO_WQ_LOG_ERROR("[http] failed to setup TLS: %s", ex.what());
            return 1;
        }
    }
#endif

    auto server_task = http_server(fdwq,
                                   host,
                                   port
#if defined(USING_SSL)
                                   ,
                                   tls_ctx_ptr
#endif
                                   ,
                                   enable_http2);
    auto             coroutine = server_task.get();
    auto&            promise   = coroutine.promise();
    std::atomic_bool finished { false };
    promise.mUserData    = &finished;
    promise.mOnCompleted = [](Promise_base& pb) {
        auto* flag = static_cast<std::atomic_bool*>(pb.mUserData);
        if (flag)
            flag->store(true, std::memory_order_release);
    };
    g_finished_ptr.store(&finished, std::memory_order_release);

    post_to(server_task, wq);

    sys_wait_until(finished);
    g_finished_ptr.store(nullptr, std::memory_order_release);
    return 0;
}
