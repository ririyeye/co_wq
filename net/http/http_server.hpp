#pragma once

#include "http_common.hpp"

#include <llhttp.h>

#include <string>
#include <string_view>

namespace co_wq::net::http {

struct HttpRequest {
    std::string method;
    std::string target;
    Headers     headers;
    std::string body;
};

class Http1RequestParser {
public:
    Http1RequestParser();

    void reset();
    bool feed(std::string_view data, std::string* error_reason = nullptr);
    bool finish(std::string* error_reason = nullptr);

    bool               headers_complete() const { return headers_complete_; }
    bool               message_complete() const { return message_complete_; }
    const HttpRequest& request() const { return request_; }
    bool               has_error() const { return has_error_; }
    const std::string& last_error() const { return error_reason_; }

private:
    static int on_message_begin(llhttp_t* parser);
    static int on_method(llhttp_t* parser, const char* at, size_t length);
    static int on_url(llhttp_t* parser, const char* at, size_t length);
    static int on_header_field(llhttp_t* parser, const char* at, size_t length);
    static int on_header_value(llhttp_t* parser, const char* at, size_t length);
    static int on_header_value_complete(llhttp_t* parser);
    static int on_headers_complete(llhttp_t* parser);
    static int on_body(llhttp_t* parser, const char* at, size_t length);
    static int on_message_complete(llhttp_t* parser);

    void store_header();
    void clear_state();

    llhttp_t          parser_ {};
    llhttp_settings_t settings_ {};
    HttpRequest       request_ {};
    std::string       current_field_;
    std::string       current_value_;
    bool              headers_complete_ { false };
    bool              message_complete_ { false };
    bool              has_error_ { false };
    std::string       error_reason_;
};

struct HttpResponse {
    int         status_code { 200 };
    std::string reason { "OK" };
    std::string body;
    Headers     headers;
};

std::string build_http1_response(const HttpResponse& response, bool close_connection = true);

} // namespace co_wq::net::http
