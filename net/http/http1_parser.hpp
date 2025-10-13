#pragma once

#include "http_client.hpp"
#include "http_server.hpp"

#include <string>
#include <string_view>

namespace co_wq::net::http {

class Http1Parser : public IHttpParser {
public:
    explicit Http1Parser(MessageRole role);

    static Http1Parser make_request();
    static Http1Parser make_response();

    ParserProtocol protocol() const override { return ParserProtocol::Http1; }
    MessageRole    role() const override { return role_; }

    void reset() override;
    bool feed(std::string_view data, std::string* error = nullptr) override;
    bool finish(std::string* error = nullptr) override;

    bool is_headers_complete() const override;
    bool is_message_complete() const override;

    bool               has_error() const override;
    const std::string& last_error() const override;

    const HttpRequest&  request() const;
    HttpRequest&        mutable_request();
    const HttpResponse& response() const;
    HttpResponse&       mutable_response();

    Http1RequestParser&        request_parser();
    const Http1RequestParser&  request_parser() const;
    Http1ResponseParser&       response_parser();
    const Http1ResponseParser& response_parser() const;

private:
    MessageRole         role_;
    Http1RequestParser  request_parser_;
    Http1ResponseParser response_parser_;
};

} // namespace co_wq::net::http
