#include "http_client.hpp"

#include "header_utils.hpp"
#include "http_common.hpp"

namespace co_wq::net::http {

Http1ResponseParser::Http1ResponseParser()
{
    llhttp_settings_init(&settings_);
    settings_.on_status                = &Http1ResponseParser::on_status_cb;
    settings_.on_header_field          = &Http1ResponseParser::on_header_field_cb;
    settings_.on_header_value          = &Http1ResponseParser::on_header_value_cb;
    settings_.on_header_value_complete = &Http1ResponseParser::on_header_value_complete_cb;
    settings_.on_headers_complete      = &Http1ResponseParser::on_headers_complete_cb;
    settings_.on_body                  = &Http1ResponseParser::on_body_cb;
    settings_.on_message_complete      = &Http1ResponseParser::on_message_complete_cb;
    llhttp_init(&parser_, HTTP_RESPONSE, &settings_);
    parser_.data = this;
}

std::optional<std::string> Http1ResponseParser::header(std::string_view name) const
{
    auto it = header_lookup.find(to_lower(name));
    if (it == header_lookup.end())
        return std::nullopt;
    return it->second;
}

void Http1ResponseParser::reset()
{
    llhttp_reset(&parser_);
    status_text.clear();
    status_code = 0;
    http_major  = 1;
    http_minor  = 1;
    header_sequence.clear();
    header_lookup.clear();
    reset_header_state();
    headers_complete = false;
    message_complete = false;
    has_error_       = false;
    last_error_.clear();
    header_block.clear();
    body_buffer.clear();
    response_.reset();
    response_.status_code = 0;
    response_.reason.clear();
}

void Http1ResponseParser::flush_buffered_output()
{
    if (!output)
        return;
    if (!header_block.empty() && include_headers)
        std::fwrite(header_block.data(), 1, header_block.size(), output);
    if (!body_buffer.empty())
        std::fwrite(body_buffer.data(), 1, body_buffer.size(), output);
    std::fflush(output);
}

bool Http1ResponseParser::feed(std::string_view data, std::string* error_reason)
{
    auto err = llhttp_execute(&parser_, data.data(), data.size());
    if (err == HPE_OK)
        return true;
    const char* reason = llhttp_get_error_reason(&parser_);
    if (reason && *reason)
        last_error_ = reason;
    else
        last_error_ = llhttp_errno_name(err);
    if (error_reason)
        *error_reason = last_error_;
    has_error_ = true;
    return false;
}

bool Http1ResponseParser::finish(std::string* error_reason)
{
    auto err = llhttp_finish(&parser_);
    if (err == HPE_OK || err == HPE_PAUSED_UPGRADE)
        return true;
    const char* reason = llhttp_get_error_reason(&parser_);
    if (reason && *reason)
        last_error_ = reason;
    else
        last_error_ = llhttp_errno_name(err);
    if (error_reason)
        *error_reason = last_error_;
    has_error_ = true;
    return false;
}

int Http1ResponseParser::on_status_cb(llhttp_t* parser, const char* at, size_t length)
{
    auto* ctx = static_cast<Http1ResponseParser*>(parser->data);
    ctx->status_text.append(at, length);
    return 0;
}

int Http1ResponseParser::on_header_field_cb(llhttp_t* parser, const char* at, size_t length)
{
    auto* ctx = static_cast<Http1ResponseParser*>(parser->data);
    ctx->collect_header_field(at, length);
    return 0;
}

int Http1ResponseParser::on_header_value_cb(llhttp_t* parser, const char* at, size_t length)
{
    auto* ctx = static_cast<Http1ResponseParser*>(parser->data);
    ctx->collect_header_value(at, length);
    return 0;
}

int Http1ResponseParser::on_header_value_complete_cb(llhttp_t* parser)
{
    auto* ctx = static_cast<Http1ResponseParser*>(parser->data);
    ctx->finalize_header_value();
    return 0;
}

int Http1ResponseParser::on_headers_complete_cb(llhttp_t* parser)
{
    auto* ctx             = static_cast<Http1ResponseParser*>(parser->data);
    ctx->status_code      = parser->status_code;
    ctx->http_major       = parser->http_major;
    ctx->http_minor       = parser->http_minor;
    ctx->headers_complete = true;
    ctx->response_.set_http_version(parser->http_major, parser->http_minor);
    ctx->response_.set_status(ctx->status_code, ctx->status_text);

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

int Http1ResponseParser::on_body_cb(llhttp_t* parser, const char* at, size_t length)
{
    auto* ctx = static_cast<Http1ResponseParser*>(parser->data);
    if (ctx->buffer_body) {
        ctx->body_buffer.append(at, length);
        ctx->response_.append_body(std::string_view(at, length));
    } else if (ctx->output) {
        std::fwrite(at, 1, length, ctx->output);
    }
    return 0;
}

int Http1ResponseParser::on_message_complete_cb(llhttp_t* parser)
{
    auto* ctx                       = static_cast<Http1ResponseParser*>(parser->data);
    ctx->message_complete           = true;
    ctx->response_.close_connection = (llhttp_should_keep_alive(parser) == 0);
    return 0;
}

void Http1ResponseParser::handle_header(std::string&& name, std::string&& value)
{
    if (name.empty())
        return;
    HeaderEntry entry;
    entry.name             = std::move(name);
    entry.value            = trim_leading_spaces(value);
    std::string lower_name = to_lower(entry.name);
    header_sequence.push_back(entry);
    header_lookup[lower_name] = header_sequence.back().value;
    response_.set_header(lower_name, header_sequence.back().value);
}

} // namespace co_wq::net::http
