#include "http1_parser.hpp"

#include <stdexcept>

namespace co_wq::net::http {

Http1Parser::Http1Parser(MessageRole role) : role_(role) { }

Http1Parser Http1Parser::make_request()
{
    return Http1Parser(MessageRole::Request);
}

Http1Parser Http1Parser::make_response()
{
    return Http1Parser(MessageRole::Response);
}

void Http1Parser::reset()
{
    if (role_ == MessageRole::Request) {
        request_parser_.reset();
    } else {
        response_parser_.reset();
    }
}

bool Http1Parser::feed(std::string_view data, std::string* error)
{
    if (role_ == MessageRole::Request)
        return request_parser_.feed(data, error);
    return response_parser_.feed(data, error);
}

bool Http1Parser::finish(std::string* error)
{
    if (role_ == MessageRole::Request)
        return request_parser_.finish(error);
    return response_parser_.finish(error);
}

bool Http1Parser::is_headers_complete() const
{
    if (role_ == MessageRole::Request)
        return request_parser_.is_headers_complete();
    return response_parser_.is_headers_complete();
}

bool Http1Parser::is_message_complete() const
{
    if (role_ == MessageRole::Request)
        return request_parser_.is_message_complete();
    return response_parser_.is_message_complete();
}

bool Http1Parser::has_error() const
{
    if (role_ == MessageRole::Request)
        return request_parser_.has_error();
    return response_parser_.has_error();
}

const std::string& Http1Parser::last_error() const
{
    if (role_ == MessageRole::Request)
        return request_parser_.last_error();
    return response_parser_.last_error();
}

const HttpRequest& Http1Parser::request() const
{
    if (role_ != MessageRole::Request)
        throw std::logic_error("Http1Parser: not a request parser");
    return request_parser_.request();
}

HttpRequest& Http1Parser::mutable_request()
{
    if (role_ != MessageRole::Request)
        throw std::logic_error("Http1Parser: not a request parser");
    return const_cast<HttpRequest&>(request_parser_.request());
}

const HttpResponse& Http1Parser::response() const
{
    if (role_ != MessageRole::Response)
        throw std::logic_error("Http1Parser: not a response parser");
    return response_parser_.response();
}

HttpResponse& Http1Parser::mutable_response()
{
    if (role_ != MessageRole::Response)
        throw std::logic_error("Http1Parser: not a response parser");
    return const_cast<HttpResponse&>(response_parser_.response());
}

Http1RequestParser& Http1Parser::request_parser()
{
    return request_parser_;
}

const Http1RequestParser& Http1Parser::request_parser() const
{
    return request_parser_;
}

Http1ResponseParser& Http1Parser::response_parser()
{
    return response_parser_;
}

const Http1ResponseParser& Http1Parser::response_parser() const
{
    return response_parser_;
}

} // namespace co_wq::net::http
