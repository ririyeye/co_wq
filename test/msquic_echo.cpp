#include "msquic_loader.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kDefaultAlpn = "http/1.0";
constexpr uint16_t    kDefaultPort = 6121;

constexpr co_wq::net::MsquicStatus kStatusNoMemory = 0xC0000017u;

std::atomic<bool>     g_running { true };
co_wq::net::MsquicApi g_msquic_api;

using co_wq::net::quic_status_failed;

struct StreamContext {
    std::string              payload;
    co_wq::net::MsquicBuffer send_buffer { 0, nullptr };
    bool                     send_pending { false };
    bool                     peer_shutdown { false };
};

struct ListenerContext {
    co_wq::net::MsquicConfigurationHandle configuration {};
};

std::string GetEnvironmentString(const char* name)
{
#if defined(_WIN32)
    size_t buffer_length = 0;
    char*  buffer        = nullptr;
    if (_dupenv_s(&buffer, &buffer_length, name) != 0 || !buffer) {
        return {};
    }
    std::string value(buffer, buffer_length == 0 ? 0 : buffer_length - 1);
    std::free(buffer);
    return value;
#else
    const char* env = std::getenv(name);
    return env ? std::string(env) : std::string();
#endif
}

std::string ResolveCertificatePath(const std::string& candidate, std::initializer_list<const char*> fallbacks)
{
    namespace fs = std::filesystem;

    auto has_parent = [](const fs::path& path) -> bool {
        return path.has_parent_path() && !(path.has_filename() && path.parent_path().empty());
    };

    if (!candidate.empty()) {
        fs::path direct(candidate);
        if (fs::exists(direct)) {
            return direct.string();
        }
        if (has_parent(direct)) {
            fs::path resolved = fs::absolute(direct);
            if (fs::exists(resolved)) {
                return resolved.string();
            }
        }
    }

    std::vector<fs::path> search_dirs;
    auto                  add_dir = [&search_dirs](fs::path dir) {
        if (dir.empty()) {
            return;
        }
        dir = dir.lexically_normal();
        for (const auto& existing : search_dirs) {
            if (existing == dir) {
                return;
            }
        }
        search_dirs.push_back(std::move(dir));
    };

    fs::path current = fs::current_path();
    for (int depth = 0; depth < 6 && !current.empty(); ++depth) {
        add_dir(current);
        add_dir(current / "certs");
        add_dir(current / "install" / "certs");
        current = current.parent_path();
    }

    if (auto env_cert_dir = GetEnvironmentString("CO_WQ_CERT_DIR"); !env_cert_dir.empty()) {
        add_dir(fs::path(env_cert_dir));
    }

    std::vector<std::string> names;
    if (!candidate.empty()) {
        names.emplace_back(candidate);
    }
    for (const char* fallback : fallbacks) {
        if (fallback) {
            names.emplace_back(fallback);
        }
    }

    for (const auto& name : names) {
        fs::path filename(name);
        if (has_parent(filename)) {
            fs::path resolved = filename.is_absolute() ? filename : fs::absolute(filename);
            if (fs::exists(resolved)) {
                return resolved.string();
            }
            continue;
        }
        for (const auto& dir : search_dirs) {
            fs::path candidate_path = dir / filename;
            if (fs::exists(candidate_path)) {
                return candidate_path.string();
            }
        }
    }

    if (!candidate.empty()) {
        return candidate;
    }
    return fallbacks.size() > 0 ? std::string(*fallbacks.begin()) : std::string {};
}

void PrintTimestamp()
{
    std::time_t now = std::time(nullptr);
    std::tm     tmInfo {};

#if defined(_WIN32)
    if (localtime_s(&tmInfo, &now) == 0) {
#else
    if (localtime_r(&now, &tmInfo) != nullptr) {
#endif
        char buffer[64];
        if (std::strftime(buffer, sizeof(buffer), "[%Y-%m-%d %H:%M:%S]", &tmInfo) > 0) {
            std::printf("%s", buffer);
        }
    }
}

void SignalHandler(int signum)
{
    (void)signum;
    std::printf("\n");
    PrintTimestamp();
    std::printf(" Shutting down server...\n");
    g_running.store(false, std::memory_order_relaxed);
}

co_wq::net::MsquicStatus
EchoStreamCallback(co_wq::net::MsquicStreamHandle stream, void* context, const co_wq::net::MsquicStreamEvent& event)
{
    auto* streamCtx = static_cast<StreamContext*>(context);

    switch (event.type) {
    case co_wq::net::MsquicStreamEventType::Receive: {
        for (const auto& buffer : event.receive.buffers) {
            streamCtx->payload.append(reinterpret_cast<const char*>(buffer.data), buffer.length);
        }
        std::printf("[stream %p] received %zu buffers, total=%zu fin=%u\n",
                    stream.value,
                    event.receive.buffers.size(),
                    streamCtx->payload.size(),
                    event.receive.fin ? 1u : 0u);
        if (event.receive.fin) {
            streamCtx->peer_shutdown = true;
            if (!streamCtx->payload.empty()) {
                streamCtx->send_pending       = true;
                streamCtx->send_buffer.length = static_cast<std::uint32_t>(streamCtx->payload.size());
                streamCtx->send_buffer.data   = reinterpret_cast<std::uint8_t*>(streamCtx->payload.data());

                co_wq::net::MsquicStatus status = g_msquic_api.stream_send(stream,
                                                                           &streamCtx->send_buffer,
                                                                           1,
                                                                           co_wq::net::MsquicSendFlags::Fin,
                                                                           nullptr);
                std::printf("[stream %p] echo send length=%u status=0x%x\n",
                            stream.value,
                            streamCtx->send_buffer.length,
                            status);
                if (quic_status_failed(status)) {
                    streamCtx->send_pending = false;
                    return status;
                }
            } else {
                std::printf("[stream %p] empty payload, abort send\n", stream.value);
                return g_msquic_api.stream_shutdown(stream, co_wq::net::MsquicStreamShutdownFlags::AbortSend, 0);
            }
        }
        break;
    }
    case co_wq::net::MsquicStreamEventType::SendComplete:
        if (streamCtx != nullptr) {
            streamCtx->send_pending = false;
            streamCtx->payload.clear();
            streamCtx->send_buffer.length = 0;
            streamCtx->send_buffer.data   = nullptr;
        }
        std::printf("[stream %p] send complete\n", stream.value);
        break;
    case co_wq::net::MsquicStreamEventType::ShutdownComplete:
        delete streamCtx;
        g_msquic_api.stream_close(stream);
        std::printf("[stream %p] shutdown complete\n", stream.value);
        break;
    default:
        break;
    }

    return 0;
}

co_wq::net::MsquicStatus EchoConnectionCallback(co_wq::net::MsquicConnectionHandle connection,
                                                void*,
                                                const co_wq::net::MsquicConnectionEvent& event)
{
    switch (event.type) {
    case co_wq::net::MsquicConnectionEventType::Connected:
        std::printf("[conn %p] connected\n", connection.value);
        break;
    case co_wq::net::MsquicConnectionEventType::ShutdownComplete:
        g_msquic_api.connection_close(connection);
        std::printf("[conn %p] shutdown complete\n", connection.value);
        break;
    case co_wq::net::MsquicConnectionEventType::PeerStreamStarted: {
        std::printf("[conn %p] peer stream started %p\n", connection.value, event.stream.value);
        auto* ctx = new (std::nothrow) StreamContext();
        if (!ctx) {
            return kStatusNoMemory;
        }
        g_msquic_api.set_stream_callback(event.stream, EchoStreamCallback, ctx);
        break;
    }
    case co_wq::net::MsquicConnectionEventType::ShutdownByTransport:
        std::printf("[conn %p] shutdown by transport error=0x%x\n", connection.value, event.status);
        break;
    case co_wq::net::MsquicConnectionEventType::ShutdownByPeer:
        std::printf("[conn %p] shutdown by peer error=0x%llx\n",
                    connection.value,
                    static_cast<unsigned long long>(event.error_code));
        break;
    default:
        break;
    }
    return 0;
}

co_wq::net::MsquicStatus EchoListenerCallback(co_wq::net::MsquicListenerHandle       listener,
                                              void*                                  context,
                                              const co_wq::net::MsquicListenerEvent& event)
{
    (void)listener;
    switch (event.type) {
    case co_wq::net::MsquicListenerEventType::NewConnection: {
        std::printf("[listener] new connection %p\n", event.connection.value);
        g_msquic_api.set_connection_callback(event.connection, EchoConnectionCallback, nullptr);
        auto* listenerCtx = static_cast<ListenerContext*>(context);
        if (listenerCtx) {
            const auto status = g_msquic_api.connection_set_configuration(event.connection, listenerCtx->configuration);
            if (quic_status_failed(status)) {
                return status;
            }
        }
        break;
    }
    case co_wq::net::MsquicListenerEventType::StopComplete:
        break;
    default:
        break;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::string certFile = "server.cert";
    std::string keyFile  = "server.key";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-cert") == 0 && i + 1 < argc) {
            certFile = argv[++i];
        } else if (std::strcmp(argv[i], "-key") == 0 && i + 1 < argc) {
            keyFile = argv[++i];
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: co_msquic_echo [-cert file] [-key file]\n");
            return 0;
        } else {
            std::printf("Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    certFile = ResolveCertificatePath(certFile, { "server.cert", "server.crt" });
    keyFile  = ResolveCertificatePath(keyFile, { "server.key" });

    if (co_wq::net::msquic_debug_enabled()) {
        std::printf("[msquic-cert] using certificate: %s\n", certFile.c_str());
        std::printf("[msquic-cert] using key: %s\n", keyFile.c_str());
    }

    auto api = co_wq::net::MsquicLoader::instance().acquire();
    if (!api) {
        std::fprintf(stderr, "Failed to load MsQuic: %s\n", co_wq::net::MsquicLoader::instance().last_error().c_str());
        return 1;
    }

    g_msquic_api = std::move(api);

    co_wq::net::MsquicRegistrationHandle       registration {};
    const co_wq::net::MsquicRegistrationConfig regConfig { "co_msquic_echo",
                                                           co_wq::net::MsquicExecutionProfile::LowLatency };
    auto                                       status = g_msquic_api.registration_open(regConfig, registration);
    if (quic_status_failed(status)) {
        std::fprintf(stderr, "RegistrationOpen failed: 0x%x\n", status);
        g_msquic_api = co_wq::net::MsquicApi {};
        co_wq::net::MsquicLoader::instance().unload();
        return 1;
    }

    co_wq::net::MsquicSettings settings {};
    settings.idle_timeout_ms_set        = true;
    settings.idle_timeout_ms            = 30000;
    settings.peer_bidi_stream_count_set = true;
    settings.peer_bidi_stream_count     = 16;

    const co_wq::net::MsquicConstBuffer alpnBuffer { static_cast<std::uint32_t>(std::strlen(kDefaultAlpn)),
                                                     reinterpret_cast<const std::uint8_t*>(kDefaultAlpn) };

    co_wq::net::MsquicConfigurationHandle configuration {};
    status = g_msquic_api.configuration_open(registration, &alpnBuffer, 1, settings, configuration);
    if (quic_status_failed(status)) {
        std::fprintf(stderr, "ConfigurationOpen failed: 0x%x\n", status);
        g_msquic_api.registration_close(registration);
        g_msquic_api = co_wq::net::MsquicApi {};
        co_wq::net::MsquicLoader::instance().unload();
        return 1;
    }

    co_wq::net::MsquicCredentialConfig credential {};
    credential.certificate_file.private_key_file = keyFile;
    credential.certificate_file.certificate_file = certFile;

    status = g_msquic_api.configuration_load_credential(configuration, credential);
    if (quic_status_failed(status)) {
        std::fprintf(stderr, "ConfigurationLoadCredential failed: 0x%x\n", status);
        g_msquic_api.configuration_close(configuration);
        g_msquic_api.registration_close(registration);
        g_msquic_api = co_wq::net::MsquicApi {};
        co_wq::net::MsquicLoader::instance().unload();
        return 1;
    }

    co_wq::net::MsquicListenerHandle listener {};
    ListenerContext                  listener_context { configuration };

    status = g_msquic_api.listener_open(registration, EchoListenerCallback, &listener_context, listener);
    if (quic_status_failed(status)) {
        std::fprintf(stderr, "ListenerOpen failed: 0x%x\n", status);
        g_msquic_api.configuration_close(configuration);
        g_msquic_api.registration_close(registration);
        g_msquic_api = co_wq::net::MsquicApi {};
        co_wq::net::MsquicLoader::instance().unload();
        return 1;
    }

    bool listener_started = false;

    status = g_msquic_api.listener_start_any(listener, &alpnBuffer, 1, kDefaultPort);
    if (quic_status_failed(status)) {
        std::fprintf(stderr, "ListenerStart failed: 0x%x\n", status);
        g_msquic_api.listener_close(listener);
        g_msquic_api.configuration_close(configuration);
        g_msquic_api.registration_close(registration);
        g_msquic_api = co_wq::net::MsquicApi {};
        co_wq::net::MsquicLoader::instance().unload();
        return 1;
    }

    listener_started = true;

    auto cleanup = [&](bool stop_listener) {
        if (listener.value) {
            if (stop_listener && listener_started) {
                g_msquic_api.listener_stop(listener);
            }
            g_msquic_api.listener_close(listener);
            listener = {};
        }
        if (configuration.value) {
            g_msquic_api.configuration_close(configuration);
            configuration = {};
        }
        if (registration.value) {
            g_msquic_api.registration_close(registration);
            registration = {};
        }
    };

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    PrintTimestamp();
    std::printf(" MsQuic echo server running on UDP port %u\n", kDefaultPort);

    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    cleanup(true);

    g_msquic_api = co_wq::net::MsquicApi {};
    co_wq::net::MsquicLoader::instance().unload();

    return 0;
}
