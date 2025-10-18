#pragma once

#include "header_utils.hpp"
#include "http1_common.hpp"
#include "http_message.hpp"
#include "parser.hpp"

#include <llhttp.h>

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace co_wq::net::http {

class Http1ResponseParser : public IHttpResponseParser, protected Http1HeaderCollector {
public:
    Http1ResponseParser();

    std::optional<std::string> header(std::string_view name) const;
    void                       reset() override;
    void                       flush_buffered_output();
    ParserProtocol             protocol() const override { return ParserProtocol::Http1; }
    bool                       feed(std::string_view data, std::string* error_reason = nullptr) override;
    bool                       finish(std::string* error_reason = nullptr) override;
    bool                       is_headers_complete() const override { return headers_complete; }
    bool                       is_message_complete() const override { return message_complete; }
    bool                       has_error() const override { return has_error_; }
    const std::string&         last_error() const override { return last_error_; }

    const HttpResponse& response() const override { return response_; }
    const HttpResponse& message() const { return response_; }

    bool                                         headers_complete { false };
    bool                                         message_complete { false };
    bool                                         verbose { false };
    bool                                         include_headers { false };
    bool                                         buffer_body { false };
    std::string                                  status_text;
    int                                          status_code { 0 };
    int                                          http_major { 1 };
    int                                          http_minor { 1 };
    std::vector<HeaderEntry>                     header_sequence;
    std::unordered_map<std::string, std::string> header_lookup;
    std::string                                  header_block;
    std::string                                  body_buffer;
    std::FILE*                                   output { nullptr };

private:
    HttpResponse response_ {};
    bool         has_error_ { false };
    std::string  last_error_;

private:
    static int on_status_cb(llhttp_t* parser, const char* at, size_t length);
    static int on_header_field_cb(llhttp_t* parser, const char* at, size_t length);
    static int on_header_value_cb(llhttp_t* parser, const char* at, size_t length);
    static int on_header_value_complete_cb(llhttp_t* parser);
    static int on_headers_complete_cb(llhttp_t* parser);
    static int on_body_cb(llhttp_t* parser, const char* at, size_t length);
    static int on_message_complete_cb(llhttp_t* parser);

    void handle_header(std::string&& name, std::string&& value) override;

    llhttp_t          parser_ {};
    llhttp_settings_t settings_ {};
};

using ResponseContext = Http1ResponseParser;

} // namespace co_wq::net::http
