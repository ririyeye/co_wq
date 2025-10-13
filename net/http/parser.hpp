#pragma once

#include "http_message.hpp"

#include <string>
#include <string_view>

namespace co_wq::net::http {

enum class ParserProtocol {
    Http1,
    Http2,
};

enum class MessageRole {
    Request,
    Response,
};

class IHttpParser {
public:
    virtual ~IHttpParser() = default;

    virtual ParserProtocol protocol() const = 0;
    virtual MessageRole    role() const     = 0;

    virtual void reset()                                                   = 0;
    virtual bool feed(std::string_view data, std::string* error = nullptr) = 0;
    virtual bool finish(std::string* error = nullptr)                      = 0;

    virtual bool is_headers_complete() const = 0;
    virtual bool is_message_complete() const = 0;

    virtual bool               has_error() const  = 0;
    virtual const std::string& last_error() const = 0;
};

class IHttpRequestParser : public IHttpParser {
public:
    MessageRole                role() const final { return MessageRole::Request; }
    virtual const HttpRequest& request() const = 0;
};

class IHttpResponseParser : public IHttpParser {
public:
    MessageRole                 role() const final { return MessageRole::Response; }
    virtual const HttpResponse& response() const = 0;
};

} // namespace co_wq::net::http
