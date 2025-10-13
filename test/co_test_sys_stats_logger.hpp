#pragma once

#include "co_syswork.hpp"
#include "task.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace co_wq::test {

struct SysAllocatorSnapshot {
    int alloc_total { 0 };
    int free_total { 0 };

    [[nodiscard]] int balance() const noexcept { return alloc_total - free_total; }
};

inline SysAllocatorSnapshot capture_sys_allocator_snapshot() noexcept
{
    SysAllocatorSnapshot snapshot;
    snapshot.alloc_total = ::sys_sta.malloc_cnt;
    snapshot.free_total  = ::sys_sta.free_cnt;
    return snapshot;
}

inline void log_sys_allocator_stats(std::string_view tag, const SysAllocatorSnapshot& snapshot)
{
    CO_WQ_LOG_INFO("[%.*s] allocations=%d frees=%d balance=%d",
                   static_cast<int>(tag.size()),
                   tag.data(),
                   snapshot.alloc_total,
                   snapshot.free_total,
                   snapshot.balance());
}

inline void log_sys_allocator_stats(std::string_view tag)
{
    log_sys_allocator_stats(tag, capture_sys_allocator_snapshot());
}

class SysStatsLogger {
public:
    using Callback = std::function<void(const SysAllocatorSnapshot&)>;

    struct Options {
        std::chrono::milliseconds interval { std::chrono::milliseconds(5000) };
        Callback                  callback {};
        bool                      emit_default_log { true };
    };

    explicit SysStatsLogger(std::string tag);
    SysStatsLogger(std::string tag, Options options);
    ~SysStatsLogger();

    SysStatsLogger(const SysStatsLogger&)            = delete;
    SysStatsLogger& operator=(const SysStatsLogger&) = delete;

private:
    void run();
    void log_once();

    std::string               tag_;
    std::chrono::milliseconds interval_;
    Callback                  callback_;
    bool                      emit_default_log_;
    std::atomic_bool          stop_ { false };
    std::mutex                mutex_;
    std::condition_variable   cv_;
    std::thread               thread_;
};

inline SysStatsLogger::SysStatsLogger(std::string tag) : SysStatsLogger(std::move(tag), Options {}) { }

inline SysStatsLogger::SysStatsLogger(std::string tag, Options options)
    : tag_(std::move(tag))
    , interval_(options.interval)
    , callback_(std::move(options.callback))
    , emit_default_log_(options.emit_default_log)
{
    thread_ = std::thread([this]() { run(); });
}

inline SysStatsLogger::~SysStatsLogger()
{
    {
        std::lock_guard lock(mutex_);
        stop_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
    log_once();
}

inline void SysStatsLogger::run()
{
    std::unique_lock lock(mutex_);
    while (!stop_.load(std::memory_order_acquire)) {
        if (cv_.wait_for(lock, interval_) == std::cv_status::timeout) {
            lock.unlock();
            log_once();
            lock.lock();
        }
    }
}

inline void SysStatsLogger::log_once()
{
    auto snapshot = capture_sys_allocator_snapshot();
    if (emit_default_log_)
        log_sys_allocator_stats(tag_, snapshot);
    if (callback_) {
        try {
            callback_(snapshot);
        } catch (const std::exception& ex) {
            CO_WQ_LOG_ERROR("[%.*s] stats callback threw: %s", static_cast<int>(tag_.size()), tag_.data(), ex.what());
        } catch (...) {
            CO_WQ_LOG_ERROR("[%.*s] stats callback threw unknown exception",
                            static_cast<int>(tag_.size()),
                            tag_.data());
        }
    }
}

} // namespace co_wq::test
