#pragma once

#include <nghttp2/nghttp2.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace co_wq::net::http {

class Http2Session {
public:
    enum class Mode {
        Server,
        Client,
    };

    explicit Http2Session(Mode mode);
    Http2Session(const Http2Session&)            = delete;
    Http2Session& operator=(const Http2Session&) = delete;
    virtual ~Http2Session();

    bool init();
    void reset();

    bool               has_error() const { return has_error_; }
    const std::string& last_error() const { return last_error_; }

    bool feed(std::string_view data, std::string* error_reason = nullptr);
    bool drain_send_queue(std::string* error_reason = nullptr);

    bool want_read() const;
    bool want_write() const;

    std::span<const std::uint8_t> pending_send_data() const;
    std::vector<std::uint8_t>     take_send_buffer();
    void                          consume_send_data(std::size_t length);

    int     submit_settings(std::span<const nghttp2_settings_entry> entries);
    int     submit_ping(const std::array<std::uint8_t, 8>& opaque, bool ack);
    int     submit_headers(int32_t                     stream_id,
                           std::span<const nghttp2_nv> headers,
                           bool                        end_stream,
                           void*                       stream_user_data = nullptr);
    int32_t submit_request(std::span<const nghttp2_nv>  headers,
                           const nghttp2_data_provider* data_provider    = nullptr,
                           void*                        stream_user_data = nullptr);
    int     submit_response(int32_t                      stream_id,
                            std::span<const nghttp2_nv>  headers,
                            const nghttp2_data_provider* data_provider = nullptr);
    int     submit_data(int32_t stream_id, nghttp2_data_provider* provider);
    int     submit_rst_stream(int32_t stream_id, uint32_t error_code);
    int     submit_window_update(int32_t stream_id, int32_t window_size_increment);
    int     submit_goaway(int32_t last_stream_id, uint32_t error_code, std::string_view debug_data = {});

protected:
    virtual void handle_error(int error_code, std::string_view message);
    virtual int  on_begin_headers(const nghttp2_frame* frame);
    virtual int  on_header(const nghttp2_frame* frame, std::string_view name, std::string_view value);
    virtual int  on_data_chunk(int32_t stream_id, std::string_view data);
    virtual int  on_frame_recv(const nghttp2_frame* frame);
    virtual int  on_stream_close(int32_t stream_id, uint32_t error_code);

private:
    static nghttp2_ssize
               send_callback(nghttp2_session* session, const uint8_t* data, size_t length, int flags, void* user_data);
    static int begin_headers_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data);
    static int header_callback(nghttp2_session*     session,
                               const nghttp2_frame* frame,
                               const uint8_t*       name,
                               size_t               namelen,
                               const uint8_t*       value,
                               size_t               valuelen,
                               uint8_t              flags,
                               void*                user_data);
    static int data_chunk_recv_callback(nghttp2_session* session,
                                        uint8_t          flags,
                                        int32_t          stream_id,
                                        const uint8_t*   data,
                                        size_t           len,
                                        void*            user_data);
    static int frame_recv_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data);
    static int stream_close_callback(nghttp2_session* session, int32_t stream_id, uint32_t error_code, void* user_data);

    void clear_error();
    void set_error(int error_code, std::string message);
    bool ensure_session();
    void destroy_session();

    Mode                       mode_;
    nghttp2_session*           session_ { nullptr };
    nghttp2_session_callbacks* callbacks_ { nullptr };
    std::vector<std::uint8_t>  send_buffer_;
    std::size_t                send_offset_ { 0 };
    bool                       has_error_ { false };
    std::string                last_error_;
};

class Http2HeaderBlock {
public:
    void        clear();
    void        reserve(std::size_t pair_capacity);
    bool        empty() const { return entries_.empty(); }
    std::size_t size() const { return entries_.size(); }

    void                        add(std::string name, std::string value, uint8_t flags = NGHTTP2_NV_FLAG_NONE);
    std::span<const nghttp2_nv> as_span() const;

private:
    struct Entry {
        std::string name;
        std::string value;
        uint8_t     flags { NGHTTP2_NV_FLAG_NONE };
    };

    std::vector<Entry>              entries_;
    mutable std::vector<nghttp2_nv> nv_cache_;
    mutable bool                    dirty_ { true };
};

class Http2StringDataProvider {
public:
    Http2StringDataProvider();

    void bind(const std::string* body);
    void reset();

    bool                         has_body() const;
    nghttp2_data_provider*       provider();
    const nghttp2_data_provider* provider() const;

private:
    static ssize_t read_callback(nghttp2_session*     session,
                                 int32_t              stream_id,
                                 uint8_t*             buf,
                                 size_t               length,
                                 uint32_t*            data_flags,
                                 nghttp2_data_source* source,
                                 void*                user_data);

    const std::string*    body_ { nullptr };
    std::size_t           offset_ { 0 };
    nghttp2_data_provider provider_ {};
};

} // namespace co_wq::net::http
