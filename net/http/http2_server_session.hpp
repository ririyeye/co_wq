#pragma once

#include "http2_session.hpp"
#include "http_message.hpp"

#include <functional>
#include <unordered_map>

namespace co_wq::net::http {

class Http2ServerSession : public Http2Session {
public:
    using RequestHandler = std::function<HttpResponse(const HttpRequest&)>;

    Http2ServerSession();
    explicit Http2ServerSession(RequestHandler handler);

    void set_request_handler(RequestHandler handler);

protected:
    int on_begin_headers(const nghttp2_frame* frame) override;
    int on_header(const nghttp2_frame* frame, std::string_view name, std::string_view value) override;
    int on_data_chunk(int32_t stream_id, std::string_view data) override;
    int on_frame_recv(const nghttp2_frame* frame) override;
    int on_stream_close(int32_t stream_id, uint32_t error_code) override;

private:
    struct StreamState {
        HttpRequest request {};
        bool        headers_received { false };
        bool        response_sent { false };
    };

    struct ResponseState {
        Http2HeaderBlock        headers;
        std::string             body;
        Http2StringDataProvider body_provider;
    };

    RequestHandler                             request_handler_ {};
    std::unordered_map<int32_t, StreamState>   streams_;
    std::unordered_map<int32_t, ResponseState> responses_;

    int         handle_request_complete(int32_t stream_id);
    int         submit_response_for_stream(int32_t stream_id, StreamState& stream_state);
    void        reset_stream_state(StreamState& state);
    static void normalize_request_paths(HttpRequest& request);
};

} // namespace co_wq::net::http
