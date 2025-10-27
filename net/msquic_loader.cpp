#include "msquic_loader.hpp"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#include "../msquic-install/include/msquic.h"

namespace co_wq::net {

#if defined(_WIN32)
struct MsquicLibraryHandle {
    HMODULE handle { nullptr };
};
#else
struct MsquicLibraryHandle {
    void* handle { nullptr };
};
#endif

namespace {

    std::atomic<int> g_msquic_debug_flag { 0 };

} // namespace

bool msquic_debug_enabled() noexcept
{
    return g_msquic_debug_flag.load(std::memory_order_acquire) > 0;
}

void set_msquic_debug_enabled(bool enabled) noexcept
{
    g_msquic_debug_flag.store(enabled ? 1 : 0, std::memory_order_release);
}

namespace {

    std::filesystem::path resolve_executable_dir()
    {
#if defined(_WIN32)
        char  buffer[MAX_PATH] = {};
        DWORD length           = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
        if (length == 0 || length == MAX_PATH) {
            return std::filesystem::current_path();
        }
        return std::filesystem::path(buffer).parent_path();
#else
        char          buffer[PATH_MAX] = {};
        const ssize_t length           = readlink("/proc/self/exe", buffer, PATH_MAX - 1);
        if (length <= 0) {
            return std::filesystem::current_path();
        }
        buffer[length] = '\0';
        return std::filesystem::path(buffer).parent_path();
#endif
    }

    std::string get_environment_string(const char* name)
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

    std::vector<std::string> build_candidate_paths(const std::vector<std::string>& user_paths)
    {
        std::vector<std::string> candidates;

        auto add_candidate = [&candidates](const std::string& value) {
            if (value.empty()) {
                return;
            }
            if (std::find(candidates.begin(), candidates.end(), value) == candidates.end()) {
                candidates.push_back(value);
            }
        };

        for (const auto& path : user_paths) {
            add_candidate(path);
        }

        if (auto env_path = get_environment_string("MSQUIC_LIB_PATH"); !env_path.empty()) {
            add_candidate(env_path);
        }

        auto add_nearby_defaults = [&add_candidate](std::filesystem::path base) {
            for (int depth = 0; depth < 6 && !base.empty(); ++depth) {
                add_candidate((base / "msquic-install" / "lib" / "libmsquic.so").string());
                add_candidate((base / "install" / "lib" / "libmsquic.so").string());
                base = base.parent_path();
            }
        };

        add_nearby_defaults(std::filesystem::current_path());

        const auto exe_dir = resolve_executable_dir();
        add_nearby_defaults(exe_dir);
        add_candidate((exe_dir / "libmsquic.so").string());

#if defined(_WIN32)
        add_candidate("msquic.dll");
#else
        add_candidate("libmsquic.so");
        add_candidate("libmsquic.so.2");
#endif

        return candidates;
    }

#if defined(_WIN32)
    MsquicLibraryHandle* open_library(const std::string& path, std::string& error)
    {
        HMODULE handle = LoadLibraryA(path.c_str());
        if (!handle) {
            error = "LoadLibrary failed for " + path;
            return nullptr;
        }
        auto* wrapper   = new MsquicLibraryHandle();
        wrapper->handle = handle;
        return wrapper;
    }

    void close_library(MsquicLibraryHandle* handle)
    {
        if (!handle) {
            return;
        }
        FreeLibrary(handle->handle);
        delete handle;
    }

    void* resolve_symbol(MsquicLibraryHandle* handle, const char* name)
    {
        if (!handle) {
            return nullptr;
        }
        return reinterpret_cast<void*>(GetProcAddress(handle->handle, name));
    }
#else
    MsquicLibraryHandle* open_library(const std::string& path, std::string& error)
    {
        dlerror();
        void* handle = dlopen(path.c_str(), RTLD_LOCAL | RTLD_NOW);
        if (!handle) {
            if (const char* dl_err = dlerror()) {
                error = dl_err;
            } else {
                error = "dlopen failed for " + path;
            }
            return nullptr;
        }
        auto* wrapper   = new MsquicLibraryHandle();
        wrapper->handle = handle;
        return wrapper;
    }

    void close_library(MsquicLibraryHandle* handle)
    {
        if (!handle) {
            return;
        }
        dlclose(handle->handle);
        delete handle;
    }

    void* resolve_symbol(MsquicLibraryHandle* handle, const char* name)
    {
        if (!handle) {
            return nullptr;
        }
        dlerror();
        return dlsym(handle->handle, name);
    }
#endif

    inline HQUIC to_raw(MsquicRegistrationHandle handle)
    {
        return reinterpret_cast<HQUIC>(handle.value);
    }

    inline HQUIC to_raw(MsquicConfigurationHandle handle)
    {
        return reinterpret_cast<HQUIC>(handle.value);
    }

    inline HQUIC to_raw(MsquicListenerHandle handle)
    {
        return reinterpret_cast<HQUIC>(handle.value);
    }

    inline HQUIC to_raw(MsquicConnectionHandle handle)
    {
        return reinterpret_cast<HQUIC>(handle.value);
    }

    inline HQUIC to_raw(MsquicStreamHandle handle)
    {
        return reinterpret_cast<HQUIC>(handle.value);
    }

    inline MsquicRegistrationHandle to_registration(HQUIC handle)
    {
        return { reinterpret_cast<void*>(handle) };
    }

    inline MsquicConfigurationHandle to_configuration(HQUIC handle)
    {
        return { reinterpret_cast<void*>(handle) };
    }

    inline MsquicListenerHandle to_listener(HQUIC handle)
    {
        return { reinterpret_cast<void*>(handle) };
    }

    inline MsquicConnectionHandle to_connection(HQUIC handle)
    {
        return { reinterpret_cast<void*>(handle) };
    }

    inline MsquicStreamHandle to_stream(HQUIC handle)
    {
        return { reinterpret_cast<void*>(handle) };
    }

} // namespace

struct MsquicApi::Impl {
    const QUIC_API_TABLE* table = nullptr;
    MsquicLoader*         owner = nullptr;

    struct ListenerEntry {
        MsquicListenerCallback callback     = nullptr;
        void*                  user_context = nullptr;
        Impl*                  self         = nullptr;
    };

    struct ConnectionEntry {
        MsquicConnectionCallback callback     = nullptr;
        void*                    user_context = nullptr;
        Impl*                    self         = nullptr;
    };

    struct StreamEntry {
        MsquicStreamCallback callback     = nullptr;
        void*                user_context = nullptr;
        Impl*                self         = nullptr;
    };

    std::mutex                                                listener_mutex;
    std::unordered_map<HQUIC, std::unique_ptr<ListenerEntry>> listener_entries;

    std::mutex                                                  connection_mutex;
    std::unordered_map<HQUIC, std::unique_ptr<ConnectionEntry>> connection_entries;

    std::mutex                                              stream_mutex;
    std::unordered_map<HQUIC, std::unique_ptr<StreamEntry>> stream_entries;

    Impl(const QUIC_API_TABLE* api_table, MsquicLoader& loader) : table(api_table), owner(&loader) { }

    ~Impl()
    {
        if (owner && table) {
            owner->release_api(table);
        }
    }

    static QUIC_STATUS QUIC_API listener_trampoline(HQUIC listener, void* context, QUIC_LISTENER_EVENT* event)
    {
        auto* entry = static_cast<ListenerEntry*>(context);
        if (!entry || !entry->callback) {
            return QUIC_STATUS_SUCCESS;
        }
        return entry->self->dispatch_listener_event(listener, *entry, *event);
    }

    static QUIC_STATUS QUIC_API connection_trampoline(HQUIC connection, void* context, QUIC_CONNECTION_EVENT* event)
    {
        auto* entry = static_cast<ConnectionEntry*>(context);
        if (!entry || !entry->callback) {
            return QUIC_STATUS_SUCCESS;
        }
        return entry->self->dispatch_connection_event(connection, *entry, *event);
    }

    static QUIC_STATUS QUIC_API stream_trampoline(HQUIC stream, void* context, QUIC_STREAM_EVENT* event)
    {
        auto* entry = static_cast<StreamEntry*>(context);
        if (!entry || !entry->callback) {
            return QUIC_STATUS_SUCCESS;
        }
        return entry->self->dispatch_stream_event(stream, *entry, *event);
    }

    QUIC_STATUS dispatch_listener_event(HQUIC listener, ListenerEntry& entry, QUIC_LISTENER_EVENT& native_event)
    {
        MsquicListenerEvent evt;
        switch (native_event.Type) {
        case QUIC_LISTENER_EVENT_NEW_CONNECTION:
            evt.type       = MsquicListenerEventType::NewConnection;
            evt.connection = to_connection(native_event.NEW_CONNECTION.Connection);
            break;
        case QUIC_LISTENER_EVENT_STOP_COMPLETE:
            evt.type = MsquicListenerEventType::StopComplete;
            break;
        default:
            evt.type = MsquicListenerEventType::Unknown;
            break;
        }
        const MsquicStatus status = entry.callback(to_listener(listener), entry.user_context, evt);
        return static_cast<QUIC_STATUS>(status);
    }

    QUIC_STATUS dispatch_connection_event(HQUIC connection, ConnectionEntry& entry, QUIC_CONNECTION_EVENT& native_event)
    {
        MsquicConnectionEvent evt;
        switch (native_event.Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED:
            evt.type = MsquicConnectionEventType::Connected;
            break;
        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            evt.type = MsquicConnectionEventType::ShutdownComplete;
            break;
        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
            evt.type   = MsquicConnectionEventType::PeerStreamStarted;
            evt.stream = to_stream(native_event.PEER_STREAM_STARTED.Stream);
            break;
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
            evt.type   = MsquicConnectionEventType::ShutdownByTransport;
            evt.status = native_event.SHUTDOWN_INITIATED_BY_TRANSPORT.Status;
            break;
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
            evt.type       = MsquicConnectionEventType::ShutdownByPeer;
            evt.error_code = native_event.SHUTDOWN_INITIATED_BY_PEER.ErrorCode;
            break;
        default:
            evt.type = MsquicConnectionEventType::Unknown;
            break;
        }
        const MsquicStatus status = entry.callback(to_connection(connection), entry.user_context, evt);
        return static_cast<QUIC_STATUS>(status);
    }

    QUIC_STATUS dispatch_stream_event(HQUIC stream, StreamEntry& entry, QUIC_STREAM_EVENT& native_event)
    {
        MsquicStreamEvent evt;
        switch (native_event.Type) {
        case QUIC_STREAM_EVENT_RECEIVE: {
            evt.type                    = MsquicStreamEventType::Receive;
            evt.receive.fin             = (native_event.RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0;
            evt.receive.flags           = native_event.RECEIVE.Flags;
            evt.receive.absolute_offset = native_event.RECEIVE.AbsoluteOffset;
            evt.receive.total_length    = native_event.RECEIVE.TotalBufferLength;
            evt.receive.buffers.clear();
            evt.receive.buffers.reserve(native_event.RECEIVE.BufferCount);
            for (uint32_t i = 0; i < native_event.RECEIVE.BufferCount; ++i) {
                const auto& buf = native_event.RECEIVE.Buffers[i];
                evt.receive.buffers.push_back({ buf.Buffer, buf.Length });
            }
            break;
        }
        case QUIC_STREAM_EVENT_SEND_COMPLETE:
            evt.type                         = MsquicStreamEventType::SendComplete;
            evt.send_complete.client_context = native_event.SEND_COMPLETE.ClientContext;
            evt.send_complete.canceled       = native_event.SEND_COMPLETE.Canceled != FALSE;
            break;
        case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
            evt.type = MsquicStreamEventType::PeerSendShutdown;
            break;
        case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
            evt.type                         = MsquicStreamEventType::PeerSendAborted;
            evt.peer_send_aborted.error_code = native_event.PEER_SEND_ABORTED.ErrorCode;
            break;
        case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
            evt.type                            = MsquicStreamEventType::PeerReceiveAborted;
            evt.peer_receive_aborted.error_code = native_event.PEER_RECEIVE_ABORTED.ErrorCode;
            break;
        case QUIC_STREAM_EVENT_SEND_SHUTDOWN_COMPLETE:
            evt.type                            = MsquicStreamEventType::SendShutdownComplete;
            evt.send_shutdown_complete.graceful = native_event.SEND_SHUTDOWN_COMPLETE.Graceful != FALSE;
            break;
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            evt.type                                    = MsquicStreamEventType::ShutdownComplete;
            evt.shutdown_complete.connection_shutdown   = native_event.SHUTDOWN_COMPLETE.ConnectionShutdown != FALSE;
            evt.shutdown_complete.app_close_in_progress = native_event.SHUTDOWN_COMPLETE.AppCloseInProgress != FALSE;
            evt.shutdown_complete.connection_closed_remotely = native_event.SHUTDOWN_COMPLETE.ConnectionClosedRemotely
                != FALSE;
            break;
        default:
            evt.type = MsquicStreamEventType::Unknown;
            if (msquic_debug_enabled()) {
                std::printf("[msquic-loader] unhandled stream event type=%u\n", native_event.Type);
                std::fflush(stdout);
            }
            break;
        }
        const MsquicStatus status = entry.callback(to_stream(stream), entry.user_context, evt);
        return static_cast<QUIC_STATUS>(status);
    }

    ListenerEntry* ensure_listener_entry(HQUIC listener)
    {
        std::lock_guard<std::mutex> lock(listener_mutex);
        auto                        it = listener_entries.find(listener);
        if (it != listener_entries.end()) {
            return it->second.get();
        }
        auto  entry = std::make_unique<ListenerEntry>();
        auto* raw   = entry.get();
        raw->self   = this;
        listener_entries.emplace(listener, std::move(entry));
        return raw;
    }

    ConnectionEntry* ensure_connection_entry(HQUIC connection)
    {
        std::lock_guard<std::mutex> lock(connection_mutex);
        auto                        it = connection_entries.find(connection);
        if (it != connection_entries.end()) {
            return it->second.get();
        }
        auto  entry = std::make_unique<ConnectionEntry>();
        auto* raw   = entry.get();
        raw->self   = this;
        connection_entries.emplace(connection, std::move(entry));
        return raw;
    }

    StreamEntry* ensure_stream_entry(HQUIC stream)
    {
        std::lock_guard<std::mutex> lock(stream_mutex);
        auto                        it = stream_entries.find(stream);
        if (it != stream_entries.end()) {
            return it->second.get();
        }
        auto  entry = std::make_unique<StreamEntry>();
        auto* raw   = entry.get();
        raw->self   = this;
        stream_entries.emplace(stream, std::move(entry));
        return raw;
    }

    void remove_listener_entry(HQUIC listener)
    {
        std::lock_guard<std::mutex> lock(listener_mutex);
        listener_entries.erase(listener);
    }

    void remove_connection_entry(HQUIC connection)
    {
        std::lock_guard<std::mutex> lock(connection_mutex);
        connection_entries.erase(connection);
    }

    void remove_stream_entry(HQUIC stream)
    {
        std::lock_guard<std::mutex> lock(stream_mutex);
        stream_entries.erase(stream);
    }
};

MsquicLoader& MsquicLoader::instance()
{
    static MsquicLoader loader;
    return loader;
}

bool MsquicLoader::load(const std::vector<std::string>& search_paths)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return ensure_loaded_locked(search_paths);
}

MsquicApi MsquicLoader::acquire(const std::vector<std::string>& search_paths)
{
    const QUIC_API_TABLE* api = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ensure_loaded_locked(search_paths)) {
            return {};
        }
        last_error_.clear();
        if (!open_version_) {
            last_error_ = "MsQuicOpenVersion/MsQuicOpen2 symbol not available";
            return {};
        }
        const void*        raw_api = nullptr;
        const MsquicStatus status  = open_version_(2u, &raw_api);
        api                        = reinterpret_cast<const QUIC_API_TABLE*>(raw_api);
        if (quic_status_failed(status) || api == nullptr) {
            std::ostringstream oss;
            oss << "MsQuicOpenVersion failed with status 0x" << std::hex << status;
            last_error_ = oss.str();
            return {};
        }
        ++active_apis_;
    }
    return MsquicApi(new MsquicApi::Impl(api, *this));
}

void MsquicLoader::unload()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_apis_ != 0) {
        last_error_ = "Attempted to unload libmsquic while APIs are active";
        return;
    }
    if (handle_ == nullptr) {
        return;
    }
    close_library(handle_);
    handle_       = nullptr;
    open_version_ = nullptr;
    close_        = nullptr;
    loaded_path_.clear();
}

bool MsquicLoader::ensure_loaded_locked(const std::vector<std::string>& search_paths)
{
    if (handle_ != nullptr) {
        return true;
    }

    last_error_.clear();

    const auto candidates = build_candidate_paths(search_paths);

    for (const auto& candidate : candidates) {
        const bool debug_logging = msquic_debug_enabled();
        if (debug_logging) {
            std::fprintf(stderr, "[msquic-loader] candidate: %s\n", candidate.c_str());
        }
        const std::filesystem::path candidate_path(candidate);
        if ((candidate_path.is_absolute() || candidate_path.has_parent_path())
            && !std::filesystem::exists(candidate_path)) {
            if (debug_logging) {
                std::fprintf(stderr, "[msquic-loader] skip missing path: %s\n", candidate.c_str());
            }
            continue;
        }
        auto* handle = open_library(candidate, last_error_);
        if (!handle) {
            if (debug_logging) {
                std::fprintf(stderr, "[msquic-loader] dlopen(%s) failed: %s\n", candidate.c_str(), last_error_.c_str());
            }
            continue;
        }

        auto* open_fn = reinterpret_cast<MsQuicOpenVersionFn>(resolve_symbol(handle, "MsQuicOpenVersion"));
        if (!open_fn) {
            open_fn = reinterpret_cast<MsQuicOpenVersionFn>(resolve_symbol(handle, "MsQuicOpen2"));
        }
        auto* close_fn = reinterpret_cast<MsQuicCloseFn>(resolve_symbol(handle, "MsQuicClose"));
        if (!open_fn || !close_fn) {
            close_library(handle);
            last_error_ = "Failed to resolve MsQuicOpenVersion/MsQuicClose from " + candidate;
            continue;
        }

        handle_       = handle;
        open_version_ = open_fn;
        close_        = close_fn;
        loaded_path_  = candidate;
        last_error_.clear();
        return true;
    }

    if (last_error_.empty()) {
        last_error_ = "Unable to locate libmsquic shared library";
    }
    return false;
}

void MsquicLoader::release_api(const void* api)
{
    if (!api) {
        return;
    }
    if (close_) {
        close_(api);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_apis_ != 0) {
        --active_apis_;
    }
}

MsquicApi::MsquicApi(Impl* impl) : impl_(impl) { }

MsquicApi::MsquicApi(MsquicApi&& other) noexcept
{
    impl_       = other.impl_;
    other.impl_ = nullptr;
}

MsquicApi& MsquicApi::operator=(MsquicApi&& other) noexcept
{
    if (this == &other) {
        return *this;
    }
    reset();
    impl_       = other.impl_;
    other.impl_ = nullptr;
    return *this;
}

MsquicApi::~MsquicApi()
{
    reset();
}

void MsquicApi::reset() noexcept
{
    delete impl_;
    impl_ = nullptr;
}

MsquicStatus MsquicApi::registration_open(const MsquicRegistrationConfig& config, MsquicRegistrationHandle& handle)
{
    if (!impl_ || !impl_->table) {
        return 1;
    }
    QUIC_REGISTRATION_CONFIG native_config { config.app_name,
                                             static_cast<QUIC_EXECUTION_PROFILE>(config.execution_profile) };
    HQUIC                    registration = nullptr;
    const MsquicStatus       status       = impl_->table->RegistrationOpen(&native_config, &registration);
    if (quic_status_failed(status)) {
        return status;
    }
    handle = to_registration(registration);
    return status;
}

void MsquicApi::registration_close(MsquicRegistrationHandle handle) noexcept
{
    if (!impl_ || !impl_->table || !handle.value) {
        return;
    }
    impl_->table->RegistrationClose(to_raw(handle));
}

MsquicStatus MsquicApi::configuration_open(MsquicRegistrationHandle   registration,
                                           const MsquicConstBuffer*   alpns,
                                           std::uint32_t              alpn_count,
                                           const MsquicSettings&      settings,
                                           MsquicConfigurationHandle& configuration)
{
    if (!impl_ || !impl_->table || !registration.value) {
        return 1;
    }

    std::vector<QUIC_BUFFER> native_alpns(alpn_count);
    for (std::uint32_t i = 0; i < alpn_count; ++i) {
        native_alpns[i].Length = alpns[i].length;
        native_alpns[i].Buffer = const_cast<std::uint8_t*>(alpns[i].data);
    }

    QUIC_SETTINGS native_settings {};
    if (settings.idle_timeout_ms_set) {
        native_settings.IsSet.IdleTimeoutMs = 1;
        native_settings.IdleTimeoutMs       = settings.idle_timeout_ms;
    }
    if (settings.peer_bidi_stream_count_set) {
        native_settings.IsSet.PeerBidiStreamCount = 1;
        native_settings.PeerBidiStreamCount       = settings.peer_bidi_stream_count;
    }

    HQUIC              native_configuration = nullptr;
    const MsquicStatus status               = impl_->table->ConfigurationOpen(to_raw(registration),
                                                                native_alpns.empty() ? nullptr : native_alpns.data(),
                                                                alpn_count,
                                                                &native_settings,
                                                                sizeof(native_settings),
                                                                nullptr,
                                                                &native_configuration);
    if (quic_status_failed(status)) {
        return status;
    }
    configuration = to_configuration(native_configuration);
    return status;
}

void MsquicApi::configuration_close(MsquicConfigurationHandle configuration) noexcept
{
    if (!impl_ || !impl_->table || !configuration.value) {
        return;
    }
    impl_->table->ConfigurationClose(to_raw(configuration));
}

MsquicStatus MsquicApi::configuration_load_credential(MsquicConfigurationHandle     configuration,
                                                      const MsquicCredentialConfig& config)
{
    if (!impl_ || !impl_->table || !configuration.value) {
        return 1;
    }

    QUIC_CERTIFICATE_FILE file_info { config.certificate_file.private_key_file.c_str(),
                                      config.certificate_file.certificate_file.c_str() };

    QUIC_CREDENTIAL_CONFIG native_config {};
    native_config.Type            = static_cast<QUIC_CREDENTIAL_TYPE>(config.type);
    native_config.Flags           = QUIC_CREDENTIAL_FLAG_NONE;
    native_config.CertificateFile = &file_info;

    return impl_->table->ConfigurationLoadCredential(to_raw(configuration), &native_config);
}

MsquicStatus MsquicApi::listener_open(MsquicRegistrationHandle registration,
                                      MsquicListenerCallback   callback,
                                      void*                    listener_context,
                                      MsquicListenerHandle&    listener)
{
    if (!impl_ || !impl_->table || !registration.value || !callback) {
        return 1;
    }

    auto entry          = std::make_unique<Impl::ListenerEntry>();
    entry->callback     = callback;
    entry->user_context = listener_context;
    entry->self         = impl_;

    HQUIC              native_listener = nullptr;
    const MsquicStatus status          = impl_->table->ListenerOpen(
        to_raw(registration),
        reinterpret_cast<QUIC_LISTENER_CALLBACK_HANDLER>(Impl::listener_trampoline),
        entry.get(),
        &native_listener);
    if (quic_status_failed(status)) {
        return status;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->listener_mutex);
        impl_->listener_entries.emplace(native_listener, std::move(entry));
    }

    listener = to_listener(native_listener);
    return status;
}

void MsquicApi::listener_close(MsquicListenerHandle listener) noexcept
{
    if (!impl_ || !impl_->table || !listener.value) {
        return;
    }
    impl_->remove_listener_entry(to_raw(listener));
    impl_->table->ListenerClose(to_raw(listener));
}

MsquicStatus MsquicApi::listener_start_any(MsquicListenerHandle     listener,
                                           const MsquicConstBuffer* alpns,
                                           std::uint32_t            alpn_count,
                                           std::uint16_t            port)
{
    if (!impl_ || !impl_->table || !listener.value) {
        return 1;
    }

    std::vector<QUIC_BUFFER> native_alpns(alpn_count);
    for (std::uint32_t i = 0; i < alpn_count; ++i) {
        native_alpns[i].Length = alpns[i].length;
        native_alpns[i].Buffer = const_cast<std::uint8_t*>(alpns[i].data);
    }

    QUIC_ADDR address {};
    QuicAddrSetFamily(&address, QUIC_ADDRESS_FAMILY_UNSPEC);
    QuicAddrSetPort(&address, port);

    return impl_->table->ListenerStart(to_raw(listener),
                                       native_alpns.empty() ? nullptr : native_alpns.data(),
                                       alpn_count,
                                       &address);
}

void MsquicApi::listener_stop(MsquicListenerHandle listener) noexcept
{
    if (!impl_ || !impl_->table || !listener.value) {
        return;
    }
    impl_->table->ListenerStop(to_raw(listener));
}

MsquicStatus MsquicApi::connection_set_configuration(MsquicConnectionHandle    connection,
                                                     MsquicConfigurationHandle configuration)
{
    if (!impl_ || !impl_->table || !connection.value || !configuration.value) {
        return 1;
    }
    return impl_->table->ConnectionSetConfiguration(to_raw(connection), to_raw(configuration));
}

void MsquicApi::connection_close(MsquicConnectionHandle connection) noexcept
{
    if (!impl_ || !impl_->table || !connection.value) {
        return;
    }
    impl_->remove_connection_entry(to_raw(connection));
    impl_->table->ConnectionClose(to_raw(connection));
}

MsquicStatus MsquicApi::stream_send(MsquicStreamHandle  stream,
                                    const MsquicBuffer* buffers,
                                    std::uint32_t       buffer_count,
                                    MsquicSendFlags     flags,
                                    void*               client_context)
{
    if (!impl_ || !impl_->table || !stream.value) {
        return 1;
    }

    static_assert(sizeof(MsquicBuffer) == sizeof(QUIC_BUFFER), "MsquicBuffer size mismatch");
    static_assert(alignof(MsquicBuffer) == alignof(QUIC_BUFFER), "MsquicBuffer alignment mismatch");

    return impl_->table->StreamSend(to_raw(stream),
                                    buffer_count == 0 ? nullptr : reinterpret_cast<const QUIC_BUFFER*>(buffers),
                                    buffer_count,
                                    static_cast<QUIC_SEND_FLAGS>(flags),
                                    client_context);
}

MsquicStatus
MsquicApi::stream_shutdown(MsquicStreamHandle stream, MsquicStreamShutdownFlags flags, std::uint64_t error_code)
{
    if (!impl_ || !impl_->table || !stream.value) {
        return 1;
    }
    return impl_->table->StreamShutdown(to_raw(stream), static_cast<QUIC_STREAM_SHUTDOWN_FLAGS>(flags), error_code);
}

void MsquicApi::stream_close(MsquicStreamHandle stream) noexcept
{
    if (!impl_ || !impl_->table || !stream.value) {
        return;
    }
    impl_->remove_stream_entry(to_raw(stream));
    impl_->table->StreamClose(to_raw(stream));
}

void MsquicApi::stream_receive_complete(MsquicStreamHandle stream, std::uint64_t length) noexcept
{
    if (!impl_ || !impl_->table || !stream.value) {
        return;
    }
    impl_->table->StreamReceiveComplete(to_raw(stream), length);
}

void MsquicApi::stream_receive_set_enabled(MsquicStreamHandle stream, bool enabled) noexcept
{
    if (!impl_ || !impl_->table || !stream.value) {
        return;
    }
    impl_->table->StreamReceiveSetEnabled(to_raw(stream), static_cast<BOOLEAN>(enabled ? 1 : 0));
}

void MsquicApi::set_stream_callback(MsquicStreamHandle stream, MsquicStreamCallback callback, void* context)
{
    if (!impl_ || !impl_->table || !stream.value) {
        return;
    }
    auto* entry         = impl_->ensure_stream_entry(to_raw(stream));
    entry->callback     = callback;
    entry->user_context = context;
    entry->self         = impl_;
    impl_->table->SetCallbackHandler(to_raw(stream), reinterpret_cast<void*>(Impl::stream_trampoline), entry);
}

void MsquicApi::set_connection_callback(MsquicConnectionHandle   connection,
                                        MsquicConnectionCallback callback,
                                        void*                    context)
{
    if (!impl_ || !impl_->table || !connection.value) {
        return;
    }
    auto* entry         = impl_->ensure_connection_entry(to_raw(connection));
    entry->callback     = callback;
    entry->user_context = context;
    entry->self         = impl_;
    impl_->table->SetCallbackHandler(to_raw(connection), reinterpret_cast<void*>(Impl::connection_trampoline), entry);
}

void MsquicApi::set_listener_callback(MsquicListenerHandle listener, MsquicListenerCallback callback, void* context)
{
    if (!impl_ || !impl_->table || !listener.value) {
        return;
    }
    auto* entry         = impl_->ensure_listener_entry(to_raw(listener));
    entry->callback     = callback;
    entry->user_context = context;
    entry->self         = impl_;
    impl_->table->SetCallbackHandler(to_raw(listener), reinterpret_cast<void*>(Impl::listener_trampoline), entry);
}

} // namespace co_wq::net
