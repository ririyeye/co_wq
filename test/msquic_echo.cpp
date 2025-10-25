#include "msquic_loader.hpp"

#include "msquic.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kDefaultAlpn = "http/1.0";
constexpr uint16_t    kDefaultPort = 6121;

std::atomic<bool>     g_running { true };
const QUIC_API_TABLE* g_msquic = nullptr;

using co_wq::net::quic_status_failed;

struct StreamContext {
    std::string payload;
    QUIC_BUFFER send_buffer { 0, nullptr };
    bool        send_pending { false };
    bool        peer_shutdown { false };
};

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

    if (const char* env_cert_dir = std::getenv("CO_WQ_CERT_DIR")) {
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

    if (localtime_r(&now, &tmInfo) != nullptr) {
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

QUIC_STATUS QUIC_API EchoStreamCallback(HQUIC stream, void* context, QUIC_STREAM_EVENT* event)
{
    auto* streamCtx = static_cast<StreamContext*>(context);

    switch (event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
        for (uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
            const auto& buffer = event->RECEIVE.Buffers[i];
            streamCtx->payload.append(reinterpret_cast<const char*>(buffer.Buffer), buffer.Length);
        }
        std::printf("[stream %p] received %u buffers, total=%zu fin=%u\n",
                    static_cast<void*>(stream),
                    event->RECEIVE.BufferCount,
                    streamCtx->payload.size(),
                    (event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0);
        if ((event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0) {
            streamCtx->peer_shutdown = true;
            if (!streamCtx->payload.empty()) {
                streamCtx->send_buffer.Length = static_cast<uint32_t>(streamCtx->payload.size());
                streamCtx->send_buffer.Buffer = reinterpret_cast<uint8_t*>(
                    const_cast<char*>(streamCtx->payload.data()));
                streamCtx->send_pending = true;

                QUIC_STATUS status = g_msquic->StreamSend(stream,
                                                          &streamCtx->send_buffer,
                                                          1,
                                                          QUIC_SEND_FLAG_FIN,
                                                          nullptr);
                std::printf("[stream %p] echo send length=%u status=0x%x\n",
                            static_cast<void*>(stream),
                            streamCtx->send_buffer.Length,
                            status);
                if (quic_status_failed(status)) {
                    streamCtx->send_pending = false;
                    return status;
                }
            } else {
                std::printf("[stream %p] empty payload, abort send\n", static_cast<void*>(stream));
                return g_msquic->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND, 0);
            }
        }
        break;
    }
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        if (streamCtx != nullptr) {
            streamCtx->send_pending = false;
            streamCtx->payload.clear();
        }
        std::printf("[stream %p] send complete\n", static_cast<void*>(stream));
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        delete streamCtx;
        g_msquic->StreamClose(stream);
        std::printf("[stream %p] shutdown complete\n", static_cast<void*>(stream));
        break;
    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API EchoConnectionCallback(HQUIC connection, void*, QUIC_CONNECTION_EVENT* event)
{
    switch (event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        std::printf("[conn %p] connected\n", static_cast<void*>(connection));
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        g_msquic->ConnectionClose(connection);
        std::printf("[conn %p] shutdown complete\n", static_cast<void*>(connection));
        break;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
        std::printf("[conn %p] peer stream started %p\n",
                    static_cast<void*>(connection),
                    static_cast<void*>(event->PEER_STREAM_STARTED.Stream));
        auto* ctx = new (std::nothrow) StreamContext();
        if (!ctx) {
            return QUIC_STATUS_OUT_OF_MEMORY;
        }
        g_msquic->SetCallbackHandler(event->PEER_STREAM_STARTED.Stream,
                                     reinterpret_cast<void*>(EchoStreamCallback),
                                     ctx);
        break;
    }
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        std::printf("[conn %p] shutdown by transport error=0x%x\n",
                    static_cast<void*>(connection),
                    event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        std::printf("[conn %p] shutdown by peer error=0x%llx\n",
                    static_cast<void*>(connection),
                    static_cast<unsigned long long>(event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode));
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API EchoListenerCallback(HQUIC listener, void* context, QUIC_LISTENER_EVENT* event)
{
    (void)listener;
    switch (event->Type) {
    case QUIC_LISTENER_EVENT_NEW_CONNECTION:
        std::printf("[listener] new connection %p\n", static_cast<void*>(event->NEW_CONNECTION.Connection));
        g_msquic->SetCallbackHandler(event->NEW_CONNECTION.Connection,
                                     reinterpret_cast<void*>(EchoConnectionCallback),
                                     nullptr);
        return g_msquic->ConnectionSetConfiguration(event->NEW_CONNECTION.Connection, static_cast<HQUIC>(context));
    case QUIC_LISTENER_EVENT_STOP_COMPLETE:
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
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

    if (std::getenv("CO_WQ_MSQUIC_DEBUG")) {
        std::printf("[msquic-cert] using certificate: %s\n", certFile.c_str());
        std::printf("[msquic-cert] using key: %s\n", keyFile.c_str());
    }

    auto api = co_wq::net::MsquicLoader::instance().acquire();
    if (!api) {
        std::fprintf(stderr, "Failed to load MsQuic: %s\n", co_wq::net::MsquicLoader::instance().last_error().c_str());
        return 1;
    }

    g_msquic           = reinterpret_cast<const QUIC_API_TABLE*>(api.get());
    QUIC_STATUS status = QUIC_STATUS_SUCCESS;

    QUIC_REGISTRATION_CONFIG regConfig { "co_msquic_echo", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
    HQUIC                    registration = nullptr;
    status                                = g_msquic->RegistrationOpen(&regConfig, &registration);
    if (quic_status_failed(status)) {
        std::fprintf(stderr, "RegistrationOpen failed: 0x%x\n", status);
        return 1;
    }

    QUIC_SETTINGS settings {};
    settings.IdleTimeoutMs             = 30000;
    settings.IsSet.IdleTimeoutMs       = TRUE;
    settings.PeerBidiStreamCount       = 16;
    settings.IsSet.PeerBidiStreamCount = TRUE;

    QUIC_BUFFER alpnBuffer;
    alpnBuffer.Length = static_cast<uint32_t>(std::strlen(kDefaultAlpn));
    alpnBuffer.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(kDefaultAlpn));

    HQUIC configuration = nullptr;
    status              = g_msquic->ConfigurationOpen(registration,
                                         &alpnBuffer,
                                         1,
                                         &settings,
                                         sizeof(settings),
                                         nullptr,
                                         &configuration);
    if (quic_status_failed(status)) {
        std::fprintf(stderr, "ConfigurationOpen failed: 0x%x\n", status);
        g_msquic->RegistrationClose(registration);
        return 1;
    }

    QUIC_CERTIFICATE_FILE certificateFile { keyFile.c_str(), certFile.c_str() };

    QUIC_CREDENTIAL_CONFIG credConfig {};
    credConfig.Type            = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    credConfig.CertificateFile = &certificateFile;

    status = g_msquic->ConfigurationLoadCredential(configuration, &credConfig);
    if (quic_status_failed(status)) {
        std::fprintf(stderr, "ConfigurationLoadCredential failed: 0x%x\n", status);
        g_msquic->ConfigurationClose(configuration);
        g_msquic->RegistrationClose(registration);
        return 1;
    }

    HQUIC listener = nullptr;
    status         = g_msquic->ListenerOpen(registration, EchoListenerCallback, configuration, &listener);
    if (quic_status_failed(status)) {
        std::fprintf(stderr, "ListenerOpen failed: 0x%x\n", status);
        g_msquic->ConfigurationClose(configuration);
        g_msquic->RegistrationClose(registration);
        return 1;
    }

    QUIC_ADDR address {};
    QuicAddrSetFamily(&address, QUIC_ADDRESS_FAMILY_UNSPEC);
    QuicAddrSetPort(&address, kDefaultPort);

    status = g_msquic->ListenerStart(listener, &alpnBuffer, 1, &address);
    if (quic_status_failed(status)) {
        std::fprintf(stderr, "ListenerStart failed: 0x%x\n", status);
        g_msquic->ListenerClose(listener);
        g_msquic->ConfigurationClose(configuration);
        g_msquic->RegistrationClose(registration);
        return 1;
    }

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    PrintTimestamp();
    std::printf(" MsQuic echo server running on UDP port %u\n", kDefaultPort);

    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    g_msquic->ListenerStop(listener);
    g_msquic->ListenerClose(listener);
    g_msquic->ConfigurationClose(configuration);
    g_msquic->RegistrationClose(registration);

    co_wq::net::MsquicLoader::instance().unload();

    return 0;
}
