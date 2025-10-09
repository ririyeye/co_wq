#include "syswork.hpp"
#include "test_sys_stats_logger.hpp"

#include "dns_resolver.hpp"
#include "fd_base.hpp"
#include "tcp_socket.hpp"
#if defined(USING_SSL)
#include "tls.hpp"
#endif

#include <llhttp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#endif

using namespace co_wq;

using NetFdWorkqueue = net::fd_workqueue<SpinLock, net::epoll_reactor>;

namespace {

struct HeaderEntry {
    std::string name;
    std::string value;
};

struct CommandLineOptions {
    std::string                method { "GET" };
    bool                       method_explicit { false };
    std::string                url;
    std::vector<HeaderEntry>   headers;
    std::string                body;
    bool                       verbose { false };
    bool                       include_headers { false };
    bool                       follow_redirects { false };
    int                        max_redirects { 10 };
    std::optional<std::string> output_path;
    bool                       insecure { false };
};

struct ParsedUrl {
    std::string scheme;
    std::string host;
    uint16_t    port { 0 };
    std::string path;
};

struct RequestState {
    std::string method;
    std::string body;
    bool        drop_content_headers { false };
};

std::string to_lower(std::string_view input)
{
    std::string out;
    out.reserve(input.size());
    for (char ch : input)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    return out;
}

void to_upper_inplace(std::string& input)
{
    for (char& ch : input)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
}

std::string trim_leading_spaces(std::string_view input)
{
    size_t pos = 0;
    while (pos < input.size() && (input[pos] == ' ' || input[pos] == '\t'))
        ++pos;
    return std::string(input.substr(pos));
}

bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

bool split_host_port(std::string_view input, uint16_t default_port, std::string& host_out, uint16_t& port_out)
{
    if (input.empty())
        return false;

    std::string host;
    std::string port_str;

    if (input.front() == '[') {
        auto end = input.find(']');
        if (end == std::string_view::npos)
            return false;
        host = std::string(input.substr(1, end - 1));
        if (end + 1 < input.size()) {
            if (input[end + 1] != ':')
                return false;
            port_str = std::string(input.substr(end + 2));
        }
    } else {
        auto colon = input.rfind(':');
        if (colon != std::string_view::npos && input.find(':', colon + 1) == std::string_view::npos) {
            host     = std::string(input.substr(0, colon));
            port_str = std::string(input.substr(colon + 1));
        } else {
            host = std::string(input);
        }
    }

    if (host.empty())
        return false;

    uint16_t port_value = default_port;
    if (!port_str.empty()) {
        if (port_str.size() > 5)
            return false;
        unsigned int value = 0;
        for (char ch : port_str) {
            if (!std::isdigit(static_cast<unsigned char>(ch)))
                return false;
            value = value * 10 + (ch - '0');
            if (value > 65535)
                return false;
        }
        port_value = static_cast<uint16_t>(value);
    }

    host_out = std::move(host);
    port_out = port_value;
    return true;
}

std::optional<ParsedUrl> parse_url(const std::string& url)
{
    auto scheme_pos = url.find("://");
    if (scheme_pos == std::string::npos)
        return std::nullopt;
    ParsedUrl result;
    result.scheme = to_lower(url.substr(0, scheme_pos));
    if (result.scheme != "http" && result.scheme != "https")
        return std::nullopt;

    size_t authority_start = scheme_pos + 3;
    if (authority_start >= url.size())
        return std::nullopt;

    auto        slash_pos = url.find('/', authority_start);
    std::string authority = slash_pos == std::string::npos ? url.substr(authority_start)
                                                           : url.substr(authority_start, slash_pos - authority_start);
    result.path           = slash_pos == std::string::npos ? std::string("/") : url.substr(slash_pos);
    if (authority.empty())
        return std::nullopt;

    uint16_t default_port = (result.scheme == "https") ? 443 : 80;
    if (!split_host_port(authority, default_port, result.host, result.port))
        return std::nullopt;
    if (result.host.empty())
        return std::nullopt;

    return result;
}

bool header_exists(const std::vector<HeaderEntry>& headers, std::string_view name)
{
    for (const auto& h : headers) {
        if (iequals(h.name, name))
            return true;
    }
    return false;
}

void remove_content_headers(std::vector<HeaderEntry>& headers)
{
    headers.erase(std::remove_if(headers.begin(),
                                 headers.end(),
                                 [](const HeaderEntry& h) {
                                     auto lower = to_lower(h.name);
                                     return lower == "content-length" || lower == "content-type"
                                         || lower == "transfer-encoding";
                                 }),
                  headers.end());
}

struct BuiltRequest {
    std::string              payload;
    std::vector<HeaderEntry> headers;
};

BuiltRequest build_http_request(const CommandLineOptions& base, const RequestState& state, const ParsedUrl& url)
{
    BuiltRequest result;
    result.headers = base.headers;
    if (state.drop_content_headers)
        remove_content_headers(result.headers);

    const bool has_body = !state.body.empty();

    if (!header_exists(result.headers, "Host")) {
        std::string value           = url.host;
        const bool  is_https        = url.scheme == "https";
        const bool  is_default_port = (is_https && url.port == 443) || (!is_https && url.port == 80);
        if (!is_default_port) {
            value.push_back(':');
            value.append(std::to_string(url.port));
        }
        result.headers.push_back({ "Host", std::move(value) });
    }

    if (!header_exists(result.headers, "User-Agent"))
        result.headers.push_back({ "User-Agent", "co_curl/1.0" });

    if (!header_exists(result.headers, "Accept"))
        result.headers.push_back({ "Accept", "*/*" });

    if (!header_exists(result.headers, "Connection"))
        result.headers.push_back({ "Connection", "close" });

    if (has_body && !header_exists(result.headers, "Content-Length")
        && !header_exists(result.headers, "Transfer-Encoding")) {
        result.headers.push_back({ "Content-Length", std::to_string(state.body.size()) });
    }

    result.payload.reserve(state.method.size() + url.path.size() + 32 + state.body.size() + result.headers.size() * 32);
    result.payload.append(state.method);
    result.payload.push_back(' ');
    result.payload.append(url.path.empty() ? "/" : url.path);
    result.payload.append(" HTTP/1.1\r\n");

    for (const auto& header : result.headers) {
        result.payload.append(header.name);
        result.payload.append(": ");
        result.payload.append(header.value);
        result.payload.append("\r\n");
    }
    result.payload.append("\r\n");
    result.payload.append(state.body);

    return result;
}

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
                 "      --insecure         Disable TLS verification (no-op; TLS is opportunistic)\n");
}

bool parse_header_line(const std::string& line, HeaderEntry& out)
{
    auto pos = line.find(':');
    if (pos == std::string::npos)
        return false;
    out.name  = line.substr(0, pos);
    out.value = trim_leading_spaces(std::string_view(line).substr(pos + 1));
    return !out.name.empty();
}

std::optional<std::string> read_file_to_string(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return std::nullopt;
    std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return data;
}

std::optional<CommandLineOptions> parse_arguments(int argc, char* argv[])
{
    CommandLineOptions options;
    int                idx = 1;

    auto require_value = [&](int current_idx, const char* option_name) -> bool {
        if (current_idx + 1 >= argc) {
            std::fprintf(stderr, "co_curl: missing value for %s\n", option_name);
            return false;
        }
        return true;
    };

    for (; idx < argc; ++idx) {
        std::string_view arg(argv[idx]);
        if (arg == "-h" || arg == "--help") {
            print_usage();
            return std::nullopt;
        } else if (arg == "-v" || arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "-i" || arg == "--include") {
            options.include_headers = true;
        } else if (arg == "-L" || arg == "--location") {
            options.follow_redirects = true;
        } else if (arg == "--max-redirs") {
            if (!require_value(idx, "--max-redirs"))
                return std::nullopt;
            ++idx;
            try {
                options.max_redirects = std::max(0, std::stoi(argv[idx]));
            } catch (const std::exception&) {
                std::fprintf(stderr, "co_curl: invalid value for --max-redirs: %s\n", argv[idx]);
                return std::nullopt;
            }
        } else if (arg == "--insecure") {
            options.insecure = true;
        } else if (arg == "-X" || arg == "--method") {
            if (!require_value(idx, arg.data()))
                return std::nullopt;
            options.method = argv[++idx];
            to_upper_inplace(options.method);
            options.method_explicit = true;
        } else if (arg == "-H" || arg == "--header") {
            if (!require_value(idx, arg.data()))
                return std::nullopt;
            HeaderEntry entry;
            if (!parse_header_line(argv[++idx], entry)) {
                std::fprintf(stderr, "co_curl: invalid header format: %s\n", argv[idx]);
                return std::nullopt;
            }
            options.headers.push_back(std::move(entry));
        } else if (arg == "-d" || arg == "--data") {
            if (!require_value(idx, arg.data()))
                return std::nullopt;
            std::string data_arg = argv[++idx];
            if (!data_arg.empty() && data_arg.front() == '@') {
                auto file_data = read_file_to_string(data_arg.substr(1));
                if (!file_data.has_value()) {
                    std::fprintf(stderr, "co_curl: failed to read data file: %s\n", data_arg.c_str());
                    return std::nullopt;
                }
                options.body = std::move(*file_data);
            } else {
                options.body = std::move(data_arg);
            }
        } else if (arg == "-o" || arg == "--output") {
            if (!require_value(idx, arg.data()))
                return std::nullopt;
            options.output_path = std::string(argv[++idx]);
        } else if (arg.size() > 0 && arg[0] == '-') {
            std::fprintf(stderr, "co_curl: unknown option: %s\n", std::string(arg).c_str());
            return std::nullopt;
        } else {
            break;
        }
    }

    if (idx >= argc) {
        std::fprintf(stderr, "co_curl: missing URL\n");
        print_usage();
        return std::nullopt;
    }

    options.url = argv[idx++];
    if (!options.method_explicit && !options.body.empty())
        options.method = "POST";
    to_upper_inplace(options.method);

    if (idx < argc) {
        std::fprintf(stderr, "co_curl: ignoring unexpected trailing arguments\n");
    }

    return options;
}

struct ResponseContext {
    llhttp_t                                     parser;
    llhttp_settings_t                            settings;
    std::string                                  status_text;
    int                                          status_code { 0 };
    int                                          http_major { 1 };
    int                                          http_minor { 1 };
    std::vector<HeaderEntry>                     header_sequence;
    std::unordered_map<std::string, std::string> header_lookup;
    std::string                                  current_field;
    std::string                                  current_value;
    bool                                         headers_complete { false };
    bool                                         message_complete { false };
    bool                                         verbose { false };
    bool                                         include_headers { false };
    bool                                         buffer_body { false };
    std::string                                  header_block;
    std::string                                  body_buffer;
    std::FILE*                                   output { nullptr };

    ResponseContext()
    {
        llhttp_settings_init(&settings);
        settings.on_status                = &ResponseContext::on_status_cb;
        settings.on_header_field          = &ResponseContext::on_header_field_cb;
        settings.on_header_value          = &ResponseContext::on_header_value_cb;
        settings.on_header_value_complete = &ResponseContext::on_header_value_complete_cb;
        settings.on_headers_complete      = &ResponseContext::on_headers_complete_cb;
        settings.on_body                  = &ResponseContext::on_body_cb;
        settings.on_message_complete      = &ResponseContext::on_message_complete_cb;
        llhttp_init(&parser, HTTP_RESPONSE, &settings);
        parser.data = this;
    }

    std::optional<std::string> header(std::string_view name) const
    {
        auto it = header_lookup.find(to_lower(name));
        if (it == header_lookup.end())
            return std::nullopt;
        return it->second;
    }

    void reset()
    {
        llhttp_reset(&parser);
        status_text.clear();
        status_code = 0;
        http_major  = 1;
        http_minor  = 1;
        header_sequence.clear();
        header_lookup.clear();
        current_field.clear();
        current_value.clear();
        headers_complete = false;
        message_complete = false;
        header_block.clear();
        body_buffer.clear();
    }

    void flush_buffered_output()
    {
        if (!output)
            return;
        if (!header_block.empty() && include_headers)
            std::fwrite(header_block.data(), 1, header_block.size(), output);
        if (!body_buffer.empty())
            std::fwrite(body_buffer.data(), 1, body_buffer.size(), output);
        std::fflush(output);
    }

    static int on_status_cb(llhttp_t* parser, const char* at, size_t length)
    {
        auto* ctx = static_cast<ResponseContext*>(parser->data);
        ctx->status_text.append(at, length);
        return 0;
    }

    static int on_header_field_cb(llhttp_t* parser, const char* at, size_t length)
    {
        auto* ctx = static_cast<ResponseContext*>(parser->data);
        if (!ctx->current_value.empty()) {
            ctx->store_header();
        }
        ctx->current_field.append(at, length);
        return 0;
    }

    static int on_header_value_cb(llhttp_t* parser, const char* at, size_t length)
    {
        auto* ctx = static_cast<ResponseContext*>(parser->data);
        ctx->current_value.append(at, length);
        return 0;
    }

    static int on_header_value_complete_cb(llhttp_t* parser)
    {
        auto* ctx = static_cast<ResponseContext*>(parser->data);
        ctx->store_header();
        return 0;
    }

    static int on_headers_complete_cb(llhttp_t* parser)
    {
        auto* ctx             = static_cast<ResponseContext*>(parser->data);
        ctx->status_code      = parser->status_code;
        ctx->http_major       = parser->http_major;
        ctx->http_minor       = parser->http_minor;
        ctx->headers_complete = true;

        ctx->header_block.clear();
        if (ctx->include_headers) {
            ctx->header_block.reserve(64 + ctx->header_sequence.size() * 32);
            ctx->header_block.append("HTTP/");
            ctx->header_block.append(std::to_string(ctx->http_major));
            ctx->header_block.push_back('.');
            ctx->header_block.append(std::to_string(ctx->http_minor));
            ctx->header_block.push_back(' ');
            ctx->header_block.append(std::to_string(ctx->status_code));
            if (!ctx->status_text.empty()) {
                ctx->header_block.push_back(' ');
                ctx->header_block.append(ctx->status_text);
            }
            ctx->header_block.append("\r\n");
            for (const auto& header : ctx->header_sequence) {
                ctx->header_block.append(header.name);
                ctx->header_block.append(": ");
                ctx->header_block.append(header.value);
                ctx->header_block.append("\r\n");
            }
            ctx->header_block.append("\r\n");
            if (!ctx->buffer_body && ctx->output)
                std::fwrite(ctx->header_block.data(), 1, ctx->header_block.size(), ctx->output);
        }

        if (ctx->verbose) {
            std::fprintf(stderr,
                         "< HTTP/%d.%d %d %s\n",
                         ctx->http_major,
                         ctx->http_minor,
                         ctx->status_code,
                         ctx->status_text.c_str());
            for (const auto& header : ctx->header_sequence) {
                std::fprintf(stderr, "< %s: %s\n", header.name.c_str(), header.value.c_str());
            }
            std::fprintf(stderr, "<\n");
        }

        return 0;
    }

    static int on_body_cb(llhttp_t* parser, const char* at, size_t length)
    {
        auto* ctx = static_cast<ResponseContext*>(parser->data);
        if (ctx->buffer_body) {
            ctx->body_buffer.append(at, length);
        } else if (ctx->output) {
            std::fwrite(at, 1, length, ctx->output);
        }
        return 0;
    }

    static int on_message_complete_cb(llhttp_t* parser)
    {
        auto* ctx             = static_cast<ResponseContext*>(parser->data);
        ctx->message_complete = true;
        return 0;
    }

private:
    void store_header()
    {
        if (current_field.empty())
            return;
        HeaderEntry entry;
        entry.name  = std::move(current_field);
        entry.value = trim_leading_spaces(current_value);
        header_sequence.push_back(entry);
        header_lookup[to_lower(header_sequence.back().name)] = header_sequence.back().value;
        current_field.clear();
        current_value.clear();
    }
};

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

void log_request_verbose(const ParsedUrl& url, const RequestState& state, const BuiltRequest& built)
{
    std::fprintf(stderr, "> %s %s HTTP/1.1\n", state.method.c_str(), url.path.empty() ? "/" : url.path.c_str());
    for (const auto& header : built.headers) {
        std::fprintf(stderr, "> %s: %s\n", header.name.c_str(), header.value.c_str());
    }
    std::fprintf(stderr, ">\n");
    if (!state.body.empty())
        std::fprintf(stderr, "> [%zu bytes of body]\n", state.body.size());
}

std::string resolve_redirect_url(const ParsedUrl& base, const std::string& location)
{
    if (location.empty())
        return {};
    if (location.find("http://") == 0 || location.find("https://") == 0)
        return location;
    if (location.front() == '/') {
        std::string result       = base.scheme + "://" + base.host;
        const bool  is_https     = base.scheme == "https";
        const bool  default_port = (is_https && base.port == 443) || (!is_https && base.port == 80);
        if (!default_port) {
            result.push_back(':');
            result.append(std::to_string(base.port));
        }
        result.append(location);
        return result;
    }
    std::string result       = base.scheme + "://" + base.host;
    const bool  is_https     = base.scheme == "https";
    const bool  default_port = (is_https && base.port == 443) || (!is_https && base.port == 80);
    if (!default_port) {
        result.push_back(':');
        result.append(std::to_string(base.port));
    }
    auto last_slash = base.path.find_last_of('/');
    if (last_slash == std::string::npos)
        result.push_back('/');
    else
        result.append(base.path.substr(0, last_slash + 1));
    result.append(location);
    return result;
}

#if defined(USING_SSL)
void apply_tls_sni(net::tls_socket<SpinLock>& socket, const std::string& host)
{
    if (!host.empty())
        SSL_set_tlsext_host_name(socket.ssl_handle(), host.c_str());
}
#endif

} // namespace

namespace {

template <typename Socket>
Task<int, Work_Promise<SpinLock, int>> perform_request(Socket& socket, const BuiltRequest& built, ResponseContext& ctx)
{
    ssize_t sent = co_await socket.send_all(built.payload.data(), built.payload.size());
    if (sent < 0) {
        CO_WQ_LOG_ERROR("[co_curl] send error: %s", std::strerror(errno));
        co_return -1;
    }

    std::array<char, 8192> buffer {};
    while (!ctx.message_complete) {
        ssize_t n = co_await socket.recv(buffer.data(), buffer.size());
        if (n < 0) {
            CO_WQ_LOG_ERROR("[co_curl] recv error: %s", std::strerror(errno));
            co_return -1;
        }
        if (n == 0)
            break;
        llhttp_errno_t err = llhttp_execute(&ctx.parser, buffer.data(), static_cast<size_t>(n));
        if (err != HPE_OK) {
            const char* reason = llhttp_get_error_reason(&ctx.parser);
            if (reason == nullptr || *reason == '\0')
                reason = llhttp_errno_name(err);
            CO_WQ_LOG_ERROR("[co_curl] parse error: %s", reason);
            co_return -1;
        }
    }

    if (!ctx.message_complete) {
        llhttp_errno_t finish_err = llhttp_finish(&ctx.parser);
        if (finish_err != HPE_OK && finish_err != HPE_PAUSED_UPGRADE) {
            const char* reason = llhttp_get_error_reason(&ctx.parser);
            if (reason == nullptr || *reason == '\0')
                reason = llhttp_errno_name(finish_err);
            CO_WQ_LOG_ERROR("[co_curl] parse error at EOF: %s", reason);
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
            output = std::fopen(options.output_path->c_str(), "wb");
            if (!output) {
                CO_WQ_LOG_ERROR("[co_curl] failed to open %s", options.output_path->c_str());
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

    int  redirect_count = 0;
    int  final_status   = 0;
    bool success        = false;

    while (redirect_count <= options.max_redirects) {
        auto parsed = parse_url(current_url);
        if (!parsed.has_value()) {
            CO_WQ_LOG_ERROR("[co_curl] invalid url during redirect: %s", current_url.c_str());
            break;
        }

        RequestState state { current_method, current_body, drop_content_headers };
        BuiltRequest built = build_http_request(options, state, *parsed);
        if (options.verbose)
            log_request_verbose(*parsed, state, built);

        ResponseContext response_ctx;
        response_ctx.verbose         = options.verbose;
        response_ctx.include_headers = options.include_headers;
        response_ctx.buffer_body     = options.follow_redirects;
        response_ctx.output          = output;

        int rc = 0;
        if (parsed->scheme == "https") {
#if defined(USING_SSL)
            auto tls_socket = fdwq.make_tls_socket(net::tls_context::make(net::tls_mode::Client),
                                                   net::tls_mode::Client);
            apply_tls_sni(tls_socket, parsed->host);
            auto&                     tcp_base = tls_socket.underlying();
            net::dns::resolve_options dns_opts;
            dns_opts.family           = tcp_base.family();
            dns_opts.allow_dual_stack = tcp_base.dual_stack();
            auto resolved             = net::dns::resolve_sync(parsed->host, parsed->port, dns_opts);
            if (!resolved.success) {
                CO_WQ_LOG_ERROR("[co_curl] dns resolve failed: %s (%d)",
                                resolved.error_message.c_str(),
                                resolved.error_code);
                co_return 1;
            }
            int connect_rc = co_await tcp_base.connect(reinterpret_cast<const sockaddr*>(&resolved.storage),
                                                       resolved.length);
            if (connect_rc != 0) {
                CO_WQ_LOG_ERROR("[co_curl] connect failed: %s", std::strerror(errno));
                co_return 1;
            }
            int handshake_rc = co_await tls_socket.handshake();
            if (handshake_rc != 0) {
                CO_WQ_LOG_ERROR("[co_curl] tls handshake failed: %d", handshake_rc);
                co_return 1;
            }
            rc = co_await perform_request(tls_socket, built, response_ctx);
            tls_socket.close();
#else
            CO_WQ_LOG_ERROR("[co_curl] https requested but TLS support is disabled");
            co_return 1;
#endif
        } else {
            auto                      tcp_socket = fdwq.make_tcp_socket(AF_INET6, true);
            net::dns::resolve_options dns_opts;
            dns_opts.family           = tcp_socket.family();
            dns_opts.allow_dual_stack = tcp_socket.dual_stack();
            auto resolved             = net::dns::resolve_sync(parsed->host, parsed->port, dns_opts);
            if (!resolved.success) {
                CO_WQ_LOG_ERROR("[co_curl] dns resolve failed: %s (%d)",
                                resolved.error_message.c_str(),
                                resolved.error_code);
                co_return 1;
            }
            int connect_rc = co_await tcp_socket.connect(reinterpret_cast<const sockaddr*>(&resolved.storage),
                                                         resolved.length);
            if (connect_rc != 0) {
                CO_WQ_LOG_ERROR("[co_curl] connect failed: %s", std::strerror(errno));
                co_return 1;
            }
            rc = co_await perform_request(tcp_socket, built, response_ctx);
            tcp_socket.close();
        }

        if (rc != 0) {
            break;
        }

        final_status = response_ctx.status_code;

        bool        should_redirect = false;
        std::string location;
        if (options.follow_redirects && redirect_count < options.max_redirects) {
            switch (response_ctx.status_code) {
            case 301:
            case 302:
            case 303:
            case 307:
            case 308:
                if (auto loc = response_ctx.header("location")) {
                    should_redirect = true;
                    location        = *loc;
                }
                break;
            default:
                break;
            }
        }

        if (!should_redirect) {
            if (response_ctx.buffer_body)
                response_ctx.flush_buffered_output();
            success = true;
            break;
        }

        ++redirect_count;
        std::string next_url = resolve_redirect_url(*parsed, location);
        if (next_url.empty()) {
            CO_WQ_LOG_ERROR("[co_curl] unable to resolve redirect location: %s", location.c_str());
            break;
        }

        switch (response_ctx.status_code) {
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
        response_ctx.reset();
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

    auto options_opt = parse_arguments(argc, argv);
    if (!options_opt.has_value())
        return 1;
    auto options = std::move(*options_opt);

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
