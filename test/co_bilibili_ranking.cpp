#include "co_syswork.hpp"
#include "co_test_sys_stats_logger.hpp"

#include "dns_resolver.hpp"
#include "fd_base.hpp"
#include "tcp_listener.hpp"
#include "tcp_socket.hpp"
#include "timer.hpp"
#include "worker.hpp"
#if defined(USING_SSL)
#include "tls.hpp"
#endif

#include "semaphore.hpp"

#include "http/http_client.hpp"
#include "http/http_easy_server.hpp"

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

#include <limits>
#include <system_error>

using namespace co_wq;
using namespace std::string_view_literals;

#if !defined(USING_SSL)
#error "co_bili demo requires TLS support (configure WITH USING_SSL=y)"
#endif

using NetFdWorkqueue = net::fd_workqueue<SpinLock, net::epoll_reactor>;
namespace http       = co_wq::net::http;
using http::Http1ResponseParser;
using http::HttpEasyServer;
using http::HttpRequest;
using http::HttpResponse;

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
    bool                                  initial_fetch_done { false };
};

std::atomic_bool               g_stop { false };
std::atomic<std::atomic_bool*> g_finished_ptr { nullptr };
std::atomic<HttpEasyServer*>   g_server_ptr { nullptr };

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

#if defined(_WIN32)
static BOOL WINAPI console_ctrl_handler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        CO_WQ_LOG_INFO("[bili] console control signal=%lu", static_cast<unsigned long>(type));
        g_stop.store(true, std::memory_order_release);
        if (auto* srv = g_server_ptr.load(std::memory_order_acquire))
            srv->request_stop();
        if (auto ptr = g_finished_ptr.load(std::memory_order_acquire))
            ptr->store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}
#else
void sigint_handler(int)
{
    CO_WQ_LOG_INFO("[bili] console control signal caught");
    g_stop.store(true, std::memory_order_release);
    if (auto* srv = g_server_ptr.load(std::memory_order_acquire))
        srv->request_stop();
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

Task<std::optional<Http1ResponseParser>, Work_Promise<SpinLock, std::optional<Http1ResponseParser>>>
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

    Http1ResponseParser response;
    response.buffer_body = true;
    std::array<char, 8192> buffer {};
    std::string            parse_error;
    while (true) {
        ssize_t n = co_await tls_socket.recv(buffer.data(), buffer.size());
        if (n < 0) {
            CO_WQ_LOG_ERROR("[bili] recv failed: %s", errno_message(errno).c_str());
            co_return std::nullopt;
        }
        if (n == 0)
            break;
        if (!response.feed(std::string_view(buffer.data(), static_cast<size_t>(n)), &parse_error)) {
            CO_WQ_LOG_ERROR("[bili] parse error: %s", parse_error.c_str());
            co_return std::nullopt;
        }
        if (response.message_complete)
            break;
    }
    if (!response.message_complete) {
        if (!response.finish(&parse_error)) {
            CO_WQ_LOG_ERROR("[bili] parse error at EOF: %s", parse_error.c_str());
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
        auto json = nlohmann::json::parse(response.body_buffer);
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

struct Slot_guard {
    Semaphore<SpinLock>* slot { nullptr };
    Semaphore<SpinLock>* done { nullptr };
    bool                 slot_acquired { false };
    bool                 keep_slots { false };

    ~Slot_guard()
    {
        if (done)
            done->release();
        if (slot && slot_acquired && !keep_slots)
            slot->release();
    }
};

Task<void, Work_Promise<SpinLock, void>> fetch_online_count_entry(NetFdWorkqueue&                     fdwq,
                                                                  RankingEntry&                       entry,
                                                                  bool                                verify_peer,
                                                                  bool                                log_error_json,
                                                                  const std::shared_ptr<SharedState>& state,
                                                                  size_t                              index,
                                                                  size_t                              total,
                                                                  std::atomic_size_t*                 completed_counter)
{
    if (g_stop.load(std::memory_order_acquire))
        co_return;

    constexpr const char* host = "api.bilibili.com";
    constexpr uint16_t    port = 443;

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
            if (log_error_json) {
                std::string dump = online_json->dump();
                CO_WQ_LOG_WARN("[bili] online api returned non-zero code for %s: %s", entry.bvid.c_str(), dump.c_str());
            } else {
                CO_WQ_LOG_WARN("[bili] online api returned non-zero code for %s (use --log-json to dump payload)",
                               entry.bvid.c_str());
            }
        } else {
            CO_WQ_LOG_WARN("[bili] online api request failed for %s", entry.bvid.c_str());
        }
    }

    size_t completed = completed_counter ? completed_counter->fetch_add(1, std::memory_order_acq_rel) + 1
                                         : std::min(index + 1, total);
    if (state) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->completed_items  = std::max(state->completed_items, completed);
        state->progress_updated = std::chrono::system_clock::now();
    }
    if (total > 0) {
        double percent = static_cast<double>(completed) / static_cast<double>(total) * 100.0;
        CO_WQ_LOG_INFO("[bili] progress %.0f%% (%zu/%zu)", percent, completed, total);
    }
    co_return;
}

Task<void, Work_Promise<SpinLock, void>> fetch_online_count_worker(NetFdWorkqueue&                     fdwq,
                                                                   RankingEntry&                       entry,
                                                                   bool                                verify_peer,
                                                                   bool                                log_error_json,
                                                                   const std::shared_ptr<SharedState>& state,
                                                                   size_t                              index,
                                                                   size_t                              total,
                                                                   std::atomic_size_t&  completed_counter,
                                                                   Semaphore<SpinLock>& slot_sem,
                                                                   Semaphore<SpinLock>& done_sem,
                                                                   bool                 keep_slot)
{
    Slot_guard guard { &slot_sem, &done_sem, false, keep_slot };

    if (!keep_slot) {
        co_await wait_sem(slot_sem);
        guard.slot_acquired = true;
    }

    if (!g_stop.load(std::memory_order_acquire))
        co_await fetch_online_count_entry(fdwq,
                                          entry,
                                          verify_peer,
                                          log_error_json,
                                          state,
                                          index,
                                          total,
                                          &completed_counter);

    co_return;
}

Task<void, Work_Promise<SpinLock, void>> fetch_online_counts(NetFdWorkqueue&                     fdwq,
                                                             std::vector<RankingEntry>&          entries,
                                                             bool                                verify_peer,
                                                             const std::shared_ptr<SharedState>& state,
                                                             bool                                rapid_mode,
                                                             bool                                log_error_json)
{
    const size_t total = entries.size();
    if (total == 0) {
        if (state) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->completed_items  = 0;
            state->progress_updated = std::chrono::system_clock::now();
            state->fetching         = false;
        }
        co_return;
    }

    size_t final_completed = 0;

    if (rapid_mode) {
        Semaphore<SpinLock> slot_sem(fdwq.base(), 20, 20);
        Semaphore<SpinLock> done_sem(fdwq.base(), 0, static_cast<int>(std::max<size_t>(total, 1)));
        std::atomic_size_t  completed_counter { 0 };

        for (size_t idx = 0; idx < total; ++idx) {
            auto task = fetch_online_count_worker(fdwq,
                                                  entries[idx],
                                                  verify_peer,
                                                  log_error_json,
                                                  state,
                                                  idx,
                                                  total,
                                                  completed_counter,
                                                  slot_sem,
                                                  done_sem,
                                                  false);
            post_to(task, fdwq.base());
        }

        for (size_t i = 0; i < total; ++i)
            co_await wait_sem(done_sem);

        final_completed = completed_counter.load(std::memory_order_acquire);
    } else {
        size_t index = 0;
        for (auto& entry : entries) {
            co_await fetch_online_count_entry(fdwq, entry, verify_peer, log_error_json, state, index, total, nullptr);
            ++index;
            if (g_stop.load(std::memory_order_acquire))
                break;
            co_await delay_ms(get_sys_timer(), 200);
        }
        final_completed = index;
    }

    if (state) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->fetching         = false;
        state->completed_items  = std::min(final_completed, total);
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
                                                      bool                         verify_peer,
                                                      bool                         log_error_json)
{
    while (!g_stop.load(std::memory_order_acquire)) {
        auto ranking = co_await fetch_ranking(fdwq, verify_peer);
        if (ranking.has_value()) {
            auto entries    = std::move(*ranking);
            bool rapid_mode = false;
            if (state) {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->fetching         = true;
                state->total_items      = entries.size();
                state->completed_items  = 0;
                state->progress_updated = std::chrono::system_clock::now();
                rapid_mode              = !state->initial_fetch_done;
            }
            co_await fetch_online_counts(fdwq, entries, verify_peer, state, rapid_mode, log_error_json);
            if (!entries.empty()) {
                auto        json_payload = build_result_payload(entries);
                auto        now          = std::chrono::system_clock::now();
                std::string serialized   = json_payload.dump(2);
                std::string last_mod     = format_http_date(now);
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->json_payload       = serialized;
                    state->last_modified      = last_mod;
                    state->updated_at         = now;
                    state->initial_fetch_done = true;
                }
                std::error_code ec;
                std::filesystem::create_directories(data_path.parent_path(), ec);
                std::ofstream ofs(data_path, std::ios::binary | std::ios::trunc);
                if (ofs.is_open()) {
                    ofs.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
                    ofs.flush();
                }
                CO_WQ_LOG_INFO("[bili] ranking updated, %zu entries", entries.size());
            } else if (state) {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->initial_fetch_done = true;
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
        int64_t wait_ms = static_cast<int64_t>(interval.count()) * 1000;
        if (wait_ms < 0)
            wait_ms = 0;
        uint32_t clamped_wait = static_cast<uint32_t>(
            std::min<int64_t>(wait_ms, static_cast<int64_t>(std::numeric_limits<uint32_t>::max())));
        co_await delay_ms(get_sys_timer(), clamped_wait);
    }
    co_return;
}

// (removed legacy helper utilities for static file serving)

// Helper response builders for HttpEasyServer routes
HttpResponse make_index_response()
{
    HttpResponse resp;
    resp.set_status(200, "OK");
    resp.set_header("content-type", "text/html; charset=utf-8");
    resp.set_header("cache-control", "no-store");
    resp.set_body(std::string(kIndexHtmlView));
    return resp;
}

HttpResponse make_data_response(const std::shared_ptr<SharedState>& state)
{
    HttpResponse resp;
    std::string  payload;
    std::string  last_modified;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        payload       = state->json_payload;
        last_modified = state->last_modified;
    }

    if (payload.empty()) {
        resp.set_status(503, "Service Unavailable");
        resp.set_header("content-type", "application/json; charset=utf-8");
        resp.set_header("cache-control", "no-store");
        resp.set_body("{\"error\":\"data not ready\"}");
        return resp;
    }

    resp.set_status(200, "OK");
    resp.set_header("content-type", "application/json; charset=utf-8");
    resp.set_header("cache-control", "no-store");
    if (!last_modified.empty())
        resp.set_header("last-modified", last_modified);
    resp.set_body(std::move(payload));
    return resp;
}

HttpResponse make_progress_response(const std::shared_ptr<SharedState>& state)
{
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

    HttpResponse resp;
    resp.set_status(200, "OK");
    resp.set_header("content-type", "application/json; charset=utf-8");
    resp.set_header("cache-control", "no-store");
    resp.set_body(status_json.dump());
    return resp;
}

// (removed unused make_static_file_response helper)

// (removed legacy handle_client coroutine)

// server_task removed: using HttpEasyServer in main instead

struct Options {
    std::string           host { "0.0.0.0" };
    uint16_t              port { 8000 };
    std::chrono::seconds  interval { 300 };
    std::filesystem::path static_root { "bilibili-online-ranking" };
    bool                  verify_peer { false };
    bool                  log_error_json { false };
};

void print_usage(const char* argv0)
{
    std::fprintf(stderr,
                 "Usage: %s [--host HOST] [--port PORT] [--interval SECONDS] [--static-root PATH] [--verify]"
                 " [--log-json]\n",
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
        } else if (arg == "--log-json") {
            opts.log_error_json = true;
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

    auto poll_task = polling_task(fdwq, state, data_path, opts.interval, opts.verify_peer, opts.log_error_json);

    // Start HttpEasyServer and register routes
    net::http::HttpEasyServer server(fdwq);
    auto&                     router = server.router();
    router.get("/", [state](const HttpRequest&) { return make_index_response(); });
    router.get("/index.html", [state](const HttpRequest&) { return make_index_response(); });
    router.get("/data.json", [state](const HttpRequest&) { return make_data_response(state); });
    router.get("/progress", [state](const HttpRequest&) { return make_progress_response(state); });
    router.get("/progress.json", [state](const HttpRequest&) { return make_progress_response(state); });
    router.get("/status", [state](const HttpRequest&) { return make_progress_response(state); });
    router.serve_static("/static", static_root);

    router.add_middleware([](const HttpRequest&, HttpResponse& resp) {
        if (!resp.has_header("content-type"))
            resp.set_header("content-type", "text/plain; charset=utf-8");
        resp.set_header("server", "co_wq-http");
    });

    HttpEasyServer::Config cfg;
    cfg.host            = opts.host;
    cfg.port            = opts.port;
    cfg.verbose_logging = true;
#if defined(USING_SSL)
    // keep options default (no TLS) unless configured via env or args
#endif

    if (!server.start(cfg)) {
        CO_WQ_LOG_ERROR("[bili] failed to start HttpEasyServer");
        return 1;
    }

    g_server_ptr.store(&server, std::memory_order_release);

    auto             poll_coroutine = poll_task.get();
    auto&            poll_promise   = poll_coroutine.promise();
    std::atomic_int  pending { 1 };
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

    bool stop_requested_early = g_stop.load(std::memory_order_acquire);
    if (stop_requested_early) {
        finished.store(true, std::memory_order_release);
        server.request_stop();
    } else {
        post_to(poll_task, wq);
    }

    // wait until polling task finishes or signal triggers stop
    sys_wait_until(finished);

    // request server stop and wait for it
    server.request_stop();
    server.wait();

    g_server_ptr.store(nullptr, std::memory_order_release);
    g_finished_ptr.store(nullptr, std::memory_order_release);

    return 0;
}
