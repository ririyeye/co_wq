#pragma once

#include "lock.hpp"
#include "os_compat.hpp"
#include "worker.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace co_wq {

template <lockable lock> struct workqueue;

namespace net::dns {

    struct resolve_options {
        int  family { AF_UNSPEC };
        bool allow_dual_stack { false };
    };

    struct resolve_result {
        bool             success { false };
        sockaddr_storage storage {};
        socklen_t        length { 0 };
        int              error_code { 0 };
        std::string      error_message;
        // 所有候选地址（按 getaddrinfo 返回顺序）。第一个匹配策略的地址仍保存在 storage/length，便于兼容旧调用点。
        struct endpoint_entry {
            sockaddr_storage addr {};
            socklen_t        len { 0 };
        };
        std::vector<endpoint_entry> endpoints;
        // 被选中的 endpoints 索引；若无可用则为 SIZE_MAX。
        size_t                                selected_index { static_cast<size_t>(-1) };
        std::chrono::steady_clock::time_point submit_time {};
        std::chrono::steady_clock::time_point start_time {};
        std::chrono::steady_clock::time_point finish_time {};
    };

    resolve_result resolve_sync(const std::string& host, uint16_t port, const resolve_options& options);

    class async_resolver {
    public:
        explicit async_resolver(size_t worker_count = 0);
        ~async_resolver();

        Task<resolve_result, Work_Promise<SpinLock, resolve_result>>
        resolve(workqueue<SpinLock>& exec, const std::string& host, uint16_t port, const resolve_options& options);

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };

} // namespace net::dns

} // namespace co_wq
