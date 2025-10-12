#include "http_client.hpp"

#include "http_common.hpp"

#include <algorithm>

namespace co_wq::net::http {

ResponseContext::ResponseContext()
{
    llhttp_settings_init(&settings_);
    settings_.on_status                = &ResponseContext::on_status_cb;
    settings_.on_header_field          = &ResponseContext::on_header_field_cb;
    settings_.on_header_value          = &ResponseContext::on_header_value_cb;
    settings_.on_header_value_complete = &ResponseContext::on_header_value_complete_cb;
    settings_.on_headers_complete      = &ResponseContext::on_headers_complete_cb;
    settings_.on_body                  = &ResponseContext::on_body_cb;
    settings_.on_message_complete      = &ResponseContext::on_message_complete_cb;
    llhttp_init(&parser_, HTTP_RESPONSE, &settings_);
    parser_.data = this;
}

std::optional<std::string> ResponseContext::header(std::string_view name) const
{
    auto it = header_lookup.find(to_lower(name));
    if (it == header_lookup.end())
        return std::nullopt;
    return it->second;
}

void ResponseContext::reset()
{
    llhttp_reset(&parser_);
    status_text.clear();
    status_code = 0;
    http_major  = 1;
    http_minor  = 1;
    header_sequence.clear();
    header_lookup.clear();
    current_field_.clear();
    current_value_.clear();
    headers_complete = false;
    message_complete = false;
    header_block.clear();
    body_buffer.clear();
}

void ResponseContext::flush_buffered_output()
{
    if (!output)
        return;
    if (!header_block.empty() && include_headers)
        std::fwrite(header_block.data(), 1, header_block.size(), output);
    if (!body_buffer.empty())
        std::fwrite(body_buffer.data(), 1, body_buffer.size(), output);
    std::fflush(output);
}

bool ResponseContext::feed(std::string_view data, std::string* error_reason)
{
    auto err = llhttp_execute(&parser_, data.data(), data.size());
    if (err == HPE_OK)
        return true;
    if (error_reason) {
        const char* reason = llhttp_get_error_reason(&parser_);
        if (reason && *reason)
            *error_reason = reason;
        else
            *error_reason = llhttp_errno_name(err);
    }
    return false;
}

bool ResponseContext::finish(std::string* error_reason)
{
    auto err = llhttp_finish(&parser_);
    if (err == HPE_OK || err == HPE_PAUSED_UPGRADE)
        return true;
    if (error_reason) {
        const char* reason = llhttp_get_error_reason(&parser_);
        if (reason && *reason)
            *error_reason = reason;
        else
            *error_reason = llhttp_errno_name(err);
    }
    return false;
}

int ResponseContext::on_status_cb(llhttp_t* parser, const char* at, size_t length)
{
    auto* ctx = static_cast<ResponseContext*>(parser->data);
    ctx->status_text.append(at, length);
    return 0;
}

int ResponseContext::on_header_field_cb(llhttp_t* parser, const char* at, size_t length)
{
    auto* ctx = static_cast<ResponseContext*>(parser->data);
    if (!ctx->current_value_.empty()) {
        ctx->store_header();
    }
    ctx->current_field_.append(at, length);
    return 0;
}

int ResponseContext::on_header_value_cb(llhttp_t* parser, const char* at, size_t length)
{
    auto* ctx = static_cast<ResponseContext*>(parser->data);
    ctx->current_value_.append(at, length);
    return 0;
}

int ResponseContext::on_header_value_complete_cb(llhttp_t* parser)
{
    auto* ctx = static_cast<ResponseContext*>(parser->data);
    ctx->store_header();
    return 0;
}

int ResponseContext::on_headers_complete_cb(llhttp_t* parser)
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

int ResponseContext::on_body_cb(llhttp_t* parser, const char* at, size_t length)
{
    auto* ctx = static_cast<ResponseContext*>(parser->data);
    if (ctx->buffer_body) {
        ctx->body_buffer.append(at, length);
    } else if (ctx->output) {
        std::fwrite(at, 1, length, ctx->output);
    }
    return 0;
}

int ResponseContext::on_message_complete_cb(llhttp_t* parser)
{
    auto* ctx             = static_cast<ResponseContext*>(parser->data);
    ctx->message_complete = true;
    return 0;
}

void ResponseContext::store_header()
{
    if (current_field_.empty())
        return;
    HeaderEntry entry;
    entry.name  = std::move(current_field_);
    entry.value = trim_leading_spaces(current_value_);
    header_sequence.push_back(entry);
    header_lookup[to_lower(header_sequence.back().name)] = header_sequence.back().value;
    current_field_.clear();
    current_value_.clear();
}

SimpleResponse::SimpleResponse()
{
    llhttp_settings_init(&settings_);
    settings_.on_header_field          = &SimpleResponse::on_header_field_cb;
    settings_.on_header_value          = &SimpleResponse::on_header_value_cb;
    settings_.on_header_value_complete = &SimpleResponse::on_header_value_complete_cb;
    settings_.on_headers_complete      = &SimpleResponse::on_headers_complete_cb;
    settings_.on_body                  = &SimpleResponse::on_body_cb;
    settings_.on_message_complete      = &SimpleResponse::on_message_complete_cb;
    llhttp_init(&parser_, HTTP_RESPONSE, &settings_);
    parser_.data = this;
}

void SimpleResponse::reset()
{
    llhttp_reset(&parser_);
    current_field_.clear();
    current_value_.clear();
    headers.clear();
    body.clear();
    status_code      = 0;
    message_complete = false;
}

bool SimpleResponse::feed(std::string_view data, std::string* error_reason)
{
    auto err = llhttp_execute(&parser_, data.data(), data.size());
    if (err == HPE_OK)
        return true;
    if (error_reason) {
        const char* reason = llhttp_get_error_reason(&parser_);
        if (reason && *reason)
            *error_reason = reason;
        else
            *error_reason = llhttp_errno_name(err);
    }
    return false;
}

bool SimpleResponse::finish(std::string* error_reason)
{
    auto err = llhttp_finish(&parser_);
    if (err == HPE_OK || err == HPE_PAUSED_UPGRADE)
        return true;
    if (error_reason) {
        const char* reason = llhttp_get_error_reason(&parser_);
        if (reason && *reason)
            *error_reason = reason;
        else
            *error_reason = llhttp_errno_name(err);
    }
    return false;
}

int SimpleResponse::on_header_field_cb(llhttp_t* parser, const char* at, size_t length)
{
    auto* self = static_cast<SimpleResponse*>(parser->data);
    if (!self->current_value_.empty())
        self->store_header();
    self->current_field_.append(at, length);
    return 0;
}

int SimpleResponse::on_header_value_cb(llhttp_t* parser, const char* at, size_t length)
{
    auto* self = static_cast<SimpleResponse*>(parser->data);
    self->current_value_.append(at, length);
    return 0;
}

int SimpleResponse::on_header_value_complete_cb(llhttp_t* parser)
{
    auto* self = static_cast<SimpleResponse*>(parser->data);
    self->store_header();
    return 0;
}

int SimpleResponse::on_headers_complete_cb(llhttp_t* parser)
{
    auto* self        = static_cast<SimpleResponse*>(parser->data);
    self->status_code = parser->status_code;
    return 0;
}

int SimpleResponse::on_body_cb(llhttp_t* parser, const char* at, size_t length)
{
    auto* self = static_cast<SimpleResponse*>(parser->data);
    self->body.append(at, length);
    return 0;
}

int SimpleResponse::on_message_complete_cb(llhttp_t* parser)
{
    auto* self             = static_cast<SimpleResponse*>(parser->data);
    self->message_complete = true;
    return 0;
}

void SimpleResponse::store_header()
{
    if (current_field_.empty())
        return;
    headers[to_lower(current_field_)] = current_value_;
    current_field_.clear();
    current_value_.clear();
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
                                     return iequals(h.name, "content-type") || iequals(h.name, "content-length")
                                         || iequals(h.name, "transfer-encoding");
                                 }),
                  headers.end());
}

} // namespace co_wq::net::http
