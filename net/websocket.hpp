#pragma once

#include "worker.hpp"

#include <llhttp.h>
#include <openssl/evp.h>

#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace co_wq::net::websocket {

namespace detail {
    inline std::string to_lower(std::string_view input)
    {
        std::string out;
        out.reserve(input.size());
        for (char ch : input) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
        return out;
    }

    inline std::string trim(std::string_view input)
    {
        size_t begin = 0;
        size_t end   = input.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(input[begin])))
            ++begin;
        while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])))
            --end;
        return std::string(input.substr(begin, end - begin));
    }

    inline std::string build_http_response(int              status_code,
                                           std::string_view reason,
                                           std::string_view body,
                                           std::string_view content_type = "text/plain; charset=utf-8")
    {
        std::string response;
        response.reserve(128 + body.size());
        response.append("HTTP/1.1 ");
        response.append(std::to_string(status_code));
        response.push_back(' ');
        response.append(reason);
        response.append("\r\n");
        response.append("Content-Type: ");
        response.append(content_type);
        response.append("\r\n");
        response.append("Content-Length: ");
        response.append(std::to_string(body.size()));
        response.append("\r\nConnection: close\r\n\r\n");
        response.append(body);
        return response;
    }

    inline std::string compute_accept_key(std::string_view client_key)
    {
        static constexpr char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        unsigned char         digest[EVP_MAX_MD_SIZE];
        unsigned int          digest_len = 0;

        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (!ctx)
            return {};

        if (EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr) != 1) {
            EVP_MD_CTX_free(ctx);
            return {};
        }
        if (EVP_DigestUpdate(ctx, client_key.data(), client_key.size()) != 1
            || EVP_DigestUpdate(ctx, guid, sizeof(guid) - 1) != 1
            || EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
            EVP_MD_CTX_free(ctx);
            return {};
        }
        EVP_MD_CTX_free(ctx);

        std::string out;
        out.resize(EVP_ENCODE_LENGTH(digest_len));
        int encoded_len = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), digest, digest_len);
        if (encoded_len < 0)
            return {};
        out.resize(static_cast<size_t>(encoded_len));
        return out;
    }

    inline bool connection_contains_upgrade(std::string_view connection_header)
    {
        size_t start = 0;
        while (start < connection_header.size()) {
            size_t end = connection_header.find(',', start);
            if (end == std::string_view::npos)
                end = connection_header.size();
            std::string token = to_lower(trim(connection_header.substr(start, end - start)));
            if (token == "upgrade")
                return true;
            start = end + 1;
        }
        return false;
    }

    struct HttpRequestContext {
        std::string                                  method;
        std::string                                  url;
        std::unordered_map<std::string, std::string> headers;
        std::string                                  current_field;
        std::string                                  current_value;
        std::string                                  body;
        std::string                                  raw_data;
        bool                                         headers_complete { false };
        bool                                         message_complete { false };

        void reset()
        {
            method.clear();
            url.clear();
            headers.clear();
            current_field.clear();
            current_value.clear();
            body.clear();
            raw_data.clear();
            headers_complete = false;
            message_complete = false;
        }
    };

    inline void install_http_settings(llhttp_settings_t& settings)
    {
        llhttp_settings_init(&settings);
        settings.on_message_begin = [](llhttp_t* parser) -> int {
            auto* ctx = static_cast<HttpRequestContext*>(parser->data);
            if (ctx)
                ctx->reset();
            return 0;
        };
        settings.on_method = [](llhttp_t* parser, const char* at, size_t length) -> int {
            auto* ctx = static_cast<HttpRequestContext*>(parser->data);
            ctx->method.append(at, length);
            return 0;
        };
        settings.on_url = [](llhttp_t* parser, const char* at, size_t length) -> int {
            auto* ctx = static_cast<HttpRequestContext*>(parser->data);
            ctx->url.append(at, length);
            return 0;
        };
        settings.on_header_field = [](llhttp_t* parser, const char* at, size_t length) -> int {
            auto* ctx = static_cast<HttpRequestContext*>(parser->data);
            ctx->current_field.append(at, length);
            return 0;
        };
        settings.on_header_value = [](llhttp_t* parser, const char* at, size_t length) -> int {
            auto* ctx = static_cast<HttpRequestContext*>(parser->data);
            ctx->current_value.append(at, length);
            return 0;
        };
        settings.on_header_value_complete = [](llhttp_t* parser) -> int {
            auto* ctx = static_cast<HttpRequestContext*>(parser->data);
            if (!ctx->current_field.empty()) {
                ctx->headers[to_lower(ctx->current_field)] = ctx->current_value;
            }
            ctx->current_field.clear();
            ctx->current_value.clear();
            return 0;
        };
        settings.on_headers_complete = [](llhttp_t* parser) -> int {
            auto* ctx = static_cast<HttpRequestContext*>(parser->data);
            if (!ctx->current_field.empty()) {
                ctx->headers[to_lower(ctx->current_field)] = ctx->current_value;
                ctx->current_field.clear();
                ctx->current_value.clear();
            }
            ctx->headers_complete = true;
            return 0;
        };
        settings.on_body = [](llhttp_t* parser, const char* at, size_t length) -> int {
            auto* ctx = static_cast<HttpRequestContext*>(parser->data);
            ctx->body.append(at, length);
            return 0;
        };
        settings.on_message_complete = [](llhttp_t* parser) -> int {
            auto* ctx             = static_cast<HttpRequestContext*>(parser->data);
            ctx->message_complete = true;
            return 0;
        };
    }

} // namespace detail

enum class opcode : std::uint8_t {
    continuation = 0x0,
    text         = 0x1,
    binary       = 0x2,
    close        = 0x8,
    ping         = 0x9,
    pong         = 0xA,
};

struct server_options {
    std::vector<std::string> subprotocols;
    std::vector<std::string> allowed_origins;
    bool                     check_origin { false };
    bool                     close_on_failure { true };
    std::size_t              max_request_size { 16 * 1024 };
};

struct accept_result {
    bool                                         ok { false };
    int                                          status_code { 500 };
    std::string                                  reason { "Internal Server Error" };
    std::string                                  selected_subprotocol;
    detail::HttpRequestContext                   request;
    std::unordered_map<std::string, std::string> response_headers;
};

struct frame {
    bool                      fin { true };
    bool                      rsv1 { false };
    bool                      rsv2 { false };
    bool                      rsv3 { false };
    opcode                    op { opcode::text };
    bool                      masked { false };
    std::uint32_t             masking_key { 0 };
    std::vector<std::uint8_t> payload;
};

struct frame_result {
    bool  ok { false };
    int   error { 0 };
    frame value;
};

struct frame_read_options {
    std::size_t max_frame_size { 1 << 20 };
    bool        require_masked_client { true };
    bool        auto_unmask { true };
};

struct message {
    opcode                    op { opcode::text };
    std::vector<std::uint8_t> payload;
    bool                      closed { false };
    std::uint16_t             close_code { 1000 };
    std::string               close_reason;
};

struct message_result {
    bool    ok { false };
    int     error { 0 };
    message value;
};

struct message_read_options : frame_read_options {
    bool respond_to_ping { true };
};

struct send_options {
    bool                         fin { true };
    bool                         mask { false };
    std::optional<std::uint32_t> masking_key {};
};

namespace detail {
    inline std::string pick_subprotocol(const std::vector<std::string>& client_protocols,
                                        const std::vector<std::string>& server_protocols)
    {
        for (const auto& sp : client_protocols) {
            for (const auto& supported : server_protocols) {
                if (detail::to_lower(sp) == detail::to_lower(supported))
                    return supported;
            }
        }
        return {};
    }

    inline std::vector<std::string> parse_header_csv(std::string_view value)
    {
        std::vector<std::string> out;
        size_t                   start = 0;
        while (start < value.size()) {
            size_t end = value.find(',', start);
            if (end == std::string_view::npos)
                end = value.size();
            out.emplace_back(trim(value.substr(start, end - start)));
            start = end + 1;
        }
        return out;
    }
} // namespace detail

template <lockable lock, class Socket>
Task<accept_result, Work_Promise<lock, accept_result>> accept(Socket& sock, const server_options& opts = {})
{
    detail::HttpRequestContext ctx;
    llhttp_settings_t          settings;
    detail::install_http_settings(settings);

    llhttp_t parser;
    llhttp_init(&parser, HTTP_REQUEST, &settings);
    parser.data = &ctx;

    accept_result result;
    result.status_code = 400;
    result.reason      = "Bad Request";

    std::array<char, 4096> buffer {};
    std::size_t            total_bytes = 0;
    std::string            parse_error_reason;
    bool                   parse_error         = false;
    bool                   client_closed_early = false;

    while (!ctx.message_complete && !parse_error) {
        auto n = co_await sock.recv(buffer.data(), buffer.size());
        if (n < 0) {
            parse_error        = true;
            parse_error_reason = "socket read error";
            result.status_code = 500;
            result.reason      = "Socket Error";
            result.response_headers.clear();
            break;
        }
        if (n == 0) {
            parse_error         = true;
            client_closed_early = true;
            parse_error_reason  = "client closed connection";
            result.status_code  = 0;
            result.reason       = "client closed connection";
            result.response_headers.clear();
            break;
        }
        ctx.raw_data.append(buffer.data(), static_cast<std::size_t>(n));
        total_bytes += static_cast<std::size_t>(n);
        if (total_bytes > opts.max_request_size) {
            parse_error        = true;
            parse_error_reason = "request header too large";
            result.status_code = 431;
            result.reason      = "Request Header Fields Too Large";
            break;
        }
        llhttp_errno_t err = llhttp_execute(&parser, buffer.data(), static_cast<size_t>(n));
        if (err == HPE_PAUSED_UPGRADE) {
            llhttp_resume_after_upgrade(&parser);
            ctx.message_complete = true;
            break;
        }
        if (err != HPE_OK) {
            parse_error        = true;
            parse_error_reason = llhttp_errno_name(err);
            result.status_code = 400;
            result.reason      = "Bad Request";
            break;
        }
    }

    if (parse_error || !ctx.message_complete) {
        if (!parse_error_reason.empty())
            result.reason = parse_error_reason;

        if (!client_closed_early && result.status_code != 0) {
            std::string body = "WebSocket handshake failed: "
                + (parse_error_reason.empty() ? std::string("incomplete request") : parse_error_reason) + "\n";
            std::string response = detail::build_http_response(result.status_code, result.reason, body);
            (void)co_await sock.send_all(response.data(), response.size());
        }

        if (opts.close_on_failure || client_closed_early)
            sock.close();

        result.ok      = false;
        result.request = std::move(ctx);
        co_return result;
    }

    result.request = std::move(ctx);

    auto find_header = [&](std::string_view key) -> std::optional<std::string> {
        auto it = result.request.headers.find(std::string(key));
        if (it == result.request.headers.end())
            return std::nullopt;
        return it->second;
    };

    if (detail::to_lower(result.request.method) != "get") {
        std::string response = detail::build_http_response(405, "Method Not Allowed", "WebSocket requires GET\n");
        (void)co_await sock.send_all(response.data(), response.size());
        if (opts.close_on_failure)
            sock.close();
        result.ok          = false;
        result.status_code = 405;
        result.reason      = "Method Not Allowed";
        co_return result;
    }

    auto upgrade_hdr    = find_header("upgrade");
    auto connection_hdr = find_header("connection");
    auto version_hdr    = find_header("sec-websocket-version");
    auto key_hdr        = find_header("sec-websocket-key");

    if (!upgrade_hdr || detail::to_lower(*upgrade_hdr) != "websocket" || !connection_hdr
        || !detail::connection_contains_upgrade(*connection_hdr) || !version_hdr || detail::trim(*version_hdr) != "13"
        || !key_hdr) {
        std::string response = detail::build_http_response(400,
                                                           "Bad Request",
                                                           "Missing or invalid WebSocket headers\n");
        (void)co_await sock.send_all(response.data(), response.size());
        if (opts.close_on_failure)
            sock.close();
        result.ok          = false;
        result.status_code = 400;
        result.reason      = "Bad Request";
        co_return result;
    }

    if (opts.check_origin && !opts.allowed_origins.empty()) {
        auto origin_hdr = find_header("origin");
        bool origin_ok  = false;
        if (origin_hdr) {
            for (const auto& allowed : opts.allowed_origins) {
                if (detail::to_lower(*origin_hdr) == detail::to_lower(allowed)) {
                    origin_ok = true;
                    break;
                }
            }
        }
        if (!origin_ok) {
            std::string response = detail::build_http_response(403, "Forbidden", "Origin not allowed\n");
            (void)co_await sock.send_all(response.data(), response.size());
            if (opts.close_on_failure)
                sock.close();
            result.ok          = false;
            result.status_code = 403;
            result.reason      = "Forbidden";
            co_return result;
        }
    }

    std::vector<std::string> client_protocols;
    if (auto proto_hdr = find_header("sec-websocket-protocol")) {
        client_protocols = detail::parse_header_csv(*proto_hdr);
    }
    std::string selected_proto;
    if (!opts.subprotocols.empty() && !client_protocols.empty()) {
        selected_proto = detail::pick_subprotocol(client_protocols, opts.subprotocols);
    }

    std::string accept_key = detail::compute_accept_key(*key_hdr);
    if (accept_key.empty()) {
        std::string response = detail::build_http_response(500,
                                                           "Internal Server Error",
                                                           "Failed to compute accept key\n");
        (void)co_await sock.send_all(response.data(), response.size());
        if (opts.close_on_failure)
            sock.close();
        result.ok          = false;
        result.status_code = 500;
        result.reason      = "Internal Server Error";
        co_return result;
    }

    std::string response;
    response.reserve(256);
    response.append("HTTP/1.1 101 Switching Protocols\r\n");
    response.append("Upgrade: websocket\r\n");
    response.append("Connection: Upgrade\r\n");
    response.append("Sec-WebSocket-Accept: ");
    response.append(accept_key);
    response.append("\r\n");
    if (!selected_proto.empty()) {
        response.append("Sec-WebSocket-Protocol: ");
        response.append(selected_proto);
        response.append("\r\n");
    }
    for (const auto& header : result.response_headers) {
        response.append(header.first);
        response.append(": ");
        response.append(header.second);
        response.append("\r\n");
    }
    response.append("\r\n");

    auto sent = co_await sock.send_all(response.data(), response.size());
    if (sent < 0) {
        if (opts.close_on_failure)
            sock.close();
        result.ok          = false;
        result.status_code = 500;
        result.reason      = "Socket Error";
        co_return result;
    }

    result.ok                   = true;
    result.status_code          = 101;
    result.reason               = "Switching Protocols";
    result.selected_subprotocol = std::move(selected_proto);
    co_return result;
}

template <lockable lock, class Socket>
Task<frame_result, Work_Promise<lock, frame_result>> read_frame(Socket& sock, const frame_read_options& opts = {})
{
    frame_result                result;
    std::array<std::uint8_t, 2> header_buf {};
    std::size_t                 read_bytes = 0;

    while (read_bytes < header_buf.size()) {
        auto n = co_await sock.recv(header_buf.data() + read_bytes, header_buf.size() - read_bytes);
        if (n <= 0) {
            result.ok    = false;
            result.error = (n == 0) ? -ECONNRESET : static_cast<int>(n);
            co_return result;
        }
        read_bytes += static_cast<std::size_t>(n);
    }

    bool          fin         = (header_buf[0] & 0x80) != 0;
    bool          rsv1        = (header_buf[0] & 0x40) != 0;
    bool          rsv2        = (header_buf[0] & 0x20) != 0;
    bool          rsv3        = (header_buf[0] & 0x10) != 0;
    opcode        op          = static_cast<opcode>(header_buf[0] & 0x0F);
    bool          mask        = (header_buf[1] & 0x80) != 0;
    std::uint64_t payload_len = header_buf[1] & 0x7F;

    if (rsv1 || rsv2 || rsv3) {
        result.ok    = false;
        result.error = -EPROTO;
        co_return result;
    }

    if (payload_len == 126) {
        std::array<std::uint8_t, 2> ext16 {};
        std::size_t                 idx = 0;
        while (idx < ext16.size()) {
            auto n = co_await sock.recv(ext16.data() + idx, ext16.size() - idx);
            if (n <= 0) {
                result.ok    = false;
                result.error = (n == 0) ? -ECONNRESET : static_cast<int>(n);
                co_return result;
            }
            idx += static_cast<std::size_t>(n);
        }
        payload_len = (static_cast<std::uint64_t>(ext16[0]) << 8) | static_cast<std::uint64_t>(ext16[1]);
    } else if (payload_len == 127) {
        std::array<std::uint8_t, 8> ext64 {};
        std::size_t                 idx = 0;
        while (idx < ext64.size()) {
            auto n = co_await sock.recv(ext64.data() + idx, ext64.size() - idx);
            if (n <= 0) {
                result.ok    = false;
                result.error = (n == 0) ? -ECONNRESET : static_cast<int>(n);
                co_return result;
            }
            idx += static_cast<std::size_t>(n);
        }
        payload_len = 0;
        for (std::uint8_t byte : ext64) {
            payload_len = (payload_len << 8) | byte;
        }
    }

    if (payload_len > opts.max_frame_size) {
        result.ok    = false;
        result.error = -EMSGSIZE;
        co_return result;
    }

    std::uint32_t masking_key   = 0;
    bool          original_mask = mask;
    if (mask) {
        std::array<std::uint8_t, 4> mask_buf {};
        std::size_t                 idx = 0;
        while (idx < mask_buf.size()) {
            auto n = co_await sock.recv(mask_buf.data() + idx, mask_buf.size() - idx);
            if (n <= 0) {
                result.ok    = false;
                result.error = (n == 0) ? -ECONNRESET : static_cast<int>(n);
                co_return result;
            }
            idx += static_cast<std::size_t>(n);
        }
        masking_key = (static_cast<std::uint32_t>(mask_buf[0]) << 24) | (static_cast<std::uint32_t>(mask_buf[1]) << 16)
            | (static_cast<std::uint32_t>(mask_buf[2]) << 8) | static_cast<std::uint32_t>(mask_buf[3]);
    } else if (opts.require_masked_client) {
        result.ok    = false;
        result.error = -EPROTO;
        co_return result;
    }

    std::vector<std::uint8_t> payload;
    payload.resize(static_cast<std::size_t>(payload_len));
    std::size_t received = 0;
    while (received < payload.size()) {
        auto n = co_await sock.recv(reinterpret_cast<char*>(payload.data()) + received, payload.size() - received);
        if (n <= 0) {
            result.ok    = false;
            result.error = (n == 0) ? -ECONNRESET : static_cast<int>(n);
            co_return result;
        }
        received += static_cast<std::size_t>(n);
    }

    std::uint32_t saved_masking_key = masking_key;
    if (mask && opts.auto_unmask) {
        std::array<std::uint8_t, 4> mask_bytes { static_cast<std::uint8_t>((masking_key >> 24) & 0xFF),
                                                 static_cast<std::uint8_t>((masking_key >> 16) & 0xFF),
                                                 static_cast<std::uint8_t>((masking_key >> 8) & 0xFF),
                                                 static_cast<std::uint8_t>(masking_key & 0xFF) };
        for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] ^= mask_bytes[i % 4];
        }
        mask        = false;
        masking_key = 0;
    }

    result.ok                = true;
    result.value.fin         = fin;
    result.value.rsv1        = rsv1;
    result.value.rsv2        = rsv2;
    result.value.rsv3        = rsv3;
    result.value.op          = op;
    result.value.masked      = original_mask;
    result.value.masking_key = original_mask ? saved_masking_key : 0;
    result.value.payload     = std::move(payload);
    co_return result;
}

template <lockable lock, class Socket>
Task<void, Work_Promise<lock, void>>
send_frame(Socket& sock, opcode op, std::span<const std::uint8_t> payload, const send_options& opts = {})
{
    std::array<std::uint8_t, 14> header {};
    std::size_t                  header_len = 0;

    header[0] = static_cast<std::uint8_t>(opts.fin ? 0x80 : 0x00) | static_cast<std::uint8_t>(op);
    ++header_len;

    std::uint8_t mask_bit    = opts.mask ? 0x80 : 0x00;
    std::size_t  payload_len = payload.size();
    if (payload_len <= 125) {
        header[1] = mask_bit | static_cast<std::uint8_t>(payload_len);
        header_len += 1;
    } else if (payload_len <= 0xFFFF) {
        header[1] = mask_bit | 126;
        header[2] = static_cast<std::uint8_t>((payload_len >> 8) & 0xFF);
        header[3] = static_cast<std::uint8_t>(payload_len & 0xFF);
        header_len += 3;
    } else {
        header[1] = mask_bit | 127;
        for (int i = 0; i < 8; ++i) {
            header[2 + i] = static_cast<std::uint8_t>((static_cast<std::uint64_t>(payload_len) >> (56 - i * 8)) & 0xFF);
        }
        header_len += 9;
    }

    std::array<std::uint8_t, 4> mask_bytes { 0, 0, 0, 0 };
    if (opts.mask) {
        std::uint32_t mask_key = opts.masking_key.value_or(std::random_device {}());
        mask_bytes             = { static_cast<std::uint8_t>((mask_key >> 24) & 0xFF),
                                   static_cast<std::uint8_t>((mask_key >> 16) & 0xFF),
                                   static_cast<std::uint8_t>((mask_key >> 8) & 0xFF),
                                   static_cast<std::uint8_t>(mask_key & 0xFF) };
        std::memcpy(header.data() + header_len, mask_bytes.data(), mask_bytes.size());
        header_len += mask_bytes.size();
    }

    auto sent = co_await sock.send_all(reinterpret_cast<const char*>(header.data()), header_len);
    if (sent < 0)
        co_return;

    if (payload.empty())
        co_return;

    if (opts.mask) {
        std::vector<std::uint8_t> masked(payload.begin(), payload.end());
        for (std::size_t i = 0; i < masked.size(); ++i)
            masked[i] ^= mask_bytes[i % 4];
        (void)co_await sock.send_all(reinterpret_cast<const char*>(masked.data()), masked.size());
    } else {
        (void)co_await sock.send_all(reinterpret_cast<const char*>(payload.data()), payload.size());
    }
    co_return;
}

template <lockable lock, class Socket>
Task<message_result, Work_Promise<lock, message_result>> read_message(Socket&                     sock,
                                                                      const message_read_options& opts = {})
{
    message_result            res;
    message                   msg;
    std::vector<std::uint8_t> buffer;
    opcode                    current_opcode = opcode::continuation;
    bool                      started        = false;

    for (;;) {
        auto frame_res = co_await read_frame<lock>(sock, opts);
        if (!frame_res.ok) {
            res.ok    = false;
            res.error = frame_res.error;
            co_return res;
        }
        auto& fr = frame_res.value;

        if (fr.op == opcode::ping) {
            if (opts.respond_to_ping) {
                co_await send_frame<lock>(sock, opcode::pong, fr.payload, send_options { .fin = true, .mask = false });
            }
            continue;
        } else if (fr.op == opcode::pong) {
            // ignore
            continue;
        } else if (fr.op == opcode::close) {
            msg.closed = true;
            if (fr.payload.size() >= 2) {
                msg.close_code = static_cast<std::uint16_t>(fr.payload[0] << 8 | fr.payload[1]);
                if (fr.payload.size() > 2)
                    msg.close_reason.assign(reinterpret_cast<const char*>(fr.payload.data() + 2),
                                            fr.payload.size() - 2);
            }
            res.ok    = true;
            res.value = std::move(msg);
            co_return res;
        }

        if (!started) {
            if (fr.op == opcode::continuation) {
                res.ok    = false;
                res.error = -EPROTO;
                co_return res;
            }
            current_opcode = fr.op;
            buffer         = std::move(fr.payload);
            started        = true;
        } else {
            if (fr.op != opcode::continuation) {
                res.ok    = false;
                res.error = -EPROTO;
                co_return res;
            }
            buffer.insert(buffer.end(), fr.payload.begin(), fr.payload.end());
        }

        if (fr.fin) {
            msg.op      = current_opcode;
            msg.payload = std::move(buffer);
            res.ok      = true;
            res.value   = std::move(msg);
            co_return res;
        }
    }
}

template <lockable lock, class Socket>
Task<void, Work_Promise<lock, void>> send_text(Socket& sock, std::string_view text, const send_options& opts = {})
{
    std::span<const std::uint8_t> payload(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    co_await send_frame<lock>(sock, opcode::text, payload, opts);
    co_return;
}

template <lockable lock, class Socket>
Task<void, Work_Promise<lock, void>>
send_binary(Socket& sock, std::span<const std::uint8_t> data, const send_options& opts = {})
{
    co_await send_frame<lock>(sock, opcode::binary, data, opts);
    co_return;
}

template <lockable lock, class Socket>
Task<void, Work_Promise<lock, void>>
send_close(Socket& sock, std::uint16_t code = 1000, std::string_view reason = {}, const send_options& opts = {})
{
    std::vector<std::uint8_t> payload;
    payload.resize(2 + reason.size());
    payload[0] = static_cast<std::uint8_t>((code >> 8) & 0xFF);
    payload[1] = static_cast<std::uint8_t>(code & 0xFF);
    if (!reason.empty()) {
        std::memcpy(payload.data() + 2, reason.data(), reason.size());
    }
    co_await send_frame<lock>(sock, opcode::close, payload, opts);
    co_return;
}

template <lockable lock, class Socket>
Task<void, Work_Promise<lock, void>>
send_pong(Socket& sock, std::span<const std::uint8_t> payload = {}, const send_options& opts = {})
{
    co_await send_frame<lock>(sock, opcode::pong, payload, opts);
    co_return;
}

template <lockable lock, class Socket>
Task<void, Work_Promise<lock, void>>
send_ping(Socket& sock, std::span<const std::uint8_t> payload = {}, const send_options& opts = {})
{
    co_await send_frame<lock>(sock, opcode::ping, payload, opts);
    co_return;
}

} // namespace co_wq::net::websocket
