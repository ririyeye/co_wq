#include "co_syswork.hpp"
#include "co_test_sys_stats_logger.hpp"
#include "fd_base.hpp"
#include "quic_dispatcher.hpp"
#include "quic_stream_session.hpp"
#include "tls.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#if defined(USING_NET) && defined(USING_SSL) && !defined(OPENSSL_NO_QUIC)

using namespace co_wq;
using NetFdWorkqueue    = net::fd_workqueue<SpinLock, net::epoll_reactor>;
using QuicDispatcher    = net::quic_dispatcher<SpinLock, net::epoll_reactor>;
using QuicAcceptAwaiter = net::quic_accept_awaiter<SpinLock, net::epoll_reactor>;
using QuicServerSession = net::quic_stream_session<SpinLock, net::epoll_reactor>;

namespace {

static bool                         g_verbose = false;
static std::atomic_bool             g_shutdown { false };
static std::atomic<void*>           g_stdin_owner { nullptr };
static std::atomic<QuicDispatcher*> g_active_dispatcher { nullptr };

#if !defined(_WIN32)
static void quic_signal_handler(int)
{
    g_shutdown.store(true, std::memory_order_release);
    QuicDispatcher* dispatcher = g_active_dispatcher.load(std::memory_order_acquire);
    if (dispatcher)
        dispatcher->wake();
}
#endif

#define LOG_VERBOSE(...)                                                                                               \
    do {                                                                                                               \
        if (g_verbose)                                                                                                 \
            CO_WQ_LOG_INFO(__VA_ARGS__);                                                                               \
    } while (false)

constexpr uint16_t kDefaultQuicPort = 28545;

struct ProgramOptions {
    bool        listen { false };
    bool        verbose { false };
    bool        tls_trace { false };
    bool        insecure { true };
    bool        auto_test { false };
    std::string host { "127.0.0.1" };
    uint16_t    port { 0 };
    std::string listen_host { "0.0.0.0" };
    std::string cert_path;
    std::string key_path;
};

static std::filesystem::path resolve_log_directory(const std::filesystem::path& exec_dir)
{
    auto canonical_or = [](const std::filesystem::path& p) {
        if (p.empty())
            return p;
        std::error_code ec;
        auto            resolved = std::filesystem::weakly_canonical(p, ec);
        if (!ec)
            return resolved;
        resolved = std::filesystem::absolute(p, ec);
        if (!ec)
            return resolved;
        return p;
    };

    std::vector<std::filesystem::path> bases;
    auto                               add_base = [&](const std::filesystem::path& candidate) {
        if (candidate.empty())
            return;
        auto normalized = canonical_or(candidate);
        if (std::find(bases.begin(), bases.end(), normalized) == bases.end())
            bases.push_back(std::move(normalized));
    };

    add_base(std::filesystem::current_path());
    add_base(exec_dir);
    auto ancestor = exec_dir;
    while (!ancestor.empty()) {
        add_base(ancestor);
        auto parent = ancestor.parent_path();
        if (parent == ancestor)
            break;
        ancestor = parent;
    }

    for (const auto& base : bases) {
        std::error_code ec;
        if (std::filesystem::exists(base / "xmake.lua", ec))
            return canonical_or(base / "logs");
    }

    for (const auto& base : bases) {
        if (base.empty())
            continue;
        return canonical_or(base / "logs");
    }

    return canonical_or(exec_dir / "logs");
}

static void setup_logging(const ProgramOptions& opt, const std::filesystem::path& exec_dir)
{
    std::filesystem::path log_dir = resolve_log_directory(exec_dir);
    std::error_code       dir_ec;
    std::filesystem::create_directories(log_dir, dir_ec);
    if (dir_ec) {
        std::fprintf(stderr,
                     "[quic] failed to create log directory %s: %s\n",
                     log_dir.string().c_str(),
                     dir_ec.message().c_str());
    }

    const char* role     = opt.listen ? "server" : "client";
    auto        log_path = log_dir / (opt.listen ? "quic_server.log" : "quic_client.log");
    auto        cons_lvl = opt.verbose ? spdlog::level::debug : spdlog::level::info;
    auto        logger   = co_wq::log::configure_file_logging(log_path.string(),
                                                     true,
                                                     true,
                                                     spdlog::level::trace,
                                                     cons_lvl,
                                                     spdlog::level::trace);
    if (logger)
        logger->set_level(spdlog::level::trace);

    auto        level_view = spdlog::level::to_string_view(cons_lvl);
    std::string level_name(level_view.data(), level_view.size());

    if (logger) {
        CO_WQ_LOG_INFO("[quic] logging to %s (role=%s console_level=%s)",
                       log_path.string().c_str(),
                       role,
                       level_name.c_str());
        logger->flush_on(spdlog::level::info);
    } else {
        std::fprintf(stderr,
                     "[quic] failed to initialize log file %s, continuing with console logging\n",
                     log_path.string().c_str());
    }
}

static void print_usage(const char* prog)
{
    std::cout << "Usage:\n"
              << "  " << prog << " [-v] host port\n"
              << "  " << prog << " -l [-v] [-p port] [--listen-host addr]\n"
              << "Options:\n"
              << "  -l, --listen             Listen for incoming QUIC connections\n"
              << "  -p, --port PORT          Connect/listen port (default " << kDefaultQuicPort << ")\n"
              << "      --listen-host HOST   Address to bind when listening (default 0.0.0.0)\n"
              << "      --cert FILE          Server certificate (default certs/server.crt)\n"
              << "      --key FILE           Server private key (default certs/server.key)\n"
              << "      --secure             Enable certificate verification on client\n"
              << "      --insecure           Disable certificate verification on client\n"
              << "      --auto               Run automated send/receive flow (two round-trips)\n"
              << "      --tls-trace          Enable TLS message trace logging\n"
              << "  -v                       Enable verbose logging\n";
}

static bool parse_uint16(const char* text, uint16_t& out)
{
    if (!text)
        return false;
    try {
        int value = std::stoi(text);
        if (value < 0 || value > 65535)
            return false;
        out = static_cast<uint16_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

static std::filesystem::path detect_executable_dir(const char* argv0)
{
    std::error_code ec;

#if defined(__linux__)
    auto exe_path = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        auto canonical = std::filesystem::weakly_canonical(exe_path, ec);
        if (!ec)
            exe_path = canonical;
        if (exe_path.has_parent_path())
            return exe_path.parent_path();
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    if (_NSGetExecutablePath(nullptr, &size) >= 0 && size > 0) {
        std::vector<char> buffer(size);
        if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
            std::filesystem::path exe_path(buffer.data());
            auto                  canonical = std::filesystem::weakly_canonical(exe_path, ec);
            if (!ec)
                exe_path = canonical;
            if (exe_path.has_parent_path())
                return exe_path.parent_path();
        }
    }
#elif defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD        copied = 0;
    while ((copied = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()))) >= buffer.size())
        buffer.resize(buffer.size() * 2, L'\0');
    if (copied > 0) {
        buffer.resize(copied);
        std::filesystem::path exe_path(buffer);
        auto                  canonical = std::filesystem::weakly_canonical(exe_path, ec);
        if (!ec)
            exe_path = canonical;
        if (exe_path.has_parent_path())
            return exe_path.parent_path();
    }
#endif

    std::filesystem::path fallback = argv0 ? std::filesystem::path(argv0) : std::filesystem::path();
    if (!fallback.empty() && fallback.is_relative())
        fallback = std::filesystem::current_path() / fallback;
    auto canonical = std::filesystem::weakly_canonical(fallback, ec);
    if (!ec)
        fallback = canonical;
    if (fallback.has_parent_path())
        return fallback.parent_path();
    return std::filesystem::current_path();
}

static std::optional<std::filesystem::path> find_default_credential_file(const std::filesystem::path& exec_dir,
                                                                         const std::string&           filename)
{
    auto add_unique_dir = [](std::vector<std::filesystem::path>& dirs, const std::filesystem::path& dir) {
        if (dir.empty())
            return;
        if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end())
            dirs.push_back(dir);
    };

    std::vector<std::filesystem::path> search_dirs;
    add_unique_dir(search_dirs, std::filesystem::current_path());

    std::filesystem::path ancestor = exec_dir;
    while (!ancestor.empty()) {
        add_unique_dir(search_dirs, ancestor);
        auto parent = ancestor.parent_path();
        if (parent == ancestor)
            break;
        ancestor = parent;
    }

    auto try_path = [](const std::filesystem::path& path) -> std::optional<std::filesystem::path> {
        if (path.empty())
            return std::nullopt;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            return std::nullopt;
        auto resolved = std::filesystem::weakly_canonical(path, ec);
        if (!ec)
            return resolved;
        resolved = std::filesystem::absolute(path, ec);
        if (!ec)
            return resolved;
        return path;
    };

    for (const auto& base : search_dirs) {
        if (auto direct = try_path(base / filename); direct)
            return direct;
        if (auto in_certs = try_path(base / "certs" / filename); in_certs)
            return in_certs;
    }

    return std::nullopt;
}

static std::optional<ProgramOptions>
parse_args(int argc, char* argv[], bool& show_usage, const std::filesystem::path& exec_dir)
{
    ProgramOptions           opt;
    std::vector<std::string> positional;
    show_usage = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-l" || arg == "--listen") {
            opt.listen = true;
        } else if (arg == "-v") {
            opt.verbose = true;
        } else if (arg == "-p" || arg == "--port") {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << arg << "\n";
                return std::nullopt;
            }
            if (!parse_uint16(argv[++i], opt.port)) {
                std::cerr << "invalid port: " << argv[i] << "\n";
                return std::nullopt;
            }
        } else if (arg == "--listen-host") {
            if (i + 1 >= argc) {
                std::cerr << "missing value for --listen-host\n";
                return std::nullopt;
            }
            opt.listen_host = argv[++i];
        } else if (arg == "--cert") {
            if (i + 1 >= argc) {
                std::cerr << "missing value for --cert\n";
                return std::nullopt;
            }
            opt.cert_path = argv[++i];
        } else if (arg == "--key") {
            if (i + 1 >= argc) {
                std::cerr << "missing value for --key\n";
                return std::nullopt;
            }
            opt.key_path = argv[++i];
        } else if (arg == "--secure") {
            opt.insecure = false;
        } else if (arg == "--insecure") {
            opt.insecure = true;
        } else if (arg == "--auto") {
            opt.auto_test = true;
        } else if (arg == "--tls-trace") {
            opt.tls_trace = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            show_usage = true;
            return std::nullopt;
        } else if (!arg.empty() && arg.front() == '-') {
            std::cerr << "unknown option: " << arg << "\n";
            return std::nullopt;
        } else {
            positional.emplace_back(std::move(arg));
        }
    }

    if (opt.listen) {
        if (!positional.empty()) {
            if (opt.port == 0) {
                if (!parse_uint16(positional.back().c_str(), opt.port)) {
                    std::cerr << "invalid port: " << positional.back() << "\n";
                    return std::nullopt;
                }
                positional.pop_back();
            }
            if (!positional.empty())
                opt.listen_host = positional.back();
        }
        if (opt.port == 0)
            opt.port = kDefaultQuicPort;

        if (opt.cert_path.empty()) {
            if (auto located = find_default_credential_file(exec_dir, "server.crt"))
                opt.cert_path = located->string();
            LOG_VERBOSE("[quic] server certificate candidate: %s",
                        opt.cert_path.empty() ? "<not found>" : opt.cert_path.c_str());
        } else
            LOG_VERBOSE("[quic] server certificate specified: %s", opt.cert_path.c_str());

        if (opt.key_path.empty()) {
            if (auto located = find_default_credential_file(exec_dir, "server.key"))
                opt.key_path = located->string();
            LOG_VERBOSE("[quic] server key candidate: %s", opt.key_path.empty() ? "<not found>" : opt.key_path.c_str());
        } else
            LOG_VERBOSE("[quic] server key specified: %s", opt.key_path.c_str());
        if (opt.cert_path.empty() || opt.key_path.empty()) {
            std::cerr << "server mode requires certificate and key files\n";
            return std::nullopt;
        }
    } else {
        if (!positional.empty()) {
            opt.host = positional.front();
            if (positional.size() > 1 && opt.port == 0) {
                if (!parse_uint16(positional.back().c_str(), opt.port)) {
                    std::cerr << "invalid port: " << positional.back() << "\n";
                    return std::nullopt;
                }
            }
        }

        if (opt.port == 0 && positional.size() >= 2) {
            if (!parse_uint16(positional.back().c_str(), opt.port)) {
                std::cerr << "invalid port: " << positional.back() << "\n";
                return std::nullopt;
            }
            opt.host = positional[positional.size() - 2];
        }

        if (opt.port == 0 && !positional.empty())
            opt.port = kDefaultQuicPort;

        if (opt.port == 0) {
            std::cerr << "client mode requires host and port\n";
            return std::nullopt;
        }
    }

    return opt;
}

static bool fill_ipv4(const std::string& host, uint16_t port, sockaddr_in& out)
{
    std::memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port   = htons(port);
    if (host.empty() || host == "*" || host == "0.0.0.0") {
        out.sin_addr.s_addr = htonl(INADDR_ANY);
        return true;
    }
#if defined(_WIN32)
    return InetPtonA(AF_INET, host.c_str(), &out.sin_addr) == 1;
#else
    return ::inet_pton(AF_INET, host.c_str(), &out.sin_addr) == 1;
#endif
}

template <class Session>
static Task<void, Work_Promise<SpinLock, void>>
send_chunk_task(Session& sock, std::shared_ptr<std::string> data, std::shared_ptr<std::promise<ssize_t>> result)
{
    ssize_t sent = co_await sock.send_all(data->data(), data->size());
    result->set_value(sent);
    co_return;
}

template <class Session>
static Task<void, Work_Promise<SpinLock, void>> shutdown_tx_task(Session&                            sock,
                                                                 std::shared_ptr<std::promise<void>> done)
{
    sock.shutdown_tx();
    done->set_value();
    co_return;
}

template <class Session> static ssize_t sync_send_via_queue(Session& sock, const char* data, size_t len)
{
    auto                 payload = std::make_shared<std::string>(data, len);
    auto                 promise = std::make_shared<std::promise<ssize_t>>();
    std::future<ssize_t> future  = promise->get_future();
    auto                 task    = send_chunk_task(sock, payload, promise);
    post_to(task, sock.exec());
    try {
        return future.get();
    } catch (...) {
        return -1;
    }
}

static Task<void, Work_Promise<SpinLock, void>> wait_for_shutdown_signal()
{
    auto& timer = get_sys_timer();
    while (!g_shutdown.load(std::memory_order_acquire)) {
        co_await co_wq::delay_ms(timer, 500);
    }
    co_return;
}

template <class Session>
static Task<void, Work_Promise<SpinLock, void>> run_auto_client_session(Session& sock, bool hold_after)
{
    static constexpr std::array<std::string_view, 2> messages { "auto-client-1\n", "auto-client-2\n" };
    static constexpr std::array<std::string_view, 2> expect_response { "auto-server-1\n", "auto-server-2\n" };
    std::array<char, 512>                            buffer {};
    for (size_t idx = 0; idx < messages.size(); ++idx) {
        std::string_view request = messages[idx];
        ssize_t          sent    = co_await sock.send_all(request.data(), request.size());
        if (sent <= 0) {
            CO_WQ_LOG_ERROR("[quic client] auto send round %zu failed rc=%zd", idx, sent);
            co_return;
        }
        ssize_t received = co_await sock.recv(buffer.data(), buffer.size());
        if (received <= 0) {
            CO_WQ_LOG_ERROR("[quic client] auto recv round %zu failed rc=%zd", idx, received);
            co_return;
        }
        std::string response(buffer.data(), static_cast<size_t>(received));
        CO_WQ_LOG_INFO("[quic client] auto round %zu recv=%s", idx, response.c_str());
        if (idx < expect_response.size() && response != expect_response[idx]) {
            CO_WQ_LOG_WARN("[quic client] unexpected response[%zu]: %s", idx, response.c_str());
        }
    }
    CO_WQ_LOG_INFO("[quic client] automated exchange complete");
    if (hold_after) {
        CO_WQ_LOG_INFO("[quic client] waiting for shutdown signal");
        co_await wait_for_shutdown_signal();
    }
    co_return;
}

template <class Session>
static Task<void, Work_Promise<SpinLock, void>> run_auto_server_session(Session& session, bool hold_after)
{
    static constexpr std::array<std::string_view, 2> responses { "auto-server-1\n", "auto-server-2\n" };
    std::array<char, 512>                            buffer {};
    for (size_t idx = 0; idx < responses.size(); ++idx) {
        ssize_t received = co_await session.recv(buffer.data(), buffer.size());
        if (received <= 0) {
            CO_WQ_LOG_WARN("[quic server] auto recv round %zu failed rc=%zd", idx, received);
            co_return;
        }
        std::string request(buffer.data(), static_cast<size_t>(received));
        CO_WQ_LOG_INFO("[quic server] auto round %zu recv=%s", idx, request.c_str());
        std::string_view response = responses[idx];
        ssize_t          sent     = co_await session.send_all(response.data(), response.size());
        if (sent <= 0) {
            CO_WQ_LOG_WARN("[quic server] auto send round %zu failed rc=%zd", idx, sent);
            co_return;
        }
    }
    CO_WQ_LOG_INFO("[quic server] automated exchange complete");
    if (hold_after) {
        CO_WQ_LOG_INFO("[quic server] waiting for shutdown signal");
        co_await wait_for_shutdown_signal();
    }
    co_return;
}

struct SessionState {
    std::atomic_bool stop { false };
};

template <class Session>
static Task<void, Work_Promise<SpinLock, void>> socket_to_stdout_task(Session& sock, SessionState& state)
{
    std::array<char, 4096> buffer {};
    while (!state.stop.load(std::memory_order_acquire)) {
        ssize_t n = co_await sock.recv(buffer.data(), buffer.size());
        if (n <= 0) {
            int shut_flags = sock.ssl_handle() ? SSL_get_shutdown(sock.ssl_handle()) : 0;
            LOG_VERBOSE("[quic] recv return=%zd shutdown_flags=0x%x rx_eof=%d", n, shut_flags, sock.rx_eof() ? 1 : 0);
            if (n < 0) {
                int         err_code = static_cast<int>(-n);
                const char* err_text = (err_code > 0) ? std::strerror(err_code) : "unknown";
                CO_WQ_LOG_ERROR("[quic] recv failed: %s (%zd) stack=%s",
                                err_text,
                                n,
                                net::tls_utils::collect_error_stack().c_str());
            } else
                LOG_VERBOSE("[quic] peer closed receive stream");
            break;
        }
        std::cout.write(buffer.data(), n);
        std::cout.flush();
        LOG_VERBOSE("[quic] received %zd bytes", static_cast<ssize_t>(n));
    }
    state.stop.store(true, std::memory_order_release);
    co_return;
}

template <class Session>
static Task<void, Work_Promise<SpinLock, void>> run_interactive_session(Session& sock, bool enable_stdin)
{
    SessionState state;

    std::thread input_thread;
    if (enable_stdin) {
        input_thread = std::thread([&sock, &state]() {
#if !defined(_WIN32)
            const int fd             = ::fileno(stdin);
            int       original_flags = -1;
            bool      changed_flags  = false;
            if (fd >= 0) {
                original_flags = ::fcntl(fd, F_GETFL, 0);
                if (original_flags != -1 && (original_flags & O_NONBLOCK) == 0) {
                    if (::fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) == 0)
                        changed_flags = true;
                }
            }
            struct FlagGuard {
                int  fd;
                int  flags;
                bool active;
                ~FlagGuard()
                {
                    if (active && fd >= 0 && flags != -1)
                        ::fcntl(fd, F_SETFL, flags);
                }
            } guard { fd, original_flags, changed_flags };

            std::vector<char> buffer(4096);
            while (!state.stop.load(std::memory_order_acquire)) {
                ssize_t got = -1;
                if (fd >= 0)
                    got = ::read(fd, buffer.data(), buffer.size());
                else {
                    std::cin.read(buffer.data(), buffer.size());
                    got = std::cin.gcount();
                    if (got == 0 && std::cin.eof())
                        break;
                }

                if (got > 0) {
                    LOG_VERBOSE("[quic] sending %zd bytes", static_cast<ssize_t>(got));
                    if (sync_send_via_queue(sock, buffer.data(), static_cast<size_t>(got)) <= 0) {
                        state.stop.store(true, std::memory_order_release);
                        break;
                    }
                    continue;
                }

                if (got == 0) {
                    LOG_VERBOSE("[quic] stdin EOF");
                    break;
                }

                int err = errno;
                if (err == EAGAIN || err == EWOULDBLOCK) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                if (err == EINTR)
                    continue;

                CO_WQ_LOG_ERROR("[quic] stdin read failed: %s", std::strerror(err));
                state.stop.store(true, std::memory_order_release);
                break;
            }
#else
            std::string line;
            while (!state.stop.load(std::memory_order_acquire) && std::getline(std::cin, line)) {
                line.push_back('\n');
                LOG_VERBOSE("[quic] sending %zu bytes", line.size());
                if (sync_send_via_queue(sock, line.data(), line.size()) <= 0) {
                    state.stop.store(true, std::memory_order_release);
                    break;
                }
            }
#endif

            auto              done    = std::make_shared<std::promise<void>>();
            std::future<void> future  = done->get_future();
            auto              shut_tk = shutdown_tx_task(sock, done);
            post_to(shut_tk, sock.exec());
            future.get();
            LOG_VERBOSE("[quic] input thread exit");
        });
    }

    co_await socket_to_stdout_task(sock, state);
    state.stop.store(true, std::memory_order_release);
    if (input_thread.joinable())
        input_thread.join();
    sock.close();
    co_return;
}

static Task<void, Work_Promise<SpinLock, void>> run_quic_client(NetFdWorkqueue& fdwq, const ProgramOptions& opt)
{
    LOG_VERBOSE("[quic client] connecting to %s:%u", opt.host.c_str(), opt.port);
    sockaddr_in remote {};
    if (!fill_ipv4(opt.host, opt.port, remote)) {
        CO_WQ_LOG_ERROR("[quic client] invalid server address %s:%u", opt.host.c_str(), opt.port);
        co_return;
    }

    auto udp = fdwq.make_udp_socket();
    if (co_await udp.connect(opt.host, opt.port) != 0) {
        CO_WQ_LOG_ERROR("[quic client] connect %s:%u failed: %s", opt.host.c_str(), opt.port, std::strerror(errno));
        co_return;
    }

    auto                       ctx = net::quic_context::make(net::quic_mode::Client, false, opt.tls_trace);
    net::quic_socket<SpinLock> sock(fdwq.base(),
                                    fdwq.reactor(),
                                    std::move(udp),
                                    std::move(ctx),
                                    net::quic_mode::Client);
    if (!sock.set_initial_peer(reinterpret_cast<const sockaddr*>(&remote), sizeof(remote)))
        LOG_VERBOSE("[quic client] failed to set initial peer address");
    if (opt.insecure)
        SSL_set_verify(sock.ssl_handle(), SSL_VERIFY_NONE, nullptr);

    // quic_socket now routes I/O via its embedded dispatcher.
    int rc = co_await sock.handshake();
    if (rc != 0) {
        CO_WQ_LOG_ERROR("[quic client] handshake failed: %d (%s)", rc, net::tls_utils::collect_error_stack().c_str());
        co_return;
    }
    LOG_VERBOSE("[quic client] handshake complete");

    if (opt.auto_test) {
        co_await run_auto_client_session(sock, false);
        sock.shutdown_tx();
        sock.close();
        co_return;
    }

    co_await run_interactive_session(sock, true);
    co_return;
}

static Task<void, Work_Promise<SpinLock, void>> handle_quic_server_session(QuicDispatcher&   dispatcher,
                                                                           net::quic_context ctx,
                                                                           SSL*              accepted_ssl,
                                                                           bool              insecure,
                                                                           bool              auto_mode)
{
    QuicServerSession session(dispatcher, std::move(ctx), accepted_ssl, net::quic_mode::Server, insecure);

    void* token = static_cast<void*>(&session);
    void* expected { nullptr };
    bool  owns_stdin = g_stdin_owner.compare_exchange_strong(expected,
                                                            token,
                                                            std::memory_order_acq_rel,
                                                            std::memory_order_acquire);
    struct InputOwnerGuard {
        bool owns;
        ~InputOwnerGuard()
        {
            if (owns)
                g_stdin_owner.store(nullptr, std::memory_order_release);
        }
    } input_guard { owns_stdin };

    int rc = co_await session.handshake();
    if (rc != 0) {
        CO_WQ_LOG_ERROR("[quic server] handshake failed: %d (%s)", rc, net::tls_utils::collect_error_stack().c_str());
        co_return;
    }
    LOG_VERBOSE("[quic server] handshake complete session=%p", static_cast<void*>(accepted_ssl));

    if (auto_mode)
        co_await run_auto_server_session(session, true);
    else
        co_await run_interactive_session(session, owns_stdin);
    co_return;
}

static Task<void, Work_Promise<SpinLock, void>> run_quic_server(NetFdWorkqueue& fdwq, const ProgramOptions& opt)
{
    LOG_VERBOSE("[quic server] listening on %s:%u", opt.listen_host.c_str(), opt.port);
    sockaddr_in local {};
    if (!fill_ipv4(opt.listen_host, opt.port, local)) {
        CO_WQ_LOG_ERROR("[quic server] invalid listen address %s:%u", opt.listen_host.c_str(), opt.port);
        co_return;
    }
    auto udp = fdwq.make_udp_socket();
    if (::bind(udp.native_handle(), reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        CO_WQ_LOG_ERROR("[quic server] bind failed: %s", std::strerror(errno));
        co_return;
    }

    net::quic_context ctx;
    try {
        ctx = net::quic_context::make_server_with_pem(opt.cert_path, opt.key_path, false, opt.tls_trace);
    } catch (const std::exception& ex) {
        CO_WQ_LOG_ERROR("[quic server] failed to create QUIC context: %s", ex.what());
        co_return;
    }

    auto listener = std::unique_ptr<SSL, decltype(&SSL_free)>(SSL_new_listener(ctx.native_handle(), 0), &SSL_free);
    if (!listener) {
        CO_WQ_LOG_ERROR("[quic server] SSL_new_listener failed (%s)", net::tls_utils::collect_error_stack().c_str());
        co_return;
    }
    SSL_set_blocking_mode(listener.get(), 0);
    SSL_set_default_stream_mode(listener.get(), SSL_DEFAULT_STREAM_MODE_AUTO_BIDI);
    QuicDispatcher dispatcher(fdwq.base(), fdwq.reactor(), udp, listener.get());
    if (!dispatcher.attach_listener_bio(listener.get())) {
        CO_WQ_LOG_ERROR("[quic server] attach_listener_bio failed: %s", net::tls_utils::collect_error_stack().c_str());
        co_return;
    }

    int listen_rc = SSL_listen(listener.get());
    if (listen_rc != 1) {
        int ssl_err = SSL_get_error(listener.get(), listen_rc);
        CO_WQ_LOG_ERROR("[quic server] SSL_listen failed rc=%d err=%d stack=%s",
                        listen_rc,
                        ssl_err,
                        net::tls_utils::collect_error_stack().c_str());
        co_return;
    }
    LOG_VERBOSE("[quic server] SSL_listen ready");

    g_active_dispatcher.store(&dispatcher, std::memory_order_release);
    struct DispatcherGuard {
        ~DispatcherGuard() { g_active_dispatcher.store(nullptr, std::memory_order_release); }
    } dispatcher_guard;

    bool waiting_logged = false;
    while (!g_shutdown.load(std::memory_order_acquire)) {
        if (!waiting_logged) {
            LOG_VERBOSE("[quic server] waiting for connection");
            waiting_logged = true;
        }
        auto accept_res = co_await QuicAcceptAwaiter(dispatcher, listener.get(), &g_shutdown);
        if (accept_res.ssl || accept_res.rc != 0) {
            LOG_VERBOSE("[quic server] accept result rc=%d ssl=%p", accept_res.rc, static_cast<void*>(accept_res.ssl));
            waiting_logged = false;
        }
        if (g_shutdown.load(std::memory_order_acquire))
            break;
        if (accept_res.rc != 0 || !accept_res.ssl) {
            if (accept_res.rc == -ECANCELED && g_shutdown.load(std::memory_order_acquire))
                break;
            if (!accept_res.ssl && accept_res.rc == 0) {
                ERR_clear_error();
                continue;
            }
            CO_WQ_LOG_ERROR("[quic server] accept failed: %d (%s)",
                            accept_res.rc,
                            net::tls_utils::collect_error_stack().c_str());
            waiting_logged = false;
            continue;
        }

        try {
            auto session = handle_quic_server_session(dispatcher, ctx, accept_res.ssl, opt.insecure, opt.auto_test);
            if (!session.get()) {
                CO_WQ_LOG_ERROR("[quic server] failed to create session task");
                SSL_free(accept_res.ssl);
                continue;
            }
            post_to(session, fdwq.base());
        } catch (const std::exception& ex) {
            CO_WQ_LOG_ERROR("[quic server] failed to spawn session: %s", ex.what());
            SSL_free(accept_res.ssl);
        }
    }

    listener.reset();
    udp.close();
    co_return;
}

} // namespace

int main(int argc, char* argv[])
{
    bool show_usage = false;
    auto exec_dir   = detect_executable_dir(argc > 0 ? argv[0] : nullptr);
    auto parsed     = parse_args(argc, argv, show_usage, exec_dir);
    if (!parsed.has_value())
        return show_usage ? 0 : 1;

    g_verbose = parsed->verbose;

    setup_logging(*parsed, exec_dir);

    if (parsed->listen && g_verbose) {
        CO_WQ_LOG_INFO("[quic] server certificate path: %s", parsed->cert_path.c_str());
        CO_WQ_LOG_INFO("[quic] server key path: %s", parsed->key_path.c_str());
    }

    co_wq::test::SysStatsLogger stats_logger("quic");
    auto&                       wq = get_sys_workqueue(0);
    NetFdWorkqueue              fdwq(wq);

    Task<void, Work_Promise<SpinLock, void>> task { nullptr };
    if (parsed->listen) {
        g_shutdown.store(false, std::memory_order_release);
        g_stdin_owner.store(nullptr, std::memory_order_release);
#if !defined(_WIN32)
        std::signal(SIGINT, quic_signal_handler);
        std::signal(SIGTERM, quic_signal_handler);
#endif
        task = run_quic_server(fdwq, *parsed);
    } else
        task = run_quic_client(fdwq, *parsed);

    if (!task.get())
        return 1;

    std::atomic_bool done { false };
    auto&            promise = task.get().promise();
    promise.mUserData        = &done;
    promise.mOnCompleted     = [](Promise_base& pb) {
        auto* flag = static_cast<std::atomic_bool*>(pb.mUserData);
        if (flag)
            flag->store(true, std::memory_order_release);
    };

    done.store(false, std::memory_order_release);
    post_to(task, wq);
    sys_wait_until(done);

    return 0;
}

#else
int main()
{
    CO_WQ_LOG_WARN("co_quic disabled (requires USING_NET, USING_SSL and OpenSSL QUIC support)");
    return 0;
}
#endif
