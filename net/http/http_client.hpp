#pragma once

#include "http_common.hpp"

#include <llhttp.h>

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace co_wq::net::http {

struct HeaderEntry {
    std::string name;
    std::string value;
};

class ResponseContext {
public:
    ResponseContext();

    std::optional<std::string> header(std::string_view name) const;
    void                       reset();
    void                       flush_buffered_output();
    bool                       feed(std::string_view data, std::string* error_reason = nullptr);
    bool                       finish(std::string* error_reason = nullptr);

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
    static int on_status_cb(llhttp_t* parser, const char* at, size_t length);
    static int on_header_field_cb(llhttp_t* parser, const char* at, size_t length);
    static int on_header_value_cb(llhttp_t* parser, const char* at, size_t length);
    static int on_header_value_complete_cb(llhttp_t* parser);
    static int on_headers_complete_cb(llhttp_t* parser);
    static int on_body_cb(llhttp_t* parser, const char* at, size_t length);
    static int on_message_complete_cb(llhttp_t* parser);

    void store_header();

    llhttp_t          parser_ {};
    llhttp_settings_t settings_ {};
    std::string       current_field_;
    std::string       current_value_;
};

class SimpleResponse {
public:
    SimpleResponse();

    void reset();
    bool feed(std::string_view data, std::string* error_reason = nullptr);
    bool finish(std::string* error_reason = nullptr);

    int         status_code { 0 };
    bool        message_complete { false };
    Headers     headers;
    std::string body;

private:
    static int on_header_field_cb(llhttp_t* parser, const char* at, size_t length);
    static int on_header_value_cb(llhttp_t* parser, const char* at, size_t length);
    static int on_header_value_complete_cb(llhttp_t* parser);
    static int on_headers_complete_cb(llhttp_t* parser);
    static int on_body_cb(llhttp_t* parser, const char* at, size_t length);
    static int on_message_complete_cb(llhttp_t* parser);

    void store_header();

    llhttp_t          parser_ {};
    llhttp_settings_t settings_ {};
    std::string       current_field_;
    std::string       current_value_;
};

bool header_exists(const std::vector<HeaderEntry>& headers, std::string_view name);
void remove_content_headers(std::vector<HeaderEntry>& headers);

} // namespace co_wq::net::http
