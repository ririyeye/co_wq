#include "http_cli.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

#include <optional>
#include <string_view>

namespace co_wq::net::http::cli {

namespace {

    bool parse_header_line(std::string_view line, HeaderEntry& out)
    {
        const auto colon = line.find(':');
        if (colon == std::string_view::npos)
            return false;
        out.name = std::string(line.substr(0, colon));
        if (out.name.empty())
            return false;
        out.value = trim_leading_spaces(line.substr(colon + 1));
        return true;
    }

    std::optional<std::string> read_file_to_string(std::string_view path)
    {
        std::ifstream file(std::string(path), std::ios::binary);
        if (!file.is_open())
            return std::nullopt;
        std::string buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return buffer;
    }

} // namespace

BuiltRequest build_http_request(const CommandLineOptions& options, const RequestState& state, const UrlParts& url)
{
    BuiltRequest result;
    result.headers = options.headers;
    if (state.drop_content_headers)
        remove_content_headers(result.headers);

    const bool has_body  = !state.body.empty();
    const bool send_body = has_body && !state.drop_content_headers;

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

    if (send_body && !header_exists(result.headers, "Content-Length")
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
    if (send_body)
        result.payload.append(state.body);
    return result;
}

void log_request_verbose(const UrlParts& url, const RequestState& state, const BuiltRequest& built)
{
    std::fprintf(stderr, "> %s %s HTTP/1.1\n", state.method.c_str(), url.path.empty() ? "/" : url.path.c_str());
    for (const auto& header : built.headers) {
        std::fprintf(stderr, "> %s: %s\n", header.name.c_str(), header.value.c_str());
    }
    std::fprintf(stderr, ">\n");
    if (!state.body.empty())
        std::fprintf(stderr, "> [%zu bytes of body]\n", state.body.size());
}

ParseResult parse_common_arguments(int argc, char* argv[], CommandLineOptions& options)
{
    ParseResult result;

    auto require_value = [&](int idx, std::string_view option) -> std::optional<std::string> {
        if (idx + 1 >= argc) {
            result.error = "missing value for " + std::string(option);
            return std::nullopt;
        }
        return std::string(argv[idx + 1]);
    };

    for (int idx = 1; idx < argc; ++idx) {
        std::string_view arg(argv[idx]);
        if (arg == "--") {
            for (int j = idx + 1; j < argc; ++j)
                result.positionals.emplace_back(argv[j]);
            break;
        }
        if (arg == "-h" || arg == "--help") {
            result.show_help = true;
            break;
        }
        if (arg == "-v" || arg == "--verbose") {
            options.verbose = true;
            continue;
        }
        if (arg == "-i" || arg == "--include") {
            options.include_headers = true;
            continue;
        }
        if (arg == "-L" || arg == "--location") {
            options.follow_redirects = true;
            continue;
        }
        if (arg == "--max-redirs") {
            auto value = require_value(idx, "--max-redirs");
            if (!value)
                break;
            ++idx;
            try {
                int limit             = std::stoi(*value);
                options.max_redirects = std::max(0, limit);
            } catch (const std::exception&) {
                result.error = "invalid value for --max-redirs: " + *value;
            } catch (...) {
                result.error = "invalid value for --max-redirs";
            }
            if (!result.error.empty())
                break;
            continue;
        }
        if (arg == "--insecure") {
            options.insecure = true;
            continue;
        }
        if (arg == "--http2") {
            options.prefer_http2          = true;
            options.prefer_http2_explicit = true;
            continue;
        }
        if (arg == "--http1.1" || arg == "--no-http2") {
            options.prefer_http2          = false;
            options.prefer_http2_explicit = true;
            continue;
        }
        if (arg == "-X" || arg == "--method") {
            auto value = require_value(idx, arg);
            if (!value)
                break;
            ++idx;
            options.method = std::move(*value);
            to_upper_inplace(options.method);
            options.method_explicit = true;
            continue;
        }
        if (arg == "-H" || arg == "--header") {
            auto value = require_value(idx, arg);
            if (!value)
                break;
            ++idx;
            HeaderEntry entry;
            if (!parse_header_line(*value, entry)) {
                result.error = "invalid header format: " + *value;
                break;
            }
            options.headers.push_back(std::move(entry));
            continue;
        }
        if (arg == "-d" || arg == "--data") {
            auto value = require_value(idx, arg);
            if (!value)
                break;
            ++idx;
            if (!value->empty() && value->front() == '@') {
                std::string path      = value->substr(1);
                auto        file_data = read_file_to_string(path);
                if (!file_data) {
                    result.error = "failed to read data file: " + path;
                    break;
                }
                options.body = std::move(*file_data);
            } else {
                options.body = std::move(*value);
            }
            continue;
        }
        if (arg == "-o" || arg == "--output") {
            auto value = require_value(idx, arg);
            if (!value)
                break;
            ++idx;
            options.output_path = std::move(*value);
            continue;
        }
        if (!arg.empty() && arg.front() == '-') {
            result.error = "unknown option: " + std::string(arg);
            break;
        }
        result.positionals.emplace_back(arg);
    }

    if (result.error.empty()) {
        if (!options.method_explicit && !options.body.empty())
            options.method = "POST";
        to_upper_inplace(options.method);
        if (options.max_redirects < 0)
            options.max_redirects = 0;
        if (options.url.empty() && !result.positionals.empty())
            options.url = result.positionals.front();
    }

    result.ok = result.error.empty() && !result.show_help;
    return result;
}

} // namespace co_wq::net::http::cli
