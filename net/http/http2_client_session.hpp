#pragma once

#include "http2_session.hpp"
#include "http_message.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace co_wq::net::http {

class Http2ClientSession : public Http2Session {
public:
    class StreamHandle {
    public:
        StreamHandle() = default;
        StreamHandle(Http2ClientSession* session, int32_t stream_id);
        StreamHandle(const StreamHandle&)            = delete;
        StreamHandle& operator=(const StreamHandle&) = delete;
        StreamHandle(StreamHandle&& other) noexcept;
        StreamHandle& operator=(StreamHandle&& other) noexcept;
        ~StreamHandle();

        bool                valid() const { return session_ && stream_id_ >= 0; }
        int32_t             id() const { return stream_id_; }
        bool                headers_complete() const;
        bool                message_complete() const;
        uint32_t            error_code() const;
        HttpResponse*       response();
        const HttpResponse* response() const;

        bool cancel(uint32_t error_code = NGHTTP2_CANCEL);
        void release();

    private:
        void reset_internal();

        Http2ClientSession* session_ { nullptr };
        int32_t             stream_id_ { -1 };
        bool                released_ { false };

        friend class Http2ClientSession;
    };

    struct Request {
        std::string                                      method;
        std::string                                      scheme;
        std::string                                      authority;
        std::string                                      path;
        std::vector<std::pair<std::string, std::string>> headers;
        const std::string*                               body { nullptr };
    };

    using HeaderHandler          = std::function<void(std::string_view name, std::string_view value)>;
    using HeadersCompleteHandler = std::function<void(bool end_stream)>;
    using DataHandler            = std::function<void(std::string_view data)>;
    using StreamCloseHandler     = std::function<void(uint32_t error_code)>;

    struct StreamHandlers {
        HeaderHandler          on_header;
        HeadersCompleteHandler on_headers_complete;
        DataHandler            on_data;
        StreamCloseHandler     on_close;
    };

    Http2ClientSession();

    void set_header_handler(HeaderHandler handler);
    void set_headers_complete_handler(HeadersCompleteHandler handler);
    void set_data_handler(DataHandler handler);
    void set_stream_close_handler(StreamCloseHandler handler);

    StreamHandle start_request_handle(const Request& request, StreamHandlers handlers = {});
    int32_t      start_request(const Request& request, StreamHandlers handlers = {});

    bool     headers_complete() const { return headers_complete(last_started_stream_id_); }
    bool     message_complete() const { return message_complete(last_started_stream_id_); }
    uint32_t stream_error_code() const { return stream_error_code(last_started_stream_id_); }

    bool                headers_complete(int32_t stream_id) const;
    bool                message_complete(int32_t stream_id) const;
    uint32_t            stream_error_code(int32_t stream_id) const;
    const HttpResponse* response_for(int32_t stream_id) const;
    HttpResponse*       mutable_response_for(int32_t stream_id);

    const HttpResponse& response() const;
    HttpResponse&       mutable_response();

    std::vector<int32_t> active_streams() const;
    void                 clear_stream(int32_t stream_id);

    bool cancel_stream(int32_t stream_id, uint32_t error_code = NGHTTP2_CANCEL);
    bool is_stream_closed(int32_t stream_id) const;

    struct RequestInfo {
        std::string method;
        std::string authority;
        std::string path;
    };

    const RequestInfo* request_info(int32_t stream_id) const;

protected:
    int on_begin_headers(const nghttp2_frame* frame) override;
    int on_header(const nghttp2_frame* frame, std::string_view name, std::string_view value) override;
    int on_data_chunk(int32_t stream_id, std::string_view data) override;
    int on_frame_recv(const nghttp2_frame* frame) override;
    int on_stream_close(int32_t stream_id, uint32_t error_code) override;

private:
    struct StreamState {
        HttpResponse            response;
        Http2HeaderBlock        request_headers;
        Http2StringDataProvider request_body_provider;
        StreamHandlers          handlers;
        RequestInfo             info;
        bool                    headers_complete { false };
        bool                    message_complete { false };
        bool                    closed { false };
        bool                    cancel_requested { false };
        uint32_t                error_code { 0 };
    };

    StreamState*       find_stream(int32_t stream_id);
    const StreamState* find_stream(int32_t stream_id) const;
    void               reset_streams();

    HeaderHandler          default_header_handler_ {};
    HeadersCompleteHandler default_headers_complete_handler_ {};
    DataHandler            default_data_handler_ {};
    StreamCloseHandler     default_stream_close_handler_ {};

    std::unordered_map<int32_t, std::unique_ptr<StreamState>> streams_;
    int32_t                                                   last_started_stream_id_ { -1 };
};

} // namespace co_wq::net::http
