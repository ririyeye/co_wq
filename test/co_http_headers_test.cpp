#include "http/http_cli.hpp"
#include "http/http_common.hpp"
#include "http/http_message.hpp"

#include <cstdio>

using namespace co_wq::net::http;

namespace {

int check(bool condition, const char* message)
{
    if (condition)
        return 0;
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return 1;
}

} // namespace

int main()
{
    int failures = 0;

    // Case-insensitive accessors on HttpMessage
    HttpResponse response;
    response.set_status(200, "OK");
    response.set_header("Content-Type", "text/plain");
    response.set_header("X-CuStOm", "value");
    failures += check(response.has_header("content-type"), "Content-Type should exist regardless of case");
    failures += check(response.header("CONTENT-TYPE").value() == "text/plain", "header() must ignore case for lookup");
    response.remove_header("x-custom");
    failures += check(!response.has_header("X-CUSTOM"), "remove_header must be case insensitive");

    // Default header injection via build_http_request
    cli::CommandLineOptions options;
    options.url  = "https://example.com/path";
    options.body = "payload";
    cli::RequestState state { "POST", options.body, false };
    auto              url = parse_url(options.url);
    failures += check(url.has_value(), "parse_url should succeed for https");
    auto built = cli::build_http_request(options, state, *url);
    failures += check(header_exists(built.headers, "Host"), "Host header should be auto-populated");
    failures += check(header_exists(built.headers, "User-Agent"), "User-Agent default should be injected");
    failures += check(header_exists(built.headers, "Content-Length"), "Content-Length expected for body requests");
    failures += check(!header_exists(built.headers, "Transfer-Encoding"),
                      "Transfer-Encoding should not be injected automatically");

    // Respect existing hop-by-hop headers
    cli::CommandLineOptions chunked_options;
    chunked_options.url = "http://example.org/data";
    chunked_options.headers.push_back({ "Transfer-Encoding", "chunked" });
    cli::RequestState chunked_state { "POST", "ignored", false };
    auto              chunked_url = parse_url(chunked_options.url);
    failures += check(chunked_url.has_value(), "parse_url should succeed for http");
    auto chunked = cli::build_http_request(chunked_options, chunked_state, *chunked_url);
    failures += check(!header_exists(chunked.headers, "Content-Length"),
                      "Content-Length must not be added when Transfer-Encoding is preset");

    // drop_content_headers should strip entity headers
    cli::CommandLineOptions drop_options;
    drop_options.url = "http://example.net/upload";
    cli::RequestState drop_state { "POST", "body", true };
    auto              drop_url = parse_url(drop_options.url);
    failures += check(drop_url.has_value(), "parse_url should succeed for drop test");
    auto dropped = cli::build_http_request(drop_options, drop_state, *drop_url);
    failures += check(!header_exists(dropped.headers, "Content-Length"),
                      "drop_content_headers should remove Content-Length");
    failures += check(!header_exists(dropped.headers, "Content-Type"),
                      "drop_content_headers should remove Content-Type");

    if (failures == 0)
        std::fprintf(stdout, "http_headers_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
