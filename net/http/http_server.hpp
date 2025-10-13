#pragma once

#include "http1_common.hpp"
#include "http_message.hpp"
#include "parser.hpp"

#include <llhttp.h>

#include <string>
#include <string_view>

namespace co_wq::net::http {

class Http1RequestParser : public IHttpRequestParser, protected Http1HeaderCollector {
public:
    Http1RequestParser();

    ParserProtocol protocol() const override { return ParserProtocol::Http1; }

    void reset() override;
    bool feed(std::string_view data, std::string* error_reason = nullptr) override;
    bool finish(std::string* error_reason = nullptr) override;

    bool               is_headers_complete() const override { return headers_complete_; }
    bool               is_message_complete() const override { return message_complete_; }
    const HttpRequest& request() const override { return request_; }
    bool               has_error() const override { return has_error_; }
    const std::string& last_error() const override { return error_reason_; }

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

    void handle_header(std::string&& name, std::string&& value) override;
    void clear_state();

    llhttp_t          parser_ {};
    llhttp_settings_t settings_ {};
    HttpRequest       request_ {};
    bool              headers_complete_ { false };
    bool              message_complete_ { false };
    bool              has_error_ { false };
    std::string       error_reason_;
};

std::string build_http1_response(const HttpResponse& response, bool close_connection = true);

} // namespace co_wq::net::http
