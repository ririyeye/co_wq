#include "http2_session.hpp"

#include <algorithm>
#include <cstring>

namespace co_wq::net::http {

Http2Session::Http2Session(Mode mode) : mode_(mode) { }

Http2Session::~Http2Session()
{
    destroy_session();
    if (callbacks_) {
        nghttp2_session_callbacks_del(callbacks_);
        callbacks_ = nullptr;
    }
}

bool Http2Session::init()
{
    clear_error();

    if (!callbacks_) {
        if (nghttp2_session_callbacks_new(&callbacks_) != 0) {
            set_error(NGHTTP2_ERR_NOMEM, "nghttp2_session_callbacks_new failed");
            return false;
        }
        nghttp2_session_callbacks_set_send_callback2(callbacks_, &Http2Session::send_callback);
        nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks_, &Http2Session::begin_headers_callback);
        nghttp2_session_callbacks_set_on_header_callback(callbacks_, &Http2Session::header_callback);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks_, &Http2Session::data_chunk_recv_callback);
        nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks_, &Http2Session::frame_recv_callback);
        nghttp2_session_callbacks_set_on_stream_close_callback(callbacks_, &Http2Session::stream_close_callback);
    }

    destroy_session();

    int rv = (mode_ == Mode::Server) ? nghttp2_session_server_new(&session_, callbacks_, this)
                                     : nghttp2_session_client_new(&session_, callbacks_, this);
    if (rv != 0) {
        set_error(rv, std::string("nghttp2_session_*_new failed: ") + nghttp2_strerror(rv));
        return false;
    }

    nghttp2_session_set_user_data(session_, this);
    send_buffer_.clear();
    send_offset_ = 0;
    return true;
}

void Http2Session::reset()
{
    destroy_session();
    init();
}

bool Http2Session::feed(std::string_view data, std::string* error_reason)
{
    if (!ensure_session()) {
        if (error_reason)
            *error_reason = last_error_;
        return false;
    }

    if (has_error_) {
        if (error_reason)
            *error_reason = last_error_;
        return false;
    }

    if (data.empty())
        return drain_send_queue(error_reason);

    auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
    auto  rv    = nghttp2_session_mem_recv(session_, bytes, data.size());
    if (rv < 0) {
        set_error(static_cast<int>(rv),
                  std::string("nghttp2_session_mem_recv failed: ") + nghttp2_strerror(static_cast<int>(rv)));
        if (error_reason)
            *error_reason = last_error_;
        return false;
    }

    return drain_send_queue(error_reason);
}

bool Http2Session::drain_send_queue(std::string* error_reason)
{
    if (!session_)
        return true;

    if (has_error_) {
        if (error_reason)
            *error_reason = last_error_;
        return false;
    }

    while (nghttp2_session_want_write(session_) || !pending_send_data().empty()) {
        int rv = nghttp2_session_send(session_);
        if (rv == 0) {
            if (!nghttp2_session_want_write(session_))
                break;
            continue;
        }
        if (rv == NGHTTP2_ERR_WOULDBLOCK)
            break;
        set_error(rv, std::string("nghttp2_session_send failed: ") + nghttp2_strerror(rv));
        if (error_reason)
            *error_reason = last_error_;
        return false;
    }

    return true;
}

bool Http2Session::want_read() const
{
    if (!session_)
        return false;
    return nghttp2_session_want_read(session_) != 0;
}

bool Http2Session::want_write() const
{
    if (!session_)
        return false;
    return nghttp2_session_want_write(session_) != 0 || !pending_send_data().empty();
}

std::span<const std::uint8_t> Http2Session::pending_send_data() const
{
    if (send_offset_ >= send_buffer_.size())
        return {};
    return std::span<const std::uint8_t>(send_buffer_.data() + send_offset_, send_buffer_.size() - send_offset_);
}

std::vector<std::uint8_t> Http2Session::take_send_buffer()
{
    std::vector<std::uint8_t> output;
    if (send_offset_ < send_buffer_.size()) {
        output.assign(send_buffer_.begin() + static_cast<std::ptrdiff_t>(send_offset_), send_buffer_.end());
    }
    send_buffer_.clear();
    send_offset_ = 0;
    return output;
}

void Http2Session::consume_send_data(std::size_t length)
{
    auto available = pending_send_data().size();
    length         = std::min(length, available);
    send_offset_ += length;
    if (send_offset_ >= send_buffer_.size()) {
        send_buffer_.clear();
        send_offset_ = 0;
    }
}

int Http2Session::submit_settings(std::span<const nghttp2_settings_entry> entries)
{
    if (!ensure_session())
        return NGHTTP2_ERR_INVALID_STATE;
    int rv = nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, entries.data(), entries.size());
    if (rv != 0)
        set_error(rv, std::string("nghttp2_submit_settings failed: ") + nghttp2_strerror(rv));
    else
        drain_send_queue(nullptr);
    return rv;
}

int Http2Session::submit_ping(const std::array<std::uint8_t, 8>& opaque, bool ack)
{
    if (!ensure_session())
        return NGHTTP2_ERR_INVALID_STATE;
    uint8_t flags = static_cast<uint8_t>(ack ? NGHTTP2_FLAG_ACK : NGHTTP2_FLAG_NONE);
    int     rv    = nghttp2_submit_ping(session_, flags, opaque.data());
    if (rv != 0)
        set_error(rv, std::string("nghttp2_submit_ping failed: ") + nghttp2_strerror(rv));
    else
        drain_send_queue(nullptr);
    return rv;
}

int Http2Session::submit_headers(int32_t                     stream_id,
                                 std::span<const nghttp2_nv> headers,
                                 bool                        end_stream,
                                 void*                       stream_user_data)
{
    if (!ensure_session())
        return NGHTTP2_ERR_INVALID_STATE;
    auto flags = static_cast<uint8_t>(end_stream ? NGHTTP2_FLAG_END_STREAM : NGHTTP2_FLAG_NONE);
    int  rv    = nghttp2_submit_headers(session_,
                                    flags,
                                    stream_id,
                                    nullptr,
                                    headers.data(),
                                    headers.size(),
                                    stream_user_data);
    if (rv != 0)
        set_error(rv, std::string("nghttp2_submit_headers failed: ") + nghttp2_strerror(rv));
    else
        drain_send_queue(nullptr);
    return rv;
}

int32_t Http2Session::submit_request(std::span<const nghttp2_nv>  headers,
                                     const nghttp2_data_provider* data_provider,
                                     void*                        stream_user_data)
{
    if (!ensure_session())
        return NGHTTP2_ERR_INVALID_STATE;
    auto*   provider_ptr = const_cast<nghttp2_data_provider*>(data_provider);
    int32_t stream_id    = nghttp2_submit_request(session_,
                                               nullptr,
                                               headers.data(),
                                               headers.size(),
                                               provider_ptr,
                                               stream_user_data);
    if (stream_id < 0)
        set_error(stream_id, std::string("nghttp2_submit_request failed: ") + nghttp2_strerror(stream_id));
    else
        drain_send_queue(nullptr);
    return stream_id;
}

int Http2Session::submit_response(int32_t                      stream_id,
                                  std::span<const nghttp2_nv>  headers,
                                  const nghttp2_data_provider* data_provider)
{
    if (!ensure_session())
        return NGHTTP2_ERR_INVALID_STATE;
    auto* provider_ptr = const_cast<nghttp2_data_provider*>(data_provider);
    int   rv           = nghttp2_submit_response(session_, stream_id, headers.data(), headers.size(), provider_ptr);
    if (rv != 0)
        set_error(rv, std::string("nghttp2_submit_response failed: ") + nghttp2_strerror(rv));
    else
        drain_send_queue(nullptr);
    return rv;
}

int Http2Session::submit_data(int32_t stream_id, nghttp2_data_provider* provider)
{
    if (!ensure_session())
        return NGHTTP2_ERR_INVALID_STATE;
    int rv = nghttp2_submit_data(session_, static_cast<uint8_t>(NGHTTP2_FLAG_NONE), stream_id, provider);
    if (rv != 0)
        set_error(rv, std::string("nghttp2_submit_data failed: ") + nghttp2_strerror(rv));
    else
        drain_send_queue(nullptr);
    return rv;
}

int Http2Session::submit_rst_stream(int32_t stream_id, uint32_t error_code)
{
    if (!ensure_session())
        return NGHTTP2_ERR_INVALID_STATE;
    int rv = nghttp2_submit_rst_stream(session_, static_cast<uint8_t>(NGHTTP2_FLAG_NONE), stream_id, error_code);
    if (rv != 0)
        set_error(rv, std::string("nghttp2_submit_rst_stream failed: ") + nghttp2_strerror(rv));
    else
        drain_send_queue(nullptr);
    return rv;
}

int Http2Session::submit_window_update(int32_t stream_id, int32_t window_size_increment)
{
    if (!ensure_session())
        return NGHTTP2_ERR_INVALID_STATE;
    int rv = nghttp2_submit_window_update(session_,
                                          static_cast<uint8_t>(NGHTTP2_FLAG_NONE),
                                          stream_id,
                                          window_size_increment);
    if (rv != 0)
        set_error(rv, std::string("nghttp2_submit_window_update failed: ") + nghttp2_strerror(rv));
    else
        drain_send_queue(nullptr);
    return rv;
}

int Http2Session::submit_goaway(int32_t last_stream_id, uint32_t error_code, std::string_view debug_data)
{
    if (!ensure_session())
        return NGHTTP2_ERR_INVALID_STATE;
    const uint8_t* debug_ptr = debug_data.empty() ? nullptr : reinterpret_cast<const uint8_t*>(debug_data.data());
    int            rv        = nghttp2_submit_goaway(session_,
                                   static_cast<uint8_t>(NGHTTP2_FLAG_NONE),
                                   last_stream_id,
                                   error_code,
                                   debug_ptr,
                                   debug_data.size());
    if (rv != 0)
        set_error(rv, std::string("nghttp2_submit_goaway failed: ") + nghttp2_strerror(rv));
    else
        drain_send_queue(nullptr);
    return rv;
}

void Http2Session::handle_error(int, std::string_view) { }

int Http2Session::on_begin_headers(const nghttp2_frame*)
{
    return 0;
}

int Http2Session::on_header(const nghttp2_frame*, std::string_view, std::string_view)
{
    return 0;
}

int Http2Session::on_data_chunk(int32_t, std::string_view)
{
    return 0;
}

int Http2Session::on_frame_recv(const nghttp2_frame*)
{
    return 0;
}

int Http2Session::on_stream_close(int32_t, uint32_t)
{
    return 0;
}

nghttp2_ssize Http2Session::send_callback(nghttp2_session*, const uint8_t* data, size_t length, int, void* user_data)
{
    auto* self = static_cast<Http2Session*>(user_data);
    if (!self)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (length == 0)
        return 0;
    auto begin = data;
    auto end   = data + length;
    self->send_buffer_.insert(self->send_buffer_.end(), begin, end);
    return static_cast<nghttp2_ssize>(length);
}

int Http2Session::begin_headers_callback(nghttp2_session*, const nghttp2_frame* frame, void* user_data)
{
    auto* self = static_cast<Http2Session*>(user_data);
    if (!self)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    return self->on_begin_headers(frame);
}

int Http2Session::header_callback(nghttp2_session*,
                                  const nghttp2_frame* frame,
                                  const uint8_t*       name,
                                  size_t               namelen,
                                  const uint8_t*       value,
                                  size_t               valuelen,
                                  uint8_t,
                                  void* user_data)
{
    auto* self = static_cast<Http2Session*>(user_data);
    if (!self)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    std::string_view name_view(reinterpret_cast<const char*>(name), namelen);
    std::string_view value_view(reinterpret_cast<const char*>(value), valuelen);
    return self->on_header(frame, name_view, value_view);
}

int Http2Session::data_chunk_recv_callback(nghttp2_session*,
                                           uint8_t,
                                           int32_t        stream_id,
                                           const uint8_t* data,
                                           size_t         len,
                                           void*          user_data)
{
    auto* self = static_cast<Http2Session*>(user_data);
    if (!self)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    std::string_view payload(reinterpret_cast<const char*>(data), len);
    return self->on_data_chunk(stream_id, payload);
}

int Http2Session::frame_recv_callback(nghttp2_session*, const nghttp2_frame* frame, void* user_data)
{
    auto* self = static_cast<Http2Session*>(user_data);
    if (!self)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    return self->on_frame_recv(frame);
}

int Http2Session::stream_close_callback(nghttp2_session*, int32_t stream_id, uint32_t error_code, void* user_data)
{
    auto* self = static_cast<Http2Session*>(user_data);
    if (!self)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    return self->on_stream_close(stream_id, error_code);
}

void Http2Session::clear_error()
{
    has_error_ = false;
    last_error_.clear();
}

void Http2Session::set_error(int error_code, std::string message)
{
    has_error_  = true;
    last_error_ = std::move(message);
    handle_error(error_code, last_error_);
}

bool Http2Session::ensure_session()
{
    if (session_)
        return true;
    return init();
}

void Http2Session::destroy_session()
{
    if (session_) {
        nghttp2_session_del(session_);
        session_ = nullptr;
    }
    send_buffer_.clear();
    send_offset_ = 0;
}

void Http2HeaderBlock::clear()
{
    entries_.clear();
    nv_cache_.clear();
    dirty_ = false;
}

void Http2HeaderBlock::reserve(std::size_t pair_capacity)
{
    entries_.reserve(pair_capacity);
    nv_cache_.reserve(pair_capacity);
}

void Http2HeaderBlock::add(std::string name, std::string value, uint8_t flags)
{
    entries_.push_back(Entry { std::move(name), std::move(value), flags });
    dirty_ = true;
}

std::span<const nghttp2_nv> Http2HeaderBlock::as_span() const
{
    if (dirty_) {
        nv_cache_.clear();
        nv_cache_.reserve(entries_.size());
        for (const auto& entry : entries_) {
            nv_cache_.push_back(nghttp2_nv {
                reinterpret_cast<uint8_t*>(const_cast<char*>(entry.name.data())),
                reinterpret_cast<uint8_t*>(const_cast<char*>(entry.value.data())),
                entry.name.size(),
                entry.value.size(),
                entry.flags,
            });
        }
        dirty_ = false;
    }
    return std::span<const nghttp2_nv>(nv_cache_.data(), nv_cache_.size());
}

Http2StringDataProvider::Http2StringDataProvider()
{
    provider_.source.ptr    = this;
    provider_.read_callback = &Http2StringDataProvider::read_callback;
}

void Http2StringDataProvider::bind(const std::string* body)
{
    body_   = body;
    offset_ = 0;
}

void Http2StringDataProvider::reset()
{
    body_   = nullptr;
    offset_ = 0;
}

bool Http2StringDataProvider::has_body() const
{
    return body_ && !body_->empty();
}

nghttp2_data_provider* Http2StringDataProvider::provider()
{
    return &provider_;
}

const nghttp2_data_provider* Http2StringDataProvider::provider() const
{
    return &provider_;
}

ssize_t Http2StringDataProvider::read_callback(nghttp2_session*,
                                               int32_t,
                                               uint8_t*             buf,
                                               size_t               length,
                                               uint32_t*            data_flags,
                                               nghttp2_data_source* source,
                                               void*)
{
    auto* self = (source && source->ptr) ? static_cast<Http2StringDataProvider*>(source->ptr) : nullptr;
    if (!self || !self->body_)
        return 0;

    const std::string& body = *self->body_;
    if (self->offset_ >= body.size()) {
        if (data_flags)
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }

    size_t remaining = body.size() - self->offset_;
    size_t to_copy   = std::min(length, remaining);
    if (to_copy > 0)
        std::memcpy(buf, body.data() + self->offset_, to_copy);
    self->offset_ += to_copy;
    if (self->offset_ >= body.size() && data_flags)
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    return static_cast<ssize_t>(to_copy);
}

} // namespace co_wq::net::http
