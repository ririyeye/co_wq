#pragma once

#include "http_common.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace co_wq::net::http {

class HttpMessage {
public:
    HttpMessage();
    virtual ~HttpMessage() = default;

    void reset();

    int  http_major() const { return http_major_; }
    int  http_minor() const { return http_minor_; }
    void set_http_version(int major, int minor);

    void                       clear_headers();
    void                       set_header(std::string name, std::string value);
    bool                       has_header(std::string_view name) const;
    std::optional<std::string> header(std::string_view name) const;
    void                       remove_header(std::string_view name);

    void set_body(std::string body_value);
    void append_body(std::string_view chunk);

    Headers     headers;
    std::string body;

protected:
    int http_major_ { 1 };
    int http_minor_ { 1 };
};

class HttpRequest : public HttpMessage {
public:
    HttpRequest();

    void reset();

    std::string method;
    std::string target;

    std::string scheme;
    std::string host;
    std::string path;
    std::string query;
    int         port { 0 };
};

class HttpResponse : public HttpMessage {
public:
    HttpResponse();

    void reset();
    void set_status(int code, std::string reason_text = {});

    int         status_code { 200 };
    std::string reason { "OK" };
    bool        close_connection { false };
};

} // namespace co_wq::net::http
