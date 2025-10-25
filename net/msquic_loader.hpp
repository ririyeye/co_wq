#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace co_wq::net {

struct QUIC_API_TABLE;
using QUIC_STATUS = unsigned int;

struct MsquicLibraryHandle;

inline bool quic_status_failed(QUIC_STATUS status) noexcept
{
    return static_cast<int>(status) > 0;
}

inline bool quic_status_succeeded(QUIC_STATUS status) noexcept
{
    return !quic_status_failed(status);
}

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

    using MsQuicOpenVersionFn = QUIC_STATUS (*)(std::uint32_t, const QUIC_API_TABLE**);
    using MsQuicCloseFn       = void (*)(const QUIC_API_TABLE*);

    bool ensure_loaded_locked(const std::vector<std::string>& search_paths);
    void release_api(const QUIC_API_TABLE* api);

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
    MsquicApi()                            = default;
    MsquicApi(const MsquicApi&)            = delete;
    MsquicApi& operator=(const MsquicApi&) = delete;

    MsquicApi(MsquicApi&& other) noexcept;
    MsquicApi& operator=(MsquicApi&& other) noexcept;

    ~MsquicApi();

    const QUIC_API_TABLE* get() const noexcept { return api_; }
    const QUIC_API_TABLE* operator->() const noexcept { return api_; }
    explicit              operator bool() const noexcept { return api_ != nullptr; }

private:
    friend class MsquicLoader;

    MsquicApi(const QUIC_API_TABLE* api, MsquicLoader& owner);

    void reset() noexcept;

    const QUIC_API_TABLE* api_   = nullptr;
    MsquicLoader*         owner_ = nullptr;
};

} // namespace co_wq::net
