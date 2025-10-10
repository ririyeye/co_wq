#include "syswork.hpp"
#include "test_sys_stats_logger.hpp"

#include "dns_resolver.hpp"
#include "fd_base.hpp"
#include "tcp_socket.hpp"
#if defined(USING_SSL)
#include "tls.hpp"
#endif

#include <llhttp.h>
#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#endif
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

#if defined(USING_SSL)
#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/objects.h>
#include <openssl/opensslv.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#endif

namespace {

#if defined(USING_SSL)

std::string asn1_time_to_string(const ASN1_TIME* time)
{
    if (!time)
        return {};
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio)
        return {};
    std::string result;
    if (ASN1_TIME_print(bio, time) == 1) {
        BUF_MEM* mem = nullptr;
        BIO_get_mem_ptr(bio, &mem);
        if (mem && mem->data)
            result.assign(mem->data, mem->length);
    }
    BIO_free(bio);
    return result;
}

std::string x509_name_to_string(X509_NAME* name)
{
    if (!name)
        return {};
    std::string result;
    const int   count = X509_NAME_entry_count(name);
    for (int i = 0; i < count; ++i) {
        X509_NAME_ENTRY* entry = X509_NAME_get_entry(name, i);
        if (!entry)
            continue;
        ASN1_OBJECT* obj  = X509_NAME_ENTRY_get_object(entry);
        ASN1_STRING* data = X509_NAME_ENTRY_get_data(entry);
        if (!obj || !data)
            continue;
        unsigned char* utf8 = nullptr;
        int            len  = ASN1_STRING_to_UTF8(&utf8, data);
        if (len <= 0 || !utf8)
            continue;
        if (!result.empty())
            result.append("; ");
        const int   nid = OBJ_obj2nid(obj);
        const char* sn  = (nid != NID_undef) ? OBJ_nid2sn(nid) : nullptr;
        if (sn)
            result.append(sn);
        else {
            char obj_buf[64] = {};
            OBJ_obj2txt(obj_buf, sizeof(obj_buf), obj, 0);
            result.append(obj_buf);
        }
        result.append("=");
        result.append(reinterpret_cast<const char*>(utf8), static_cast<size_t>(len));
        OPENSSL_free(utf8);
    }
    return result;
}

std::string collect_subject_alt_names(X509* cert)
{
    if (!cert)
        return {};
    STACK_OF(GENERAL_NAME)* names = static_cast<STACK_OF(GENERAL_NAME)*>(
        X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
    if (!names)
        return {};
    std::string result;
    const int   count = sk_GENERAL_NAME_num(names);
    for (int i = 0; i < count; ++i) {
        const GENERAL_NAME* name = sk_GENERAL_NAME_value(names, i);
        if (!name)
            continue;
        if (name->type == GEN_DNS && name->d.dNSName) {
            unsigned char* utf8 = nullptr;
            int            len  = ASN1_STRING_to_UTF8(&utf8, name->d.dNSName);
            if (len <= 0 || !utf8)
                continue;
            if (!result.empty())
                result.append(", ");
            result.append("DNS:");
            result.append(reinterpret_cast<const char*>(utf8), static_cast<size_t>(len));
            OPENSSL_free(utf8);
        }
    }
    GENERAL_NAMES_free(names);
    return result;
}

std::string describe_public_key(EVP_PKEY* key)
{
    if (!key)
        return "unknown";
    const int   bits = EVP_PKEY_bits(key);
    std::string type;
    switch (EVP_PKEY_base_id(key)) {
    case EVP_PKEY_RSA:
        type = "RSA";
        break;
    case EVP_PKEY_EC:
        type = "EC";
        break;
    case EVP_PKEY_DSA:
        type = "DSA";
        break;
    default:
        type = "UNKNOWN";
        break;
    }
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%s (%d bits)", type.c_str(), bits);
    return buffer;
}

std::string describe_signature_algorithm(const X509* cert)
{
    if (!cert)
        return "unknown";
    int nid = X509_get_signature_nid(cert);
    if (nid == NID_undef)
        return "unknown";
    const char* name = OBJ_nid2sn(nid);
    return name ? name : "unknown";
}

#endif // defined(USING_SSL)

void verbose_printf(bool enabled, const char* fmt, ...)
{
    if (!enabled)
        return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
}

std::string format_endpoint(const sockaddr* addr)
{
    if (!addr)
        return {};
#if defined(_WIN32)
    return "[address unavailable on Windows]";
#else
    char     host[INET6_ADDRSTRLEN] = {};
    uint16_t port                   = 0;
    switch (addr->sa_family) {
    case AF_INET: {
        auto* in = reinterpret_cast<const sockaddr_in*>(addr);
        if (!inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host)))
            std::strncpy(host, "0.0.0.0", sizeof(host) - 1);
        port = ntohs(in->sin_port);
        std::string out(host);
        out.append(":");
        out.append(std::to_string(port));
        return out;
    }
    case AF_INET6: {
        auto* in6 = reinterpret_cast<const sockaddr_in6*>(addr);
        if (!inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host)))
            std::strncpy(host, "::", sizeof(host) - 1);
        port = ntohs(in6->sin6_port);
        std::string out("[");
        out.append(host);
        out.append("]:");
        out.append(std::to_string(port));
        return out;
    }
    default:
        return "[unknown family]";
    }
#endif
}

#if defined(USING_SSL)

void log_tls_session_details(SSL* ssl, const std::string& host, bool verbose)
{
    if (!verbose || !ssl)
        return;

    const char* protocol = SSL_get_version(ssl);
    const char* cipher   = SSL_get_cipher_name(ssl);
    verbose_printf(verbose,
                   "* SSL connection using %s / %s\n",
                   protocol ? protocol : "unknown",
                   cipher ? cipher : "unknown");

    const unsigned char* alpn     = nullptr;
    unsigned int         alpn_len = 0;
    SSL_get0_alpn_selected(ssl, &alpn, &alpn_len);
    if (alpn && alpn_len > 0)
        verbose_printf(verbose, "* ALPN: server accepted %.*s\n", static_cast<int>(alpn_len), alpn);
    else
        verbose_printf(verbose, "* ALPN: no protocol negotiated\n");

    X509* leaf = SSL_get1_peer_certificate(ssl);
    if (leaf) {
        verbose_printf(verbose, "* Server certificate:\n");
        const auto subject = x509_name_to_string(X509_get_subject_name(leaf));
        const auto issuer  = x509_name_to_string(X509_get_issuer_name(leaf));
        verbose_printf(verbose, "*  subject: %s\n", subject.empty() ? "<unknown>" : subject.c_str());
        const auto not_before = asn1_time_to_string(X509_get0_notBefore(leaf));
        const auto not_after  = asn1_time_to_string(X509_get0_notAfter(leaf));
        verbose_printf(verbose, "*  start date: %s\n", not_before.empty() ? "<unknown>" : not_before.c_str());
        verbose_printf(verbose, "*  expire date: %s\n", not_after.empty() ? "<unknown>" : not_after.c_str());
        if (!host.empty()) {
            const int host_match = X509_check_host(leaf, host.c_str(), host.size(), 0, nullptr);
            if (host_match == 1)
                verbose_printf(verbose, "*  subjectAltName: host \"%s\" matched certificate\n", host.c_str());
            else if (host_match == 0)
                verbose_printf(verbose, "*  subjectAltName: host \"%s\" did not match certificate\n", host.c_str());
        }
        if (auto san = collect_subject_alt_names(leaf); !san.empty())
            verbose_printf(verbose, "*  subjectAltName: %s\n", san.c_str());
        verbose_printf(verbose, "*  issuer: %s\n", issuer.empty() ? "<unknown>" : issuer.c_str());

        if (EVP_PKEY* key = X509_get_pubkey(leaf)) {
            auto key_desc = describe_public_key(key);
            EVP_PKEY_free(key);
            verbose_printf(verbose, "*   Public key: %s\n", key_desc.c_str());
        }
        auto sig_desc = describe_signature_algorithm(leaf);
        verbose_printf(verbose, "*   Signature algorithm: %s\n", sig_desc.c_str());
        X509_free(leaf);
    } else {
        verbose_printf(verbose, "* Server certificate: <none>\n");
    }

    if (STACK_OF(X509)* chain = SSL_get_peer_cert_chain(ssl)) {
        const int count = sk_X509_num(chain);
        for (int i = 0; i < count; ++i) {
            X509* chain_cert = sk_X509_value(chain, i);
            if (!chain_cert)
                continue;
            if (EVP_PKEY* key = X509_get_pubkey(chain_cert)) {
                auto key_desc = describe_public_key(key);
                EVP_PKEY_free(key);
                auto sig_desc = describe_signature_algorithm(chain_cert);
                verbose_printf(verbose,
                               "*   Certificate level %d: %s, signed using %s\n",
                               i,
                               key_desc.c_str(),
                               sig_desc.c_str());
            }
        }
    }

    const long verify_result = SSL_get_verify_result(ssl);
    if (verify_result == X509_V_OK)
        verbose_printf(verbose, "* SSL certificate verify ok.\n");
    else
        verbose_printf(verbose,
                       "* SSL certificate verify result: %s (%ld)\n",
                       X509_verify_cert_error_string(verify_result),
                       verify_result);
}

#endif // defined(USING_SSL)

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
    bool                       prefer_http2 { false };
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
                 "      --insecure         Disable TLS verification (no-op; TLS is opportunistic)\n"
                 "      --http2            Prefer HTTP/2 when available (HTTPS only)\n");
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
        } else if (arg == "--http2") {
            options.prefer_http2 = true;
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

namespace http2_client {

    struct RequestBody {
        const std::string* data { nullptr };
        size_t             offset { 0 };
    };

    struct ResponseState {
        int                                          status_code { 0 };
        std::string                                  status_text;
        std::vector<HeaderEntry>                     headers;
        std::unordered_map<std::string, std::string> header_lookup;
        bool                                         headers_complete { false };
        bool                                         message_complete { false };
        bool                                         verbose { false };
        bool                                         include_headers { false };
        bool                                         buffer_body { false };
        std::string                                  header_block;
        std::string                                  body_buffer;
        std::FILE*                                   output { nullptr };

        std::optional<std::string> header(std::string_view name) const
        {
            auto it = header_lookup.find(to_lower(name));
            if (it == header_lookup.end())
                return std::nullopt;
            return it->second;
        }

        void append_header(std::string name, std::string value)
        {
            HeaderEntry entry { std::move(name), std::move(value) };
            header_lookup[to_lower(entry.name)] = entry.value;
            headers.push_back(std::move(entry));
        }

        void emit_headers()
        {
            if (!output || !include_headers)
                return;
            if (header_block.empty()) {
                header_block.append("HTTP/2 ");
                if (!status_text.empty())
                    header_block.append(status_text);
                else
                    header_block.append(std::to_string(status_code));
                header_block.append("\r\n");
                for (const auto& header : headers) {
                    header_block.append(header.name);
                    header_block.append(": ");
                    header_block.append(header.value);
                    header_block.append("\r\n");
                }
                header_block.append("\r\n");
            }
            std::fwrite(header_block.data(), 1, header_block.size(), output);
        }

        void flush_body()
        {
            if (!output)
                return;
            if (!body_buffer.empty())
                std::fwrite(body_buffer.data(), 1, body_buffer.size(), output);
            std::fflush(output);
        }
    };

    struct SessionState {
        std::vector<uint8_t> send_buffer;
        ResponseState*       response { nullptr };
        RequestBody          request_body;
        bool                 has_request_body { false };
        int32_t              stream_id { -1 };
        bool                 completed { false };
        bool                 failed { false };
    };

    [[maybe_unused]] static ssize_t
    send_callback(nghttp2_session*, const uint8_t* data, size_t length, int, void* user_data)
    {
        auto* state = static_cast<SessionState*>(user_data);
        state->send_buffer.insert(state->send_buffer.end(), data, data + length);
        return static_cast<ssize_t>(length);
    }

    [[maybe_unused]] static int on_header_callback(nghttp2_session*,
                                                   const nghttp2_frame* frame,
                                                   const uint8_t*       name,
                                                   size_t               namelen,
                                                   const uint8_t*       value,
                                                   size_t               valuelen,
                                                   uint8_t,
                                                   void* user_data)
    {
        auto* state = static_cast<SessionState*>(user_data);
        if (!state->response)
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        if (frame->hd.stream_id != state->stream_id)
            return 0;
        if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_RESPONSE)
            return 0;
        std::string key(reinterpret_cast<const char*>(name), namelen);
        std::string val(reinterpret_cast<const char*>(value), valuelen);
        if (key == ":status") {
            state->response->status_text = val;
            try {
                state->response->status_code = std::stoi(val);
            } catch (const std::exception&) {
                state->response->status_code = 0;
            }
        } else {
            state->response->append_header(std::move(key), std::move(val));
        }
        return 0;
    }

    [[maybe_unused]] static int on_frame_recv_callback(nghttp2_session*, const nghttp2_frame* frame, void* user_data)
    {
        auto* state = static_cast<SessionState*>(user_data);
        if (!state->response)
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        if (frame->hd.stream_id != state->stream_id)
            return 0;
        if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
            state->response->headers_complete = true;
            if (state->response->verbose) {
                std::fprintf(stderr,
                             "< HTTP/2 %d %s\n",
                             state->response->status_code,
                             state->response->status_text.c_str());
                for (const auto& header : state->response->headers)
                    std::fprintf(stderr, "< %s: %s\n", header.name.c_str(), header.value.c_str());
                std::fprintf(stderr, "<\n");
            }
            state->response->emit_headers();
            if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
                state->response->message_complete = true;
                state->completed                  = true;
                state->response->flush_body();
            }
        } else if (frame->hd.type == NGHTTP2_DATA) {
            if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
                state->response->message_complete = true;
                state->completed                  = true;
                state->response->flush_body();
            }
        }
        return 0;
    }

    [[maybe_unused]] static int
    on_stream_close_callback(nghttp2_session*, int32_t stream_id, uint32_t error_code, void* user_data)
    {
        auto* state = static_cast<SessionState*>(user_data);
        if (stream_id == state->stream_id) {
            state->completed = true;
            if (error_code != NGHTTP2_NO_ERROR)
                state->failed = true;
            if (state->response)
                state->response->message_complete = true;
        }
        return 0;
    }

    [[maybe_unused]] static int on_data_chunk_recv_callback(nghttp2_session*,
                                                            uint8_t,
                                                            int32_t        stream_id,
                                                            const uint8_t* data,
                                                            size_t         len,
                                                            void*          user_data)
    {
        auto* state = static_cast<SessionState*>(user_data);
        if (!state->response || stream_id != state->stream_id)
            return 0;
        if (state->response->buffer_body) {
            state->response->body_buffer.append(reinterpret_cast<const char*>(data), len);
        } else if (state->response->output) {
            std::fwrite(data, 1, len, state->response->output);
        }
        return 0;
    }

    [[maybe_unused]] static ssize_t request_body_read_callback(nghttp2_session*,
                                                               int32_t,
                                                               uint8_t*             buf,
                                                               size_t               length,
                                                               uint32_t*            data_flags,
                                                               nghttp2_data_source* source,
                                                               void*                user_data)
    {
        auto* state = static_cast<SessionState*>(user_data);
        if (!state->has_request_body)
            return 0;
        auto*  body      = static_cast<RequestBody*>(source->ptr);
        size_t remaining = body->data->size() > body->offset ? body->data->size() - body->offset : 0;
        size_t to_copy   = std::min(length, remaining);
        if (to_copy > 0) {
            std::memcpy(buf, body->data->data() + body->offset, to_copy);
            body->offset += to_copy;
        }
        if (body->offset >= body->data->size())
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return static_cast<ssize_t>(to_copy);
    }

    template <typename Socket>
    Task<int, Work_Promise<SpinLock, int>> perform_request(Socket&             socket,
                                                           const ParsedUrl&    url,
                                                           const RequestState& state,
                                                           const BuiltRequest& built,
                                                           ResponseState&      response)
    {
        SessionState session;
        session.response = &response;
        if (!state.body.empty()) {
            session.request_body.data   = &state.body;
            session.request_body.offset = 0;
            session.has_request_body    = true;
        }

        nghttp2_session_callbacks* callbacks = nullptr;
        if (nghttp2_session_callbacks_new(&callbacks) != 0)
            co_return -1;
        nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
        nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);
        nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
        nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_recv_callback);

        nghttp2_session* h2 = nullptr;
        int              rv = nghttp2_session_client_new(&h2, callbacks, &session);
        nghttp2_session_callbacks_del(callbacks);
        if (rv != 0)
            co_return -1;

        rv = nghttp2_submit_settings(h2, NGHTTP2_FLAG_NONE, nullptr, 0);
        if (rv != 0) {
            nghttp2_session_del(h2);
            co_return -1;
        }

        rv = nghttp2_session_send(h2);
        if (rv != 0) {
            nghttp2_session_del(h2);
            co_return -1;
        }
        if (!session.send_buffer.empty()) {
            ssize_t sent = co_await socket.send_all(session.send_buffer.data(), session.send_buffer.size());
            if (sent <= 0) {
                nghttp2_session_del(h2);
                co_return -1;
            }
            session.send_buffer.clear();
        }

        std::vector<std::pair<std::string, std::string>> header_pairs;
        header_pairs.reserve(built.headers.size() + 4);
        header_pairs.emplace_back(":method", state.method);
        header_pairs.emplace_back(":path", url.path.empty() ? std::string("/") : url.path);
        header_pairs.emplace_back(":scheme", url.scheme);
        std::string authority = url.host;
        bool default_port     = (url.scheme == "https" && url.port == 443) || (url.scheme == "http" && url.port == 80);
        if (!default_port) {
            authority.push_back(':');
            authority.append(std::to_string(url.port));
        }
        header_pairs.emplace_back(":authority", std::move(authority));

        for (const auto& header : built.headers) {
            std::string lower = to_lower(header.name);
            if (lower == "host" || lower == "connection" || lower == "proxy-connection" || lower == "upgrade"
                || lower == "keep-alive" || lower == "transfer-encoding")
                continue;
            header_pairs.emplace_back(std::move(lower), header.value);
        }

        std::vector<nghttp2_nv> nva;
        nva.reserve(header_pairs.size());
        for (auto& kv : header_pairs) {
            nva.push_back(nghttp2_nv { reinterpret_cast<uint8_t*>(kv.first.data()),
                                       reinterpret_cast<uint8_t*>(kv.second.data()),
                                       kv.first.size(),
                                       kv.second.size(),
                                       NGHTTP2_NV_FLAG_NONE });
        }

        nghttp2_data_provider data_provider {};
        if (session.has_request_body) {
            data_provider.source.ptr    = &session.request_body;
            data_provider.read_callback = request_body_read_callback;
        }

        session.stream_id = nghttp2_submit_request(h2,
                                                   nullptr,
                                                   nva.data(),
                                                   nva.size(),
                                                   session.has_request_body ? &data_provider : nullptr,
                                                   nullptr);
        if (session.stream_id < 0) {
            nghttp2_session_del(h2);
            co_return -1;
        }

        rv = nghttp2_session_send(h2);
        if (rv != 0) {
            nghttp2_session_del(h2);
            co_return -1;
        }
        if (!session.send_buffer.empty()) {
            ssize_t sent = co_await socket.send_all(session.send_buffer.data(), session.send_buffer.size());
            if (sent <= 0) {
                nghttp2_session_del(h2);
                co_return -1;
            }
            session.send_buffer.clear();
        }

        std::array<uint8_t, 8192> recv_buffer {};
        while (!session.completed) {
            ssize_t n = co_await socket.recv(reinterpret_cast<char*>(recv_buffer.data()), recv_buffer.size());
            if (n <= 0) {
                session.failed = true;
                break;
            }
            ssize_t consumed = nghttp2_session_mem_recv(h2, recv_buffer.data(), static_cast<size_t>(n));
            if (consumed < 0) {
                session.failed = true;
                break;
            }
            rv = nghttp2_session_send(h2);
            if (rv != 0) {
                session.failed = true;
                break;
            }
            if (!session.send_buffer.empty()) {
                ssize_t sent = co_await socket.send_all(session.send_buffer.data(), session.send_buffer.size());
                if (sent <= 0) {
                    session.failed = true;
                    break;
                }
                session.send_buffer.clear();
            }
        }

        if (response.buffer_body)
            response.flush_body();

        nghttp2_session_del(h2);
        if (!session.completed || session.failed)
            co_return -1;
        co_return 0;
    }

} // namespace http2_client

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
Task<int, Work_Promise<SpinLock, int>>
perform_request_http1(Socket& socket, const BuiltRequest& built, ResponseContext& ctx)
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

        http2_client::ResponseState h2_response;
        h2_response.verbose         = options.verbose;
        h2_response.include_headers = options.include_headers;
        h2_response.buffer_body     = options.follow_redirects;
        h2_response.output          = output;

        int  rc         = 0;
        bool used_http2 = false;
        if (parsed->scheme == "https") {
#if defined(USING_SSL)
            auto tls_ctx = net::tls_context::make(net::tls_mode::Client);
            if (SSL_CTX* ctx_handle = tls_ctx.native_handle()) {
                SSL_CTX_set_default_verify_paths(ctx_handle);
                SSL_CTX_set_verify(ctx_handle, SSL_VERIFY_PEER, nullptr);
            }
            auto tls_socket = fdwq.make_tls_socket(std::move(tls_ctx), net::tls_mode::Client);
            apply_tls_sni(tls_socket, parsed->host);
#if defined(USING_SSL) && OPENSSL_VERSION_NUMBER >= 0x10100000L
            if (SSL* ssl_handle = tls_socket.ssl_handle())
                SSL_set1_host(ssl_handle, parsed->host.c_str());
#endif
            if (options.prefer_http2) {
                static const unsigned char alpn_protos[] = { 2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1' };
                SSL_set_alpn_protos(tls_socket.ssl_handle(), alpn_protos, sizeof(alpn_protos));
            }
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
            const auto endpoint = format_endpoint(reinterpret_cast<const sockaddr*>(&resolved.storage));
            verbose_printf(options.verbose, "*   Trying %s...\n", endpoint.c_str());
            int connect_rc = co_await tcp_base.connect(reinterpret_cast<const sockaddr*>(&resolved.storage),
                                                       resolved.length);
            if (connect_rc != 0) {
                CO_WQ_LOG_ERROR("[co_curl] connect failed: %s", std::strerror(errno));
                co_return 1;
            }
            verbose_printf(options.verbose, "* Connected to %s\n", endpoint.c_str());
            int handshake_rc = co_await tls_socket.handshake();
            if (handshake_rc != 0) {
                CO_WQ_LOG_ERROR("[co_curl] tls handshake failed: %d", handshake_rc);
                co_return 1;
            }
            log_tls_session_details(tls_socket.ssl_handle(), parsed->host, options.verbose);
            bool negotiated_http2 = false;
            if (options.prefer_http2) {
                const unsigned char* alpn     = nullptr;
                unsigned int         alpn_len = 0;
                SSL_get0_alpn_selected(tls_socket.ssl_handle(), &alpn, &alpn_len);
                negotiated_http2 = (alpn_len == 2 && alpn && std::memcmp(alpn, "h2", 2) == 0);
            }
            if (negotiated_http2) {
                rc         = co_await http2_client::perform_request(tls_socket, *parsed, state, built, h2_response);
                used_http2 = true;
            } else {
                if (options.prefer_http2)
                    CO_WQ_LOG_INFO("[co_curl] upstream did not negotiate HTTP/2; using HTTP/1.1");
                rc = co_await perform_request_http1(tls_socket, built, response_ctx);
            }
            tls_socket.close();
#else
            CO_WQ_LOG_ERROR("[co_curl] https requested but TLS support is disabled");
            co_return 1;
#endif
        } else {
            if (options.prefer_http2)
                CO_WQ_LOG_WARN("[co_curl] HTTP/2 preference ignored for plain HTTP targets");
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
            const auto endpoint = format_endpoint(reinterpret_cast<const sockaddr*>(&resolved.storage));
            verbose_printf(options.verbose, "*   Trying %s...\n", endpoint.c_str());
            int connect_rc = co_await tcp_socket.connect(reinterpret_cast<const sockaddr*>(&resolved.storage),
                                                         resolved.length);
            if (connect_rc != 0) {
                CO_WQ_LOG_ERROR("[co_curl] connect failed: %s", std::strerror(errno));
                co_return 1;
            }
            verbose_printf(options.verbose, "* Connected to %s\n", endpoint.c_str());
            rc = co_await perform_request_http1(tcp_socket, built, response_ctx);
            tcp_socket.close();
        }

        if (rc != 0) {
            break;
        }

        int status_code = used_http2 ? h2_response.status_code : response_ctx.status_code;
        final_status    = status_code;

        auto get_header = [&](std::string_view name) -> std::optional<std::string> {
            if (used_http2)
                return h2_response.header(name);
            return response_ctx.header(name);
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
            if (!used_http2 && response_ctx.buffer_body)
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
