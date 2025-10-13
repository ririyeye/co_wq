#pragma once

#include "http_client.hpp"
#include "http_common.hpp"

#include <optional>
#include <string>
#include <vector>

namespace co_wq::net::http::cli {

struct RequestState {
    std::string method;
    std::string body;
    bool        drop_content_headers { false };
};

struct CommandLineOptions {
    std::string                url;
    std::string                method { "GET" };
    bool                       method_explicit { false };
    std::vector<HeaderEntry>   headers;
    std::string                body;
    bool                       verbose { false };
    bool                       include_headers { false };
    bool                       follow_redirects { false };
    int                        max_redirects { 10 };
    bool                       insecure { false };
    bool                       prefer_http2 { false };
    bool                       prefer_http2_explicit { false };
    std::optional<std::string> output_path;
};

struct BuiltRequest {
    std::string              payload;
    std::vector<HeaderEntry> headers;
};

BuiltRequest build_http_request(const CommandLineOptions& options, const RequestState& state, const UrlParts& url);
void         log_request_verbose(const UrlParts& url, const RequestState& state, const BuiltRequest& built);

struct ParseResult {
    bool                     ok { false };
    bool                     show_help { false };
    std::string              error;
    std::vector<std::string> positionals;
};

ParseResult parse_common_arguments(int argc, char* argv[], CommandLineOptions& options);

} // namespace co_wq::net::http::cli
