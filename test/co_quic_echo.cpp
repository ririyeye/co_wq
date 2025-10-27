#include "co_syswork.hpp"
#include "msquic_loader.hpp"
#include "quic.hpp"
#include "worker.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

using namespace co_wq;

namespace {

constexpr const char*   kDefaultAlpnList = "http/1.0,co-wq/echo";
constexpr std::uint16_t kDefaultPort     = 6121;

std::atomic_bool     g_stop { false };
std::atomic_uint64_t g_next_session_id { 1 };

std::mutex            g_log_mutex;
std::filesystem::path g_session_log_path;

void append_log(uint64_t session_id, const std::string& message)
{
    if (g_session_log_path.empty())
        return;
    std::filesystem::path       target = g_session_log_path;
    std::lock_guard<std::mutex> guard(g_log_mutex);
    std::ofstream               log(target, std::ios::app);
    if (!log.is_open())
        return;
    log << "[session " << session_id << "] " << message << '\n';
}

std::filesystem::path find_project_root(std::filesystem::path current)
{
    if (current.empty())
        return {};
    current = current.lexically_normal();
    std::error_code ec;
    while (!current.empty()) {
        if (std::filesystem::exists(current / "xmake.lua", ec) || std::filesystem::exists(current / ".git", ec))
            return current;
        auto parent = current.parent_path();
        if (parent == current)
            break;
        current = std::move(parent);
    }
    return {};
}

std::string resolve_certificate_path(const std::string& candidate, std::initializer_list<const char*> fallbacks)
{
    namespace fs = std::filesystem;

    auto normalize = [](fs::path path) { return path.is_absolute() ? std::move(path) : fs::absolute(path); };

    auto exists_file = [](const fs::path& path) -> bool {
        return !path.empty() && fs::exists(path) && fs::is_regular_file(path);
    };

    if (!candidate.empty()) {
        fs::path direct(candidate);
        if (exists_file(normalize(direct)))
            return normalize(direct).string();
    }

    std::vector<fs::path> search_dirs;
    auto                  push_unique = [&search_dirs](fs::path dir) {
        if (dir.empty())
            return;
        dir = dir.lexically_normal();
        for (const auto& existing : search_dirs) {
            if (existing == dir)
                return;
        }
        search_dirs.push_back(std::move(dir));
    };

    fs::path current = fs::current_path();
    for (int depth = 0; depth < 6 && !current.empty(); ++depth) {
        push_unique(current);
        push_unique(current / "certs");
        push_unique(current / "install" / "certs");
        current = current.parent_path();
    }
    if (const char* env_dir = std::getenv("CO_WQ_CERT_DIR"))
        push_unique(fs::path(env_dir));

    std::vector<std::string> candidates;
    if (!candidate.empty())
        candidates.emplace_back(candidate);
    for (const char* path : fallbacks) {
        if (path)
            candidates.emplace_back(path);
    }

    for (const auto& name : candidates) {
        fs::path file(name);
        if (file.has_parent_path()) {
            fs::path resolved = normalize(file);
            if (exists_file(resolved))
                return resolved.string();
            continue;
        }
        for (const auto& dir : search_dirs) {
            fs::path resolved = dir / file;
            if (exists_file(resolved))
                return resolved.string();
        }
    }

    return candidate;
}

std::string resolve_key_path(const std::string& candidate)
{
    return resolve_certificate_path(candidate, { "server.key", "certs/server.key", "install/certs/server.key" });
}

std::string resolve_cert_path(const std::string& candidate)
{
    return resolve_certificate_path(candidate,
                                    { "server.cert",
                                      "server.crt",
                                      "server.pfx",
                                      "certs/server.crt",
                                      "certs/server.pfx",
                                      "install/certs/server.crt",
                                      "install/certs/server.pfx" });
}

std::vector<std::string> parse_alpn_tokens(const std::string& spec)
{
    std::vector<std::string> tokens;
    size_t                   pos = 0;
    while (pos < spec.size()) {
        size_t next       = spec.find(',', pos);
        size_t end        = (next == std::string::npos) ? spec.size() : next;
        size_t begin_trim = pos;
        while (begin_trim < end && std::isspace(static_cast<unsigned char>(spec[begin_trim])))
            ++begin_trim;
        size_t end_trim = end;
        while (end_trim > begin_trim && std::isspace(static_cast<unsigned char>(spec[end_trim - 1])))
            --end_trim;
        if (end_trim > begin_trim)
            tokens.emplace_back(spec.substr(begin_trim, end_trim - begin_trim));
        pos = (next == std::string::npos) ? spec.size() : next + 1;
    }
    return tokens;
}

std::vector<net::MsquicConstBuffer> build_alpn_buffers(const std::vector<std::string>& tokens)
{
    std::vector<net::MsquicConstBuffer> buffers;
    buffers.reserve(tokens.size());
    for (const auto& token : tokens) {
        net::MsquicConstBuffer buffer {};
        buffer.length = static_cast<std::uint32_t>(token.size());
        buffer.data   = reinterpret_cast<const std::uint8_t*>(token.data());
        buffers.push_back(buffer);
    }
    return buffers;
}

template <typename SocketPtr>
static Task<void, Work_Promise<SpinLock, void>> quic_echo_session(SocketPtr socket, uint64_t session_id)
{
    auto guard = std::move(socket);
    if (!guard)
        co_return;

    const std::string session_label = "[quic#" + std::to_string(session_id) + "]";
    CO_WQ_LOG_DEBUG("%s stream=%p handshake starting", session_label.c_str(), static_cast<const void*>(guard.get()));

    int handshake_rc = co_await guard->handshake();
    if (handshake_rc != 0) {
        CO_WQ_LOG_ERROR("%s handshake failed rc=%d", session_label.c_str(), handshake_rc);
        guard->close();
        co_return;
    }
    CO_WQ_LOG_INFO("%s handshake completed", session_label.c_str());

    std::array<char, 2048> buffer {};
    while (!g_stop.load(std::memory_order_acquire)) {
        ssize_t n = co_await guard->recv(buffer.data(), buffer.size());
        if (n <= 0) {
            if (n < 0)
                CO_WQ_LOG_WARN("%s recv error=%zd", session_label.c_str(), n);
            else
                CO_WQ_LOG_INFO("%s recv completed n=%zd", session_label.c_str(), n);
            break;
        }
        append_log(session_id, "recv=" + std::to_string(n));
        CO_WQ_LOG_INFO("%s recv bytes=%zd", session_label.c_str(), n);
        auto to_hex = [](const char* data, std::size_t len) {
            static const char* digits = "0123456789abcdef";
            std::string        s;
            s.reserve(len * 2);
            for (std::size_t i = 0; i < len; ++i) {
                unsigned char c = static_cast<unsigned char>(data[i]);
                s.push_back(digits[c >> 4]);
                s.push_back(digits[c & 0x0F]);
            }
            return s;
        };
        std::string preview = to_hex(buffer.data(), static_cast<std::size_t>(std::min<std::size_t>(n, 16)));
        CO_WQ_LOG_DEBUG("%s recv preview=%s", session_label.c_str(), preview.c_str());
        ssize_t m = co_await guard->send_all(buffer.data(), static_cast<size_t>(n));
        if (m <= 0) {
            if (m < 0)
                CO_WQ_LOG_WARN("%s send error=%zd", session_label.c_str(), m);
            break;
        }
        append_log(session_id, "send=" + std::to_string(m));
        CO_WQ_LOG_INFO("%s send bytes=%zd", session_label.c_str(), m);
    }
    guard->shutdown_tx();
    CO_WQ_LOG_INFO("%s shutdown_tx requested", session_label.c_str());
    guard->close();
    CO_WQ_LOG_INFO("%s session closed", session_label.c_str());
    co_return;
}

template <typename Listener> static Task<void, Work_Promise<SpinLock, void>> quic_accept_loop(Listener& listener)
{
    while (!g_stop.load(std::memory_order_acquire)) {
        auto socket = co_await listener.accept();
        if (!socket) {
            if (g_stop.load(std::memory_order_acquire))
                break;
            continue;
        }
        uint64_t session_id = g_next_session_id.fetch_add(1, std::memory_order_relaxed);
        CO_WQ_LOG_INFO("[quic] accepted stream session=%llu socket=%p",
                       static_cast<unsigned long long>(session_id),
                       static_cast<const void*>(socket.get()));
        auto task = quic_echo_session(std::move(socket), session_id);
        post_to(task, get_sys_workqueue());
    }
    co_return;
}

void handle_signal(int)
{
    g_stop.store(true, std::memory_order_release);
}

} // namespace

int main(int argc, char** argv)
{
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    auto& exec = get_sys_workqueue();

    std::string               cert_path           = "server.crt";
    std::string               key_path            = "server.key";
    std::string               alpn_spec           = kDefaultAlpnList;
    bool                      msquic_debug        = false;
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
    }

    for (int i = 1; i < argc; ++i) {
        if ((std::strcmp(argv[i], "-cert") == 0 || std::strcmp(argv[i], "--cert") == 0) && i + 1 < argc) {
            cert_path = argv[++i];
        } else if ((std::strcmp(argv[i], "-key") == 0 || std::strcmp(argv[i], "--key") == 0) && i + 1 < argc) {
            key_path = argv[++i];
        } else if ((std::strcmp(argv[i], "-alpn") == 0 || std::strcmp(argv[i], "--alpn") == 0) && i + 1 < argc) {
            alpn_spec = argv[++i];
        } else if ((std::strcmp(argv[i], "--msquic-debug") == 0 || std::strcmp(argv[i], "--debug") == 0)) {
            msquic_debug        = true;
            requested_log_level = spdlog::level::debug;
        } else if ((std::strcmp(argv[i], "--no-msquic-debug") == 0 || std::strcmp(argv[i], "--no-debug") == 0)) {
            msquic_debug        = false;
            requested_log_level = spdlog::level::info;
        } else if ((std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0)) {
            std::printf(
                "Usage: co_quic_echo [-cert file] [-key file] [-alpn string] [--msquic-debug] [--no-msquic-debug]\n");
            return 0;
        } else {
            CO_WQ_LOG_ERROR("[quic] unknown option: %s", argv[i]);
            return 1;
        }
    }

    net::set_msquic_debug_enabled(msquic_debug);

    bool        log_configured = false;
    std::string log_file_path;
    try {
        std::filesystem::path log_path = "logs/quic_echo.log";
        std::filesystem::path resolved = log_path;
        if (!resolved.is_absolute()) {
            if (!project_root.empty())
                resolved = project_root / resolved;
            else if (!cwd_path.empty())
                resolved = cwd_path / resolved;
        }
        resolved = resolved.lexically_normal();

        if (!resolved.empty()) {
            auto parent = resolved.parent_path();
            if (!parent.empty()) {
                std::error_code dir_ec;
                std::filesystem::create_directories(parent, dir_ec);
                if (dir_ec) {
                    std::fprintf(stderr,
                                 "[quic] failed to create log directory %s: %s\n",
                                 parent.string().c_str(),
                                 dir_ec.message().c_str());
                }
            }
        }

        co_wq::log::configure_file_logging(resolved.string(), false, true);
        log_configured = true;
        log_file_path  = resolved.string();

        auto session_path = resolved;
        session_path.replace_filename("quic_session.log");
        session_path = session_path.lexically_normal();

        if (requested_log_level <= spdlog::level::debug) {
            g_session_log_path = session_path;
        } else {
            g_session_log_path.clear();
            std::error_code remove_ec;
            std::filesystem::remove(session_path, remove_ec);
        }
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[quic] failed to initialize log file: %s\n", ex.what());
    }

    co_wq::log::set_level(requested_log_level);
    if (log_configured) {
        CO_WQ_LOG_INFO("[quic] logging to %s", log_file_path.c_str());
    }

    auto alpn_tokens  = parse_alpn_tokens(alpn_spec);
    auto ensure_token = [&alpn_tokens](const std::string& token) {
        if (std::find(alpn_tokens.begin(), alpn_tokens.end(), token) == alpn_tokens.end())
            alpn_tokens.emplace_back(token);
    };
    if (alpn_tokens.empty()) {
        alpn_tokens.emplace_back("http/1.0");
        alpn_tokens.emplace_back("co-wq/echo");
    } else {
        ensure_token("http/1.0");
        ensure_token("co-wq/echo");
    }

    auto alpn_buffers = build_alpn_buffers(alpn_tokens);

    cert_path = resolve_cert_path(cert_path);
    key_path  = resolve_key_path(key_path);

    if (!std::filesystem::exists(cert_path)) {
        CO_WQ_LOG_ERROR("[quic] certificate file not found: %s", cert_path.c_str());
        return 1;
    }

    bool        use_pkcs12 = false;
    std::string pfx_password;
#if defined(_WIN32)
    if (const char* env_pwd = std::getenv("CO_WQ_PFX_PASSWORD")) {
        pfx_password = env_pwd;
    }

    auto to_lower_ext = [](std::string ext) {
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return ext;
    };

    auto try_pkcs12 = [&](const std::filesystem::path& candidate) {
        if (!candidate.empty() && std::filesystem::exists(candidate)) {
            cert_path  = candidate.string();
            use_pkcs12 = true;
        }
    };

    const std::filesystem::path cert_resolved { cert_path };
    const std::string           cert_ext = to_lower_ext(cert_resolved.extension().string());

    if (cert_ext == ".pfx" || cert_ext == ".p12") {
        use_pkcs12 = true;
    }

    if (!use_pkcs12) {
        auto candidate = cert_resolved;
        candidate.replace_extension(".pfx");
        try_pkcs12(candidate);
    }

    if (!use_pkcs12) {
        auto candidate = cert_resolved;
        candidate.replace_extension(".p12");
        try_pkcs12(candidate);
    }

    if (!use_pkcs12) {
        if (auto fallback = resolve_cert_path("server.pfx"); !fallback.empty()) {
            use_pkcs12 = true;
            cert_path  = fallback;
        }
    }
#endif

    if (!use_pkcs12 && !std::filesystem::exists(key_path)) {
        CO_WQ_LOG_ERROR("[quic] key file not found: %s", key_path.c_str());
        return 1;
    }

    if (msquic_debug) {
        CO_WQ_LOG_INFO("[quic] using certificate: %s", cert_path.c_str());
        if (use_pkcs12) {
            CO_WQ_LOG_INFO("[quic] using PKCS#12 bundle (password %s)", pfx_password.empty() ? "<empty>" : "<hidden>");
        } else {
            CO_WQ_LOG_INFO("[quic] using key: %s", key_path.c_str());
        }
        for (const auto& token : alpn_tokens)
            CO_WQ_LOG_INFO("[quic] using ALPN: %s", token.c_str());
    }

    auto api = net::MsquicLoader::instance().acquire();
    if (!api) {
        CO_WQ_LOG_ERROR("[quic] MsQuic not available");
        return 1;
    }
    auto api_ptr = std::make_shared<net::MsquicApi>(std::move(api));

    net::MsquicRegistrationConfig reg_cfg {};
    reg_cfg.app_name = "co_wq_quic_echo";

    net::MsquicSettings settings {};
    settings.peer_bidi_stream_count_set = true;
    settings.peer_bidi_stream_count     = 32;
    settings.idle_timeout_ms_set        = true;
    settings.idle_timeout_ms            = 120000;

    std::shared_ptr<net::quic_context> ctx;
    try {
        ctx = net::quic_context::create(api_ptr,
                                        reg_cfg,
                                        settings,
                                        alpn_buffers.data(),
                                        static_cast<std::uint32_t>(alpn_buffers.size()));
    } catch (const std::exception& ex) {
        CO_WQ_LOG_ERROR("[quic] context create failed: %s", ex.what());
        return 1;
    }

    net::MsquicCredentialConfig cred {};

    if (use_pkcs12) {
        cred.type            = net::MsquicCredentialConfig::Type::CertificatePkcs12;
        cred.pkcs12.file     = cert_path;
        cred.pkcs12.password = pfx_password;
    } else {
        cred.type                              = net::MsquicCredentialConfig::Type::CertificateFile;
        cred.certificate_file.certificate_file = cert_path;
        cred.certificate_file.private_key_file = key_path;
    }

    auto status = ctx->load_credential(cred);
    if (net::quic_status_failed(status)) {
        if (use_pkcs12) {
            CO_WQ_LOG_ERROR("[quic] load credential failed status=0x%x pkcs12=%s",
                            static_cast<unsigned int>(status),
                            cred.pkcs12.file.c_str());
        } else {
            CO_WQ_LOG_ERROR("[quic] load credential failed status=0x%x cert=%s key=%s",
                            static_cast<unsigned int>(status),
                            cred.certificate_file.certificate_file.c_str(),
                            cred.certificate_file.private_key_file.c_str());
        }
        return 1;
    }

    net::quic_listener<SpinLock> listener(exec, ctx);
    status = listener.start(kDefaultPort, alpn_buffers.data(), static_cast<std::uint32_t>(alpn_buffers.size()));
    if (net::quic_status_failed(status)) {
        CO_WQ_LOG_ERROR("[quic] listener start failed status=0x%x", static_cast<unsigned int>(status));
        return 1;
    }

    CO_WQ_LOG_INFO("[quic] listening on UDP port %u", static_cast<unsigned>(kDefaultPort));

    auto accept_task = quic_accept_loop(listener);
    post_to(accept_task, exec);

    sys_wait_until(g_stop);

    listener.stop();
    CO_WQ_LOG_INFO("[quic] shutting down");

    return 0;
}
