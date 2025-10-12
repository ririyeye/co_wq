#include "syswork.hpp"
#include "test_sys_stats_logger.hpp"

#include "dns_resolver.hpp"
#include "fd_base.hpp"
#include "tcp_listener.hpp"
#include "tcp_socket.hpp"
#include "timer.hpp"
#if defined(USING_SSL)
#include "tls.hpp"
#endif

#include <llhttp.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if !defined(_WIN32)
#include <csignal>
#include <netdb.h>
#include <sys/socket.h>
#else
#include <ws2tcpip.h>
#endif
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <system_error>

using namespace co_wq;
using namespace std::string_view_literals;

#if !defined(USING_SSL)
#error "co_bili demo requires TLS support (configure WITH USING_SSL=y)"
#endif

using NetFdWorkqueue = net::fd_workqueue<SpinLock, net::epoll_reactor>;

namespace {

static const unsigned char kIndexHtmlData[] = {
#if __has_include("index.html.h")
#include "index.html.h"
#else
#include "index_html.h"
#endif
};

static constexpr std::size_t  kIndexHtmlSize = sizeof(kIndexHtmlData);
static const std::string_view kIndexHtmlView { reinterpret_cast<const char*>(kIndexHtmlData),
                                               kIndexHtmlSize > 0 ? kIndexHtmlSize - 1 : 0 };

struct SharedState {
    std::mutex                            mutex;
    std::string                           json_payload;
    std::string                           last_modified;
    std::chrono::system_clock::time_point updated_at { std::chrono::system_clock::time_point::min() };
    std::chrono::system_clock::time_point progress_updated { std::chrono::system_clock::time_point::min() };
    size_t                                total_items { 0 };
    size_t                                completed_items { 0 };
    bool                                  fetching { false };
};

std::atomic_bool               g_stop { false };
std::atomic<net::os::fd_t>     g_listener_fd { net::os::invalid_fd() };
std::atomic<std::atomic_bool*> g_finished_ptr { nullptr };

std::string to_lower(std::string_view in)
{
    std::string out;
    out.reserve(in.size());
    for (char ch : in)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    return out;
}

std::string format_http_date(std::chrono::system_clock::time_point tp)
{
    using namespace std::chrono;
    static constexpr const char* k_weekdays[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static constexpr const char* k_months[]   = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

    auto    secs = time_point_cast<seconds>(tp);
    auto    tt   = system_clock::to_time_t(secs);
    std::tm tm_buf {};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &tt);
#else
    gmtime_r(&tt, &tm_buf);
#endif
    char buffer[64];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%s, %02d %s %04d %02d:%02d:%02d GMT",
                  k_weekdays[tm_buf.tm_wday % 7],
                  tm_buf.tm_mday,
                  k_months[tm_buf.tm_mon % 12],
                  tm_buf.tm_year + 1900,
                  tm_buf.tm_hour,
                  tm_buf.tm_min,
                  tm_buf.tm_sec);
    return std::string(buffer);
}

std::string format_online_count(int64_t total)
{
    if (total >= 10000) {
        double             value = static_cast<double>(total) / 10000.0;
        std::ostringstream oss;
        if (value >= 100.0)
            oss << std::fixed << std::setprecision(0) << value;
        else if (value >= 10.0)
            oss << std::fixed << std::setprecision(1) << value;
        else
            oss << std::fixed << std::setprecision(2) << value;
        oss << "万+";
        return oss.str();
    }
    if (total >= 1000) {
        int64_t base = (total / 1000) * 1000;
        return std::to_string(base) + "+";
    }
    return std::to_string(total);
}

std::optional<int64_t> parse_online_total_string(std::string_view value)
{
    std::string filtered;
    filtered.reserve(value.size());
    bool has_wan = false;
    bool has_yi  = false;
    for (size_t i = 0; i < value.size();) {
        unsigned char ch = static_cast<unsigned char>(value[i]);
        if (ch <= 0x7F) {
            if (std::isdigit(ch) || ch == '.')
                filtered.push_back(static_cast<char>(ch));
            ++i;
            continue;
        }
        if (value.size() - i >= 3) {
            if (value.compare(i, 3, "\xE4\xB8\x87") == 0) {
                has_wan = true;
                i += 3;
                continue;
            }
            if (value.compare(i, 3, "\xE4\xBA\xBF") == 0) {
                has_yi = true;
                i += 3;
                continue;
            }
        }
        size_t advance = 1;
        if ((ch & 0xE0) == 0xC0)
            advance = 2;
        else if ((ch & 0xF0) == 0xE0)
            advance = 3;
        else if ((ch & 0xF8) == 0xF0)
            advance = 4;
        i += advance;
    }
    if (filtered.empty())
        return std::nullopt;
    try {
        double base  = std::stod(filtered);
        double scale = 1.0;
        if (has_yi)
            scale *= 100000000.0;
        if (has_wan)
            scale *= 10000.0;
        double result = base * scale;
        if (result < 0.0)
            return std::nullopt;
        return static_cast<int64_t>(std::llround(result));
    } catch (...) {
        return std::nullopt;
    }
}

std::string describe_remote_endpoint(const net::tcp_socket<SpinLock>& socket)
{
    sockaddr_storage addr {};
    socklen_t        len = sizeof(addr);
    if (::getpeername(socket.native_handle(), reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        return {};

    char host[NI_MAXHOST] = {};
    char serv[NI_MAXSERV] = {};
    int  rc               = ::getnameinfo(reinterpret_cast<const sockaddr*>(&addr),
                           len,
                           host,
                           sizeof(host),
                           serv,
                           sizeof(serv),
                           NI_NUMERICHOST | NI_NUMERICSERV);
    if (rc != 0)
        return {};

    std::string host_str = host;
    if (addr.ss_family == AF_INET6 && host_str.find(':') != std::string::npos)
        host_str = "[" + host_str + "]";
    return host_str + ":" + std::string(serv);
}

struct SimpleResponse {
    llhttp_t                                     parser {};
    llhttp_settings_t                            settings {};
    std::string                                  current_field;
    std::string                                  current_value;
    std::unordered_map<std::string, std::string> headers;
    std::string                                  body;
    int                                          status_code { 0 };
    bool                                         message_complete { false };

    SimpleResponse()
    {
        llhttp_settings_init(&settings);
        settings.on_header_field          = &SimpleResponse::on_header_field_cb;
        settings.on_header_value          = &SimpleResponse::on_header_value_cb;
        settings.on_header_value_complete = &SimpleResponse::on_header_value_complete_cb;
        settings.on_headers_complete      = &SimpleResponse::on_headers_complete_cb;
        settings.on_body                  = &SimpleResponse::on_body_cb;
        settings.on_message_complete      = &SimpleResponse::on_message_complete_cb;
        llhttp_init(&parser, HTTP_RESPONSE, &settings);
        parser.data = this;
    }

    void reset()
    {
        llhttp_reset(&parser);
        current_field.clear();
        current_value.clear();
        headers.clear();
        body.clear();
        status_code      = 0;
        message_complete = false;
    }

    static int on_header_field_cb(llhttp_t* parser, const char* at, size_t length)
    {
        auto* self = static_cast<SimpleResponse*>(parser->data);
        if (!self->current_value.empty())
            self->store_header();
        self->current_field.append(at, length);
        return 0;
    }

    static int on_header_value_cb(llhttp_t* parser, const char* at, size_t length)
    {
        auto* self = static_cast<SimpleResponse*>(parser->data);
        self->current_value.append(at, length);
        return 0;
    }

    static int on_header_value_complete_cb(llhttp_t* parser)
    {
        auto* self = static_cast<SimpleResponse*>(parser->data);
        self->store_header();
        return 0;
    }

    static int on_headers_complete_cb(llhttp_t* parser)
    {
        auto* self        = static_cast<SimpleResponse*>(parser->data);
        self->status_code = parser->status_code;
        return 0;
    }

    static int on_body_cb(llhttp_t* parser, const char* at, size_t length)
    {
        auto* self = static_cast<SimpleResponse*>(parser->data);
        self->body.append(at, length);
        return 0;
    }

    static int on_message_complete_cb(llhttp_t* parser)
    {
        auto* self             = static_cast<SimpleResponse*>(parser->data);
        self->message_complete = true;
        return 0;
    }

    void store_header()
    {
        if (!current_field.empty())
            headers[to_lower(current_field)] = current_value;
        current_field.clear();
        current_value.clear();
    }
};

#if defined(_WIN32)
static BOOL WINAPI console_ctrl_handler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        g_stop.store(true, std::memory_order_release);
        auto fd = g_listener_fd.exchange(net::os::invalid_fd(), std::memory_order_acq_rel);
        if (fd != net::os::invalid_fd())
            net::os::close_fd(fd);
        if (auto ptr = g_finished_ptr.load(std::memory_order_acquire))
            ptr->store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}
#else
void sigint_handler(int)
{
    g_stop.store(true, std::memory_order_release);
    auto fd = g_listener_fd.exchange(net::os::invalid_fd(), std::memory_order_acq_rel);
    if (fd != net::os::invalid_fd())
        net::os::close_fd(fd);
    if (auto ptr = g_finished_ptr.load(std::memory_order_acquire))
        ptr->store(true, std::memory_order_release);
}
#endif

#if !defined(_WIN32)
static std::string errno_message(int err)
{
    return std::error_code(err, std::generic_category()).message();
}
#else
static std::string errno_message(int err)
{
    char buf[128];
    if (strerror_s(buf, sizeof(buf), err) == 0)
        return std::string(buf);
    return std::string("error") + std::to_string(err);
}
#endif

Task<std::optional<SimpleResponse>, Work_Promise<SpinLock, std::optional<SimpleResponse>>>
http_get(NetFdWorkqueue&                                     fdwq,
         const std::string&                                  host,
         uint16_t                                            port,
         const std::string&                                  target,
         const std::unordered_map<std::string, std::string>& extra_headers,
         bool                                                verify_peer)
{
    auto tls_ctx = net::tls_context::make(net::tls_mode::Client);
    if (SSL_CTX* ctx = tls_ctx.native_handle()) {
        if (verify_peer) {
            SSL_CTX_set_default_verify_paths(ctx);
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        } else {
            SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
        }
    }
    auto tls_socket = fdwq.make_tls_socket(std::move(tls_ctx), net::tls_mode::Client);
    if (SSL* ssl = tls_socket.ssl_handle())
        SSL_set_tlsext_host_name(ssl, host.c_str());
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x10100000L
    if (verify_peer) {
        if (SSL* ssl_handle = tls_socket.ssl_handle())
            SSL_set1_host(ssl_handle, host.c_str());
    }
#endif

    auto&                     tcp_base = tls_socket.underlying();
    net::dns::resolve_options dns_opts;
    dns_opts.family           = tcp_base.family();
    dns_opts.allow_dual_stack = tcp_base.dual_stack();
    auto resolved             = net::dns::resolve_sync(host, port, dns_opts);
    if (!resolved.success) {
        CO_WQ_LOG_ERROR("[bili] dns resolve failed: %s (%d)", resolved.error_message.c_str(), resolved.error_code);
        co_return std::nullopt;
    }

    int connect_rc = co_await tcp_base.connect(reinterpret_cast<const sockaddr*>(&resolved.storage), resolved.length);
    if (connect_rc != 0) {
        CO_WQ_LOG_ERROR("[bili] connect failed: %s", errno_message(errno).c_str());
        co_return std::nullopt;
    }

    int handshake_rc = co_await tls_socket.handshake();
    if (handshake_rc != 0) {
        auto err_stack = co_wq::net::detail::collect_errors();
        CO_WQ_LOG_ERROR("[bili] tls handshake failed: %d (%s)", handshake_rc, err_stack.c_str());
        co_return std::nullopt;
    }

    std::string request;
    request.reserve(256 + extra_headers.size() * 64);
    request.append("GET ");
    request.append(target);
    request.append(" HTTP/1.1\r\nHost: ");
    request.append(host);
    request.append("\r\nUser-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                   "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36\r\n");
    request.append("Accept: application/json\r\n");
    request.append("Accept-Language: zh-CN,zh;q=0.9\r\n");
    request.append("Accept-Encoding: identity\r\n");
    request.append("Connection: close\r\n");
    for (const auto& [key, value] : extra_headers) {
        request.append(key);
        request.append(": ");
        request.append(value);
        request.append("\r\n");
    }
    request.append("\r\n");

    ssize_t sent = co_await tls_socket.send_all(request.data(), request.size());
    if (sent < 0) {
        CO_WQ_LOG_ERROR("[bili] send failed: %s", errno_message(errno).c_str());
        co_return std::nullopt;
    }

    SimpleResponse         response;
    std::array<char, 8192> buffer {};
    while (true) {
        ssize_t n = co_await tls_socket.recv(buffer.data(), buffer.size());
        if (n < 0) {
            CO_WQ_LOG_ERROR("[bili] recv failed: %s", errno_message(errno).c_str());
            co_return std::nullopt;
        }
        if (n == 0)
            break;
        llhttp_errno_t err = llhttp_execute(&response.parser, buffer.data(), static_cast<size_t>(n));
        if (err != HPE_OK) {
            const char* reason = llhttp_get_error_reason(&response.parser);
            if (!reason || *reason == '\0')
                reason = llhttp_errno_name(err);
            CO_WQ_LOG_ERROR("[bili] parse error: %s", reason);
            co_return std::nullopt;
        }
        if (response.message_complete)
            break;
    }
    if (!response.message_complete) {
        llhttp_errno_t finish_err = llhttp_finish(&response.parser);
        if (finish_err != HPE_OK && finish_err != HPE_PAUSED_UPGRADE) {
            const char* reason = llhttp_get_error_reason(&response.parser);
            if (!reason || *reason == '\0')
                reason = llhttp_errno_name(finish_err);
            CO_WQ_LOG_ERROR("[bili] parse error at EOF: %s", reason);
            co_return std::nullopt;
        }
    }

    tls_socket.close();
    co_return response;
}

Task<std::optional<nlohmann::json>, Work_Promise<SpinLock, std::optional<nlohmann::json>>>
http_get_json(NetFdWorkqueue&                                     fdwq,
              const std::string&                                  host,
              uint16_t                                            port,
              const std::string&                                  target,
              const std::unordered_map<std::string, std::string>& headers,
              bool                                                verify_peer)
{
    auto response_opt = co_await http_get(fdwq, host, port, target, headers, verify_peer);
    if (!response_opt.has_value())
        co_return std::nullopt;
    auto response = std::move(*response_opt);
    if (response.status_code != 200) {
        CO_WQ_LOG_WARN("[bili] request %s returned %d", target.c_str(), response.status_code);
        co_return std::nullopt;
    }
    try {
        auto json = nlohmann::json::parse(response.body);
        co_return std::move(json);
    } catch (const std::exception& ex) {
        CO_WQ_LOG_ERROR("[bili] json parse failed for %s: %s", target.c_str(), ex.what());
        co_return std::nullopt;
    }
}

struct RankingEntry {
    std::string bvid;
    std::string title;
    std::string owner_name;
    std::string owner_mid;
    std::string pic;
    int64_t     cid { 0 };
    int64_t     online_total { 0 };
};

Task<std::optional<std::vector<RankingEntry>>, Work_Promise<SpinLock, std::optional<std::vector<RankingEntry>>>>
fetch_ranking(NetFdWorkqueue& fdwq, bool verify_peer)
{
    constexpr const char* host   = "api.bilibili.com";
    constexpr uint16_t    port   = 443;
    constexpr const char* target = "/x/web-interface/ranking/v2?rid=0&type=all";

    auto json_opt = co_await http_get_json(fdwq,
                                           host,
                                           port,
                                           target,
                                           {
                                               { "Referer", "https://www.bilibili.com/v/popular/rank/all"            },
                                               { "Origin",  "https://www.bilibili.com"                               },
                                               { "Cookie",  "buvid3=2D4B09A5-0E5F-4537-9F7C-E293CE7324F7167646infoc" }
    },
                                           verify_peer);
    if (!json_opt.has_value())
        co_return std::nullopt;

    const auto& root = *json_opt;
    if (!root.contains("code") || root["code"].get<int>() != 0) {
        CO_WQ_LOG_WARN("[bili] ranking api returned unexpected payload");
        co_return std::nullopt;
    }
    if (!root.contains("data") || !root["data"].contains("list")) {
        CO_WQ_LOG_WARN("[bili] ranking api missing list field");
        co_return std::nullopt;
    }

    std::vector<RankingEntry> entries;
    for (const auto& item : root["data"]["list"]) {
        if (!item.contains("bvid") || !item.contains("cid") || !item.contains("title") || !item.contains("owner"))
            continue;
        RankingEntry entry;
        entry.bvid = item["bvid"].get<std::string>();
        if (entry.bvid.empty())
            continue;
        try {
            entry.cid = item["cid"].get<int64_t>();
        } catch (...) {
            continue;
        }
        entry.title       = item["title"].get<std::string>();
        const auto& owner = item["owner"];
        if (owner.contains("name"))
            entry.owner_name = owner["name"].get<std::string>();
        if (owner.contains("mid")) {
            try {
                entry.owner_mid = owner["mid"].get<std::string>();
            } catch (...) {
                try {
                    entry.owner_mid = std::to_string(owner["mid"].get<int64_t>());
                } catch (...) {
                    entry.owner_mid.clear();
                }
            }
        }
        if (item.contains("pic"))
            entry.pic = item["pic"].get<std::string>();
        entries.push_back(std::move(entry));
    }
    co_return std::move(entries);
}

Task<void, Work_Promise<SpinLock, void>> fetch_online_counts(NetFdWorkqueue&                     fdwq,
                                                             std::vector<RankingEntry>&          entries,
                                                             bool                                verify_peer,
                                                             const std::shared_ptr<SharedState>& state)
{
    constexpr const char* host = "api.bilibili.com";
    constexpr uint16_t    port = 443;

    size_t       index = 0;
    const size_t total = entries.size();
    for (auto& entry : entries) {
        std::string path        = "/x/player/online/total?bvid=" + entry.bvid + "&cid=" + std::to_string(entry.cid);
        auto        online_json = co_await http_get_json(
            fdwq,
            host,
            port,
            path,
            {
                { "Referer", "https://www.bilibili.com/video/" + entry.bvid           },
                { "Origin",  "https://www.bilibili.com"                               },
                { "Cookie",  "buvid3=2D4B09A5-0E5F-4537-9F7C-E293CE7324F7167646infoc" }
        },
            verify_peer);
        if (online_json.has_value() && online_json->contains("code") && (*online_json)["code"].get<int>() == 0) {
            try {
                const auto& total_field = (*online_json)["data"]["total"];
                if (total_field.is_number_integer()) {
                    entry.online_total = total_field.get<int64_t>();
                } else if (total_field.is_number_float()) {
                    entry.online_total = static_cast<int64_t>(total_field.get<double>());
                } else if (total_field.is_string()) {
                    const std::string str_value = total_field.get<std::string>();
                    if (auto parsed = parse_online_total_string(str_value); parsed.has_value()) {
                        entry.online_total = *parsed;
                    } else {
                        entry.online_total = 0;
                        CO_WQ_LOG_WARN("[bili] online total parse failed for %s: %s",
                                       entry.bvid.c_str(),
                                       str_value.c_str());
                    }
                } else {
                    entry.online_total = 0;
                    CO_WQ_LOG_WARN("[bili] unexpected total type for %s", entry.bvid.c_str());
                }
            } catch (const std::exception& ex) {
                entry.online_total = 0;
                CO_WQ_LOG_WARN("[bili] online total parse failed for %s: %s", entry.bvid.c_str(), ex.what());
            }
        } else {
            entry.online_total = 0;
            if (online_json.has_value()) {
                std::string dump = online_json->dump();
                CO_WQ_LOG_WARN("[bili] online api returned non-zero code for %s: %s", entry.bvid.c_str(), dump.c_str());
            } else {
                CO_WQ_LOG_WARN("[bili] online api request failed for %s", entry.bvid.c_str());
            }
        }
        if (state) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->completed_items  = std::min(index + 1, total);
            state->progress_updated = std::chrono::system_clock::now();
        }
        ++index;
        if (total > 0) {
            double percent = static_cast<double>(index) / static_cast<double>(total) * 100.0;
            CO_WQ_LOG_INFO("[bili] progress %.0f%% (%zu/%zu)", percent, index, total);
        }
        if (g_stop.load(std::memory_order_acquire)) {
            if (state) {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->fetching         = false;
                state->progress_updated = std::chrono::system_clock::now();
            }
            co_return;
        }
        co_await delay_ms(get_sys_timer(), 200);
    }
    if (state) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->fetching         = false;
        state->completed_items  = total;
        state->progress_updated = std::chrono::system_clock::now();
    }
    co_return;
}

nlohmann::json build_result_payload(const std::vector<RankingEntry>& entries)
{
    nlohmann::json result = nlohmann::json::object();
    for (const auto& entry : entries) {
        nlohmann::json node;
        node["title"]        = entry.title;
        node["owner"]        = entry.owner_name;
        node["mid"]          = entry.owner_mid;
        node["pic"]          = entry.pic;
        node["online_count"] = format_online_count(entry.online_total);
        node["count_num"]    = entry.online_total;
        result[entry.bvid]   = std::move(node);
    }
    return result;
}

Task<void, Work_Promise<SpinLock, void>> polling_task(NetFdWorkqueue&              fdwq,
                                                      std::shared_ptr<SharedState> state,
                                                      std::filesystem::path        data_path,
                                                      std::chrono::seconds         interval,
                                                      bool                         verify_peer)
{
    while (!g_stop.load(std::memory_order_acquire)) {
        auto ranking = co_await fetch_ranking(fdwq, verify_peer);
        if (ranking.has_value()) {
            auto entries = std::move(*ranking);
            if (state) {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->fetching         = true;
                state->total_items      = entries.size();
                state->completed_items  = 0;
                state->progress_updated = std::chrono::system_clock::now();
            }
            co_await fetch_online_counts(fdwq, entries, verify_peer, state);
            if (!entries.empty()) {
                auto        json_payload = build_result_payload(entries);
                auto        now          = std::chrono::system_clock::now();
                std::string serialized   = json_payload.dump(2);
                std::string last_mod     = format_http_date(now);
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->json_payload  = serialized;
                    state->last_modified = last_mod;
                    state->updated_at    = now;
                }
                std::error_code ec;
                std::filesystem::create_directories(data_path.parent_path(), ec);
                std::ofstream ofs(data_path, std::ios::binary | std::ios::trunc);
                if (ofs.is_open()) {
                    ofs.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
                    ofs.flush();
                }
                CO_WQ_LOG_INFO("[bili] ranking updated, %zu entries", entries.size());
            }
        } else if (state) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->fetching         = false;
            state->total_items      = 0;
            state->completed_items  = 0;
            state->progress_updated = std::chrono::system_clock::now();
        }
        if (g_stop.load(std::memory_order_acquire))
            break;
        co_await delay_ms(get_sys_timer(), static_cast<int64_t>(interval.count()) * 1000);
    }
    co_return;
}

std::string detect_content_type(const std::filesystem::path& file)
{
    auto ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (ext == ".html" || ext == ".htm")
        return "text/html; charset=utf-8";
    if (ext == ".css")
        return "text/css; charset=utf-8";
    if (ext == ".js")
        return "application/javascript";
    if (ext == ".json")
        return "application/json";
    if (ext == ".png")
        return "image/png";
    if (ext == ".jpg" || ext == ".jpeg")
        return "image/jpeg";
    if (ext == ".svg")
        return "image/svg+xml";
    return "application/octet-stream";
}

std::optional<std::filesystem::path> resolve_static_path(const std::filesystem::path& root,
                                                         std::string_view             request_path)
{
    std::filesystem::path clean = root;
    if (request_path.empty() || request_path == "/")
        clean /= "index.html";
    else {
        std::string segment(request_path);
        if (!segment.empty() && segment.front() == '/')
            segment.erase(segment.begin());
        clean /= segment;
    }
    std::error_code ec;
    auto            canonical = std::filesystem::weakly_canonical(clean, ec);
    if (ec)
        return std::nullopt;
    auto canonical_str = canonical.generic_u8string();
    auto root_str      = root.generic_u8string();
    if (canonical_str.size() < root_str.size() || canonical_str.compare(0, root_str.size(), root_str) != 0)
        return std::nullopt;
    if (!std::filesystem::is_regular_file(canonical, ec))
        return std::nullopt;
    return canonical;
}

// NOTE: socket 以值传递，确保协程帧持有 fd 生命周期，避免调用处作用域结束时被提早关闭。
Task<void, Work_Promise<SpinLock, void>>
handle_client(net::tcp_socket<SpinLock> socket, std::shared_ptr<SharedState> state, std::filesystem::path static_root)
{
    std::string request;
    request.reserve(1024);
    std::array<char, 1024> buffer {};
    while (request.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = co_await socket.recv(buffer.data(), buffer.size());
        if (n <= 0)
            break;
        request.append(buffer.data(), static_cast<size_t>(n));
        if (request.size() > 8192)
            break;
    }

    std::string_view method;
    std::string_view path;
    {
        auto pos = request.find(" ");
        if (pos != std::string::npos)
            method = std::string_view(request.data(), pos);
        auto pos2 = request.find(' ', pos + 1);
        if (pos != std::string::npos && pos2 != std::string::npos)
            path = std::string_view(request.data() + pos + 1, pos2 - pos - 1);
    }

    std::string body;
    std::string headers;
    std::string status_line;
    auto        path_view = path;
    if (!path_view.empty()) {
        auto qpos = path_view.find('?');
        if (qpos != std::string_view::npos)
            path_view = path_view.substr(0, qpos);
    }

    std::string method_str(method);
    if (method_str.empty())
        method_str = "UNKNOWN";
    std::string path_str(path);
    if (path_str.empty())
        path_str = std::string(path_view);
    if (path_str.empty())
        path_str = "/";
    std::string client_desc = describe_remote_endpoint(socket);
    if (client_desc.empty())
        client_desc = "unknown";
    CO_WQ_LOG_INFO("[bili] request %s %s from %s", method_str.c_str(), path_str.c_str(), client_desc.c_str());

    if (method != "GET"sv) {
        status_line = "HTTP/1.1 405 Method Not Allowed\r\n";
        headers     = "Content-Length: 0\r\nConnection: close\r\n\r\n";
    } else if (path_view == "/data.json"sv) {
        std::string payload;
        std::string last_modified;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->json_payload.empty()) {
                payload       = state->json_payload;
                last_modified = state->last_modified;
            }
        }
        if (payload.empty()) {
            status_line = "HTTP/1.1 503 Service Unavailable\r\n";
            body        = "{"
                          "error"
                          ":"
                          "data not ready"
                          "}";
            headers     = "Content-Type: application/json\r\n";
        } else {
            status_line = "HTTP/1.1 200 OK\r\n";
            body        = std::move(payload);
            headers     = "Content-Type: application/json\r\n";
            if (!last_modified.empty()) {
                headers.append("Last-Modified: ");
                headers.append(last_modified);
                headers.append("\r\n");
            }
        }
        headers.append("Cache-Control: no-store\r\n");
        headers.append("Content-Length: ");
        headers.append(std::to_string(body.size()));
        headers.append("\r\nConnection: close\r\n\r\n");
    } else if (path_view == "/progress"sv || path_view == "/progress.json"sv || path_view == "/status"sv) {
        size_t                                total_items { 0 };
        size_t                                completed_items { 0 };
        bool                                  fetching { false };
        std::string                           last_data_time;
        std::string                           last_progress_time;
        std::chrono::system_clock::time_point progress_tp { std::chrono::system_clock::time_point::min() };
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            total_items     = state->total_items;
            completed_items = state->completed_items;
            fetching        = state->fetching;
            progress_tp     = state->progress_updated;
            last_data_time  = state->last_modified;
        }
        if (progress_tp != std::chrono::system_clock::time_point::min())
            last_progress_time = format_http_date(progress_tp);
        double percent = 0.0;
        if (total_items > 0)
            percent = static_cast<double>(completed_items) / static_cast<double>(total_items) * 100.0;
        nlohmann::json status_json;
        status_json["fetching"]       = fetching;
        status_json["completed"]      = completed_items;
        status_json["total"]          = total_items;
        status_json["percent"]        = percent;
        status_json["last_data_time"] = last_data_time;
        status_json["progress_time"]  = last_progress_time;
        body                          = status_json.dump();
        status_line                   = "HTTP/1.1 200 OK\r\n";
        headers                       = "Content-Type: application/json\r\n";
        headers.append("Cache-Control: no-store\r\n");
        headers.append("Content-Length: ");
        headers.append(std::to_string(body.size()));
        headers.append("\r\nConnection: close\r\n\r\n");
    } else if (path_view == "/"sv || path_view == "/index.html"sv) {
        status_line = "HTTP/1.1 200 OK\r\n";
        headers     = "Content-Type: text/html; charset=utf-8\r\n";
        headers.append("Cache-Control: no-store\r\n");
        headers.append("Content-Length: ");
        headers.append(std::to_string(kIndexHtmlView.size()));
        headers.append("\r\nConnection: close\r\n\r\n");
        body.assign(kIndexHtmlView.data(), kIndexHtmlView.size());
    } else {
        auto resolved = resolve_static_path(static_root, path_view);
        if (!resolved.has_value()) {
            status_line = "HTTP/1.1 404 Not Found\r\n";
            body        = "Not found";
            headers     = "Content-Type: text/plain; charset=utf-8\r\n";
            headers.append("Content-Length: ");
            headers.append(std::to_string(body.size()));
            headers.append("\r\nConnection: close\r\n\r\n");
        } else {
            std::ifstream ifs(*resolved, std::ios::binary);
            if (!ifs.is_open()) {
                status_line = "HTTP/1.1 404 Not Found\r\n";
                body        = "Not found";
                headers     = "Content-Type: text/plain; charset=utf-8\r\n";
                headers.append("Content-Length: ");
                headers.append(std::to_string(body.size()));
                headers.append("\r\nConnection: close\r\n\r\n");
            } else {
                std::ostringstream oss;
                oss << ifs.rdbuf();
                body        = oss.str();
                status_line = "HTTP/1.1 200 OK\r\n";
                headers     = "Content-Type: ";
                headers.append(detect_content_type(*resolved));
                headers.append("\r\n");
                if (path_view == "/index.html"sv || path_view.empty()) {
                    std::error_code ec;
                    auto            ftime = std::filesystem::last_write_time(*resolved, ec);
                    if (!ec) {
                        auto sys_time = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
                        headers.append("Last-Modified: ");
                        headers.append(format_http_date(sys_time));
                        headers.append("\r\n");
                    }
                }
                headers.append("Content-Length: ");
                headers.append(std::to_string(body.size()));
                headers.append("\r\nConnection: close\r\n\r\n");
            }
        }
    }

    std::string response_head;
    response_head.reserve(status_line.size() + headers.size());
    response_head.append(status_line);
    response_head.append(headers);
    if (!response_head.empty())
        co_await socket.send_all(response_head.data(), response_head.size());
    if (!body.empty())
        co_await socket.send_all(body.data(), body.size());
    socket.close();
    co_return;
}

Task<void, Work_Promise<SpinLock, void>> server_task(NetFdWorkqueue&              fdwq,
                                                     std::shared_ptr<SharedState> state,
                                                     std::filesystem::path        static_root,
                                                     std::string                  host,
                                                     uint16_t                     port)
{
    net::tcp_listener<SpinLock> listener(fdwq.base(), fdwq.reactor());
    listener.bind_listen(host, port, 128);
    g_listener_fd.store(listener.native_handle(), std::memory_order_release);

    CO_WQ_LOG_INFO("[bili] http server listening on %s:%u", host.c_str(), static_cast<unsigned>(port));

    while (!g_stop.load(std::memory_order_acquire)) {
        int fd = co_await listener.accept();
        if (fd == net::k_accept_fatal) {
            CO_WQ_LOG_ERROR("[bili] accept fatal error, shutting down");
            break;
        }
        if (fd < 0)
            continue;
        auto socket = fdwq.adopt_tcp_socket(fd);
        auto task   = handle_client(std::move(socket), state, static_root);
        post_to(task, fdwq.base());
    }

    listener.close();
    g_listener_fd.store(net::os::invalid_fd(), std::memory_order_release);
    co_return;
}

struct Options {
    std::string           host { "0.0.0.0" };
    uint16_t              port { 8000 };
    std::chrono::seconds  interval { 300 };
    std::filesystem::path static_root { "bilibili-online-ranking" };
    bool                  verify_peer { false };
};

void print_usage(const char* argv0)
{
    std::fprintf(stderr,
                 "Usage: %s [--host HOST] [--port PORT] [--interval SECONDS] [--static-root PATH] [--verify]\n",
                 argv0);
}

std::optional<Options> parse_options(int argc, char* argv[])
{
    Options opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            opts.host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            opts.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--interval" && i + 1 < argc) {
            int value = std::stoi(argv[++i]);
            if (value <= 0)
                value = 300;
            opts.interval = std::chrono::seconds(value);
        } else if (arg == "--static-root" && i + 1 < argc) {
            opts.static_root = std::filesystem::path(argv[++i]);
        } else if (arg == "--verify") {
            opts.verify_peer = true;
        } else if (arg == "--insecure") {
            opts.verify_peer = false;
        } else if (arg == "--help") {
            print_usage(argv[0]);
            return std::nullopt;
        } else {
            print_usage(argv[0]);
            return std::nullopt;
        }
    }
    return opts;
}

Options adjust_port(NetFdWorkqueue& fdwq, Options opts)
{
    const uint16_t max_tries = 50;
    for (uint16_t attempt = 0; attempt < max_tries; ++attempt) {
        try {
            net::tcp_listener<SpinLock> probe(fdwq.base(), fdwq.reactor());
            probe.bind_listen(opts.host, opts.port, 1);
            probe.close();
            CO_WQ_LOG_INFO("[bili] using port %u", static_cast<unsigned>(opts.port));
            return opts;
        } catch (const std::exception&) {
            ++opts.port;
        }
    }
    throw std::runtime_error("no available port in range");
}

} // namespace

int main(int argc, char* argv[])
{
    auto opts_opt = parse_options(argc, argv);
    if (!opts_opt.has_value())
        return 1;
    auto opts = std::move(*opts_opt);

#if defined(_WIN32)
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    std::signal(SIGINT, sigint_handler);
    std::signal(SIGTERM, sigint_handler);
#endif

    co_wq::test::SysStatsLogger stats_logger("bili_ranking");

    auto&          wq = get_sys_workqueue(0);
    NetFdWorkqueue fdwq(wq);

    try {
        opts = adjust_port(fdwq, opts);
    } catch (const std::exception& ex) {
        CO_WQ_LOG_ERROR("[bili] failed to find available port: %s", ex.what());
        return 1;
    }

    std::error_code ec;
    auto            static_root = std::filesystem::absolute(opts.static_root, ec);
    if (!ec) {
        auto canonical_root = std::filesystem::weakly_canonical(static_root, ec);
        if (!ec)
            static_root = canonical_root;
    } else {
        static_root = opts.static_root;
    }
    std::error_code       cwd_ec;
    auto                  cwd = std::filesystem::current_path(cwd_ec);
    std::filesystem::path data_path;
    if (!cwd_ec)
        data_path = cwd / "data.json";
    else
        data_path = static_root / "data.json";

    auto state = std::make_shared<SharedState>();

    auto poll_task = polling_task(fdwq, state, data_path, opts.interval, opts.verify_peer);
    auto srv_task  = server_task(fdwq, state, static_root, opts.host, opts.port);

    auto             poll_coroutine = poll_task.get();
    auto&            poll_promise   = poll_coroutine.promise();
    auto             srv_coroutine  = srv_task.get();
    auto&            srv_promise    = srv_coroutine.promise();
    std::atomic_int  pending { 2 };
    std::atomic_bool finished { false };
    struct CompletionState {
        std::atomic_int*  pending;
        std::atomic_bool* finished;
    } completion { &pending, &finished };

    g_finished_ptr.store(&finished, std::memory_order_release);

    poll_promise.mUserData    = &completion;
    poll_promise.mOnCompleted = [](Promise_base& pb) {
        auto* state = static_cast<CompletionState*>(pb.mUserData);
        if (!state || !state->pending || !state->finished)
            return;
        if (state->pending->fetch_sub(1, std::memory_order_acq_rel) == 1)
            state->finished->store(true, std::memory_order_release);
    };
    srv_promise.mUserData    = &completion;
    srv_promise.mOnCompleted = [](Promise_base& pb) {
        auto* state = static_cast<CompletionState*>(pb.mUserData);
        if (!state || !state->pending || !state->finished)
            return;
        if (state->pending->fetch_sub(1, std::memory_order_acq_rel) == 1)
            state->finished->store(true, std::memory_order_release);
    };

    post_to(poll_task, wq);
    post_to(srv_task, wq);

    sys_wait_until(finished);
    g_finished_ptr.store(nullptr, std::memory_order_release);

    return 0;
}
