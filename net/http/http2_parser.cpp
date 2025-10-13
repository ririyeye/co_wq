#include "http2_parser.hpp"

#include <charconv>
#include <stdexcept>
#include <string>

namespace co_wq::net::http {

Http2ParserBase::~Http2ParserBase()
{
    destroy_session();
    if (callbacks_) {
        nghttp2_session_callbacks_del(callbacks_);
        callbacks_ = nullptr;
    }
}

void Http2ParserBase::reset_state()
{
    headers_complete_ = false;
    message_complete_ = false;
    has_error_        = false;
    last_error_.clear();
    active_stream_id_ = 0;
}

void Http2ParserBase::set_error(std::string message)
{
    has_error_  = true;
    last_error_ = std::move(message);
}

bool Http2ParserBase::feed_bytes(std::string_view data, std::string* error_reason)
{
    if (has_error_) {
        if (error_reason)
            *error_reason = last_error_;
        return false;
    }

    if (!session_ && !ensure_session()) {
        if (error_reason)
            *error_reason = last_error_;
        return false;
    }

    if (data.empty())
        return true;

    auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
    auto  rv    = nghttp2_session_mem_recv(session_, bytes, data.size());
    if (rv < 0) {
        set_error(std::string("nghttp2_session_mem_recv failed: ") + nghttp2_strerror(static_cast<int>(rv)));
        if (error_reason)
            *error_reason = last_error_;
        return false;
    }

    return true;
}

bool Http2ParserBase::finish_parse(std::string* error_reason)
{
    if (has_error_) {
        if (error_reason)
            *error_reason = last_error_;
        return false;
    }
    return true;
}

bool Http2ParserBase::ensure_session()
{
    if (!callbacks_) {
        if (nghttp2_session_callbacks_new(&callbacks_) != 0) {
            set_error("nghttp2_session_callbacks_new failed");
            return false;
        }
        configure_callbacks(callbacks_);
    }

    destroy_session();

    auto rv = create_session(callbacks_);
    if (rv != 0) {
        set_error(std::string("nghttp2_session_*_new failed: ") + nghttp2_strerror(rv));
        return false;
    }

    nghttp2_session_set_user_data(session_, this);
    return true;
}

void Http2ParserBase::destroy_session()
{
    if (session_) {
        nghttp2_session_del(session_);
        session_ = nullptr;
    }
}

Http2RequestParser::Http2RequestParser()
{
    reset();
}

Http2RequestParser::~Http2RequestParser() = default;

void Http2RequestParser::reset()
{
    request_.reset();
    request_.set_http_version(2, 0);
    reset_state();
    ensure_session();
}

bool Http2RequestParser::feed(std::string_view data, std::string* error_reason)
{
    if (!feed_bytes(data, error_reason))
        return false;
    return !has_error_;
}

bool Http2RequestParser::finish(std::string* error_reason)
{
    if (!finish_parse(error_reason))
        return false;
    if (!message_complete_) {
        set_error("HTTP/2 request stream not finished");
        if (error_reason)
            *error_reason = last_error_;
        return false;
    }
    return true;
}

int Http2RequestParser::on_begin_headers_cb(nghttp2_session*, const nghttp2_frame* frame, void* user_data)
{
    auto* self = static_cast<Http2RequestParser*>(user_data);
    if (!self)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (frame->hd.type != NGHTTP2_HEADERS)
        return 0;
    if (frame->headers.cat != NGHTTP2_HCAT_REQUEST)
        return 0;
    if (self->active_stream_id_ == 0)
        self->active_stream_id_ = frame->hd.stream_id;
    if (self->active_stream_id_ != frame->hd.stream_id)
        return 0;
    self->request_.reset();
    self->request_.set_http_version(2, 0);
    self->headers_complete_ = false;
    self->message_complete_ = false;
    return 0;
}

int Http2RequestParser::on_header_cb(nghttp2_session*,
                                     const nghttp2_frame* frame,
                                     const uint8_t*       name,
                                     size_t               namelen,
                                     const uint8_t*       value,
                                     size_t               valuelen,
                                     uint8_t,
                                     void* user_data)
{
    auto* self = static_cast<Http2RequestParser*>(user_data);
    if (!self)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (frame->hd.type != NGHTTP2_HEADERS)
        return 0;
    if (frame->headers.cat != NGHTTP2_HCAT_REQUEST)
        return 0;
    if (self->active_stream_id_ != 0 && self->active_stream_id_ != frame->hd.stream_id)
        return 0;

    self->handle_header(std::string_view(reinterpret_cast<const char*>(name), namelen),
                        std::string_view(reinterpret_cast<const char*>(value), valuelen));
    return 0;
}

int Http2RequestParser::on_data_chunk_recv_cb(nghttp2_session*,
                                              uint8_t,
                                              int32_t        stream_id,
                                              const uint8_t* data,
                                              size_t         len,
                                              void*          user_data)
{
    auto* self = static_cast<Http2RequestParser*>(user_data);
    if (!self)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (self->active_stream_id_ != stream_id)
        return 0;
    self->request_.append_body(std::string_view(reinterpret_cast<const char*>(data), len));
    return 0;
}

int Http2RequestParser::on_frame_recv_cb(nghttp2_session*, const nghttp2_frame* frame, void* user_data)
{
    auto* self = static_cast<Http2RequestParser*>(user_data);
    if (!self)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (self->active_stream_id_ != 0 && self->active_stream_id_ != frame->hd.stream_id)
        return 0;

    if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        self->headers_complete_ = true;
        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
            self->message_complete_ = true;
    } else if (frame->hd.type == NGHTTP2_DATA) {
        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
            self->message_complete_ = true;
    }
    return 0;
}

int Http2RequestParser::on_stream_close_cb(nghttp2_session*, int32_t stream_id, uint32_t error_code, void* user_data)
{
    auto* self = static_cast<Http2RequestParser*>(user_data);
    if (!self)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (self->active_stream_id_ != stream_id)
        return 0;

    self->message_complete_ = true;
    if (error_code != NGHTTP2_NO_ERROR)
        self->set_error("HTTP/2 stream closed with error code " + std::to_string(error_code));
    return 0;
}

void Http2RequestParser::configure_callbacks(nghttp2_session_callbacks* callbacks)
{
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, &Http2RequestParser::on_begin_headers_cb);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, &Http2RequestParser::on_header_cb);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, &Http2RequestParser::on_data_chunk_recv_cb);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, &Http2RequestParser::on_frame_recv_cb);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, &Http2RequestParser::on_stream_close_cb);
}

int Http2RequestParser::create_session(nghttp2_session_callbacks* callbacks)
{
    return nghttp2_session_server_new(&session_, callbacks, this);
}

void Http2RequestParser::handle_header(std::string_view name, std::string_view value)
{
    if (name.empty())
        return;
    if (name == ":method") {
        request_.method.assign(value.begin(), value.end());
        return;
    }
    if (name == ":scheme") {
        request_.scheme.assign(value.begin(), value.end());
        return;
    }
    if (name == ":path") {
        request_.target.assign(value.begin(), value.end());
        request_.path.assign(value.begin(), value.end());
        return;
    }
    if (name == ":authority") {
        request_.host.assign(value.begin(), value.end());
        request_.set_header("host", std::string(value));
        return;
    }
    if (name.front() == ':')
        return;
    request_.set_header(std::string(name), std::string(value));
}

Http2ResponseParser::Http2ResponseParser()
{
    reset();
}

Http2ResponseParser::~Http2ResponseParser() = default;

void Http2ResponseParser::reset()
{
    response_.reset();
    response_.set_http_version(2, 0);
    reset_state();
    ensure_session();
}

bool Http2ResponseParser::feed(std::string_view data, std::string* error_reason)
{
    if (!feed_bytes(data, error_reason))
        return false;
    return !has_error_;
}

bool Http2ResponseParser::finish(std::string* error_reason)
{
    if (!finish_parse(error_reason))
        return false;
    if (!message_complete_) {
        set_error("HTTP/2 response stream not finished");
        if (error_reason)
            *error_reason = last_error_;
        return false;
    }
    return true;
}

int Http2ResponseParser::on_begin_headers_cb(nghttp2_session*, const nghttp2_frame* frame, void* user_data)
{
    auto* self = static_cast<Http2ResponseParser*>(user_data);
    if (!self)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (frame->hd.type != NGHTTP2_HEADERS)
        return 0;
    if (frame->headers.cat != NGHTTP2_HCAT_RESPONSE)
        return 0;
    if (self->active_stream_id_ == 0)
        self->active_stream_id_ = frame->hd.stream_id;
    if (self->active_stream_id_ != frame->hd.stream_id)
        return 0;
    self->response_.reset();
    self->response_.set_http_version(2, 0);
    self->headers_complete_ = false;
    self->message_complete_ = false;
    return 0;
}

int Http2ResponseParser::on_header_cb(nghttp2_session*,
                                      const nghttp2_frame* frame,
                                      const uint8_t*       name,
                                      size_t               namelen,
                                      const uint8_t*       value,
                                      size_t               valuelen,
                                      uint8_t,
                                      void* user_data)
{
    auto* self = static_cast<Http2ResponseParser*>(user_data);
    if (!self)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (frame->hd.type != NGHTTP2_HEADERS)
        return 0;
    if (frame->headers.cat != NGHTTP2_HCAT_RESPONSE)
        return 0;
    if (self->active_stream_id_ != 0 && self->active_stream_id_ != frame->hd.stream_id)
        return 0;

    self->handle_header(std::string_view(reinterpret_cast<const char*>(name), namelen),
                        std::string_view(reinterpret_cast<const char*>(value), valuelen));
    return 0;
}

int Http2ResponseParser::on_data_chunk_recv_cb(nghttp2_session*,
                                               uint8_t,
                                               int32_t        stream_id,
                                               const uint8_t* data,
                                               size_t         len,
                                               void*          user_data)
{
    auto* self = static_cast<Http2ResponseParser*>(user_data);
    if (!self)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (self->active_stream_id_ != stream_id)
        return 0;
    self->response_.append_body(std::string_view(reinterpret_cast<const char*>(data), len));
    return 0;
}

int Http2ResponseParser::on_frame_recv_cb(nghttp2_session*, const nghttp2_frame* frame, void* user_data)
{
    auto* self = static_cast<Http2ResponseParser*>(user_data);
    if (!self)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (self->active_stream_id_ != 0 && self->active_stream_id_ != frame->hd.stream_id)
        return 0;

    if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
        self->headers_complete_ = true;
        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
            self->message_complete_ = true;
    } else if (frame->hd.type == NGHTTP2_DATA) {
        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
            self->message_complete_ = true;
    }
    return 0;
}

int Http2ResponseParser::on_stream_close_cb(nghttp2_session*, int32_t stream_id, uint32_t error_code, void* user_data)
{
    auto* self = static_cast<Http2ResponseParser*>(user_data);
    if (!self)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (self->active_stream_id_ != stream_id)
        return 0;

    self->message_complete_ = true;
    if (error_code != NGHTTP2_NO_ERROR)
        self->set_error("HTTP/2 stream closed with error code " + std::to_string(error_code));
    return 0;
}

void Http2ResponseParser::configure_callbacks(nghttp2_session_callbacks* callbacks)
{
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, &Http2ResponseParser::on_begin_headers_cb);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, &Http2ResponseParser::on_header_cb);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, &Http2ResponseParser::on_data_chunk_recv_cb);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, &Http2ResponseParser::on_frame_recv_cb);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, &Http2ResponseParser::on_stream_close_cb);
}

int Http2ResponseParser::create_session(nghttp2_session_callbacks* callbacks)
{
    return nghttp2_session_client_new(&session_, callbacks, this);
}

void Http2ResponseParser::handle_header(std::string_view name, std::string_view value)
{
    if (name.empty())
        return;
    if (name == ":status") {
        int  status = 0;
        auto result = std::from_chars(value.data(), value.data() + value.size(), status);
        if (result.ec == std::errc()) {
            response_.status_code = status;
            response_.reason.clear();
        } else {
            set_error("invalid HTTP/2 :status value");
        }
        return;
    }
    if (name.front() == ':')
        return;
    response_.set_header(std::string(name), std::string(value));
}

Http2Parser::Http2Parser(MessageRole role) : role_(role) { }

Http2Parser Http2Parser::make_request()
{
    return Http2Parser(MessageRole::Request);
}

Http2Parser Http2Parser::make_response()
{
    return Http2Parser(MessageRole::Response);
}

void Http2Parser::reset()
{
    if (role_ == MessageRole::Request)
        request_parser_.reset();
    else
        response_parser_.reset();
}

bool Http2Parser::feed(std::string_view data, std::string* error)
{
    if (role_ == MessageRole::Request)
        return request_parser_.feed(data, error);
    return response_parser_.feed(data, error);
}

bool Http2Parser::finish(std::string* error)
{
    if (role_ == MessageRole::Request)
        return request_parser_.finish(error);
    return response_parser_.finish(error);
}

bool Http2Parser::is_headers_complete() const
{
    if (role_ == MessageRole::Request)
        return request_parser_.is_headers_complete();
    return response_parser_.is_headers_complete();
}

bool Http2Parser::is_message_complete() const
{
    if (role_ == MessageRole::Request)
        return request_parser_.is_message_complete();
    return response_parser_.is_message_complete();
}

bool Http2Parser::has_error() const
{
    if (role_ == MessageRole::Request)
        return request_parser_.has_error();
    return response_parser_.has_error();
}

const std::string& Http2Parser::last_error() const
{
    if (role_ == MessageRole::Request)
        return request_parser_.last_error();
    return response_parser_.last_error();
}

const HttpRequest& Http2Parser::request() const
{
    if (role_ != MessageRole::Request)
        throw std::logic_error("Http2Parser: not a request parser");
    return request_parser_.request();
}

HttpRequest& Http2Parser::mutable_request()
{
    if (role_ != MessageRole::Request)
        throw std::logic_error("Http2Parser: not a request parser");
    return request_parser_.mutable_request();
}

const HttpResponse& Http2Parser::response() const
{
    if (role_ != MessageRole::Response)
        throw std::logic_error("Http2Parser: not a response parser");
    return response_parser_.response();
}

HttpResponse& Http2Parser::mutable_response()
{
    if (role_ != MessageRole::Response)
        throw std::logic_error("Http2Parser: not a response parser");
    return response_parser_.mutable_response();
}

Http2RequestParser& Http2Parser::request_parser()
{
    return request_parser_;
}

const Http2RequestParser& Http2Parser::request_parser() const
{
    return request_parser_;
}

Http2ResponseParser& Http2Parser::response_parser()
{
    return response_parser_;
}

const Http2ResponseParser& Http2Parser::response_parser() const
{
    return response_parser_;
}

} // namespace co_wq::net::http
