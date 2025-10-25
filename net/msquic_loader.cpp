#include "msquic_loader.hpp"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

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

        if (const char* env_path = std::getenv("MSQUIC_LIB_PATH")) {
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

} // namespace

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
        const QUIC_STATUS status = open_version_(2u, &api);
        if (quic_status_failed(status) || api == nullptr) {
            std::ostringstream oss;
            oss << "MsQuicOpenVersion failed with status 0x" << std::hex << status;
            last_error_ = oss.str();
            return {};
        }
        ++active_apis_;
    }
    return MsquicApi(api, *this);
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
        const bool debug_logging = std::getenv("CO_WQ_MSQUIC_DEBUG") != nullptr;
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

void MsquicLoader::release_api(const QUIC_API_TABLE* api)
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

MsquicApi::MsquicApi(const QUIC_API_TABLE* api, MsquicLoader& owner) : api_(api), owner_(&owner) { }

MsquicApi::MsquicApi(MsquicApi&& other) noexcept
{
    *this = std::move(other);
}

MsquicApi& MsquicApi::operator=(MsquicApi&& other) noexcept
{
    if (this == &other) {
        return *this;
    }
    reset();
    api_         = other.api_;
    owner_       = other.owner_;
    other.api_   = nullptr;
    other.owner_ = nullptr;
    return *this;
}

MsquicApi::~MsquicApi()
{
    reset();
}

void MsquicApi::reset() noexcept
{
    if (owner_ && api_) {
        owner_->release_api(api_);
    }
    api_   = nullptr;
    owner_ = nullptr;
}

} // namespace co_wq::net
