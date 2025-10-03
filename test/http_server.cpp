#include "syswork.hpp"

#include "tcp_listener.hpp"
#include "tcp_socket.hpp"
#if defined(USING_SSL)
#include "tls.hpp"
#endif
#include "fd_base.hpp"
#include "worker.hpp"

#include <llhttp.h>
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
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
std::atomic<int>               g_listener_fd { -1 };
std::atomic<std::atomic_bool*> g_finished_ptr { nullptr };

#if defined(_WIN32)
static BOOL WINAPI console_ctrl_handler(DWORD type)
{
    if (type == CTRL_C_EVENT) {
        g_stop.store(true, std::memory_order_release);
        int fd = g_listener_fd.exchange(-1, std::memory_order_acq_rel);
        if (fd != -1)
            ::closesocket((SOCKET)fd);
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
    int fd = g_listener_fd.exchange(-1, std::memory_order_acq_rel);
    if (fd != -1)
        ::close(fd);
    if (auto* flag = g_finished_ptr.load(std::memory_order_acquire))
        flag->store(true, std::memory_order_release);
}
#endif

template <typename Socket> Task<void, Work_Promise<SpinLock, void>> handle_http_connection(Socket sock)
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

    if constexpr (requires(Socket& s) { s.handshake(); }) {
        int hs = co_await sock.handshake();
        if (hs != 0) {
            std::cerr << "[http] tls handshake failed, code=" << hs << "\n";
            sock.close();
            co_return;
        }
    }

    while (!ctx.message_complete) {
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
        std::cerr << "[http] parse error: " << error_reason << "\n";
    } else {
        if (ctx.method == "GET" && (ctx.url == "/" || ctx.url == "/index" || ctx.url == "/index.html")) {
            response_body = "Hello from co_wq HTTP server!\n";
            response_body += "Method: " + ctx.method + "\n";
            response_body += "Path: " + ctx.url + "\n";
            response = build_http_response(200, "OK", response_body);
        } else if (ctx.method == "GET" && ctx.url == "/health") {
            response_body = "OK\n";
            response      = build_http_response(200, "OK", response_body);
        } else if (ctx.method == "POST" && ctx.url == "/echo-json") {
            try {
                nlohmann::json request_json;
                if (!ctx.body.empty()) {
                    request_json = nlohmann::json::parse(ctx.body);
                } else {
                    request_json = nlohmann::json::object();
                }

                nlohmann::json response_json {
                    { "status",  "ok"         },
                    { "method",  ctx.method   },
                    { "path",    ctx.url      },
                    { "request", request_json }
                };

                if (auto it = ctx.headers.find("content-type"); it != ctx.headers.end())
                    response_json["request_content_type"] = it->second;

                response_body = response_json.dump();
                response      = build_http_response(200, "OK", response_body, "application/json; charset=utf-8");
            } catch (const nlohmann::json::exception& ex) {
                nlohmann::json error_json {
                    { "status", "error"   },
                    { "reason", ex.what() }
                };
                response_body = error_json.dump();
                response = build_http_response(400, "Bad Request", response_body, "application/json; charset=utf-8");
            }
        } else {
            response_body = "Not Found\n";
            response      = build_http_response(404, "Not Found", response_body);
        }
    }

    (void)co_await sock.send_all(response.data(), response.size());
    sock.close();
    co_return;
}

Task<void, Work_Promise<SpinLock, void>> http_server(net::fd_workqueue<SpinLock>& fdwq,
                                                     std::string                  host,
                                                     uint16_t                     port
#if defined(USING_SSL)
                                                     ,
                                                     const net::tls_context* tls_ctx
#endif
)
{
    net::tcp_listener<SpinLock> listener(fdwq.base(), fdwq.reactor());
    listener.bind_listen(host, port, 128);
    g_listener_fd.store(listener.native_handle(), std::memory_order_release);

    std::cout << "[http] listening on " << host << ':' << port << "\n";

    while (!g_stop.load(std::memory_order_acquire)) {
        int fd = co_await listener.accept();
        if (fd == net::k_accept_fatal) {
            std::cerr << "[http] accept fatal error, shutting down\n";
            break;
        }
        if (fd < 0)
            continue;
#if defined(USING_SSL)
        if (tls_ctx) {
            try {
                auto tls_sock = fdwq.adopt_tls_socket(fd, *tls_ctx, net::tls_mode::Server);
                auto task     = handle_http_connection(std::move(tls_sock));
                post_to(task, fdwq.base());
            } catch (const std::exception& ex) {
                std::cerr << "[http] tls socket setup failed: " << ex.what() << "\n";
            }
        } else
#endif
        {
            auto socket = fdwq.adopt_tcp_socket(fd);
            auto task   = handle_http_connection(std::move(socket));
            post_to(task, fdwq.base());
        }
    }

    listener.close();
    g_listener_fd.store(-1, std::memory_order_release);
    co_return;
}

} // namespace

int main(int argc, char* argv[])
{
    std::string host = "0.0.0.0";
    uint16_t    port = 8080;
    std::string cert_path;
    std::string key_path;
    bool        use_tls = false;

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
        }
    }

#if defined(_WIN32)
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    std::signal(SIGINT, sigint_handler);
#endif

    auto&                       wq = get_sys_workqueue(0);
    net::fd_workqueue<SpinLock> fdwq(wq);

    const net::tls_context* tls_ctx_ptr = nullptr;
#if defined(USING_SSL)
    std::optional<net::tls_context> tls_ctx;
    if (use_tls) {
        if (cert_path.empty() || key_path.empty()) {
            std::cerr << "[http] missing --cert/--key when TLS enabled\n";
            return 1;
        }
        try {
            tls_ctx.emplace(net::tls_context::make_server_with_pem(cert_path, key_path));
            tls_ctx_ptr = &(*tls_ctx);
            std::cout << "[http] TLS enabled, cert=" << cert_path << "\n";
        } catch (const std::exception& ex) {
            std::cerr << "[http] failed to setup TLS: " << ex.what() << "\n";
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
    );
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
