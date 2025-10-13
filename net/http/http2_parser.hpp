#pragma once

#include "parser.hpp"

#include <nghttp2/nghttp2.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace co_wq::net::http {

class Http2ParserBase {
protected:
    Http2ParserBase()                                  = default;
    Http2ParserBase(const Http2ParserBase&)            = delete;
    Http2ParserBase& operator=(const Http2ParserBase&) = delete;
    virtual ~Http2ParserBase();

    void reset_state();
    bool feed_bytes(std::string_view data, std::string* error_reason);
    bool finish_parse(std::string* error_reason);
    void set_error(std::string message);

    bool ensure_session();
    void destroy_session();

    bool                       headers_complete_ { false };
    bool                       message_complete_ { false };
    bool                       has_error_ { false };
    std::string                last_error_;
    std::int32_t               active_stream_id_ { 0 };
    nghttp2_session*           session_ { nullptr };
    nghttp2_session_callbacks* callbacks_ { nullptr };

    virtual void configure_callbacks(nghttp2_session_callbacks* callbacks) = 0;
    virtual int  create_session(nghttp2_session_callbacks* callbacks)      = 0;
};

class Http2RequestParser : public IHttpRequestParser, public Http2ParserBase {
public:
    Http2RequestParser();
    ~Http2RequestParser() override;

    ParserProtocol protocol() const override { return ParserProtocol::Http2; }
    void           reset() override;
    bool           feed(std::string_view data, std::string* error_reason = nullptr) override;
    bool           finish(std::string* error_reason = nullptr) override;

    bool               is_headers_complete() const override { return headers_complete_; }
    bool               is_message_complete() const override { return message_complete_; }
    bool               has_error() const override { return has_error_; }
    const std::string& last_error() const override { return last_error_; }

    const HttpRequest& request() const override { return request_; }
    HttpRequest&       mutable_request() { return request_; }

private:
    static int on_begin_headers_cb(nghttp2_session* session, const nghttp2_frame* frame, void* user_data);
    static int on_header_cb(nghttp2_session*     session,
                            const nghttp2_frame* frame,
                            const uint8_t*       name,
                            size_t               namelen,
                            const uint8_t*       value,
                            size_t               valuelen,
                            uint8_t              flags,
                            void*                user_data);
    static int on_data_chunk_recv_cb(nghttp2_session* session,
                                     uint8_t          flags,
                                     int32_t          stream_id,
                                     const uint8_t*   data,
                                     size_t           len,
                                     void*            user_data);
    static int on_frame_recv_cb(nghttp2_session* session, const nghttp2_frame* frame, void* user_data);
    static int on_stream_close_cb(nghttp2_session* session, int32_t stream_id, uint32_t error_code, void* user_data);

    void configure_callbacks(nghttp2_session_callbacks* callbacks) override;
    int  create_session(nghttp2_session_callbacks* callbacks) override;

    void handle_header(std::string_view name, std::string_view value);

    HttpRequest request_ {};
};

class Http2ResponseParser : public IHttpResponseParser, public Http2ParserBase {
public:
    Http2ResponseParser();
    ~Http2ResponseParser() override;

    ParserProtocol protocol() const override { return ParserProtocol::Http2; }
    void           reset() override;
    bool           feed(std::string_view data, std::string* error_reason = nullptr) override;
    bool           finish(std::string* error_reason = nullptr) override;

    bool               is_headers_complete() const override { return headers_complete_; }
    bool               is_message_complete() const override { return message_complete_; }
    bool               has_error() const override { return has_error_; }
    const std::string& last_error() const override { return last_error_; }

    const HttpResponse& response() const override { return response_; }
    HttpResponse&       mutable_response() { return response_; }

private:
    static int on_begin_headers_cb(nghttp2_session* session, const nghttp2_frame* frame, void* user_data);
    static int on_header_cb(nghttp2_session*     session,
                            const nghttp2_frame* frame,
                            const uint8_t*       name,
                            size_t               namelen,
                            const uint8_t*       value,
                            size_t               valuelen,
                            uint8_t              flags,
                            void*                user_data);
    static int on_data_chunk_recv_cb(nghttp2_session* session,
                                     uint8_t          flags,
                                     int32_t          stream_id,
                                     const uint8_t*   data,
                                     size_t           len,
                                     void*            user_data);
    static int on_frame_recv_cb(nghttp2_session* session, const nghttp2_frame* frame, void* user_data);
    static int on_stream_close_cb(nghttp2_session* session, int32_t stream_id, uint32_t error_code, void* user_data);

    void configure_callbacks(nghttp2_session_callbacks* callbacks) override;
    int  create_session(nghttp2_session_callbacks* callbacks) override;

    void handle_header(std::string_view name, std::string_view value);

    HttpResponse response_ {};
};

class Http2Parser : public IHttpParser {
public:
    explicit Http2Parser(MessageRole role);

    static Http2Parser make_request();
    static Http2Parser make_response();

    ParserProtocol protocol() const override { return ParserProtocol::Http2; }
    MessageRole    role() const override { return role_; }

    void reset() override;
    bool feed(std::string_view data, std::string* error = nullptr) override;
    bool finish(std::string* error = nullptr) override;

    bool               is_headers_complete() const override;
    bool               is_message_complete() const override;
    bool               has_error() const override;
    const std::string& last_error() const override;

    const HttpRequest&  request() const;
    HttpRequest&        mutable_request();
    const HttpResponse& response() const;
    HttpResponse&       mutable_response();

    Http2RequestParser&        request_parser();
    const Http2RequestParser&  request_parser() const;
    Http2ResponseParser&       response_parser();
    const Http2ResponseParser& response_parser() const;

private:
    MessageRole         role_;
    Http2RequestParser  request_parser_;
    Http2ResponseParser response_parser_;
};

} // namespace co_wq::net::http
