#include "co_syswork.hpp"
#include "co_test_sys_stats_logger.hpp"
#include "fd_base.hpp"

#if defined(USING_SSL)
#include "tls.hpp"
#include "tls_utils.hpp"
#endif

#if defined(_WIN32)
#include <basetsd.h>
#ifndef _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#define _SSIZE_T_DEFINED
#endif
#endif

#include "http/http1_parser.hpp"
#include "http/http_cli.hpp"
#include "http/http_client.hpp"
#include "http/http_common.hpp"
#include "http/http_easy_client.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#endif
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace net      = co_wq::net;
namespace http     = co_wq::net::http;
namespace http_cli = co_wq::net::http::cli;

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

std::string errno_message(int err)
{
    if (err < 0)
        err = -err;
    std::error_code ec(err, std::generic_category());
    return ec.message();
}

[[maybe_unused]] std::string format_sockaddr(const sockaddr* addr)
{
    if (!addr)
        return "unknown";

    socklen_t len = 0;
    switch (addr->sa_family) {
    case AF_INET:
        len = static_cast<socklen_t>(sizeof(sockaddr_in));
        break;
    case AF_INET6:
        len = static_cast<socklen_t>(sizeof(sockaddr_in6));
        break;
    default:
        len = static_cast<socklen_t>(sizeof(sockaddr_storage));
        break;
    }

    char host[NI_MAXHOST] = {};
    char serv[NI_MAXSERV] = {};
    int  rc = ::getnameinfo(addr, len, host, sizeof(host), serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV);
    if (rc != 0)
        return "unknown";

    std::string host_str(host);
    std::string serv_str(serv);

    if (addr->sa_family == AF_INET6 && !host_str.empty())
        host_str = "[" + host_str + "]";
    if (host_str.empty())
        host_str = "unknown";

    if (!serv_str.empty())
        return host_str + ':' + serv_str;
    return host_str;
}

#if defined(USING_SSL)

[[maybe_unused]] void log_tls_session_details(SSL* ssl, const std::string& host, bool verbose)
{
    if (!verbose || !ssl)
        return;

    const auto lines = co_wq::net::tls_utils::summarize_session(ssl, host, true);
    for (const auto& line : lines)
        verbose_printf(verbose, "* %s\n", line.c_str());
}

#endif // defined(USING_SSL)

using ParsedUrl          = http::UrlParts;
using RequestState       = http_cli::RequestState;
using CommandLineOptions = http_cli::CommandLineOptions;
using BuiltRequest       = http_cli::BuiltRequest;
using HeaderEntry        = http::HeaderEntry;
using NetFdWorkqueue     = http::HttpClientWorkqueue;
using Http1Parser        = http::Http1Parser;
using co_wq::post_to;
using co_wq::Promise_base;
using co_wq::SpinLock;
using co_wq::Task;
using co_wq::Work_Promise;
using http::parse_url;
using http::resolve_redirect_url;
using http_cli::build_http_request;

void print_usage()
{
    std::fprintf(stderr,
                 "Usage: co_curl [options] <url>\n\n"
                 "Options:\n"
                 "  -h, --help             Show this message\n"
                 "  -v, --verbose          Print request/response metadata\n"
                 "  -i, --include          Include response headers in output\n"
                 "  -X, --method <verb>    Override HTTP method (default GET)\n"
                 "  -H, --header <h:v>     Add custom request header\n"
                 "  -d, --data <data>      Send request body (default POST when data present)\n"
                 "  -o, --output <file>    Write body to file instead of stdout\n"
                 "  -L, --location         Follow 3xx redirects (max 10)\n"
                 "      --max-redirs <n>   Set redirect limit\n"
                 "      --insecure         Disable TLS verification (no-op; TLS is opportunistic)\n"
                 "      --http2            Force HTTP/2 (default for HTTPS targets)\n"
                 "      --http1.1          Force HTTP/1.1\n"
                 "      --no-http2         Alias for --http1.1\n");
}
#if !defined(_WIN32)
void install_signal_handlers()
{
    std::signal(SIGPIPE, SIG_IGN);
}
#else
void install_signal_handlers()
{
    // no-op on Windows
}
#endif

#if defined(USING_SSL)
[[maybe_unused]] void apply_tls_sni(net::tls_socket<SpinLock>& socket, const std::string& host)
{
    if (!host.empty())
        SSL_set_tlsext_host_name(socket.ssl_handle(), host.c_str());
}
#endif

} // namespace

namespace {

template <typename Socket>
[[maybe_unused]] Task<int, Work_Promise<SpinLock, int>>
perform_request_http1(Socket& socket, const BuiltRequest& built, Http1Parser& parser)
{
    ssize_t sent = co_await socket.send_all(built.payload.data(), built.payload.size());
    if (sent < 0) {
        CO_WQ_LOG_ERROR("[co_curl] send error: %s", errno_message(errno).c_str());
        co_return -1;
    }

    std::array<char, 8192> buffer {};
    while (!parser.is_message_complete()) {
        ssize_t n = co_await socket.recv(buffer.data(), buffer.size());
        if (n < 0) {
            CO_WQ_LOG_ERROR("[co_curl] recv error: %s", errno_message(errno).c_str());
            co_return -1;
        }
        if (n == 0)
            break;
        std::string_view chunk(buffer.data(), static_cast<size_t>(n));
        std::string      parse_error;
        if (!parser.feed(chunk, &parse_error)) {
            if (parse_error.empty())
                parse_error = "unknown parser error";
            CO_WQ_LOG_ERROR("[co_curl] parse error: %s", parse_error.c_str());
            co_return -1;
        }
    }

    if (!parser.is_message_complete()) {
        std::string parse_error;
        if (!parser.finish(&parse_error)) {
            if (parse_error.empty())
                parse_error = "unknown parser error";
            CO_WQ_LOG_ERROR("[co_curl] parse error at EOF: %s", parse_error.c_str());
            co_return -1;
        }
    }

    co_return 0;
}

Task<int, Work_Promise<SpinLock, int>> run_client(NetFdWorkqueue& fdwq, CommandLineOptions options)
{
    auto parsed_initial = parse_url(options.url);
    if (!parsed_initial.has_value()) {
        CO_WQ_LOG_ERROR("[co_curl] invalid url: %s", options.url.c_str());
        co_return 1;
    }

    std::FILE* output       = nullptr;
    bool       close_output = false;
    if (options.output_path.has_value()) {
        if (*options.output_path == "-") {
            output = stdout;
        } else {
            std::error_code open_ec;
#if defined(_WIN32)
            FILE*   file_handle = nullptr;
            errno_t ferr        = ::fopen_s(&file_handle, options.output_path->c_str(), "wb");
            if (ferr != 0)
                open_ec = std::error_code(ferr, std::generic_category());
            output = file_handle;
#else
            output = std::fopen(options.output_path->c_str(), "wb");
            if (!output)
                open_ec = std::error_code(errno, std::generic_category());
#endif
            if (!output) {
                CO_WQ_LOG_ERROR("[co_curl] failed to open %s: %s",
                                options.output_path->c_str(),
                                open_ec ? open_ec.message().c_str() : "unknown error");
                co_return 1;
            }
            close_output = true;
        }
    } else {
        output = stdout;
    }

    if (options.insecure)
        CO_WQ_LOG_WARN("[co_curl] --insecure currently skips additional certificate checks (default behavior)");

    std::string current_url          = options.url;
    std::string current_method       = options.method;
    std::string current_body         = options.body;
    bool        drop_content_headers = false;

    http::HttpEasyClient easy_client(fdwq);

    int  redirect_count = 0;
    int  final_status   = 0;
    bool success        = false;

    while (redirect_count <= options.max_redirects) {
        auto parsed = parse_url(current_url);
        if (!parsed.has_value()) {
            CO_WQ_LOG_ERROR("[co_curl] invalid url during redirect: %s", current_url.c_str());
            break;
        }

        bool prefer_http2 = options.prefer_http2_explicit ? options.prefer_http2 : (parsed->scheme == "https");

        RequestState state { current_method, current_body, drop_content_headers };
        BuiltRequest built = build_http_request(options, state, *parsed);
        if (options.verbose)
            http_cli::log_request_verbose(*parsed, state, built);

        if (prefer_http2 && parsed->scheme != "https") {
            CO_WQ_LOG_WARN("[co_curl] HTTP/2 preference ignored for plain HTTP targets");
            prefer_http2 = false;
        }

        http::EasyRequest easy_request;
        easy_request.url                  = current_url;
        easy_request.method               = current_method;
        easy_request.headers              = options.headers;
        easy_request.body                 = current_body;
        easy_request.drop_content_headers = drop_content_headers;
        easy_request.verbose              = options.verbose;
        easy_request.include_headers      = options.include_headers;
        easy_request.buffer_body          = options.follow_redirects;
        easy_request.prefer_http2         = prefer_http2;
        easy_request.insecure             = options.insecure;
        easy_request.output               = output;

        http::EasyResponse easy_response;

        int rc = co_await easy_client.perform(easy_request, easy_response);
        if (rc != 0) {
            std::string err = (rc < 0) ? errno_message(-rc) : std::string("error code ") + std::to_string(rc);
            CO_WQ_LOG_ERROR("[co_curl] request failed: %s (rc=%d)", err.c_str(), rc);
            break;
        }

        if (prefer_http2 && parsed->scheme == "https" && !easy_response.used_http2)
            CO_WQ_LOG_INFO("[co_curl] upstream did not negotiate HTTP/2; using HTTP/1.1");

        int status_code = easy_response.status_code;
        final_status    = status_code;

        auto get_header = [&](std::string_view name) -> std::optional<std::string> {
            return easy_response.header(name);
        };

        bool        should_redirect = false;
        std::string location;
        if (options.follow_redirects && redirect_count < options.max_redirects) {
            switch (status_code) {
            case 301:
            case 302:
            case 303:
            case 307:
            case 308:
                if (auto loc = get_header("location")) {
                    should_redirect = true;
                    location        = *loc;
                }
                break;
            default:
                break;
            }
        }

        if (!should_redirect) {
            if (easy_request.buffer_body && easy_request.output && !easy_response.body.empty()) {
                std::fwrite(easy_response.body.data(), 1, easy_response.body.size(), easy_request.output);
                std::fflush(easy_request.output);
            }
            success = true;
            break;
        }

        ++redirect_count;
        std::string next_url = resolve_redirect_url(*parsed, location);
        if (next_url.empty()) {
            CO_WQ_LOG_ERROR("[co_curl] unable to resolve redirect location: %s", location.c_str());
            break;
        }

        switch (status_code) {
        case 303:
            current_method = "GET";
            current_body.clear();
            drop_content_headers = true;
            break;
        case 301:
        case 302:
            if (current_method != "GET" && current_method != "HEAD") {
                current_method = "GET";
                current_body.clear();
                drop_content_headers = true;
            } else {
                drop_content_headers = current_body.empty();
            }
            break;
        case 307:
        case 308:
            drop_content_headers = current_body.empty();
            break;
        default:
            drop_content_headers = current_body.empty();
            break;
        }

        current_url = std::move(next_url);
    }

    if (close_output && output)
        std::fclose(output);

    if (!success) {
        if (redirect_count > options.max_redirects)
            CO_WQ_LOG_ERROR("[co_curl] exceeded redirect limit (%d)", options.max_redirects);
        co_return 1;
    }

    if (final_status >= 400)
        co_return 22;
    co_return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    install_signal_handlers();

    CommandLineOptions options;
    auto               parse_result = http_cli::parse_common_arguments(argc, argv, options);
    if (parse_result.show_help) {
        print_usage();
        return 0;
    }
    if (!parse_result.ok) {
        if (!parse_result.error.empty())
            std::fprintf(stderr, "co_curl: %s\n", parse_result.error.c_str());
        print_usage();
        return 1;
    }

    if (options.url.empty() && !parse_result.positionals.empty())
        options.url = parse_result.positionals.front();
    if (options.url.empty()) {
        std::fprintf(stderr, "co_curl: missing URL\n");
        print_usage();
        return 1;
    }
    if (options.url.find("://") == std::string::npos)
        options.url = "http://" + options.url;
    if (parse_result.positionals.size() > 1)
        std::fprintf(stderr, "co_curl: ignoring unexpected trailing arguments\n");

    co_wq::test::SysStatsLogger stats_logger("co_curl");

    auto&          wq = get_sys_workqueue(0);
    NetFdWorkqueue fdwq(wq);

    auto  client_task = run_client(fdwq, std::move(options));
    auto  coroutine   = client_task.get();
    auto& promise     = coroutine.promise();

    std::atomic_bool finished { false };
    promise.mUserData    = &finished;
    promise.mOnCompleted = [](Promise_base& pb) {
        auto* flag = static_cast<std::atomic_bool*>(pb.mUserData);
        if (flag)
            flag->store(true, std::memory_order_release);
    };

    post_to(client_task, wq);
    sys_wait_until(finished);

    return promise.result();
}
