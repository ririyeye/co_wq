#include "http_message.hpp"

#include <utility>

namespace co_wq::net::http {

HttpMessage::HttpMessage() = default;

void HttpMessage::reset()
{
    http_major_ = 1;
    http_minor_ = 1;
    clear_headers();
    body.clear();
}

void HttpMessage::set_http_version(int major, int minor)
{
    http_major_ = major;
    http_minor_ = minor;
}

void HttpMessage::clear_headers()
{
    headers.clear();
}

void HttpMessage::set_header(std::string name, std::string value)
{
    remove_header(name);
    headers.emplace(std::move(name), std::move(value));
}

bool HttpMessage::has_header(std::string_view name) const
{
    for (const auto& kv : headers) {
        if (iequals(kv.first, name))
            return true;
    }
    return false;
}

std::optional<std::string> HttpMessage::header(std::string_view name) const
{
    for (const auto& kv : headers) {
        if (iequals(kv.first, name))
            return kv.second;
    }
    return std::nullopt;
}

void HttpMessage::remove_header(std::string_view name)
{
    for (auto it = headers.begin(); it != headers.end();) {
        if (iequals(it->first, name)) {
            it = headers.erase(it);
        } else {
            ++it;
        }
    }
}

void HttpMessage::set_body(std::string body_value)
{
    body = std::move(body_value);
}

void HttpMessage::append_body(std::string_view chunk)
{
    body.append(chunk.data(), chunk.size());
}

HttpRequest::HttpRequest() = default;

void HttpRequest::reset()
{
    HttpMessage::reset();
    method.clear();
    target.clear();
    scheme.clear();
    host.clear();
    path.clear();
    query.clear();
    port = 0;
}

HttpResponse::HttpResponse() = default;

void HttpResponse::reset()
{
    HttpMessage::reset();
    status_code      = 200;
    reason           = "OK";
    close_connection = false;
}

void HttpResponse::set_status(int code, std::string reason_text)
{
    status_code = code;
    if (!reason_text.empty()) {
        reason = std::move(reason_text);
    }
}

} // namespace co_wq::net::http
