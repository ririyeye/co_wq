#include "syswork.hpp"

#include "tcp_listener.hpp"
#include "tcp_socket.hpp"
#include "fd_base.hpp"
#include "worker.hpp"

#include <llhttp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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
    std::string                                   method;
    std::string                                   url;
    std::unordered_map<std::string, std::string>  headers;
    std::string                                   current_field;
    std::string                                   current_value;
    std::string                                   body;
    bool                                          headers_complete { false };
    bool                                          message_complete { false };

    void reset()
    {
        method.clear();
        url.clear();
        headers.clear();
        current_field.clear();
        current_value.clear();
        body.clear();
        headers_complete  = false;
        message_complete  = false;
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
    auto* ctx          = static_cast<HttpRequestContext*>(parser->data);
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
    auto* ctx            = static_cast<HttpRequestContext*>(parser->data);
    ctx->message_complete = true;
    return 0;
}

std::string build_http_response(int status_code, std::string_view reason, std::string_view body)
{
    std::string response;
    response.reserve(128 + body.size());
    response.append("HTTP/1.1 ");
    response.append(std::to_string(status_code));
    response.push_back(' ');
    response.append(reason);
    response.append("\r\n");
    response.append("Content-Type: text/plain; charset=utf-8\r\n");
    response.append("Content-Length: ");
    response.append(std::to_string(body.size()));
    response.append("\r\nConnection: close\r\n\r\n");
    response.append(body);
    return response;
}

std::atomic_bool g_stop { false };
std::atomic<int> g_listener_fd { -1 };

#if defined(_WIN32)
static BOOL WINAPI console_ctrl_handler(DWORD type)
{
    if (type == CTRL_C_EVENT) {
        g_stop.store(true, std::memory_order_release);
        int fd = g_listener_fd.exchange(-1, std::memory_order_acq_rel);
        if (fd != -1)
            ::closesocket((SOCKET)fd);
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
}
#endif

Task<void, Work_Promise<SpinLock, void>> handle_http_connection(net::tcp_socket<SpinLock> sock)
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

    while (!ctx.message_complete) {
        ssize_t n = co_await sock.recv(buffer.data(), buffer.size());
        if (n < 0) {
            parse_error  = true;
            error_reason = "socket read error";
            break;
        }
        if (n == 0) {
            llhttp_errno_t finish_err = llhttp_finish(&parser);
            if (finish_err != HPE_OK) {
                parse_error  = true;
                error_reason = llhttp_errno_name(finish_err);
            }
            break;
        }
        llhttp_errno_t err = llhttp_execute(&parser, buffer.data(), static_cast<size_t>(n));
        if (err != HPE_OK) {
            parse_error  = true;
            const char* reason = llhttp_get_error_reason(&parser);
            if (reason && *reason)
                error_reason = reason;
            else
                error_reason = llhttp_errno_name(err);
            break;
        }
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
            response      = build_http_response(200, "OK", response_body);
        } else if (ctx.method == "GET" && ctx.url == "/health") {
            response_body = "OK\n";
            response      = build_http_response(200, "OK", response_body);
        } else {
            response_body = "Not Found\n";
            response      = build_http_response(404, "Not Found", response_body);
        }
    }

    (void)co_await sock.send_all(response.data(), response.size());
    sock.close();
    co_return;
}

Task<void, Work_Promise<SpinLock, void>> http_server(net::fd_workqueue<SpinLock>& fdwq, std::string host, uint16_t port)
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
        auto socket = fdwq.adopt_tcp_socket(fd);
        auto task   = handle_http_connection(std::move(socket));
        post_to(task, fdwq.base());
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
    if (argc > 1)
        port = static_cast<uint16_t>(std::stoi(argv[1]));

#if defined(_WIN32)
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    std::signal(SIGINT, sigint_handler);
#endif

    auto& wq = get_sys_workqueue(0);
    net::fd_workqueue<SpinLock> fdwq(wq);

    auto server_task = http_server(fdwq, host, port);
    auto coroutine   = server_task.get();
    auto& promise    = coroutine.promise();
    std::atomic_bool finished { false };
    promise.mUserData    = &finished;
    promise.mOnCompleted = [](Promise_base& pb) {
        auto* flag = static_cast<std::atomic_bool*>(pb.mUserData);
        if (flag)
            flag->store(true, std::memory_order_release);
    };

    post_to(server_task, wq);

    sys_wait_until(finished);
    return 0;
}


