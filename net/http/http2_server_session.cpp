#include "http2_server_session.hpp"

#include <charconv>

#include "http_common.hpp"

namespace co_wq::net::http {

Http2ServerSession::Http2ServerSession() : Http2Session(Mode::Server) { }

Http2ServerSession::Http2ServerSession(RequestHandler handler)
    : Http2Session(Mode::Server), request_handler_(std::move(handler))
{
}

void Http2ServerSession::set_request_handler(RequestHandler handler)
{
    request_handler_ = std::move(handler);
}

int Http2ServerSession::on_begin_headers(const nghttp2_frame* frame)
{
    if (!frame || frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST)
        return 0;

    auto& state = streams_[frame->hd.stream_id];
    reset_stream_state(state);
    return 0;
}

int Http2ServerSession::on_header(const nghttp2_frame* frame, std::string_view name, std::string_view value)
{
    if (!frame || frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST)
        return 0;

    auto it = streams_.find(frame->hd.stream_id);
    if (it == streams_.end())
        return 0;

    auto& request = it->second.request;

    if (name == ":method") {
        request.method.assign(value.begin(), value.end());
        return 0;
    }
    if (name == ":scheme") {
        request.scheme.assign(value.begin(), value.end());
        return 0;
    }
    if (name == ":path") {
        request.target.assign(value.begin(), value.end());
        request.path = request.target;
        request.query.clear();
        if (auto pos = request.target.find('?'); pos != std::string::npos) {
            request.path  = request.target.substr(0, pos);
            request.query = request.target.substr(pos + 1);
        }
        return 0;
    }
    if (name == ":authority") {
        request.host.assign(value.begin(), value.end());
        request.set_header("host", std::string(value));
        request.port = 0;
        if (auto pos = request.host.find(':'); pos != std::string::npos && pos + 1 < request.host.size()) {
            int  port_value = 0;
            auto result     = std::from_chars(request.host.data() + pos + 1,
                                          request.host.data() + request.host.size(),
                                          port_value);
            if (result.ec == std::errc())
                request.port = port_value;
        }
        return 0;
    }
    if (!name.empty() && name.front() == ':')
        return 0;

    request.set_header(std::string(name), std::string(value));
    return 0;
}

int Http2ServerSession::on_data_chunk(int32_t stream_id, std::string_view data)
{
    auto it = streams_.find(stream_id);
    if (it == streams_.end())
        return 0;
    it->second.request.append_body(data);
    return 0;
}

int Http2ServerSession::on_frame_recv(const nghttp2_frame* frame)
{
    if (!frame)
        return 0;

    auto it = streams_.find(frame->hd.stream_id);
    if (it == streams_.end())
        return 0;

    auto& stream_state = it->second;

    if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        stream_state.headers_received = true;
        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
            return handle_request_complete(frame->hd.stream_id);
    } else if (frame->hd.type == NGHTTP2_DATA) {
        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
            return handle_request_complete(frame->hd.stream_id);
    }

    return 0;
}

int Http2ServerSession::on_stream_close(int32_t stream_id, uint32_t)
{
    streams_.erase(stream_id);
    responses_.erase(stream_id);
    return 0;
}

int Http2ServerSession::handle_request_complete(int32_t stream_id)
{
    auto it = streams_.find(stream_id);
    if (it == streams_.end())
        return 0;

    auto& stream_state = it->second;
    if (stream_state.response_sent)
        return 0;

    normalize_request_paths(stream_state.request);

    return submit_response_for_stream(stream_id, stream_state);
}

int Http2ServerSession::submit_response_for_stream(int32_t stream_id, StreamState& stream_state)
{
    HttpResponse response {};
    if (request_handler_) {
        response = request_handler_(stream_state.request);
    } else {
        response.status_code = 500;
        response.reason      = "Internal Server Error";
        response.body        = "HTTP/2 handler not installed";
        response.headers.clear();
        response.headers.emplace("content-type", "text/plain; charset=utf-8");
    }

    auto& response_state = responses_[stream_id];
    response_state.headers.clear();
    response_state.headers.reserve(response.headers.size() + 2);
    response_state.body = std::move(response.body);
    response_state.body_provider.reset();

    response_state.headers.add(":status", std::to_string(response.status_code));

    bool has_content_length = false;
    for (const auto& kv : response.headers) {
        if (iequals(kv.first, "connection"))
            continue;
        if (iequals(kv.first, "content-length"))
            has_content_length = true;
        response_state.headers.add(kv.first, kv.second);
    }

    if (!has_content_length)
        response_state.headers.add("content-length", std::to_string(response_state.body.size()));

    nghttp2_data_provider* provider_ptr = nullptr;
    if (!response_state.body.empty()) {
        response_state.body_provider.bind(&response_state.body);
        provider_ptr = response_state.body_provider.provider();
    }

    auto header_span = response_state.headers.as_span();
    int  rv          = submit_response(stream_id, header_span, provider_ptr);
    if (rv == 0)
        stream_state.response_sent = true;
    return rv;
}

void Http2ServerSession::reset_stream_state(StreamState& state)
{
    state.request.reset();
    state.request.set_http_version(2, 0);
    state.headers_received = false;
    state.response_sent    = false;
}

void Http2ServerSession::normalize_request_paths(HttpRequest& request)
{
    if (request.target.empty())
        request.target = "/";

    if (request.target.front() != '/' && request.target.front() != '*')
        request.target.insert(request.target.begin(), '/');

    request.path  = request.target;
    request.query = {};

    if (auto pos = request.path.find('?'); pos != std::string::npos) {
        request.query = request.path.substr(pos + 1);
        request.path.resize(pos);
    }

    if (request.path.empty())
        request.path = "/";
}

} // namespace co_wq::net::http
