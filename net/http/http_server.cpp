#include "http_server.hpp"

#include <llhttp.h>

namespace co_wq::net::http {

Http1RequestParser::Http1RequestParser()
{
    llhttp_settings_init(&settings_);
    settings_.on_message_begin         = &Http1RequestParser::on_message_begin;
    settings_.on_method                = &Http1RequestParser::on_method;
    settings_.on_url                   = &Http1RequestParser::on_url;
    settings_.on_header_field          = &Http1RequestParser::on_header_field;
    settings_.on_header_value          = &Http1RequestParser::on_header_value;
    settings_.on_header_value_complete = &Http1RequestParser::on_header_value_complete;
    settings_.on_headers_complete      = &Http1RequestParser::on_headers_complete;
    settings_.on_body                  = &Http1RequestParser::on_body;
    settings_.on_message_complete      = &Http1RequestParser::on_message_complete;
    llhttp_init(&parser_, HTTP_REQUEST, &settings_);
    parser_.data = this;
    clear_state();
}

void Http1RequestParser::reset()
{
    llhttp_reset(&parser_);
    clear_state();
    has_error_ = false;
    error_reason_.clear();
}

void Http1RequestParser::clear_state()
{
    request_.method.clear();
    request_.target.clear();
    request_.headers.clear();
    request_.body.clear();
    current_field_.clear();
    current_value_.clear();
    headers_complete_ = false;
    message_complete_ = false;
}

bool Http1RequestParser::feed(std::string_view data, std::string* error_reason)
{
    if (has_error_)
        return false;

    auto err = llhttp_execute(&parser_, data.data(), data.size());
    if (err != HPE_OK) {
        has_error_    = true;
        error_reason_ = llhttp_get_error_reason(&parser_);
        if (error_reason_.empty()) {
            error_reason_ = llhttp_errno_name(err);
        }
        if (error_reason)
            *error_reason = error_reason_;
        return false;
    }
    return true;
}

bool Http1RequestParser::finish(std::string* error_reason)
{
    if (has_error_)
        return false;

    auto err = llhttp_finish(&parser_);
    if (err != HPE_OK) {
        has_error_    = true;
        error_reason_ = llhttp_get_error_reason(&parser_);
        if (error_reason_.empty())
            error_reason_ = llhttp_errno_name(err);
        if (error_reason)
            *error_reason = error_reason_;
        return false;
    }
    return true;
}

int Http1RequestParser::on_message_begin(llhttp_t* parser)
{
    auto* self = static_cast<Http1RequestParser*>(parser->data);
    self->clear_state();
    return 0;
}

int Http1RequestParser::on_method(llhttp_t* parser, const char* at, size_t length)
{
    auto* self = static_cast<Http1RequestParser*>(parser->data);
    self->request_.method.append(at, length);
    return 0;
}

int Http1RequestParser::on_url(llhttp_t* parser, const char* at, size_t length)
{
    auto* self = static_cast<Http1RequestParser*>(parser->data);
    self->request_.target.append(at, length);
    return 0;
}

int Http1RequestParser::on_header_field(llhttp_t* parser, const char* at, size_t length)
{
    auto* self = static_cast<Http1RequestParser*>(parser->data);
    if (!self->current_value_.empty())
        self->store_header();
    self->current_field_.append(at, length);
    return 0;
}

int Http1RequestParser::on_header_value(llhttp_t* parser, const char* at, size_t length)
{
    auto* self = static_cast<Http1RequestParser*>(parser->data);
    self->current_value_.append(at, length);
    return 0;
}

int Http1RequestParser::on_header_value_complete(llhttp_t* parser)
{
    auto* self = static_cast<Http1RequestParser*>(parser->data);
    self->store_header();
    return 0;
}

int Http1RequestParser::on_headers_complete(llhttp_t* parser)
{
    auto* self              = static_cast<Http1RequestParser*>(parser->data);
    self->headers_complete_ = true;
    return 0;
}

int Http1RequestParser::on_body(llhttp_t* parser, const char* at, size_t length)
{
    auto* self = static_cast<Http1RequestParser*>(parser->data);
    self->request_.body.append(at, length);
    return 0;
}

int Http1RequestParser::on_message_complete(llhttp_t* parser)
{
    auto* self              = static_cast<Http1RequestParser*>(parser->data);
    self->message_complete_ = true;
    return 0;
}

void Http1RequestParser::store_header()
{
    if (current_field_.empty())
        return;
    request_.headers[to_lower(current_field_)] = current_value_;
    current_field_.clear();
    current_value_.clear();
}

std::string build_http1_response(const HttpResponse& response, bool close_connection)
{
    std::string out;
    out.reserve(128 + response.body.size() + response.headers.size() * 32);
    out.append("HTTP/1.1 ");
    out.append(std::to_string(response.status_code));
    out.push_back(' ');
    if (!response.reason.empty())
        out.append(response.reason);
    out.append("\r\n");

    bool has_content_type   = false;
    bool has_content_length = false;
    bool has_connection     = false;

    for (const auto& kv : response.headers) {
        if (iequals(kv.first, "content-type"))
            has_content_type = true;
        if (iequals(kv.first, "content-length"))
            has_content_length = true;
        if (iequals(kv.first, "connection"))
            has_connection = true;
        out.append(kv.first);
        out.append(": ");
        out.append(kv.second);
        out.append("\r\n");
    }

    if (!has_content_type)
        out.append("Content-Type: text/plain; charset=utf-8\r\n");

    if (!has_content_length) {
        out.append("Content-Length: ");
        out.append(std::to_string(response.body.size()));
        out.append("\r\n");
    }

    if (close_connection && !has_connection)
        out.append("Connection: close\r\n");

    out.append("\r\n");
    out.append(response.body);
    return out;
}

} // namespace co_wq::net::http
