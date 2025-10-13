#include "http2_client_session.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <span>

namespace co_wq::net::http {

Http2ClientSession::StreamHandle::StreamHandle(Http2ClientSession* session, int32_t stream_id)
    : session_(session), stream_id_(stream_id)
{
}

Http2ClientSession::StreamHandle::StreamHandle(StreamHandle&& other) noexcept
{
    *this = std::move(other);
}

Http2ClientSession::StreamHandle& Http2ClientSession::StreamHandle::operator=(StreamHandle&& other) noexcept
{
    if (this == &other)
        return *this;
    reset_internal();
    session_         = other.session_;
    stream_id_       = other.stream_id_;
    released_        = other.released_;
    other.session_   = nullptr;
    other.stream_id_ = -1;
    other.released_  = true;
    return *this;
}

Http2ClientSession::StreamHandle::~StreamHandle()
{
    reset_internal();
}

bool Http2ClientSession::StreamHandle::headers_complete() const
{
    return valid() ? session_->headers_complete(stream_id_) : false;
}

bool Http2ClientSession::StreamHandle::message_complete() const
{
    return valid() ? session_->message_complete(stream_id_) : false;
}

uint32_t Http2ClientSession::StreamHandle::error_code() const
{
    return valid() ? session_->stream_error_code(stream_id_) : 0;
}

HttpResponse* Http2ClientSession::StreamHandle::response()
{
    return valid() ? session_->mutable_response_for(stream_id_) : nullptr;
}

const HttpResponse* Http2ClientSession::StreamHandle::response() const
{
    return valid() ? session_->response_for(stream_id_) : nullptr;
}

bool Http2ClientSession::StreamHandle::cancel(uint32_t error_code)
{
    if (!valid())
        return false;
    bool ok = session_->cancel_stream(stream_id_, error_code);
    if (ok)
        released_ = true;
    return ok;
}

void Http2ClientSession::StreamHandle::release()
{
    released_ = true;
}

void Http2ClientSession::StreamHandle::reset_internal()
{
    if (valid() && !released_)
        session_->cancel_stream(stream_id_, NGHTTP2_CANCEL);
    session_   = nullptr;
    stream_id_ = -1;
    released_  = false;
}

Http2ClientSession::Http2ClientSession() : Http2Session(Mode::Client)
{
    reset_streams();
}

void Http2ClientSession::set_header_handler(HeaderHandler handler)
{
    default_header_handler_ = std::move(handler);
}

void Http2ClientSession::set_headers_complete_handler(HeadersCompleteHandler handler)
{
    default_headers_complete_handler_ = std::move(handler);
}

void Http2ClientSession::set_data_handler(DataHandler handler)
{
    default_data_handler_ = std::move(handler);
}

void Http2ClientSession::set_stream_close_handler(StreamCloseHandler handler)
{
    default_stream_close_handler_ = std::move(handler);
}

Http2ClientSession::StreamHandle Http2ClientSession::start_request_handle(const Request& request,
                                                                          StreamHandlers handlers)
{
    auto state      = std::make_unique<StreamState>();
    state->handlers = std::move(handlers);
    state->response.reset();
    state->response.set_http_version(2, 0);
    state->info.method    = request.method;
    state->info.authority = request.authority;
    state->info.path      = request.path.empty() ? std::string("/") : request.path;

    size_t estimated_pairs = request.headers.size() + 4;
    state->request_headers.clear();
    state->request_headers.reserve(estimated_pairs);

    state->request_headers.add(":method", request.method);
    state->request_headers.add(":scheme", request.scheme);
    state->request_headers.add(":path", state->info.path);
    state->request_headers.add(":authority", request.authority);

    for (const auto& header : request.headers) {
        if (header.first.empty())
            continue;
        if (header.first.front() == ':')
            continue;
        std::string lower(header.first);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        uint8_t flags = NGHTTP2_NV_FLAG_NONE;
        if (lower == "authorization" || lower == "cookie")
            flags |= NGHTTP2_NV_FLAG_NO_INDEX;
        state->request_headers.add(std::move(lower), header.second, flags);
    }

    state->request_body_provider.reset();
    if (request.body && !request.body->empty())
        state->request_body_provider.bind(request.body);

    auto  span         = state->request_headers.as_span();
    auto* provider_ptr = state->request_body_provider.has_body() ? state->request_body_provider.provider() : nullptr;

    int32_t id = submit_request(span, provider_ptr, this);
    if (id < 0) {
        StreamHandle error;
        error.stream_id_ = id;
        return error;
    }

    last_started_stream_id_ = id;
    streams_.emplace(id, std::move(state));
    return StreamHandle(this, id);
}

int32_t Http2ClientSession::start_request(const Request& request, StreamHandlers handlers)
{
    auto handle = start_request_handle(request, std::move(handlers));
    if (!handle.valid())
        return handle.id();
    int32_t id = handle.id();
    handle.release();
    return id;
}

bool Http2ClientSession::headers_complete(int32_t stream_id) const
{
    const StreamState* state = find_stream(stream_id);
    return state ? state->headers_complete : false;
}

bool Http2ClientSession::message_complete(int32_t stream_id) const
{
    const StreamState* state = find_stream(stream_id);
    return state ? state->message_complete : false;
}

uint32_t Http2ClientSession::stream_error_code(int32_t stream_id) const
{
    const StreamState* state = find_stream(stream_id);
    return state ? state->error_code : 0;
}

const HttpResponse* Http2ClientSession::response_for(int32_t stream_id) const
{
    const StreamState* state = find_stream(stream_id);
    return state ? &state->response : nullptr;
}

HttpResponse* Http2ClientSession::mutable_response_for(int32_t stream_id)
{
    StreamState* state = find_stream(stream_id);
    return state ? &state->response : nullptr;
}

const HttpResponse& Http2ClientSession::response() const
{
    if (const auto* state = find_stream(last_started_stream_id_))
        return state->response;
    static HttpResponse empty_response;
    return empty_response;
}

HttpResponse& Http2ClientSession::mutable_response()
{
    if (auto* state = find_stream(last_started_stream_id_))
        return state->response;
    static HttpResponse empty_response;
    empty_response.reset();
    return empty_response;
}

std::vector<int32_t> Http2ClientSession::active_streams() const
{
    std::vector<int32_t> ids;
    ids.reserve(streams_.size());
    for (const auto& entry : streams_)
        ids.push_back(entry.first);
    return ids;
}

void Http2ClientSession::clear_stream(int32_t stream_id)
{
    streams_.erase(stream_id);
    if (stream_id == last_started_stream_id_)
        last_started_stream_id_ = -1;
}

bool Http2ClientSession::cancel_stream(int32_t stream_id, uint32_t error_code)
{
    StreamState* state = find_stream(stream_id);
    if (!state || state->closed || state->cancel_requested)
        return false;
    if (int rc = submit_rst_stream(stream_id, error_code); rc != 0)
        return false;
    state->cancel_requested = true;
    return true;
}

bool Http2ClientSession::is_stream_closed(int32_t stream_id) const
{
    const StreamState* state = find_stream(stream_id);
    if (!state)
        return true;
    return state->closed;
}

const Http2ClientSession::RequestInfo* Http2ClientSession::request_info(int32_t stream_id) const
{
    const StreamState* state = find_stream(stream_id);
    return state ? &state->info : nullptr;
}

int Http2ClientSession::on_begin_headers(const nghttp2_frame* frame)
{
    if (!frame)
        return 0;
    StreamState* state = find_stream(frame->hd.stream_id);
    if (!state)
        return 0;
    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_RESPONSE)
        return 0;

    state->response.reset();
    state->response.set_http_version(2, 0);
    return 0;
}

int Http2ClientSession::on_header(const nghttp2_frame* frame, std::string_view name, std::string_view value)
{
    if (!frame)
        return 0;

    StreamState* state = find_stream(frame->hd.stream_id);
    if (!state)
        return 0;

    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_RESPONSE)
        return 0;

    if (name == ":status") {
        int  status = 0;
        auto result = std::from_chars(value.data(), value.data() + value.size(), status);
        if (result.ec == std::errc())
            state->response.status_code = status;
        state->response.reason.assign(value.begin(), value.end());
    } else if (!name.empty() && name.front() != ':') {
        state->response.set_header(std::string(name), std::string(value));
    }

    if (state->handlers.on_header)
        state->handlers.on_header(name, value);
    else if (default_header_handler_)
        default_header_handler_(name, value);

    return 0;
}

int Http2ClientSession::on_data_chunk(int32_t stream_id, std::string_view data)
{
    StreamState* state = find_stream(stream_id);
    if (!state)
        return 0;

    state->response.append_body(data);
    if (state->handlers.on_data)
        state->handlers.on_data(data);
    else if (default_data_handler_)
        default_data_handler_(data);
    return 0;
}

int Http2ClientSession::on_frame_recv(const nghttp2_frame* frame)
{
    if (!frame)
        return 0;

    StreamState* state = find_stream(frame->hd.stream_id);
    if (!state)
        return 0;

    if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
        state->headers_complete = true;
        bool end_stream         = (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0;
        if (state->handlers.on_headers_complete)
            state->handlers.on_headers_complete(end_stream);
        else if (default_headers_complete_handler_)
            default_headers_complete_handler_(end_stream);
        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
            state->message_complete = true;
    } else if (frame->hd.type == NGHTTP2_DATA) {
        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
            state->message_complete = true;
    }

    return 0;
}

int Http2ClientSession::on_stream_close(int32_t stream_id, uint32_t error_code)
{
    StreamState* state = find_stream(stream_id);
    if (!state)
        return 0;

    state->message_complete = true;
    state->closed           = true;
    state->error_code       = error_code;
    state->request_headers.clear();
    state->request_body_provider.reset();

    if (state->handlers.on_close)
        state->handlers.on_close(error_code);
    else if (default_stream_close_handler_)
        default_stream_close_handler_(error_code);
    return 0;
}

Http2ClientSession::StreamState* Http2ClientSession::find_stream(int32_t stream_id)
{
    if (stream_id < 0)
        return nullptr;
    auto it = streams_.find(stream_id);
    if (it == streams_.end())
        return nullptr;
    return it->second.get();
}

const Http2ClientSession::StreamState* Http2ClientSession::find_stream(int32_t stream_id) const
{
    if (stream_id < 0)
        return nullptr;
    auto it = streams_.find(stream_id);
    if (it == streams_.end())
        return nullptr;
    return it->second.get();
}

void Http2ClientSession::reset_streams()
{
    streams_.clear();
    last_started_stream_id_ = -1;
}

} // namespace co_wq::net::http
