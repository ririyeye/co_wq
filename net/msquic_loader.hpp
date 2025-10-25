#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace co_wq::net {

using MsquicStatus = std::uint32_t;

struct MsquicLibraryHandle;

inline bool quic_status_failed(MsquicStatus status) noexcept
{
    return static_cast<std::int32_t>(status) > 0;
}

inline bool quic_status_succeeded(MsquicStatus status) noexcept
{
    return !quic_status_failed(status);
}

struct MsquicRegistrationHandle {
    void* value { nullptr };
};

struct MsquicConfigurationHandle {
    void* value { nullptr };
};

struct MsquicListenerHandle {
    void* value { nullptr };
};

struct MsquicConnectionHandle {
    void* value { nullptr };
};

struct MsquicStreamHandle {
    void* value { nullptr };
};

enum class MsquicExecutionProfile : std::uint32_t {
    LowLatency    = 0,
    MaxThroughput = 1,
    Scavenger     = 2,
    RealTime      = 3,
};

struct MsquicRegistrationConfig {
    const char*            app_name          = nullptr;
    MsquicExecutionProfile execution_profile = MsquicExecutionProfile::LowLatency;
};

struct MsquicSettings {
    bool          idle_timeout_ms_set        = false;
    std::uint64_t idle_timeout_ms            = 0;
    bool          peer_bidi_stream_count_set = false;
    std::uint16_t peer_bidi_stream_count     = 0;
};

struct MsquicBuffer {
    std::uint32_t length = 0;
    std::uint8_t* data   = nullptr;
};

struct MsquicConstBuffer {
    std::uint32_t       length = 0;
    const std::uint8_t* data   = nullptr;
};

enum class MsquicSendFlags : std::uint32_t {
    None = 0x0000,
    Fin  = 0x0004,
};

inline MsquicSendFlags operator|(MsquicSendFlags lhs, MsquicSendFlags rhs) noexcept
{
    return static_cast<MsquicSendFlags>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

inline MsquicSendFlags operator&(MsquicSendFlags lhs, MsquicSendFlags rhs) noexcept
{
    return static_cast<MsquicSendFlags>(static_cast<std::uint32_t>(lhs) & static_cast<std::uint32_t>(rhs));
}

enum class MsquicStreamShutdownFlags : std::uint32_t {
    None      = 0x0000,
    AbortSend = 0x0002,
};

struct MsquicCertificateFileConfig {
    std::string private_key_file;
    std::string certificate_file;
};

struct MsquicCredentialConfig {
    enum class Type : std::uint32_t {
        CertificateFile = 4,
    };

    Type                        type = Type::CertificateFile;
    MsquicCertificateFileConfig certificate_file;
};

struct MsquicReceiveBuffer {
    const std::uint8_t* data   = nullptr;
    std::uint32_t       length = 0;
};

struct MsquicStreamReceiveEvent {
    std::vector<MsquicReceiveBuffer> buffers;
    bool                             fin = false;
};

enum class MsquicStreamEventType {
    Receive,
    SendComplete,
    ShutdownComplete,
    Unknown,
};

struct MsquicStreamEvent {
    MsquicStreamEventType    type = MsquicStreamEventType::Unknown;
    MsquicStreamReceiveEvent receive;
};

using MsquicStreamCallback = MsquicStatus (*)(MsquicStreamHandle, void*, const MsquicStreamEvent&);

enum class MsquicConnectionEventType {
    Connected,
    ShutdownComplete,
    PeerStreamStarted,
    ShutdownByTransport,
    ShutdownByPeer,
    Unknown,
};

struct MsquicConnectionEvent {
    MsquicConnectionEventType type = MsquicConnectionEventType::Unknown;
    MsquicStreamHandle        stream;
    MsquicStatus              status     = 0;
    std::uint64_t             error_code = 0;
};

using MsquicConnectionCallback = MsquicStatus (*)(MsquicConnectionHandle, void*, const MsquicConnectionEvent&);

enum class MsquicListenerEventType {
    NewConnection,
    StopComplete,
    Unknown,
};

struct MsquicListenerEvent {
    MsquicListenerEventType type = MsquicListenerEventType::Unknown;
    MsquicConnectionHandle  connection;
};

using MsquicListenerCallback = MsquicStatus (*)(MsquicListenerHandle, void*, const MsquicListenerEvent&);

class MsquicApi;

class MsquicLoader {
public:
    static MsquicLoader& instance();

    bool      load(const std::vector<std::string>& search_paths = {});
    MsquicApi acquire(const std::vector<std::string>& search_paths = {});
    void      unload();

    const std::string& last_error() const noexcept { return last_error_; }
    const std::string& loaded_path() const noexcept { return loaded_path_; }

private:
    friend class MsquicApi;

    using MsQuicOpenVersionFn = MsquicStatus (*)(std::uint32_t, const void**);
    using MsQuicCloseFn       = void (*)(const void*);

    bool ensure_loaded_locked(const std::vector<std::string>& search_paths);
    void release_api(const void* api);

    MsquicLibraryHandle* handle_       = nullptr;
    MsQuicOpenVersionFn  open_version_ = nullptr;
    MsQuicCloseFn        close_        = nullptr;
    std::string          last_error_;
    std::string          loaded_path_;
    std::size_t          active_apis_ = 0;
    mutable std::mutex   mutex_;
};

class MsquicApi {
public:
    MsquicApi() = default;
    MsquicApi(MsquicApi&& other) noexcept;
    MsquicApi& operator=(MsquicApi&& other) noexcept;

    ~MsquicApi();

    MsquicApi(const MsquicApi&)            = delete;
    MsquicApi& operator=(const MsquicApi&) = delete;

    explicit operator bool() const noexcept { return impl_ != nullptr; }

    MsquicStatus registration_open(const MsquicRegistrationConfig&, MsquicRegistrationHandle&);
    void         registration_close(MsquicRegistrationHandle) noexcept;

    MsquicStatus configuration_open(MsquicRegistrationHandle,
                                    const MsquicConstBuffer* alpns,
                                    std::uint32_t            alpn_count,
                                    const MsquicSettings&    settings,
                                    MsquicConfigurationHandle&);
    void         configuration_close(MsquicConfigurationHandle) noexcept;

    MsquicStatus configuration_load_credential(MsquicConfigurationHandle, const MsquicCredentialConfig&);

    MsquicStatus
         listener_open(MsquicRegistrationHandle, MsquicListenerCallback, void* listener_context, MsquicListenerHandle&);
    void listener_close(MsquicListenerHandle) noexcept;

    MsquicStatus listener_start_any(MsquicListenerHandle,
                                    const MsquicConstBuffer* alpns,
                                    std::uint32_t            alpn_count,
                                    std::uint16_t            port);
    void         listener_stop(MsquicListenerHandle) noexcept;

    MsquicStatus connection_set_configuration(MsquicConnectionHandle, MsquicConfigurationHandle);
    void         connection_close(MsquicConnectionHandle) noexcept;

    MsquicStatus stream_send(MsquicStreamHandle,
                             const MsquicBuffer* buffers,
                             std::uint32_t       buffer_count,
                             MsquicSendFlags     flags,
                             void*               client_context);
    MsquicStatus stream_shutdown(MsquicStreamHandle, MsquicStreamShutdownFlags, std::uint64_t error_code);
    void         stream_close(MsquicStreamHandle) noexcept;

    void set_stream_callback(MsquicStreamHandle, MsquicStreamCallback, void* context);
    void set_connection_callback(MsquicConnectionHandle, MsquicConnectionCallback, void* context);
    void set_listener_callback(MsquicListenerHandle, MsquicListenerCallback, void* context);

private:
    friend class MsquicLoader;

    struct Impl;

    explicit MsquicApi(Impl* impl);

    void reset() noexcept;

    Impl* impl_ = nullptr;
};

} // namespace co_wq::net
