//
// HTTP forward proxy example built on the co_wq coroutine framework.
// 支持常见的 HTTP/1.1 正向代理语义（绝对 URI + CONNECT 隧道），示例演示如何
// 使用 fd_workqueue 接受客户端连接、解析请求并桥接到上游服务器。

#include "syswork.hpp"

#include "fd_base.hpp"
#include "io_waiter.hpp"
#include "tcp_listener.hpp"
#include "tcp_socket.hpp"
#include "when_all.hpp"
#include "worker.hpp"

#include <llhttp.h>

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <basetsd.h>
#include <dbghelp.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "Dbghelp.lib")
#else
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>

#endif

using namespace co_wq;

namespace {

#if defined(_WIN32)
extern "C" void wq_debug_check_func_addr(std::uintptr_t addr)
{
    const char* reason = nullptr;
    if (addr == 0) {
        reason = "null";
    } else if (addr <= 0xFFFF) {
        reason = "low-address";
    }
    void*           stack[32] {};
    constexpr DWORD capacity = static_cast<DWORD>(sizeof(stack) / sizeof(stack[0]));
    USHORT          frames   = CaptureStackBackTrace(0, capacity, stack, nullptr);
    HANDLE          process  = GetCurrentProcess();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
    SymInitialize(process, nullptr, TRUE);
    DWORD64                            displacement = 0;
    constexpr DWORD                    max_name_len = 256;
    alignas(SYMBOL_INFO) unsigned char func_symbol_buffer[sizeof(SYMBOL_INFO) + max_name_len];
    auto*                              func_symbol = reinterpret_cast<PSYMBOL_INFO>(func_symbol_buffer);
    func_symbol->SizeOfStruct                      = sizeof(SYMBOL_INFO);
    func_symbol->MaxNameLen                        = max_name_len;
    BOOL has_symbol = SymFromAddr(process, static_cast<DWORD64>(addr), &displacement, func_symbol);
    bool misaligned = (addr & 0xF) != 0;
    if (!reason && misaligned) {
        if (has_symbol && func_symbol->Name[0] != '\0') {
            const char* name = func_symbol->Name;
            if (std::strstr(name, "ILT") != nullptr || std::strstr(name, "__guard_dispatch") != nullptr
                || std::strstr(name, "__guard_check") != nullptr) {
                misaligned = false; // treat import thunks / guard helpers as expected
            }
        }
        if (misaligned)
            reason = "misaligned";
    }
    if (!reason && has_symbol)
        return; // resolved symbol looks valid, skip noise

    if (has_symbol) {
        std::fprintf(stderr,
                     "[proxy] wq_debug_check_func_addr suspicious func=%p (%s+0x%llx) frames=%u reason=%s\n",
                     reinterpret_cast<void*>(addr),
                     func_symbol->Name,
                     static_cast<unsigned long long>(displacement),
                     frames,
                     reason ? reason : "?");
    } else {
        std::fprintf(stderr,
                     "[proxy] wq_debug_check_func_addr suspicious func=%p frames=%u reason=%s\n",
                     reinterpret_cast<void*>(addr),
                     frames,
                     reason ? reason : "?");
    }
    for (USHORT i = 0; i < frames; ++i) {
        auto* frame_addr = stack[i];
        std::fprintf(stderr, "  [%u] %p", static_cast<unsigned>(i), frame_addr);
        DWORD64                            frame_disp = 0;
        alignas(SYMBOL_INFO) unsigned char symbol_buffer[sizeof(SYMBOL_INFO) + max_name_len];
        auto*                              symbol_info = reinterpret_cast<PSYMBOL_INFO>(symbol_buffer);
        symbol_info->SizeOfStruct                      = sizeof(SYMBOL_INFO);
        symbol_info->MaxNameLen                        = max_name_len;
        if (SymFromAddr(process, reinterpret_cast<DWORD64>(frame_addr), &frame_disp, symbol_info)) {
            std::fprintf(stderr, " (%s+0x%llx)\n", symbol_info->Name, static_cast<unsigned long long>(frame_disp));
        } else {
            std::fprintf(stderr, "\n");
        }
    }
}
#endif

// 是否开启调试日志，可根据需要在运行时调整。
std::atomic_bool g_debug_logging { false };

#if defined(_WIN32)
LONG WINAPI proxy_exception_filter(EXCEPTION_POINTERS* info)
{
    auto* record = info ? info->ExceptionRecord : nullptr;
    DWORD code   = record ? record->ExceptionCode : 0;
    void* addr   = record ? record->ExceptionAddress : nullptr;
    std::fprintf(stderr, "[proxy] unhandled exception code=0x%08lx addr=%p", static_cast<unsigned long>(code), addr);
    auto ensure_dbghelp_initialized = [] {
        static std::once_flag initialized;
        std::call_once(initialized, [] {
            HANDLE process = GetCurrentProcess();
            SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
            if (!SymInitialize(process, nullptr, TRUE)) {
                DWORD err = GetLastError();
                std::fprintf(stderr, "\n[proxy] SymInitialize failed: %lu\n", static_cast<unsigned long>(err));
            }
        });
    };
    ensure_dbghelp_initialized();
    HANDLE                             process_handle = GetCurrentProcess();
    DWORD64                            displacement   = 0;
    constexpr DWORD                    max_name_len   = 256;
    alignas(SYMBOL_INFO) unsigned char symbol_buffer[sizeof(SYMBOL_INFO) + max_name_len];
    auto*                              symbol_info = reinterpret_cast<PSYMBOL_INFO>(symbol_buffer);
    symbol_info->SizeOfStruct                      = sizeof(SYMBOL_INFO);
    symbol_info->MaxNameLen                        = max_name_len;
    if (addr != nullptr && SymFromAddr(process_handle, reinterpret_cast<DWORD64>(addr), &displacement, symbol_info)) {
        IMAGEHLP_LINE64 line_info {};
        line_info.SizeOfStruct  = sizeof(line_info);
        DWORD line_displacement = 0;
        if (SymGetLineFromAddr64(process_handle, reinterpret_cast<DWORD64>(addr), &line_displacement, &line_info)) {
            std::fprintf(stderr,
                         " (%s+0x%llx) %s:%lu\n",
                         symbol_info->Name,
                         static_cast<unsigned long long>(displacement),
                         line_info.FileName ? line_info.FileName : "<unknown>",
                         static_cast<unsigned long>(line_info.LineNumber));
        } else {
            std::fprintf(stderr, " (%s+0x%llx)\n", symbol_info->Name, static_cast<unsigned long long>(displacement));
        }
    } else {
        std::fprintf(stderr, "\n");
    }
    if (info && info->ContextRecord) {
        ensure_dbghelp_initialized();
        CONTEXT      context_copy = *info->ContextRecord;
        STACKFRAME64 frame {};
        frame.AddrPC.Offset    = context_copy.Rip;
        frame.AddrPC.Mode      = AddrModeFlat;
        frame.AddrFrame.Offset = context_copy.Rsp;
        frame.AddrFrame.Mode   = AddrModeFlat;
        frame.AddrStack.Offset = context_copy.Rsp;
        frame.AddrStack.Mode   = AddrModeFlat;

        HANDLE thread_handle  = GetCurrentThread();
        HANDLE process_handle = GetCurrentProcess();
        for (USHORT i = 0;; ++i) {
            BOOL ok = StackWalk64(IMAGE_FILE_MACHINE_AMD64,
                                  process_handle,
                                  thread_handle,
                                  &frame,
                                  &context_copy,
                                  nullptr,
                                  SymFunctionTableAccess64,
                                  SymGetModuleBase64,
                                  nullptr);
            if (!ok)
                break;
            DWORD64 frame_addr = frame.AddrPC.Offset;
            if (frame_addr == 0)
                break;
            std::fprintf(stderr, "  frame[%u] = %p", static_cast<unsigned>(i), reinterpret_cast<void*>(frame_addr));
            DWORD64                            frame_disp   = 0;
            constexpr DWORD                    max_name_len = 256;
            alignas(SYMBOL_INFO) unsigned char frame_symbol_buffer[sizeof(SYMBOL_INFO) + max_name_len];
            auto*                              frame_symbol_info = reinterpret_cast<PSYMBOL_INFO>(frame_symbol_buffer);
            frame_symbol_info->SizeOfStruct                      = sizeof(SYMBOL_INFO);
            frame_symbol_info->MaxNameLen                        = max_name_len;
            if (SymFromAddr(process_handle, frame_addr, &frame_disp, frame_symbol_info)) {
                IMAGEHLP_LINE64 line_info {};
                line_info.SizeOfStruct = sizeof(line_info);
                DWORD line_disp        = 0;
                if (SymGetLineFromAddr64(process_handle, frame_addr, &line_disp, &line_info)) {
                    std::fprintf(stderr,
                                 " (%s+0x%llx) %s:%lu\n",
                                 frame_symbol_info->Name,
                                 static_cast<unsigned long long>(frame_disp),
                                 line_info.FileName ? line_info.FileName : "<unknown>",
                                 static_cast<unsigned long>(line_info.LineNumber));
                } else {
                    std::fprintf(stderr,
                                 " (%s+0x%llx)\n",
                                 frame_symbol_info->Name,
                                 static_cast<unsigned long long>(frame_disp));
                }
            } else {
                std::fprintf(stderr, "\n");
            }
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

/**
 * @brief 打印带时间戳/线程 ID 的调试日志。
 */
template <typename Str> void debug_log(Str&& message_input)
{
    if (!g_debug_logging.load(std::memory_order_relaxed))
        return;

    static std::mutex           log_mutex;
    std::lock_guard<std::mutex> guard(log_mutex);

    std::string message(std::forward<Str>(message_input));

    using clock = std::chrono::system_clock;
    auto    now = clock::now();
    auto    tt  = clock::to_time_t(now);
    std::tm tm_buf {};
#if defined(_WIN32)
    localtime_s(&tm_buf, &tt);
#else
    localtime_r(&tt, &tm_buf);
#endif
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%F %T") << '.' << std::setw(3) << std::setfill('0') << ms.count();
    oss << " [" << std::this_thread::get_id() << "] " << message;
    std::clog << oss.str() << std::endl;
}

std::string format_peer_id(int fd)
{
    sockaddr_storage addr {};
    socklen_t        len                      = sizeof(addr);
    char             ip_buf[INET6_ADDRSTRLEN] = {};
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        if (addr.ss_family == AF_INET) {
            auto* in = reinterpret_cast<sockaddr_in*>(&addr);
            if (::inet_ntop(AF_INET, &in->sin_addr, ip_buf, sizeof(ip_buf))) {
                std::ostringstream oss;
                oss << "[client " << ip_buf << ':' << ntohs(in->sin_port) << ']';
                return oss.str();
            }
        } else if (addr.ss_family == AF_INET6) {
            auto* in6 = reinterpret_cast<sockaddr_in6*>(&addr);
            if (::inet_ntop(AF_INET6, &in6->sin6_addr, ip_buf, sizeof(ip_buf))) {
                std::ostringstream oss;
                oss << "[client [" << ip_buf << "]:" << ntohs(in6->sin6_port) << ']';
                return oss.str();
            }
        }
    }
    std::ostringstream fallback;
    fallback << "[client fd=" << fd << ']';
    return fallback.str();
}

// 顺序保留的单个头字段，用于重新构造转发请求。
struct HeaderEntry {
    std::string name;
    std::string value;
};

// HTTP 请求解析上下文，llhttp 回调直接写入该结构。
struct HttpProxyContext {
    std::string                                  method;
    std::string                                  url;
    std::unordered_map<std::string, std::string> headers;
    std::vector<HeaderEntry>                     header_sequence;
    std::string                                  current_field;
    std::string                                  current_value;
    std::string                                  body;
    bool                                         headers_complete { false };
    bool                                         message_complete { false };
    int                                          http_major { 1 };
    int                                          http_minor { 1 };
    void                                         reset();
};

inline void HttpProxyContext::reset()
{
    method.clear();
    url.clear();
    headers.clear();
    header_sequence.clear();
    current_field.clear();
    current_value.clear();
    body.clear();
    headers_complete = false;
    message_complete = false;
    http_major       = 1;
    http_minor       = 1;
}

void debug_check_string_integrity(const char* label, const std::string& value)
{
    const char* data           = value.data();
    const auto  size           = value.size();
    const auto  cap            = value.capacity();
    bool        suspicious_ptr = false;
    bool        suspicious_buf = false;

#if defined(_WIN32)
    auto addr = reinterpret_cast<std::uintptr_t>(data);
#if INTPTR_MAX == INT64_MAX
    constexpr std::uintptr_t POISON_CD = 0xcdcdcdcdcdcdcdcdULL;
    constexpr std::uintptr_t POISON_CC = 0xccccccccccccccccULL;
    constexpr std::uintptr_t POISON_FE = 0xfeeefeeefeeefeeeULL;
#else
    constexpr std::uintptr_t POISON_CD = 0xcdcdcdcdUL;
    constexpr std::uintptr_t POISON_CC = 0xccccccccUL;
    constexpr std::uintptr_t POISON_FE = 0xfeeefeeeUL;
#endif
    if (addr == POISON_CD || addr == POISON_CC || addr == POISON_FE) {
        suspicious_ptr = true;
    }
#endif

    if (size > cap) {
        suspicious_buf = true;
    }

    if (size > 0 && data != nullptr) {
        const size_t sample = std::min<size_t>(size, 16);
        bool         all_cd = true;
        for (size_t i = 0; i < sample; ++i) {
            if (static_cast<unsigned char>(data[i]) != 0xCD) {
                all_cd = false;
                break;
            }
        }
        if (all_cd) {
            suspicious_buf = true;
        }
    }

    if (data == nullptr) {
        suspicious_ptr = true;
    }

    if (suspicious_ptr || suspicious_buf) {
        std::fprintf(stderr,
                     "[proxy] string integrity alert: %s data=%p size=%zu cap=%zu ptr_bad=%d buf_bad=%d\n",
                     label,
                     static_cast<const void*>(data),
                     static_cast<size_t>(size),
                     static_cast<size_t>(cap),
                     suspicious_ptr ? 1 : 0,
                     suspicious_buf ? 1 : 0);
#if defined(_WIN32)
        __debugbreak();
#endif
    } else if (g_debug_logging.load(std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[proxy] string ok: %s data=%p size=%zu cap=%zu\n",
                     label,
                     static_cast<const void*>(data),
                     static_cast<size_t>(size),
                     static_cast<size_t>(cap));
    }
}

std::string to_lower(const std::string& input)
{
    std::string out;
    out.reserve(input.size());
    for (char ch : input) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

// 将十进制端口字符串转换为 uint16_t。
bool parse_port(const std::string& str, uint16_t& out)
{
    if (str.empty())
        return false;
    unsigned long value = 0;
    for (char ch : str) {
        if (ch < '0' || ch > '9')
            return false;
        value = value * 10 + static_cast<unsigned long>(ch - '0');
        if (value > 65535)
            return false;
    }
    if (value == 0)
        return false;
    out = static_cast<uint16_t>(value);
    return true;
}

struct UrlParts {
    std::string host;
    uint16_t    port { 80 };
    std::string path { "/" };
};

// 解析绝对形式的 HTTP URL（scheme://host[:port]/path）。
std::optional<UrlParts> parse_http_url(const std::string& url)
{
    debug_check_string_integrity("parse_http_url.url", url);
    auto pos = url.find("://");
    if (pos == std::string::npos)
        return std::nullopt;
    std::string scheme = to_lower(url.substr(0, pos));
    if (scheme != "http")
        return std::nullopt;
    size_t rest_idx = pos + 3;
    if (rest_idx >= url.size())
        return std::nullopt;

    auto        slash_pos = url.find('/', rest_idx);
    std::string authority = slash_pos == std::string::npos ? url.substr(rest_idx)
                                                           : url.substr(rest_idx, slash_pos - rest_idx);
    std::string path      = slash_pos == std::string::npos ? std::string("/") : url.substr(slash_pos);
    if (authority.empty())
        return std::nullopt;
    UrlParts parts;
    auto     colon_pos = authority.find(':');
    debug_check_string_integrity("parse_http_url.authority", authority);
    if (colon_pos != std::string::npos) {
        parts.host = authority.substr(0, colon_pos);
        uint16_t port { 0 };
        auto     port_str = authority.substr(colon_pos + 1);
        debug_check_string_integrity("parse_http_url.port", port_str);
        if (!parse_port(port_str, port))
            return std::nullopt;
        parts.port = port;
    } else {
        parts.host = authority;
    }
    if (parts.host.empty())
        return std::nullopt;
    parts.path = std::move(path);
    return parts;
}

struct ConnectTarget {
    std::string host;
    uint16_t    port { 443 };
};

// 解析 CONNECT 动词目标（host[:port]）。
std::optional<ConnectTarget> parse_connect_target(const std::string& target)
{
    if (target.empty())
        return std::nullopt;
    debug_check_string_integrity("parse_connect_target", target);
    auto          colon_pos = target.find(':');
    ConnectTarget ct;
    if (colon_pos == std::string::npos) {
        ct.host = target;
        ct.port = 443;
    } else {
        ct.host = target.substr(0, colon_pos);
        if (ct.host.empty())
            return std::nullopt;
        uint16_t port { 0 };
        auto     port_str = target.substr(colon_pos + 1);
        debug_check_string_integrity("parse_connect_target.port", port_str);
        if (!parse_port(port_str, port))
            return std::nullopt;
        ct.port = port;
    }
    if (ct.host.empty())
        return std::nullopt;
    return ct;
}

#if defined(_WIN32)
extern "C" void wq_debug_null_func(co_wq::worknode* node)
{
    std::fprintf(stderr, "[proxy] wq_debug_null_func fired: node=%p\n", static_cast<void*>(node));
    if (node != nullptr) {
        auto* waiter            = reinterpret_cast<co_wq::net::io_waiter_base*>(node);
        bool  looks_like_waiter = waiter->debug_magic == co_wq::net::io_waiter_base::debug_magic_value;
        if (looks_like_waiter) {
            std::fprintf(stderr,
                         "  debug_name=%s h=%p callback_enqueued=%s route_post=%p route_ctx=%p\n",
                         waiter->debug_name ? waiter->debug_name : "<null>",
                         waiter->h ? waiter->h.address() : nullptr,
                         waiter->callback_enqueued.load(std::memory_order_relaxed) ? "true" : "false",
                         reinterpret_cast<void*>(waiter->route_post),
                         waiter->route_ctx);
        } else {
            std::fprintf(stderr, "  node is not recognized as io_waiter_base (debug_magic mismatch)\n");
        }
    }
    void*           stack[32] {};
    constexpr DWORD capacity = static_cast<DWORD>(std::size(stack));
    USHORT          frames   = CaptureStackBackTrace(0, capacity, stack, nullptr);
    for (USHORT i = 0; i < frames; ++i) {
        std::fprintf(stderr, "  stack[%u]=%p\n", static_cast<unsigned>(i), stack[i]);
    }
}
#endif

Task<int, Work_Promise<SpinLock, int>>
connect_upstream(net::tcp_socket<SpinLock>& socket, const std::string& host, uint16_t port)
{
    struct addrinfo hints {};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* result   = nullptr;
    std::string      port_str = std::to_string(port);
    int              rc       = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
    if (rc != 0 || result == nullptr) {
        if (result)
            ::freeaddrinfo(result);
        co_return -1;
    }

    int ret = -1;
#if !defined(_WIN32)
    struct StaticEndpoint {
        sockaddr_storage addr {};
        socklen_t        len { 0 };
        bool             build(sockaddr_storage& storage, socklen_t& out_len) const
        {
            std::memcpy(&storage, &addr, len);
            out_len = len;
            return true;
        }
    };
#endif

    for (auto* ai = result; ai != nullptr; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET)
            continue;
#if defined(_WIN32)
        auto* in = reinterpret_cast<sockaddr_in*>(ai->ai_addr);
        char  ip_buf[INET_ADDRSTRLEN] {};
        if (::inet_ntop(AF_INET, &in->sin_addr, ip_buf, sizeof(ip_buf)) == nullptr)
            continue;
        ret = co_await socket.connect(ip_buf, static_cast<uint16_t>(ntohs(in->sin_port)));
#else
        StaticEndpoint endpoint;
        endpoint.len = static_cast<socklen_t>(ai->ai_addrlen);
        if (endpoint.len > static_cast<socklen_t>(sizeof(endpoint.addr)))
            endpoint.len = static_cast<socklen_t>(sizeof(endpoint.addr));
        std::memcpy(&endpoint.addr, ai->ai_addr, endpoint.len);
        ret = co_await socket.connect_with(endpoint);
#endif
        if (ret == 0)
            break;
    }

    ::freeaddrinfo(result);
    co_return ret;
}

// 构造最小化的 HTTP/1.1 错误/控制响应。
std::string build_http_response(int status_code, const std::string& reason, const std::string& body)
{
    std::string response;
    response.reserve(128 + body.size());
    response.append("HTTP/1.1 ");
    response.append(std::to_string(status_code));
    response.push_back(' ');
    response.append(reason);
    response.append("\r\n");
    response.append("Content-Type: text/plain; charset=utf-8\r\n");
    response.append("Content-Length: ");
    response.append(std::to_string(body.size()));
    response.append("\r\nConnection: close\r\n\r\n");
    response.append(body);
    return response;
}

int on_message_begin(llhttp_t* parser)
{
    if (auto* ctx = static_cast<HttpProxyContext*>(parser->data))
        ctx->reset();
    return 0;
}

int on_method(llhttp_t* parser, const char* at, size_t length)
{
    auto* ctx = static_cast<HttpProxyContext*>(parser->data);
    ctx->method.append(at, length);
    return 0;
}

int on_url(llhttp_t* parser, const char* at, size_t length)
{
    auto* ctx = static_cast<HttpProxyContext*>(parser->data);
    ctx->url.append(at, length);
    return 0;
}

int on_header_field(llhttp_t* parser, const char* at, size_t length)
{
    auto* ctx = static_cast<HttpProxyContext*>(parser->data);
    ctx->current_field.append(at, length);
    return 0;
}

int on_header_value(llhttp_t* parser, const char* at, size_t length)
{
    auto* ctx = static_cast<HttpProxyContext*>(parser->data);
    ctx->current_value.append(at, length);
    return 0;
}

int on_header_value_complete(llhttp_t* parser)
{
    auto* ctx = static_cast<HttpProxyContext*>(parser->data);
    if (!ctx->current_field.empty()) {
        ctx->headers[to_lower(ctx->current_field)] = ctx->current_value;
        ctx->header_sequence.push_back(HeaderEntry { ctx->current_field, ctx->current_value });
    }
    ctx->current_field.clear();
    ctx->current_value.clear();
    return 0;
}

int on_headers_complete(llhttp_t* parser)
{
    auto* ctx             = static_cast<HttpProxyContext*>(parser->data);
    ctx->headers_complete = true;
    ctx->http_major       = parser->http_major;
    ctx->http_minor       = parser->http_minor;
    return 0;
}

int on_body(llhttp_t* parser, const char* at, size_t length)
{
    auto* ctx = static_cast<HttpProxyContext*>(parser->data);
    ctx->body.append(at, length);
    return 0;
}

int on_message_complete(llhttp_t* parser)
{
    auto* ctx             = static_cast<HttpProxyContext*>(parser->data);
    ctx->message_complete = true;
    return 0;
}

// 进程级运行状态标记，配合信号处理实现 Ctrl+C 安全退出。
std::atomic_bool               g_stop { false };
std::atomic<int>               g_listener_fd { -1 };
std::atomic<std::atomic_bool*> g_finished_ptr { nullptr };

#if defined(_WIN32)
static BOOL WINAPI console_ctrl_handler(DWORD type)
{
    if (type == CTRL_C_EVENT) {
        g_stop.store(true, std::memory_order_release);
        int fd = g_listener_fd.exchange(-1, std::memory_order_acq_rel);
        if (fd != -1)
            ::closesocket((SOCKET)fd);
        if (auto* flag = g_finished_ptr.load(std::memory_order_acquire))
            flag->store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}
#else
void sigint_handler(int)
{
    g_stop.store(true, std::memory_order_release);
    int fd = g_listener_fd.exchange(-1, std::memory_order_acq_rel);
    if (fd != -1)
        ::close(fd);
    if (auto* flag = g_finished_ptr.load(std::memory_order_acquire))
        flag->store(true, std::memory_order_release);
}
#endif

// 将解析得到的绝对 URI 请求转换为上游服务器期望的 origin-form。
std::string build_upstream_request(const HttpProxyContext& ctx, const UrlParts& parts)
{
    debug_check_string_integrity("build_request.method", ctx.method);
    debug_check_string_integrity("build_request.url", ctx.url);
    debug_check_string_integrity("build_request.path", parts.path);
    std::string request;
    request.reserve(ctx.method.size() + parts.path.size() + ctx.body.size() + 128);
    request.append(ctx.method);
    request.push_back(' ');
    request.append(parts.path);
    request.append(" HTTP/");
    request.append(std::to_string(ctx.http_major));
    request.push_back('.');
    request.append(std::to_string(ctx.http_minor));
    request.append("\r\n");

    bool has_host_header = false;
    for (const auto& entry : ctx.header_sequence) {
        std::string lower = to_lower(entry.name);
        if (lower == "proxy-connection")
            continue;
        if (lower == "connection")
            continue;
        if (lower == "host")
            has_host_header = true;
        request.append(entry.name);
        request.append(": ");
        debug_check_string_integrity("build_request.header.name", entry.name);
        debug_check_string_integrity("build_request.header.value", entry.value);
        request.append(entry.value);
        request.append("\r\n");
    }
    if (!has_host_header) {
        request.append("Host: ");
        request.append(parts.host);
        if (parts.port != 80) {
            request.push_back(':');
            request.append(std::to_string(parts.port));
        }
        request.append("\r\n");
    }
    request.append("Connection: close\r\n");
    request.append("Proxy-Connection: close\r\n");
    request.append("\r\n");
    request.append(ctx.body);
    return request;
}

template <typename SrcSocket, typename DstSocket>
Task<void, Work_Promise<SpinLock, void>>
pipe_data(SrcSocket& src, DstSocket& dst, std::string label, const std::string& peer_id)
{
    std::array<char, 8192> buffer {};
    {
        debug_log(peer_id + " " + label + " pipe started");
    }
    while (true) {
        ssize_t n = co_await src.recv(buffer.data(), buffer.size());
        if (n <= 0) {
            if (n == 0) {
                debug_log(peer_id + " " + std::string(label) + " pipe stopping (peer closed)");
            } else {
                std::ostringstream oss;
                oss << peer_id << " " << label << " pipe stopping (recv error: " << n << ")";
                debug_log(oss.str());
            }
            dst.shutdown_tx();
            co_return;
        }
        size_t offset = 0;
        while (offset < static_cast<size_t>(n)) {
            ssize_t sent = co_await dst.send(buffer.data() + offset, static_cast<size_t>(n) - offset);
            if (sent <= 0) {
                debug_log(peer_id + " " + label + " pipe stopping (send error)");
                dst.shutdown_tx();
                co_return;
            }
            offset += static_cast<size_t>(sent);
        }
        {
            std::ostringstream oss;
            oss << peer_id << " " << label << " forwarded " << n << " bytes";
            debug_log(oss.str());
        }
    }
}

// 建立 CONNECT 隧道，将客户端/上游 sockets 互相转发，直到任一方关闭。
Task<void, Work_Promise<SpinLock, void>> handle_connect(net::tcp_socket<SpinLock>&   client,
                                                        net::fd_workqueue<SpinLock>& fdwq,
                                                        const ConnectTarget&         target,
                                                        const std::string&           peer_id)
{
    auto upstream = fdwq.make_tcp_socket();
    {
        std::ostringstream oss;
        oss << peer_id << " CONNECT dialing " << target.host << ':' << target.port;
        auto msg = oss.str();
        debug_log(msg);
    }
    int rc = co_await connect_upstream(upstream, target.host, target.port);
    if (rc != 0) {
        std::ostringstream oss;
        oss << peer_id << " CONNECT upstream failed: " << target.host << ':' << target.port << " rc=" << rc;
        auto msg = oss.str();
        debug_log(msg);
        std::string response = build_http_response(502, "Bad Gateway", "Failed to connect upstream\n");
        (void)co_await client.send_all(response.data(), response.size());
        client.close();
        co_return;
    }

    std::string established = "HTTP/1.1 200 Connection Established\r\nProxy-Agent: co_wq-proxy\r\n\r\n";
    if (co_await client.send_all(established.data(), established.size()) <= 0) {
        debug_log(peer_id + " CONNECT response send failed");
        client.close();
        upstream.close();
        co_return;
    }

    debug_log(peer_id + " CONNECT tunnel established, starting bidirectional piping");

    auto c_to_u = pipe_data(client, upstream, "client->upstream", peer_id);
    auto u_to_c = pipe_data(upstream, client, "upstream->client", peer_id);
    co_await when_all(c_to_u, u_to_c);

    debug_log(peer_id + " CONNECT tunnel closed");

    client.close();
    upstream.close();
    co_return;
}

// 处理常规 HTTP 请求：重新构造请求行/头部并回源，然后将响应回写给客户端。
Task<void, Work_Promise<SpinLock, void>> handle_http_request(HttpProxyContext&            ctx,
                                                             net::tcp_socket<SpinLock>&   client,
                                                             net::fd_workqueue<SpinLock>& fdwq,
                                                             const UrlParts&              parts,
                                                             const std::string&           peer_id)
{
    auto upstream = fdwq.make_tcp_socket();
    {
        std::ostringstream oss;
        oss << peer_id << " " << ctx.method << " " << parts.host << ':' << parts.port << parts.path;
        auto msg = oss.str();
        debug_log(msg);
    }
    int rc = co_await connect_upstream(upstream, parts.host, parts.port);
    if (rc != 0) {
        std::ostringstream oss;
        oss << peer_id << " upstream connect failed: " << parts.host << ':' << parts.port << " rc=" << rc;
        auto msg = oss.str();
        debug_log(msg);
        std::string response = build_http_response(502, "Bad Gateway", "Failed to connect upstream\n");
        (void)co_await client.send_all(response.data(), response.size());
        client.close();
        co_return;
    }

    std::string request = build_upstream_request(ctx, parts);
    if (co_await upstream.send_all(request.data(), request.size()) <= 0) {
        debug_log(peer_id + " failed to forward request to upstream");
        std::string response = build_http_response(502, "Bad Gateway", "Failed to send request upstream\n");
        (void)co_await client.send_all(response.data(), response.size());
        client.close();
        upstream.close();
        co_return;
    }

    debug_log(peer_id + " forwarding upstream response to client");

    std::array<char, 8192> buffer {};
    while (true) {
        ssize_t n = co_await upstream.recv(buffer.data(), buffer.size());
        if (n <= 0)
            break;
        size_t offset = 0;
        while (offset < static_cast<size_t>(n)) {
            ssize_t sent = co_await client.send(buffer.data() + offset, static_cast<size_t>(n) - offset);
            if (sent <= 0) {
                debug_log(peer_id + " client send error while relaying response");
                client.close();
                upstream.close();
                co_return;
            }
            offset += static_cast<size_t>(sent);
        }
    }

    debug_log(peer_id + " upstream response completed");

    client.close();
    upstream.close();
    co_return;
}

// 单个客户端连接生命周期：解析首个请求并根据方法调度处理逻辑。
Task<void, Work_Promise<SpinLock, void>>
handle_proxy_connection(net::fd_workqueue<SpinLock>& fdwq, net::tcp_socket<SpinLock> client, std::string peer_id)
{
    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_message_begin         = on_message_begin;
    settings.on_method                = on_method;
    settings.on_url                   = on_url;
    settings.on_header_field          = on_header_field;
    settings.on_header_value          = on_header_value;
    settings.on_header_value_complete = on_header_value_complete;
    settings.on_headers_complete      = on_headers_complete;
    settings.on_body                  = on_body;
    settings.on_message_complete      = on_message_complete;

    llhttp_t parser;
    llhttp_init(&parser, HTTP_REQUEST, &settings);

    HttpProxyContext ctx;
    parser.data = &ctx;

    std::array<char, 4096> buffer {};
    bool                   parse_error { false };
    std::string            error_reason;
    bool                   received_any_data { false };

    while (!ctx.message_complete) {
        ssize_t n = co_await client.recv(buffer.data(), buffer.size());
        if (n < 0) {
            parse_error  = true;
            error_reason = "socket read error";
            break;
        }
        if (n == 0) {
            if (!received_any_data) {
                client.close();
                co_return;
            }
            llhttp_errno_t finish_err = llhttp_finish(&parser);
            if (finish_err != HPE_OK) {
                parse_error  = true;
                error_reason = llhttp_errno_name(finish_err);
            }
            break;
        }
        received_any_data  = true;
        llhttp_errno_t err = llhttp_execute(&parser, buffer.data(), static_cast<size_t>(n));
        if (err == HPE_PAUSED_UPGRADE || err == HPE_PAUSED) {
            // CONNECT/Upgrade 请求在 headers 完成后会触发暂停；标记完成并退出循环。
            llhttp_resume_after_upgrade(&parser);
            ctx.message_complete = true;
            break;
        } else if (err != HPE_OK) {
            parse_error  = true;
            error_reason = llhttp_get_error_reason(&parser);
            if (error_reason.empty())
                error_reason = llhttp_errno_name(err);
            break;
        }
    }

    if (!ctx.message_complete && !parse_error) {
        parse_error  = true;
        error_reason = "incomplete request";
    }

    if (parse_error) {
        std::ostringstream oss;
        oss << peer_id << " request parse error: " << error_reason;
        auto msg = oss.str();
        debug_log(msg);
        std::cerr << "[proxy] parse error: " << error_reason << "\n";
        std::string response = build_http_response(400, "Bad Request", "Failed to parse request\n");
        (void)co_await client.send_all(response.data(), response.size());
        client.close();
        co_return;
    }

    {
        std::ostringstream oss;
        oss << peer_id << " received request " << ctx.method << ' ' << ctx.url;
        auto msg = oss.str();
        debug_log(msg);
    }

    if (ctx.method == "CONNECT") {
        auto target = parse_connect_target(ctx.url);
        if (!target) {
            debug_log(peer_id + " invalid CONNECT target received");
            std::string response = build_http_response(400, "Bad Request", "Invalid CONNECT target\n");
            (void)co_await client.send_all(response.data(), response.size());
            client.close();
            co_return;
        }
        co_await handle_connect(client, fdwq, *target, peer_id);
        co_return;
    }

    auto parts = parse_http_url(ctx.url);
    if (!parts) {
        debug_log(peer_id + " received non-absolute URI request");
        std::string response = build_http_response(400, "Bad Request", "Proxy requires absolute URI\n");
        (void)co_await client.send_all(response.data(), response.size());
        client.close();
        co_return;
    }

    co_await handle_http_request(ctx, client, fdwq, *parts, peer_id);
    co_return;
}

// 监听入站代理端口并为每个连接派生协程处理。
Task<void, Work_Promise<SpinLock, void>>
proxy_server(net::fd_workqueue<SpinLock>& fdwq, const std::string& host, uint16_t port)
{
    net::tcp_listener<SpinLock> listener(fdwq.base(), fdwq.reactor());
    try {
        listener.bind_listen(host, port, 128);
    } catch (const std::exception& ex) {
        std::ostringstream oss;
        oss << "[proxy] failed to bind " << host << ':' << port << ": " << ex.what();
#if defined(_WIN32)
        int wsa_err = WSAGetLastError();
        if (wsa_err != 0) {
            oss << " (WSA error " << wsa_err << ')';
        }
#else
        int sys_err = errno;
        if (sys_err != 0) {
            oss << " (" << std::strerror(sys_err) << " errno=" << sys_err << ')';
        }
#endif
        auto msg = oss.str();
        std::cerr << msg << '\n';
        debug_log(msg);
        listener.close();
        g_listener_fd.store(-1, std::memory_order_release);
        co_return;
    }
    g_listener_fd.store(listener.native_handle(), std::memory_order_release);

    std::cout << "[proxy] listening on " << host << ':' << port << "\n";
    {
        std::ostringstream oss;
        oss << "proxy listening on " << host << ':' << port;
        auto msg = oss.str();
        debug_log(msg);
    }

    while (!g_stop.load(std::memory_order_acquire)) {
        int fd = co_await listener.accept();
        if (fd == net::k_accept_fatal) {
            std::cerr << "[proxy] accept fatal error, exiting\n";
            break;
        }
        if (fd < 0)
            continue;
        std::string peer_id = format_peer_id(fd);
        {
            std::ostringstream oss;
            oss << peer_id << " accepted (fd=" << fd << ')';
            auto msg = oss.str();
            debug_log(msg);
        }
        auto socket = fdwq.adopt_tcp_socket(fd);
        auto task   = handle_proxy_connection(fdwq, std::move(socket), std::move(peer_id));
        post_to(task, fdwq.base());
    }

    debug_log("proxy server stopping");

    listener.close();
    g_listener_fd.store(-1, std::memory_order_release);
    co_return;
}

} // namespace

// 程序入口：解析命令行参数并启动主协程。
int main(int argc, char* argv[])
{
    std::string host = "0.0.0.0";
    uint16_t    port = 8081;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--verbose" || arg == "--debug-log") {
            g_debug_logging.store(true, std::memory_order_relaxed);
        }
    }

#if defined(_WIN32)
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    SetUnhandledExceptionFilter(proxy_exception_filter);
    HMODULE self_module = ::GetModuleHandleW(nullptr);
    std::fprintf(stderr, "[proxy] module base=%p\n", static_cast<void*>(self_module));
#else
    std::signal(SIGINT, sigint_handler);
#endif

    auto&                       wq = get_sys_workqueue(0);
    net::fd_workqueue<SpinLock> fdwq(wq);

    {
        std::ostringstream oss;
        oss << "starting proxy host=" << host << " port=" << port;
        auto msg = oss.str();
        debug_log(msg);
    }

    auto  proxy_task = proxy_server(fdwq, host, port);
    auto  coroutine  = proxy_task.get();
    auto& promise    = coroutine.promise();

    std::atomic_bool finished { false };
    promise.mUserData    = &finished;
    promise.mOnCompleted = [](Promise_base& pb) {
        auto* flag = static_cast<std::atomic_bool*>(pb.mUserData);
        if (flag)
            flag->store(true, std::memory_order_release);
    };

    g_finished_ptr.store(&finished, std::memory_order_release);
    post_to(proxy_task, wq);

    sys_wait_until(finished);
    g_finished_ptr.store(nullptr, std::memory_order_release);
    return 0;
}
