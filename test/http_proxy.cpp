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

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <basetsd.h>
#include <dbghelp.h>
#include <mstcpip.h>
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

using NetFdWorkqueue = net::fd_workqueue<SpinLock, net::epoll_reactor>;

#if defined(_WIN32)
void ensure_dbghelp_initialized()
{
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
        if (!SymInitialize(process, nullptr, TRUE)) {
            DWORD err = GetLastError();
            CO_WQ_LOG_WARN("[proxy] SymInitialize failed: %lu", static_cast<unsigned long>(err));
        }
    });
}

extern "C" void wq_debug_check_func_addr(std::uintptr_t addr)
{
    static std::mutex                         addr_mutex;
    static std::unordered_set<std::uintptr_t> benign_addrs;
    static std::unordered_set<std::uintptr_t> warned_addrs;

    {
        std::lock_guard guard(addr_mutex);
        if (benign_addrs.find(addr) != benign_addrs.end())
            return;
    }

    const char* reason = nullptr;
    if (addr == 0) {
        reason = "null";
    } else if (addr <= 0xFFFF) {
        reason = "low-address";
    }
    void*           stack[32] {};
    constexpr DWORD capacity = static_cast<DWORD>(sizeof(stack) / sizeof(stack[0]));
    USHORT          frames   = CaptureStackBackTrace(0, capacity, stack, nullptr);
    ensure_dbghelp_initialized();
    HANDLE                             process      = GetCurrentProcess();
    DWORD64                            displacement = 0;
    constexpr DWORD                    max_name_len = 256;
    alignas(SYMBOL_INFO) unsigned char func_symbol_buffer[sizeof(SYMBOL_INFO) + max_name_len];
    auto*                              func_symbol = reinterpret_cast<PSYMBOL_INFO>(func_symbol_buffer);
    func_symbol->SizeOfStruct                      = sizeof(SYMBOL_INFO);
    func_symbol->MaxNameLen                        = max_name_len;
    BOOL                     has_symbol = SymFromAddr(process, static_cast<DWORD64>(addr), &displacement, func_symbol);
    constexpr std::uintptr_t align_mask =
#if INTPTR_MAX == INT64_MAX
        0x7;
#else
        0x3;
#endif
    bool misaligned = (addr & align_mask) != 0;
    if (!reason && misaligned) {
        if (has_symbol) {
            std::lock_guard guard(addr_mutex);
            if (benign_addrs.size() > 4096)
                benign_addrs.clear();
            benign_addrs.insert(addr);
            return; // coroutine / ILT thunks often appear misaligned but are harmless
        }
        reason = "misaligned";
    }
    if (!reason && has_symbol)
        return; // resolved symbol looks valid, skip noise

    {
        std::lock_guard guard(addr_mutex);
        auto [_, inserted] = warned_addrs.insert(addr);
        if (!inserted)
            return;
        if (warned_addrs.size() > 4096) {
            warned_addrs.clear();
            warned_addrs.insert(addr);
        }
    }

    if (has_symbol) {
        CO_WQ_LOG_WARN("[proxy] wq_debug_check_func_addr suspicious func=%p (%s+0x%llx) frames=%u reason=%s",
                       reinterpret_cast<void*>(addr),
                       func_symbol->Name,
                       static_cast<unsigned long long>(displacement),
                       frames,
                       reason ? reason : "?");
    } else {
        CO_WQ_LOG_WARN("[proxy] wq_debug_check_func_addr suspicious func=%p frames=%u reason=%s",
                       reinterpret_cast<void*>(addr),
                       frames,
                       reason ? reason : "?");
    }
    for (USHORT i = 0; i < frames; ++i) {
        auto*                              frame_addr = stack[i];
        DWORD64                            frame_disp = 0;
        alignas(SYMBOL_INFO) unsigned char symbol_buffer[sizeof(SYMBOL_INFO) + max_name_len];
        auto*                              symbol_info = reinterpret_cast<PSYMBOL_INFO>(symbol_buffer);
        symbol_info->SizeOfStruct                      = sizeof(SYMBOL_INFO);
        symbol_info->MaxNameLen                        = max_name_len;
        if (SymFromAddr(process, reinterpret_cast<DWORD64>(frame_addr), &frame_disp, symbol_info)) {
            CO_WQ_LOG_WARN("  [%u] %p (%s+0x%llx)",
                           static_cast<unsigned>(i),
                           frame_addr,
                           symbol_info->Name,
                           static_cast<unsigned long long>(frame_disp));
        } else {
            CO_WQ_LOG_WARN("  [%u] %p", static_cast<unsigned>(i), frame_addr);
        }
    }
}
#endif

// 是否开启调试日志，可根据需要在运行时调整。
std::atomic_bool g_debug_logging { false };

std::string getenv_string(const char* name)
{
    if (!name)
        return {};
#if defined(_WIN32)
    char*  buffer = nullptr;
    size_t length = 0;
    if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr)
        return {};
    std::string value(buffer);
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string {};
#endif
}

std::string hex_encode_upper(const std::uint8_t* data, std::size_t len)
{
    static constexpr char hex_table[] = "0123456789ABCDEF";
    std::string           result;
    result.resize(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        std::uint8_t byte = data[i];
        result[2 * i]     = hex_table[(byte >> 4) & 0x0F];
        result[2 * i + 1] = hex_table[byte & 0x0F];
    }
    return result;
}

class TlsKeyLogRegistry {
public:
    static TlsKeyLogRegistry& instance()
    {
        static TlsKeyLogRegistry registry;
        return registry;
    }

    void configure(const std::filesystem::path& project_root)
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (configured_)
            return;

        std::string src_env = getenv_string("CO_WQ_PROXY_KEYLOG_SOURCE");
        std::string out_env = getenv_string("CO_WQ_PROXY_KEYLOG_OUTPUT");
        std::string ssl_env = getenv_string("SSLKEYLOGFILE");

        std::filesystem::path source = src_env.empty() ? std::filesystem::path {} : std::filesystem::path(src_env);
        if (source.empty() && !ssl_env.empty())
            source = std::filesystem::path(ssl_env);
        std::filesystem::path output = out_env.empty() ? std::filesystem::path {} : std::filesystem::path(out_env);

        if (!project_root.empty()) {
            if (!source.empty() && !source.is_absolute())
                source = project_root / source;
            if (!output.empty() && !output.is_absolute())
                output = project_root / output;
        }

        if (source.empty() && !project_root.empty())
            source = project_root / "logs" / "tls_keys.log";
        if (output.empty() && !project_root.empty())
            output = project_root / "logs" / "proxy_tls_keys.log";

        if (!source.empty()) {
            auto parent = source.parent_path();
            if (!parent.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(parent, ec);
            }
        }
        if (!output.empty()) {
            auto parent = output.parent_path();
            if (!parent.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(parent, ec);
            }
        }

        if (source.empty() || output.empty()) {
            configured_ = true;
            return;
        }

        source_path_ = source.lexically_normal();
        output_path_ = output.lexically_normal();
        enabled_.store(true, std::memory_order_release);
        configured_ = true;
        CO_WQ_LOG_INFO("[proxy] TLS key log source=%s output=%s",
                       source_path_.string().c_str(),
                       output_path_.string().c_str());
        std::atexit(&TlsKeyLogRegistry::shutdown_static);
    }

    bool is_enabled() const { return enabled_.load(std::memory_order_acquire); }

    void enqueue(const std::string& peer_id, uint16_t remote_port, const std::string& client_random)
    {
        if (!is_enabled() || client_random.empty())
            return;
        Pending pending;
        pending.peer_id       = peer_id;
        pending.remote_port   = remote_port;
        pending.client_random = client_random;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            queue_.push_back(std::move(pending));
            if (!worker_started_)
                start_worker_locked();
        }
        cv_.notify_one();
    }

    void shutdown()
    {
        if (!worker_started_)
            return;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            stop_requested_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable())
            worker_.join();
        worker_started_ = false;
    }

    static void shutdown_static() { instance().shutdown(); }

private:
    struct Pending {
        std::string client_random;
        std::string peer_id;
        uint16_t    remote_port { 0 };
    };

    void start_worker_locked()
    {
        if (worker_started_)
            return;
        stop_requested_ = false;
        worker_         = std::thread([this] { worker_loop(); });
        worker_started_ = true;
    }

    std::optional<std::string> lookup_secret_once(const std::string& client_random)
    {
        if (source_path_.empty())
            return std::nullopt;
        std::ifstream in(source_path_);
        if (!in.is_open())
            return std::nullopt;
        std::string line;
        std::string prefix = "CLIENT_RANDOM " + client_random + ' ';
        while (std::getline(in, line)) {
            if (line.rfind(prefix, 0) == 0) {
                return line.substr(prefix.size());
            }
        }
        return std::nullopt;
    }

    std::optional<std::string> wait_for_secret(const std::string& client_random)
    {
        using namespace std::chrono_literals;
        constexpr int max_attempts = 15;
        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            if (auto secret = lookup_secret_once(client_random); secret.has_value())
                return secret;
            std::this_thread::sleep_for(100ms);
        }
        return std::nullopt;
    }

    void write_output(const Pending& pending, const std::string& secret)
    {
        if (output_path_.empty())
            return;
        std::ofstream out(output_path_, std::ios::app);
        if (!out.is_open()) {
            CO_WQ_LOG_WARN("[proxy] failed to open TLS key log output %s", output_path_.string().c_str());
            return;
        }
        out << pending.peer_id;
        if (pending.remote_port != 0)
            out << " remote_port=" << pending.remote_port;
        out << " CLIENT_RANDOM " << pending.client_random << ' ' << secret << '\n';
        out.flush();
    }

    void process_pending(Pending pending)
    {
        if (source_path_.empty() || output_path_.empty())
            return;
        auto secret = wait_for_secret(pending.client_random);
        if (secret) {
            write_output(pending, *secret);
            CO_WQ_LOG_INFO("[proxy] recorded TLS key for %s random=%s",
                           pending.peer_id.c_str(),
                           pending.client_random.c_str());
        } else {
            CO_WQ_LOG_WARN("[proxy] TLS key for %s random=%s not found in %s",
                           pending.peer_id.c_str(),
                           pending.client_random.c_str(),
                           source_path_.string().c_str());
        }
    }

    void worker_loop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            cv_.wait(lock, [this] { return stop_requested_ || !queue_.empty(); });
            if (stop_requested_ && queue_.empty())
                break;
            if (queue_.empty())
                continue;
            Pending pending = std::move(queue_.front());
            queue_.pop_front();
            lock.unlock();
            process_pending(std::move(pending));
            lock.lock();
        }
    }

    mutable std::mutex      mutex_;
    std::condition_variable cv_;
    std::deque<Pending>     queue_;
    std::thread             worker_;
    std::filesystem::path   source_path_;
    std::filesystem::path   output_path_;
    std::atomic_bool        enabled_ { false };
    bool                    configured_ { false };
    bool                    worker_started_ { false };
    bool                    stop_requested_ { false };
};

struct TlsClientHelloSniffer {
    bool                      active { false };
    bool                      recorded { false };
    std::string               peer_id;
    uint16_t                  remote_port { 0 };
    std::vector<std::uint8_t> buffer;

    void activate(const std::string& peer, uint16_t port)
    {
        active      = true;
        peer_id     = peer;
        remote_port = port;
        buffer.clear();
        buffer.reserve(512);
    }

    void feed(const char* data, std::size_t len)
    {
        if (!active || recorded || len == 0)
            return;
        buffer.insert(buffer.end(),
                      reinterpret_cast<const std::uint8_t*>(data),
                      reinterpret_cast<const std::uint8_t*>(data) + len);
        parse();
        if (buffer.size() > 4096)
            buffer.erase(buffer.begin(), buffer.end());
    }

private:
    void parse()
    {
        if (buffer.size() < 43)
            return;
        std::size_t offset = 0;
        while (offset + 43 <= buffer.size()) {
            std::uint8_t record_type = buffer[offset];
            std::size_t  record_len  = (static_cast<std::size_t>(buffer[offset + 3]) << 8)
                | static_cast<std::size_t>(buffer[offset + 4]);
            if (offset + 5 + record_len > buffer.size())
                return; // need more data
            if (record_type != 0x16) {
                recorded = true;
                return;
            }
            if (record_len < 38) {
                recorded = true;
                return;
            }
            std::uint8_t handshake_type = buffer[offset + 5];
            if (handshake_type != 0x01) {
                recorded = true;
                return;
            }
            if (offset + 43 > buffer.size())
                return;
            const std::uint8_t* random_ptr = buffer.data() + offset + 5 + 4 + 2;
            std::string         random_hex = hex_encode_upper(random_ptr, 32);
            TlsKeyLogRegistry::instance().enqueue(peer_id, remote_port, random_hex);
            recorded = true;
            return;
        }
    }
};

uint16_t get_remote_port(net::tcp_socket<SpinLock>& socket)
{
#if defined(_WIN32)
    SOCKET handle = socket.native_handle();
    if (handle == INVALID_SOCKET)
        return 0;
    sockaddr_storage addr {};
    int              len = static_cast<int>(sizeof(addr));
    if (::getpeername(handle, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        return 0;
#else
    int handle = socket.native_handle();
    if (handle < 0)
        return 0;
    sockaddr_storage addr {};
    socklen_t        len = static_cast<socklen_t>(sizeof(addr));
    if (::getpeername(handle, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        return 0;
#endif
    if (addr.ss_family == AF_INET) {
        auto* in = reinterpret_cast<sockaddr_in*>(&addr);
        return ntohs(in->sin_port);
    }
    if (addr.ss_family == AF_INET6) {
        auto* in6 = reinterpret_cast<sockaddr_in6*>(&addr);
        return ntohs(in6->sin6_port);
    }
    return 0;
}

#if defined(_WIN32)
LONG WINAPI proxy_exception_filter(EXCEPTION_POINTERS* info)
{
    auto* record = info ? info->ExceptionRecord : nullptr;
    DWORD code   = record ? record->ExceptionCode : 0;
    void* addr   = record ? record->ExceptionAddress : nullptr;
    CO_WQ_LOG_ERROR("[proxy] unhandled exception code=0x%08lx addr=%p", static_cast<unsigned long>(code), addr);
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
            CO_WQ_LOG_ERROR("  location: %s+0x%llx %s:%lu",
                            symbol_info->Name,
                            static_cast<unsigned long long>(displacement),
                            line_info.FileName ? line_info.FileName : "<unknown>",
                            static_cast<unsigned long>(line_info.LineNumber));
        } else {
            CO_WQ_LOG_ERROR("  location: %s+0x%llx", symbol_info->Name, static_cast<unsigned long long>(displacement));
        }
    } else {
        CO_WQ_LOG_ERROR("  location: <unknown>");
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

        HANDLE thread_handle        = GetCurrentThread();
        HANDLE process_handle_stack = GetCurrentProcess();
        for (USHORT i = 0;; ++i) {
            BOOL ok = StackWalk64(IMAGE_FILE_MACHINE_AMD64,
                                  process_handle_stack,
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
            CO_WQ_LOG_ERROR("  frame[%u] = %p", static_cast<unsigned>(i), reinterpret_cast<void*>(frame_addr));
            DWORD64                            frame_disp = 0;
            alignas(SYMBOL_INFO) unsigned char frame_symbol_buffer[sizeof(SYMBOL_INFO) + max_name_len];
            auto*                              frame_symbol_info = reinterpret_cast<PSYMBOL_INFO>(frame_symbol_buffer);
            frame_symbol_info->SizeOfStruct                      = sizeof(SYMBOL_INFO);
            frame_symbol_info->MaxNameLen                        = max_name_len;
            if (SymFromAddr(process_handle_stack, frame_addr, &frame_disp, frame_symbol_info)) {
                IMAGEHLP_LINE64 line_info {};
                line_info.SizeOfStruct = sizeof(line_info);
                DWORD line_disp        = 0;
                if (SymGetLineFromAddr64(process_handle_stack, frame_addr, &line_disp, &line_info)) {
                    CO_WQ_LOG_ERROR("    -> (%s+0x%llx) %s:%lu",
                                    frame_symbol_info->Name,
                                    static_cast<unsigned long long>(frame_disp),
                                    line_info.FileName ? line_info.FileName : "<unknown>",
                                    static_cast<unsigned long>(line_info.LineNumber));
                } else {
                    CO_WQ_LOG_ERROR("    -> (%s+0x%llx)",
                                    frame_symbol_info->Name,
                                    static_cast<unsigned long long>(frame_disp));
                }
            } else {
                CO_WQ_LOG_ERROR("    -> <no symbol>");
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

    std::ostringstream oss;
    oss << "[proxy] " << message;
    auto formatted = oss.str();
    CO_WQ_LOG_DEBUG("%s", formatted.c_str());
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

#if defined(_WIN32)
std::string describe_upstream_peer(net::tcp_socket<SpinLock>& socket)
{
    SOCKET handle = socket.native_handle();
    if (handle == INVALID_SOCKET)
        return "upstream peer=<invalid>";
    sockaddr_storage addr {};
    int              len = static_cast<int>(sizeof(addr));
    if (::getpeername(handle, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        int                err = WSAGetLastError();
        std::ostringstream oss;
        oss << "upstream peer=<error " << err << '>';
        return oss.str();
    }
    char     ip_buf[INET6_ADDRSTRLEN] = {};
    uint16_t port                     = 0;
    if (addr.ss_family == AF_INET) {
        auto* in = reinterpret_cast<sockaddr_in*>(&addr);
        port     = ntohs(in->sin_port);
        if (::inet_ntop(AF_INET, &in->sin_addr, ip_buf, sizeof(ip_buf)) == nullptr) {
            std::ostringstream oss;
            oss << "upstream peer=<inet_ntop4 err=" << WSAGetLastError() << '>';
            return oss.str();
        }
        std::ostringstream oss;
        oss << "upstream peer=" << ip_buf << ':' << port << " (IPv4)";
        return oss.str();
    }
    if (addr.ss_family == AF_INET6) {
        auto* in6 = reinterpret_cast<sockaddr_in6*>(&addr);
        port      = ntohs(in6->sin6_port);
        if (::inet_ntop(AF_INET6, &in6->sin6_addr, ip_buf, sizeof(ip_buf)) == nullptr) {
            std::ostringstream oss;
            oss << "upstream peer=<inet_ntop6 err=" << WSAGetLastError() << '>';
            return oss.str();
        }
        std::ostringstream oss;
        oss << "upstream peer=[" << ip_buf << "]:" << port << " (IPv6)";
        return oss.str();
    }
    std::ostringstream oss;
    oss << "upstream peer=<family " << addr.ss_family << '>';
    return oss.str();
}
#else
std::string describe_upstream_peer(net::tcp_socket<SpinLock>& socket)
{
    int                fd = socket.native_handle();
    auto               id = format_peer_id(fd);
    std::ostringstream oss;
    oss << "upstream peer=" << id;
    return oss.str();
}
#endif

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
        CO_WQ_LOG_WARN("[proxy] string integrity alert: %s data=%p size=%zu cap=%zu ptr_bad=%d buf_bad=%d",
                       label,
                       static_cast<const void*>(data),
                       static_cast<size_t>(size),
                       static_cast<size_t>(cap),
                       suspicious_ptr ? 1 : 0,
                       suspicious_buf ? 1 : 0);
#if defined(_WIN32)
        __debugbreak();
#endif
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

std::string strip_brackets(const std::string& input)
{
    if (input.size() >= 2 && input.front() == '[' && input.back() == ']')
        return input.substr(1, input.size() - 2);
    return input;
}

struct UpstreamCandidateStats {
    std::chrono::steady_clock::time_point last_attempt {};
    std::chrono::steady_clock::time_point last_success {};
    std::chrono::steady_clock::time_point last_failure {};
    std::chrono::steady_clock::time_point retry_after {};
    std::chrono::milliseconds             smoothed_rtt { 0 };
    unsigned                              consecutive_failures { 0 };
    std::uint64_t                         total_attempts { 0 };
    std::uint64_t                         total_failures { 0 };
};

struct UpstreamEndpointState {
    std::unordered_map<std::string, UpstreamCandidateStats> candidates;
};

SpinLock                                               g_upstream_stats_lock;
std::unordered_map<std::string, UpstreamEndpointState> g_upstream_stats;
constexpr std::chrono::milliseconds                    k_default_connect_rtt { 200 };
constexpr std::chrono::seconds                         k_failure_decay_window { 30 };

std::string canonicalize_host_for_key(const std::string& host)
{
    return to_lower(strip_brackets(host));
}

std::string make_endpoint_key(const std::string& host, uint16_t port)
{
    std::ostringstream oss;
    oss << canonicalize_host_for_key(host) << ':' << port;
    return oss.str();
}

std::string format_numeric_endpoint(const sockaddr* addr, int len)
{
    if (!addr || len <= 0)
        return {};
    char host_buf[NI_MAXHOST] = {};
    char serv_buf[NI_MAXSERV] = {};
    if (::getnameinfo(addr,
                      len,
                      host_buf,
                      sizeof(host_buf),
                      serv_buf,
                      sizeof(serv_buf),
                      NI_NUMERICHOST | NI_NUMERICSERV)
        != 0) {
        return {};
    }
    std::string host(host_buf);
    if (addr->sa_family == AF_INET6 && host.find(':') != std::string::npos && host.front() != '[') {
        host.insert(host.begin(), '[');
        host.push_back(']');
    }
    if (serv_buf[0] != '\0') {
        host.push_back(':');
        host.append(serv_buf);
    }
    return host;
}

std::string format_candidate_label(const sockaddr* addr, int len)
{
    if (!addr || len <= 0)
        return "<invalid>";

    if (addr->sa_family == AF_INET) {
        auto* in                   = reinterpret_cast<const sockaddr_in*>(addr);
        char  buf[INET_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET, &in->sin_addr, buf, sizeof(buf))) {
            std::ostringstream oss;
            oss << buf << ':' << ntohs(in->sin_port);
            return oss.str();
        }
        return "<ipv4-error>";
    }

    if (addr->sa_family == AF_INET6) {
        auto* in6 = reinterpret_cast<const sockaddr_in6*>(addr);
        if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
            in_addr v4 {};
            std::memcpy(&v4, reinterpret_cast<const unsigned char*>(&in6->sin6_addr) + 12, sizeof(v4));
            char buf[INET_ADDRSTRLEN] = {};
            if (::inet_ntop(AF_INET, &v4, buf, sizeof(buf))) {
                std::ostringstream oss;
                oss << buf << ':' << ntohs(in6->sin6_port);
                return oss.str();
            }
        }
        char buf[INET6_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET6, &in6->sin6_addr, buf, sizeof(buf))) {
            std::ostringstream oss;
            oss << '[' << buf << "]:" << ntohs(in6->sin6_port);
            return oss.str();
        }
        return "<ipv6-error>";
    }

    return "<unknown-family>";
}

std::chrono::milliseconds compute_failure_backoff(unsigned failure_streak)
{
    static constexpr std::array<int, 8> k_backoff_ms { 0, 250, 500, 1000, 2000, 4000, 8000, 16000 };
    size_t                              idx = std::min<size_t>(failure_streak, k_backoff_ms.size() - 1);
    return std::chrono::milliseconds { k_backoff_ms[idx] };
}

UpstreamCandidateStats record_upstream_outcome(const std::string&        endpoint_key,
                                               const std::string&        candidate_key,
                                               bool                      success,
                                               std::chrono::milliseconds latency)
{
    auto            now = std::chrono::steady_clock::now();
    std::lock_guard guard(g_upstream_stats_lock);
    auto&           endpoint = g_upstream_stats[endpoint_key];
    auto&           stats    = endpoint.candidates[candidate_key];
    stats.total_attempts++;
    stats.last_attempt = now;
    if (success) {
        stats.last_success         = now;
        stats.consecutive_failures = 0;
        stats.retry_after          = now;
        if (latency.count() > 0) {
            if (stats.smoothed_rtt.count() == 0) {
                stats.smoothed_rtt = latency;
            } else {
                auto delta = latency - stats.smoothed_rtt;
                stats.smoothed_rtt += delta / 4;
            }
        }
    } else {
        stats.total_failures++;
        stats.last_failure = now;
        if (stats.consecutive_failures < std::numeric_limits<unsigned>::max())
            stats.consecutive_failures++;
        auto backoff      = compute_failure_backoff(stats.consecutive_failures);
        stats.retry_after = now + backoff;
    }
    return stats;
}

struct RawUpstreamCandidate {
    sockaddr_storage addr {};
    int              len { 0 };
    std::string      label;
    std::string      stats_key;
    std::size_t      ordinal { 0 };
};

struct CandidateSnapshot {
    RawUpstreamCandidate   candidate;
    UpstreamCandidateStats stats;
};

std::vector<RawUpstreamCandidate>
resolve_upstream_candidates(net::tcp_socket<SpinLock>& socket, const std::string& host, uint16_t port)
{
    std::vector<RawUpstreamCandidate> output;
    std::string                       node = strip_brackets(host);
    if (node.empty())
        return output;

    int  family     = socket.family();
    bool dual_stack = socket.dual_stack();

    addrinfo hints {};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_family   = family;
    if (family == AF_INET6 && dual_stack)
        hints.ai_family = AF_UNSPEC;

    char service[16] = {};
    std::snprintf(service, sizeof(service), "%u", static_cast<unsigned>(port));

    addrinfo* result = nullptr;
    int       rc     = ::getaddrinfo(node.c_str(), service, &hints, &result);
    if (rc != 0 || !result)
        return output;

    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard(result, ::freeaddrinfo);

    struct CandidateEntry {
        sockaddr_storage addr {};
        int              len { 0 };
        int              family { AF_UNSPEC };
    };

    std::vector<CandidateEntry> preferred;
    std::vector<CandidateEntry> fallback;

    for (auto* ai = result; ai; ai = ai->ai_next) {
        if (!ai->ai_addr || ai->ai_addrlen <= 0 || ai->ai_addrlen > static_cast<int>(sizeof(sockaddr_storage)))
            continue;
        CandidateEntry entry;
        if (family == AF_INET6 && dual_stack && ai->ai_family == AF_INET) {
            auto* src = reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
            auto* dst = reinterpret_cast<sockaddr_in6*>(&entry.addr);
            std::memset(dst, 0, sizeof(*dst));
            dst->sin6_family     = AF_INET6;
            dst->sin6_port       = src->sin_port;
            unsigned char* bytes = reinterpret_cast<unsigned char*>(&dst->sin6_addr);
            bytes[10]            = 0xFF;
            bytes[11]            = 0xFF;
            std::memcpy(bytes + 12, &src->sin_addr, sizeof(src->sin_addr));
            entry.len    = static_cast<int>(sizeof(sockaddr_in6));
            entry.family = ai->ai_family;
            preferred.push_back(entry);
        } else if (ai->ai_family == family) {
            std::memcpy(&entry.addr, ai->ai_addr, static_cast<size_t>(ai->ai_addrlen));
            entry.len    = static_cast<int>(ai->ai_addrlen);
            entry.family = ai->ai_family;
            if (family == AF_INET6 && dual_stack)
                fallback.push_back(entry);
            else
                preferred.push_back(entry);
        }
    }

    size_t ordinal        = 0;
    auto   append_entries = [&](const std::vector<CandidateEntry>& entries) {
        for (const auto& src_entry : entries) {
            RawUpstreamCandidate candidate;
            candidate.addr = src_entry.addr;
            candidate.len  = src_entry.len;
            candidate.label = format_candidate_label(reinterpret_cast<const sockaddr*>(&candidate.addr), candidate.len);
            candidate.stats_key = format_numeric_endpoint(reinterpret_cast<const sockaddr*>(&candidate.addr),
                                                          candidate.len);
            if (candidate.stats_key.empty()) {
                std::ostringstream key;
                key << "candidate#" << ordinal;
                candidate.stats_key = key.str();
            }
            if (candidate.label.empty())
                candidate.label = candidate.stats_key;
            candidate.ordinal = ordinal;
            output.push_back(std::move(candidate));
            ++ordinal;
        }
    };

    output.reserve(preferred.size() + fallback.size());
    append_entries(preferred);
    append_entries(fallback);

    return output;
}

std::vector<CandidateSnapshot> snapshot_upstream_candidates(const std::string&                       endpoint_key,
                                                            const std::vector<RawUpstreamCandidate>& raw)
{
    std::vector<CandidateSnapshot> result;
    result.reserve(raw.size());
    std::lock_guard guard(g_upstream_stats_lock);
    auto&           endpoint = g_upstream_stats[endpoint_key];
    for (const auto& candidate : raw) {
        auto [it, inserted] = endpoint.candidates.emplace(candidate.stats_key, UpstreamCandidateStats {});
        (void)inserted;
        result.push_back(CandidateSnapshot { candidate, it->second });
    }
    return result;
}

struct CandidatePriority {
    bool                                  available { true };
    std::chrono::steady_clock::time_point ready_after {};
    std::chrono::milliseconds             rtt { k_default_connect_rtt };
    unsigned                              failure_penalty { 0 };
    std::uint64_t                         total_failures { 0 };
    std::chrono::steady_clock::time_point last_success {};
};

CandidatePriority compute_candidate_priority(const UpstreamCandidateStats&         stats,
                                             std::chrono::steady_clock::time_point now)
{
    CandidatePriority priority;
    priority.available   = (stats.retry_after.time_since_epoch().count() == 0) || (stats.retry_after <= now);
    priority.ready_after = stats.retry_after;
    if (stats.smoothed_rtt.count() > 0)
        priority.rtt = stats.smoothed_rtt;

    unsigned penalty = stats.consecutive_failures;
    if (penalty > 0 && stats.last_failure.time_since_epoch().count() != 0) {
        auto elapsed     = now - stats.last_failure;
        auto decay_steps = static_cast<unsigned>(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count()
                                                 / k_failure_decay_window.count());
        if (decay_steps >= penalty)
            penalty = 0;
        else
            penalty -= decay_steps;
    }
    priority.failure_penalty = penalty;
    priority.total_failures  = stats.total_failures;
    priority.last_success    = stats.last_success;
    return priority;
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

bool split_host_port(const std::string& input, uint16_t default_port, std::string& host_out, uint16_t& port_out)
{
    if (input.empty())
        return false;

    std::string host;
    std::string port_str;

    if (input.front() == '[') {
        auto close = input.find(']');
        if (close == std::string::npos)
            return false;
        host        = input.substr(1, close - 1);
        size_t next = close + 1;
        if (next < input.size()) {
            if (input[next] != ':')
                return false;
            port_str = input.substr(next + 1);
        }
    } else {
        auto colon = input.find(':');
        if (colon != std::string::npos) {
            if (input.find(':', colon + 1) != std::string::npos)
                return false; // IPv6 literal must be bracketed
            host     = input.substr(0, colon);
            port_str = input.substr(colon + 1);
        } else {
            host = input;
        }
    }

    if (host.empty())
        return false;

    uint16_t port_value = default_port;
    if (!port_str.empty()) {
        if (!parse_port(port_str, port_value))
            return false;
    }

    host_out = host;
    port_out = port_value;
    return true;
}

std::string format_host_for_log(const std::string& host)
{
    if (host.find(':') != std::string::npos) {
        std::ostringstream oss;
        oss << '[' << host << ']';
        return oss.str();
    }
    return host;
}

std::filesystem::path find_project_root(const std::filesystem::path& start_dir)
{
    std::error_code ec;
    auto            current = std::filesystem::weakly_canonical(start_dir, ec);
    if (ec)
        current = start_dir;

    while (!current.empty()) {
        auto candidate_xmake = current / "xmake.lua";
        if (std::filesystem::exists(candidate_xmake, ec) && !ec)
            return current;

        auto candidate_git = current / ".git";
        if (std::filesystem::exists(candidate_git, ec) && !ec)
            return current;

        auto parent = current.parent_path();
        if (parent == current)
            break;
        current = std::move(parent);
    }

    return {};
}

struct RelayMetrics {
    std::uint64_t total_recv_bytes { 0 };
    std::uint64_t total_sent_bytes { 0 };
    std::size_t   recv_ops { 0 };
    std::size_t   send_ops { 0 };
    std::size_t   partial_send_ops { 0 };
    std::size_t   send_overrun_ops { 0 };
    std::uint64_t send_overrun_bytes { 0 };
    ssize_t       last_recv_rc { 0 };
    ssize_t       last_send_rc { 0 };
    bool          send_failed { false };
    bool          recv_failed { false };
    bool          finished { false };
    bool          request_shutdown_tx { false };
    bool          shutdown_tx_performed { false };
    std::string   termination_reason;
};

struct IoErrorInfo {
    std::string reason;
    bool        failure { false };
    bool        cancelled { false };
    bool        peer_reset { false };
};

#if defined(_WIN32)
std::string lookup_windows_socket_symbol(int err)
{
#ifdef WSA_OPERATION_ABORTED
    if (err == WSA_OPERATION_ABORTED)
        return "WSA_OPERATION_ABORTED";
#endif
    switch (err) {
    case ERROR_OPERATION_ABORTED:
        return "ERROR_OPERATION_ABORTED";
    case ERROR_NETNAME_DELETED:
        return "ERROR_NETNAME_DELETED";
    case WSAECONNRESET:
        return "WSAECONNRESET";
    case WSAECONNABORTED:
        return "WSAECONNABORTED";
    case WSAENOTCONN:
        return "WSAENOTCONN";
    case WSAESHUTDOWN:
        return "WSAESHUTDOWN";
    case WSAETIMEDOUT:
        return "WSAETIMEDOUT";
    case WSAECONNREFUSED:
        return "WSAECONNREFUSED";
    case WSAENETRESET:
        return "WSAENETRESET";
    case WSAENETUNREACH:
        return "WSAENETUNREACH";
    case WSAEHOSTUNREACH:
        return "WSAEHOSTUNREACH";
    default:
        break;
    }
    return {};
}

std::string windows_error_message(int err)
{
    LPWSTR      buffer = nullptr;
    DWORD       flags  = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD       len    = FormatMessageW(flags,
                               nullptr,
                               static_cast<DWORD>(err),
                               MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                               reinterpret_cast<LPWSTR>(&buffer),
                               0,
                               nullptr);
    std::string message;
    if (len != 0 && buffer != nullptr) {
        int utf8_len = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
        if (utf8_len > 0) {
            message.resize(static_cast<size_t>(utf8_len));
            WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(len), message.data(), utf8_len, nullptr, nullptr);
        }
        LocalFree(buffer);
        while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) {
            message.pop_back();
        }
    }
    return message;
}

bool is_windows_cancellation_error(int err)
{
    // ERROR_IO_INCOMPLETE/WSA_IO_INCOMPLETE surfaces when a pending overlapped
    // operation is torn down by CancelIoEx during socket shutdown. Treat it as
    // cancellation so the relay summary does not report spurious failures.
    if (err == ERROR_OPERATION_ABORTED || err == ERROR_IO_INCOMPLETE)
        return true;
#ifdef WSA_OPERATION_ABORTED
    if (err == WSA_OPERATION_ABORTED)
        return true;
#endif
#ifdef WSA_IO_INCOMPLETE
    if (err == WSA_IO_INCOMPLETE)
        return true;
#endif
    return false;
}

bool is_windows_peer_reset(int err)
{
    switch (err) {
    case ERROR_NETNAME_DELETED:
    case WSAECONNRESET:
    case WSAECONNABORTED:
    case WSAENOTCONN:
    case WSAESHUTDOWN:
        return true;
    default:
        break;
    }
    return false;
}
#else
std::string lookup_posix_error_symbol(int err)
{
#if defined(__GLIBC__)
    if (const char* name = ::strerrorname_np(err))
        return name;
#endif
    return {};
}

std::string posix_error_message(int err)
{
    const char* msg = std::strerror(err);
    if (!msg)
        return {};
    return msg;
}

bool is_posix_cancellation_error(int err)
{
#ifdef ECANCELED
    if (err == ECANCELED)
        return true;
#endif
#ifdef EINTR
    if (err == EINTR)
        return true;
#endif
    return false;
}

bool is_posix_peer_reset(int err)
{
#ifdef ECONNRESET
    if (err == ECONNRESET)
        return true;
#endif
#ifdef ECONNABORTED
    if (err == ECONNABORTED)
        return true;
#endif
#ifdef ENOTCONN
    if (err == ENOTCONN)
        return true;
#endif
#ifdef EPIPE
    if (err == EPIPE)
        return true;
#endif
#ifdef ESHUTDOWN
    if (err == ESHUTDOWN)
        return true;
#endif
    return false;
}
#endif

IoErrorInfo classify_io_error(ssize_t rc, bool is_recv)
{
    IoErrorInfo info;
    if (rc == 0) {
        info.reason  = is_recv ? "recv_error(0)" : "send_error(0)";
        info.failure = true;
        return info;
    }
    if (rc > 0)
        return info;
    int err = static_cast<int>(-rc);
#if defined(_WIN32)
    bool        cancelled = is_windows_cancellation_error(err);
    bool        peer      = is_windows_peer_reset(err);
    std::string symbol    = lookup_windows_socket_symbol(err);
    std::string message   = windows_error_message(err);
#else
    bool        cancelled = is_posix_cancellation_error(err);
    bool        peer      = is_posix_peer_reset(err);
    std::string symbol    = lookup_posix_error_symbol(err);
    std::string message   = posix_error_message(err);
#endif
    std::ostringstream oss;
    if (cancelled) {
        oss << (is_recv ? "recv_cancelled" : "send_cancelled");
    } else if (peer) {
        oss << (is_recv ? "recv_reset" : "send_reset");
    } else {
        oss << (is_recv ? "recv_error" : "send_error");
    }
    oss << '(' << err;
    if (!symbol.empty())
        oss << '/' << symbol;
    oss << ')';
    if (!message.empty())
        oss << ": " << message;
    info.reason     = oss.str();
    info.failure    = !(cancelled || peer);
    info.cancelled  = cancelled;
    info.peer_reset = peer;
    return info;
}

void log_relay_summary(const RelayMetrics& metrics, const std::string& context, const std::string& peer_id)
{
    std::ostringstream oss;
    oss << peer_id << ' ' << context << " summary: recv=" << metrics.total_recv_bytes << "B"
        << " send=" << metrics.total_sent_bytes << "B" << " recv_ops=" << metrics.recv_ops
        << " send_ops=" << metrics.send_ops << " partial_send_ops=" << metrics.partial_send_ops
        << " send_overrun_ops=" << metrics.send_overrun_ops << " send_overrun_bytes=" << metrics.send_overrun_bytes
        << " last_recv=" << metrics.last_recv_rc << " last_send=" << metrics.last_send_rc;
    long long diff = static_cast<long long>(metrics.total_recv_bytes)
        - static_cast<long long>(metrics.total_sent_bytes);
    if (diff != 0) {
        oss << " diff=" << diff;
    }
    oss << " reason=" << (metrics.termination_reason.empty() ? "?" : metrics.termination_reason);
    if (metrics.request_shutdown_tx || metrics.shutdown_tx_performed) {
        oss << " shutdown_tx="
            << (metrics.shutdown_tx_performed ? "performed" : (metrics.request_shutdown_tx ? "pending" : "skipped"));
    }
    auto message = oss.str();
    if (metrics.send_failed || metrics.recv_failed || diff != 0 || metrics.send_overrun_ops > 0
        || metrics.send_overrun_bytes > 0) {
        CO_WQ_LOG_WARN("%s", message.c_str());
    } else {
        CO_WQ_LOG_INFO("%s", message.c_str());
    }
    debug_log(message);
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
    debug_check_string_integrity("parse_http_url.authority", authority);
    std::string host_value;
    uint16_t    port_value = 80;
    if (!split_host_port(authority, 80, host_value, port_value))
        return std::nullopt;
    debug_check_string_integrity("parse_http_url.host", host_value);
    parts.host = std::move(host_value);
    parts.port = port_value;
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
    std::string   host;
    uint16_t      port = 443;
    ConnectTarget ct;
    if (!split_host_port(target, 443, host, port))
        return std::nullopt;
    ct.host = std::move(host);
    ct.port = port;
    return ct;
}

#if defined(_WIN32)
extern "C" void wq_debug_null_func(co_wq::worknode* node)
{
    CO_WQ_LOG_WARN("[proxy] wq_debug_null_func fired: node=%p", static_cast<void*>(node));
    if (node != nullptr) {
        auto* waiter            = reinterpret_cast<co_wq::net::io_waiter_base*>(node);
        bool  looks_like_waiter = waiter->debug_magic == co_wq::net::io_waiter_base::debug_magic_value;
        if (looks_like_waiter) {
            CO_WQ_LOG_WARN("  debug_name=%s h=%p callback_enqueued=%s route_post=%p route_ctx=%p",
                           waiter->debug_name ? waiter->debug_name : "<null>",
                           waiter->h ? waiter->h.address() : nullptr,
                           waiter->callback_enqueued.load(std::memory_order_relaxed) ? "true" : "false",
                           reinterpret_cast<void*>(waiter->route_post),
                           waiter->route_ctx);
        } else {
            CO_WQ_LOG_WARN("  node is not recognized as io_waiter_base (debug_magic mismatch)");
        }
    }
    void*           stack[32] {};
    constexpr DWORD capacity = static_cast<DWORD>(std::size(stack));
    USHORT          frames   = CaptureStackBackTrace(0, capacity, stack, nullptr);
    for (USHORT i = 0; i < frames; ++i) {
        CO_WQ_LOG_WARN("  stack[%u]=%p", static_cast<unsigned>(i), stack[i]);
    }
}
#endif

net::tcp_socket<SpinLock> make_upstream_socket(NetFdWorkqueue& fdwq)
{
    static std::atomic_bool warned { false };
    try {
        return fdwq.make_tcp_socket(AF_INET6, true);
    } catch (const std::exception& ex) {
        if (!warned.exchange(true)) {
            CO_WQ_LOG_WARN("[proxy] dual-stack upstream socket unavailable, falling back to IPv4: %s", ex.what());
        }
    }
    return fdwq.make_tcp_socket(AF_INET, false);
}

void configure_graceful_close(net::tcp_socket<SpinLock>& socket, const std::string& peer_id, std::string_view role)
{
#if defined(_WIN32)
    SOCKET handle = socket.native_handle();
    if (handle == INVALID_SOCKET)
        return;
    // Avoid forcing linger on non-blocking sockets; the default FIN/ACK
    // shutdown path has proven more reliable for long HTTPS transfers.
    // Allow the operating system to manage FIN/ACK without forcing a timed
    // linger. For high-throughput TLS tunnels, an enforced linger on a
    // non-blocking socket can translate into RSTs when the grace window expires.
    ::linger linger_opts {};
    // Bias toward low-latency forwarding and provide ample buffering to absorb
    // bursty upstream records without stalling the relay workqueue.
    linger_opts.l_onoff = 0;
    if (::setsockopt(handle,
                     SOL_SOCKET,
                     SO_LINGER,
                     reinterpret_cast<const char*>(&linger_opts),
                     static_cast<int>(sizeof(linger_opts)))
        == SOCKET_ERROR) {
        int err = WSAGetLastError();
        CO_WQ_LOG_WARN("%s %.*s disable SO_LINGER failed: %d", peer_id.c_str(), (int)role.size(), role.data(), err);
    }
    DWORD nodelay = 1;
    if (::setsockopt(handle,
                     IPPROTO_TCP,
                     TCP_NODELAY,
                     reinterpret_cast<const char*>(&nodelay),
                     static_cast<int>(sizeof(nodelay)))
        == SOCKET_ERROR) {
        int err = WSAGetLastError();
        CO_WQ_LOG_WARN("%s %.*s enable TCP_NODELAY failed: %d", peer_id.c_str(), (int)role.size(), role.data(), err);
    } else {
        debug_log(peer_id + " " + std::string(role) + " TCP_NODELAY=on");
    }
    int buffer_hint = 1 << 20; // 1 MiB
    if (::setsockopt(handle,
                     SOL_SOCKET,
                     SO_SNDBUF,
                     reinterpret_cast<const char*>(&buffer_hint),
                     static_cast<int>(sizeof(buffer_hint)))
        == SOCKET_ERROR) {
        int err = WSAGetLastError();
        CO_WQ_LOG_WARN("%s %.*s enlarge SO_SNDBUF failed: %d", peer_id.c_str(), (int)role.size(), role.data(), err);
    }
    if (::setsockopt(handle,
                     SOL_SOCKET,
                     SO_RCVBUF,
                     reinterpret_cast<const char*>(&buffer_hint),
                     static_cast<int>(sizeof(buffer_hint)))
        == SOCKET_ERROR) {
        int err = WSAGetLastError();
        CO_WQ_LOG_WARN("%s %.*s enlarge SO_RCVBUF failed: %d", peer_id.c_str(), (int)role.size(), role.data(), err);
    }
#else
    (void)socket;
    (void)peer_id;
    (void)role;
#endif
}

void configure_tcp_keepalive(net::tcp_socket<SpinLock>& socket, const std::string& peer_id, std::string_view role)
{
#if defined(_WIN32)
    SOCKET handle = socket.native_handle();
    if (handle == INVALID_SOCKET)
        return;
    tcp_keepalive settings {};
    settings.onoff             = 1;
    settings.keepalivetime     = 60'000; // 60s idle
    settings.keepaliveinterval = 5'000;  // retry every 5s
    DWORD       ignored        = 0;
    const DWORD bytes          = 0;
    (void)bytes;
    if (WSAIoctl(handle,
                 SIO_KEEPALIVE_VALS,
                 &settings,
                 static_cast<DWORD>(sizeof(settings)),
                 nullptr,
                 0,
                 &ignored,
                 nullptr,
                 nullptr)
        == SOCKET_ERROR) {
        int err = WSAGetLastError();
        CO_WQ_LOG_WARN("%s %.*s configure keepalive failed: %d", peer_id.c_str(), (int)role.size(), role.data(), err);
    } else {
        debug_log(peer_id + " " + std::string(role) + " keepalive=60s/5s");
    }
#else
    (void)socket;
    (void)peer_id;
    (void)role;
#endif
}

Task<int, Work_Promise<SpinLock, int>>
connect_upstream(net::tcp_socket<SpinLock>& socket, NetFdWorkqueue& fdwq, const std::string& host, uint16_t port)
{
    auto endpoint_key = make_endpoint_key(host, port);
    auto raw          = resolve_upstream_candidates(socket, host, port);

    auto attempt_single = [&](const std::string& stats_key) -> Task<int, Work_Promise<SpinLock, int>> {
        auto start   = std::chrono::steady_clock::now();
        int  rc      = co_await socket.connect(host, port);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        auto updated = record_upstream_outcome(endpoint_key, stats_key, rc == 0, elapsed);
        std::ostringstream oss;
        oss << format_host_for_log(host) << ':' << port << " direct dial rc=" << rc;
        if (rc == 0) {
            oss << " rtt=" << elapsed.count() << "ms";
        } else {
            oss << " failures=" << updated.consecutive_failures << " cooldown=";
            if (updated.retry_after.time_since_epoch().count() == 0) {
                oss << "0ms";
            } else {
                auto now    = std::chrono::steady_clock::now();
                auto remain = updated.retry_after <= now
                    ? std::chrono::milliseconds { 0 }
                    : std::chrono::duration_cast<std::chrono::milliseconds>(updated.retry_after - now);
                oss << remain.count() << "ms";
            }
        }
        debug_log(oss.str());
        co_return rc;
    };

    if (raw.empty()) {
        std::string stats_key = canonicalize_host_for_key(host);
        stats_key.push_back(':');
        stats_key.append(std::to_string(port));
        co_return co_await attempt_single(stats_key);
    }

    auto snapshots = snapshot_upstream_candidates(endpoint_key, raw);
    auto now       = std::chrono::steady_clock::now();

    struct AttemptPlan {
        CandidateSnapshot snapshot;
        CandidatePriority priority;
    };

    std::vector<AttemptPlan> plans;
    plans.reserve(snapshots.size());
    for (auto& snapshot : snapshots) {
        AttemptPlan plan { snapshot, compute_candidate_priority(snapshot.stats, now) };
        plans.push_back(plan);
    }

    std::sort(plans.begin(), plans.end(), [](const AttemptPlan& lhs, const AttemptPlan& rhs) {
        const auto& a = lhs.priority;
        const auto& b = rhs.priority;
        if (a.available != b.available)
            return a.available > b.available;
        if (a.available && b.available) {
            if (a.failure_penalty != b.failure_penalty)
                return a.failure_penalty < b.failure_penalty;
            if (a.rtt != b.rtt)
                return a.rtt < b.rtt;
        } else {
            if (a.ready_after != b.ready_after)
                return a.ready_after < b.ready_after;
        }
        if (a.total_failures != b.total_failures)
            return a.total_failures < b.total_failures;
        if (a.last_success != b.last_success)
            return a.last_success > b.last_success;
        if (lhs.snapshot.candidate.ordinal != rhs.snapshot.candidate.ordinal)
            return lhs.snapshot.candidate.ordinal < rhs.snapshot.candidate.ordinal;
        return lhs.snapshot.candidate.stats_key < rhs.snapshot.candidate.stats_key;
    });

    std::string endpoint_label = format_host_for_log(host) + ':' + std::to_string(port);
    int         last_rc        = -1;

    for (size_t i = 0; i < plans.size(); ++i) {
        auto& plan = plans[i];
        if (i > 0) {
            socket.close();
            socket = make_upstream_socket(fdwq);
        }

        auto start = std::chrono::steady_clock::now();
        {
            std::ostringstream oss;
            oss << endpoint_label << " candidate#" << (i + 1) << '/' << plans.size() << " "
                << plan.snapshot.candidate.label << " attempt";
            if (!plan.priority.available) {
                auto remain = plan.priority.ready_after <= start
                    ? std::chrono::milliseconds { 0 }
                    : std::chrono::duration_cast<std::chrono::milliseconds>(plan.priority.ready_after - start);
                oss << " cooldown=" << remain.count() << "ms";
            }
            oss << " est_rtt=" << plan.priority.rtt.count() << "ms failures=" << plan.priority.failure_penalty;
            debug_log(oss.str());
        }

        last_rc = co_await socket.connect(reinterpret_cast<const sockaddr*>(&plan.snapshot.candidate.addr),
                                          static_cast<socklen_t>(plan.snapshot.candidate.len));

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        auto stats   = record_upstream_outcome(endpoint_key, plan.snapshot.candidate.stats_key, last_rc == 0, elapsed);

        {
            std::ostringstream oss;
            oss << endpoint_label << " candidate#" << (i + 1) << '/' << plans.size() << " "
                << plan.snapshot.candidate.label;
            if (last_rc == 0) {
                oss << " success rtt=" << elapsed.count() << "ms smoothed=" << stats.smoothed_rtt.count() << "ms";
                debug_log(oss.str());
                co_return 0;
            }
            oss << " failed rc=" << last_rc << " streak=" << stats.consecutive_failures;
            if (stats.retry_after.time_since_epoch().count() != 0) {
                auto now_after = std::chrono::steady_clock::now();
                auto remain    = stats.retry_after <= now_after
                       ? std::chrono::milliseconds { 0 }
                       : std::chrono::duration_cast<std::chrono::milliseconds>(stats.retry_after - now_after);
                oss << " cooldown=" << remain.count() << "ms";
            }
            debug_log(oss.str());
        }
    }

    co_return last_rc;
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

    auto append_host_literal = [](std::string& dest, const std::string& host) {
        if (host.find(':') != std::string::npos) {
            dest.push_back('[');
            dest.append(host);
            dest.push_back(']');
        } else {
            dest.append(host);
        }
    };

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
        append_host_literal(request, parts.host);
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
Task<void, Work_Promise<SpinLock, void>> pipe_data(SrcSocket&         src,
                                                   DstSocket&         dst,
                                                   std::string        label,
                                                   const std::string& peer_id,
                                                   RelayMetrics&      metrics,
                                                   bool               propagate_shutdown)
{
    std::array<char, 8192> buffer {};
    debug_log(peer_id + " " + label + " pipe started");
    bool                  request_shutdown_tx = false;
    bool                  shutdown_applied    = false;
    TlsClientHelloSniffer tls_sniffer;
    if (TlsKeyLogRegistry::instance().is_enabled() && label == "client->upstream") {
        if constexpr (std::is_same_v<std::decay_t<SrcSocket>, net::tcp_socket<SpinLock>>) {
            uint16_t remote_port = get_remote_port(src);
            tls_sniffer.activate(peer_id, remote_port);
        }
    }
    while (true) {
        ssize_t n            = co_await src.recv(buffer.data(), buffer.size());
        metrics.last_recv_rc = n;
        if (n == 0) {
            metrics.recv_failed        = false;
            metrics.termination_reason = "recv_eof";
            request_shutdown_tx        = true;
            break;
        }
        if (n < 0) {
            auto err_info = classify_io_error(n, true);
            if (err_info.cancelled) {
                std::ostringstream oss;
                oss << peer_id << " " << label << " recv transient "
                    << (err_info.reason.empty() ? std::to_string(static_cast<long long>(-n)) : err_info.reason)
                    << ", retrying";
                debug_log(oss.str());
                continue;
            }
            metrics.recv_failed        = !err_info.peer_reset;
            metrics.termination_reason = err_info.reason.empty()
                ? ("recv_error(" + std::to_string(static_cast<long long>(-n)) + ")")
                : err_info.reason;
            if (err_info.peer_reset)
                request_shutdown_tx = true;
            break;
        }
        metrics.recv_ops++;
        metrics.total_recv_bytes += static_cast<std::uint64_t>(n);
        {
            std::ostringstream oss;
            oss << peer_id << " " << label << " recv chunk=" << n << " bytes total=" << metrics.total_recv_bytes;
            debug_log(oss.str());
        }
        if constexpr (std::is_same_v<std::decay_t<SrcSocket>, net::tcp_socket<SpinLock>>) {
            tls_sniffer.feed(buffer.data(), static_cast<std::size_t>(n));
        }
        size_t offset = 0;
        while (offset < static_cast<size_t>(n)) {
            ssize_t sent         = co_await dst.send(buffer.data() + offset, static_cast<size_t>(n) - offset);
            metrics.last_send_rc = sent;
            if (sent <= 0) {
                auto err_info = classify_io_error(sent, false);
                if (err_info.cancelled) {
                    std::ostringstream oss;
                    oss << peer_id << " " << label << " send transient "
                        << (err_info.reason.empty() ? std::to_string(static_cast<long long>(-sent)) : err_info.reason)
                        << ", retrying";
                    debug_log(oss.str());
                    continue;
                }
                metrics.send_failed        = !err_info.peer_reset;
                metrics.termination_reason = err_info.reason.empty()
                    ? (sent == 0 ? "send_error(0)"
                                 : ("send_error(" + std::to_string(static_cast<long long>(-sent)) + ")"))
                    : err_info.reason;
                debug_log(peer_id + " " + label + " pipe stopping (" + metrics.termination_reason + ")");
                if (err_info.peer_reset) {
                    request_shutdown_tx = true;
                }
                if constexpr (requires(SrcSocket& s) { s.close(); }) {
                    src.close();
                }
                goto PIPE_DONE;
            }
            metrics.send_ops++;
            size_t remain    = static_cast<size_t>(n) - offset;
            size_t sent_size = static_cast<size_t>(sent);
            if (sent_size < remain) {
                metrics.partial_send_ops++;
                std::ostringstream oss;
                oss << peer_id << " " << label << " partial send=" << sent
                    << " remaining=" << (static_cast<size_t>(n) - offset - static_cast<size_t>(sent));
                debug_log(oss.str());
            } else if (sent_size > remain) {
                metrics.send_overrun_ops++;
                metrics.send_overrun_bytes += static_cast<std::uint64_t>(sent_size - remain);
                std::ostringstream oss;
                oss << peer_id << " " << label << " send overrun returned=" << sent_size << " remain=" << remain
                    << " extra=" << (sent_size - remain);
                debug_log(oss.str());
                CO_WQ_LOG_ERROR("%s %s send overrun: returned=%zu remain=%zu extra=%zu",
                                peer_id.c_str(),
                                label.c_str(),
                                static_cast<size_t>(sent_size),
                                remain,
                                static_cast<size_t>(sent_size - remain));
            }
            size_t accounted = std::min(remain, sent_size);
            metrics.total_sent_bytes += static_cast<std::uint64_t>(accounted);
            offset += accounted;
        }
    }
    if (metrics.termination_reason.empty()) {
        metrics.termination_reason = "recv_loop_break";
    }

PIPE_DONE:
    metrics.finished            = true;
    metrics.request_shutdown_tx = metrics.request_shutdown_tx || request_shutdown_tx;
    if (request_shutdown_tx && propagate_shutdown) {
        if constexpr (requires(DstSocket& s) {
                          s.tx_shutdown();
                          s.shutdown_tx();
                      }) {
            if (!dst.tx_shutdown()) {
                debug_log(peer_id + " " + label + " shutting down TX after clean EOF");
                dst.shutdown_tx();
                shutdown_applied = true;
            } else {
                shutdown_applied = true;
            }
        } else {
            debug_log(peer_id + " " + label + " destination lacks shutdown_tx(); skipping half-close");
        }
    } else if (!metrics.termination_reason.empty()) {
        debug_log(peer_id + " " + label
                  + " leaving destination TX open due to termination=" + metrics.termination_reason);
    }
    metrics.shutdown_tx_performed = metrics.shutdown_tx_performed || shutdown_applied;
    {
        std::ostringstream oss;
        oss << peer_id << " " << label
            << " pipe stopping reason=" << (metrics.termination_reason.empty() ? "?" : metrics.termination_reason)
            << " recv=" << metrics.total_recv_bytes << "B send=" << metrics.total_sent_bytes << "B";
        debug_log(oss.str());
    }
    co_return;
}

// 建立 CONNECT 隧道，将客户端/上游 sockets 互相转发，直到任一方关闭。
Task<void, Work_Promise<SpinLock, void>> handle_connect(net::tcp_socket<SpinLock>& client,
                                                        NetFdWorkqueue&            fdwq,
                                                        const ConnectTarget&       target,
                                                        const std::string&         peer_id)
{
    auto upstream = make_upstream_socket(fdwq);
    {
        std::ostringstream oss;
        oss << peer_id << " CONNECT dialing " << format_host_for_log(target.host) << ':' << target.port;
        auto msg = oss.str();
        debug_log(msg);
    }
    int rc = co_await connect_upstream(upstream, fdwq, target.host, target.port);
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
    debug_log(peer_id + " " + describe_upstream_peer(upstream));

    std::string established = "HTTP/1.1 200 Connection Established\r\nProxy-Agent: co_wq-proxy\r\n\r\n";
    if (co_await client.send_all(established.data(), established.size()) <= 0) {
        debug_log(peer_id + " CONNECT response send failed");
        client.close();
        upstream.close();
        co_return;
    }

    debug_log(peer_id + " CONNECT tunnel established, starting bidirectional piping");

    configure_graceful_close(client, peer_id, "client");
    configure_graceful_close(upstream, peer_id, "upstream");
    configure_tcp_keepalive(client, peer_id, "client");
    configure_tcp_keepalive(upstream, peer_id, "upstream");

    RelayMetrics client_to_upstream_metrics;
    RelayMetrics upstream_to_client_metrics;
    auto         c_to_u = pipe_data(client, upstream, "client->upstream", peer_id, client_to_upstream_metrics, false);
    auto         u_to_c = pipe_data(upstream, client, "upstream->client", peer_id, upstream_to_client_metrics, true);
    co_await when_all(c_to_u, u_to_c);

    log_relay_summary(client_to_upstream_metrics, "CONNECT client->upstream", peer_id);
    log_relay_summary(upstream_to_client_metrics, "CONNECT upstream->client", peer_id);

    debug_log(peer_id + " CONNECT tunnel closed");

    client.close();
    upstream.close();
    co_return;
}

// 处理常规 HTTP 请求：重新构造请求行/头部并回源，然后将响应回写给客户端。
Task<void, Work_Promise<SpinLock, void>> handle_http_request(HttpProxyContext&          ctx,
                                                             net::tcp_socket<SpinLock>& client,
                                                             NetFdWorkqueue&            fdwq,
                                                             const UrlParts&            parts,
                                                             const std::string&         peer_id)
{
    auto upstream = make_upstream_socket(fdwq);
    {
        std::ostringstream oss;
        oss << peer_id << " " << ctx.method << " " << format_host_for_log(parts.host) << ':' << parts.port
            << parts.path;
        auto msg = oss.str();
        debug_log(msg);
    }
    int rc = co_await connect_upstream(upstream, fdwq, parts.host, parts.port);
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

    configure_graceful_close(client, peer_id, "client");
    configure_graceful_close(upstream, peer_id, "upstream");
    configure_tcp_keepalive(client, peer_id, "client");
    configure_tcp_keepalive(upstream, peer_id, "upstream");

    RelayMetrics           response_metrics;
    std::array<char, 8192> buffer {};
    while (true) {
        ssize_t n                     = co_await upstream.recv(buffer.data(), buffer.size());
        response_metrics.last_recv_rc = n;
        if (n <= 0) {
            response_metrics.recv_failed        = (n < 0);
            response_metrics.termination_reason = (n == 0) ? "upstream_eof"
                                                           : ("upstream_recv_error(" + std::to_string(n) + ")");
            break;
        }
        response_metrics.recv_ops++;
        response_metrics.total_recv_bytes += static_cast<std::uint64_t>(n);
        {
            std::ostringstream oss;
            oss << peer_id << " upstream->client recv chunk=" << n
                << " bytes total_received=" << response_metrics.total_recv_bytes;
            debug_log(oss.str());
        }
        size_t offset = 0;
        while (offset < static_cast<size_t>(n)) {
            ssize_t sent = co_await client.send(buffer.data() + offset, static_cast<size_t>(n) - offset);
            response_metrics.send_ops++;
            if (sent <= 0) {
                response_metrics.send_failed        = true;
                response_metrics.last_send_rc       = sent;
                response_metrics.termination_reason = "client_send_error(" + std::to_string(sent) + ")";
                debug_log(peer_id + " client send error while relaying response");
                goto RESPONSE_DONE;
            }
            response_metrics.total_sent_bytes += static_cast<std::uint64_t>(sent);
            if (static_cast<size_t>(sent) < static_cast<size_t>(n) - offset) {
                response_metrics.partial_send_ops++;
                std::ostringstream oss;
                oss << peer_id << " upstream->client partial send=" << sent
                    << " remaining=" << (static_cast<size_t>(n) - offset - static_cast<size_t>(sent));
                debug_log(oss.str());
            }
            offset += static_cast<size_t>(sent);
        }
    }
    if (response_metrics.termination_reason.empty()) {
        response_metrics.termination_reason = "upstream_loop_break";
    }

RESPONSE_DONE:
    response_metrics.finished = true;
    log_relay_summary(response_metrics, "HTTP upstream->client", peer_id);

    if (response_metrics.send_failed) {
        client.close();
        upstream.close();
        co_return;
    }

    debug_log(peer_id + " upstream response completed");

    client.close();
    upstream.close();
    co_return;
}

// 单个客户端连接生命周期：解析首个请求并根据方法调度处理逻辑。
Task<void, Work_Promise<SpinLock, void>>
handle_proxy_connection(NetFdWorkqueue& fdwq, net::tcp_socket<SpinLock> client, std::string peer_id)
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
        CO_WQ_LOG_ERROR("[proxy] parse error: %s", error_reason.c_str());
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
Task<void, Work_Promise<SpinLock, void>> proxy_server(NetFdWorkqueue& fdwq, const std::string& host, uint16_t port)
{
    auto analyze_listen_host = [](const std::string& input) {
        struct Config {
            int         family { AF_INET };
            bool        dual_stack { false };
            std::string bind_host;
        } cfg;
        std::string host = input;
        if (host.empty())
            host = "";
        std::string view = host;
        if (!view.empty() && view.front() == '[' && view.back() == ']')
            view = view.substr(1, view.size() - 2);
        bool host_unspecified = view.empty() || view == "0.0.0.0" || view == "*" || view == "::";
        bool host_ipv6        = view.find(':') != std::string::npos;
        if (host_unspecified) {
            cfg.family     = AF_INET6;
            cfg.dual_stack = true;
            cfg.bind_host  = "::";
        } else if (host_ipv6) {
            cfg.family    = AF_INET6;
            cfg.bind_host = host;
        } else {
            cfg.family    = AF_INET;
            cfg.bind_host = host;
        }
        return cfg;
    };

    auto                        listen_cfg = analyze_listen_host(host);
    net::tcp_listener<SpinLock> listener(fdwq.base(), fdwq.reactor(), listen_cfg.family);
    try {
        listener.bind_listen(listen_cfg.bind_host, port, 128, listen_cfg.dual_stack);
    } catch (const std::exception& ex) {
        std::ostringstream oss;
        oss << "[proxy] failed to bind " << (listen_cfg.bind_host.empty() ? host : listen_cfg.bind_host) << ':' << port
            << ": " << ex.what();
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
        CO_WQ_LOG_ERROR("%s", msg.c_str());
        debug_log(msg);
        listener.close();
        g_listener_fd.store(-1, std::memory_order_release);
        co_return;
    }
    g_listener_fd.store(listener.native_handle(), std::memory_order_release);

    std::string log_host = listen_cfg.bind_host.empty() ? host : listen_cfg.bind_host;
    std::string log_suffix;
    if (listen_cfg.dual_stack && listen_cfg.family == AF_INET6)
        log_suffix = " (dual-stack)";
    auto formatted_host = format_host_for_log(log_host);

    CO_WQ_LOG_INFO("[proxy] listening on %s:%u%s",
                   formatted_host.c_str(),
                   static_cast<unsigned>(port),
                   log_suffix.c_str());
    {
        std::ostringstream oss;
        oss << "proxy listening on " << formatted_host << ':' << port << log_suffix;
        auto msg = oss.str();
        debug_log(msg);
    }

    while (!g_stop.load(std::memory_order_acquire)) {
        int fd = co_await listener.accept();
        if (fd == net::k_accept_fatal) {
            CO_WQ_LOG_ERROR("[proxy] accept fatal error, exiting");
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
    std::string               host                = "0.0.0.0";
    uint16_t                  port                = 8081;
    std::string               log_file_path       = "co_http_proxy.log";
    bool                      log_truncate        = false;
    spdlog::level::level_enum requested_log_level = spdlog::level::info;

    std::filesystem::path project_root;
    std::filesystem::path cwd_path;
    {
        std::error_code cwd_ec;
        cwd_path = std::filesystem::current_path(cwd_ec);
        if (cwd_ec)
            cwd_path.clear();
        if (!cwd_path.empty())
            project_root = find_project_root(cwd_path);
        if (project_root.empty())
            project_root = cwd_path;
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--verbose" || arg == "--debug-log") {
            g_debug_logging.store(true, std::memory_order_relaxed);
            requested_log_level = spdlog::level::debug;
        } else if (arg == "--log-file" && i + 1 < argc) {
            log_file_path = argv[++i];
        } else if (arg == "--log-truncate") {
            log_truncate = true;
        }
    }

    bool log_configured = false;
    try {
        std::filesystem::path cli_log_path(log_file_path);

        auto cwd = cwd_path;

        std::filesystem::path resolved = cli_log_path;
        if (!resolved.is_absolute()) {
            if (!project_root.empty()) {
                resolved = project_root / resolved;
            } else if (!cwd.empty()) {
                resolved = cwd / resolved;
            }
        }

        resolved = resolved.lexically_normal();

        if (!resolved.empty()) {
            auto parent = resolved.parent_path();
            if (!parent.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(parent, ec);
                if (ec) {
                    std::fprintf(stderr,
                                 "[proxy] failed to create log directory %s: %s\n",
                                 parent.string().c_str(),
                                 ec.message().c_str());
                }
            }
        }

        co_wq::log::configure_file_logging(resolved.string(), log_truncate, true);
        log_configured = true;
        log_file_path  = resolved.string();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[proxy] failed to initialize log file %s: %s\n", log_file_path.c_str(), ex.what());
    }

    co_wq::log::set_level(requested_log_level);
    if (log_configured) {
        CO_WQ_LOG_INFO("[proxy] logging to %s (truncate=%d)", log_file_path.c_str(), log_truncate ? 1 : 0);
    }

    TlsKeyLogRegistry::instance().configure(project_root);

#if defined(_WIN32)
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    SetUnhandledExceptionFilter(proxy_exception_filter);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    HMODULE self_module = ::GetModuleHandleW(nullptr);
    CO_WQ_LOG_INFO("[proxy] module base=%p", static_cast<void*>(self_module));
#else
    std::signal(SIGINT, sigint_handler);
#endif

    auto&          wq = get_sys_workqueue(0);
    NetFdWorkqueue fdwq(wq);

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
