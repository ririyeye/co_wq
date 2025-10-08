// io_waiter.hpp - common waiter base for fd-based async operations
#pragma once
#include "workqueue.hpp"
#include <atomic>
#include <coroutine>
#include <cstdint>
#include <memory>
#if defined(_WIN32)
#include <windows.h>

#endif

namespace co_wq::net {

/**
 * @brief 所有 IO awaiter 的公共基类：继承 worknode，提供 resume 回调。
 */
struct io_waiter_base : worknode {
    static constexpr std::uint32_t debug_magic_value = 0x5A17BEEF;

    io_waiter_base()
    {
        INIT_LIST_HEAD(&this->ws_node);
        this->func        = nullptr;
        this->debug_magic = debug_magic_value;
        this->debug_name  = "io_waiter_base";
    }
    ~io_waiter_base() { this->debug_magic = 0; }
    std::coroutine_handle<> h; ///< 目标协程句柄
    // 可选：指定一个“回调队列”路由，保证在多线程执行器下按队列顺序执行
    void* route_ctx { nullptr };
    void (*route_post)(void* ctx, worknode* node) { nullptr };
    using route_guard_ptr = std::shared_ptr<void>;

private:
    std::atomic<route_guard_ptr> route_guard_storage {};

    static long adjust_use_count(const route_guard_ptr& guard) noexcept
    {
        if (!guard)
            return 0;
        auto use = guard.use_count();
        return use > 0 ? static_cast<long>(use - 1) : 0;
    }

public:
    route_guard_ptr load_route_guard() const noexcept { return route_guard_storage.load(std::memory_order_acquire); }

    void store_route_guard(route_guard_ptr guard) noexcept
    {
        route_guard_storage.store(std::move(guard), std::memory_order_release);
    }

    route_guard_ptr exchange_route_guard(route_guard_ptr guard = {}) noexcept
    {
        return route_guard_storage.exchange(std::move(guard), std::memory_order_acq_rel);
    }

    bool has_route_guard() const noexcept { return load_route_guard() != nullptr; }

    long route_guard_use_count() const noexcept { return adjust_use_count(load_route_guard()); }

    static long route_guard_use_count(const route_guard_ptr& guard) noexcept { return adjust_use_count(guard); }

    void             clear_route_guard() noexcept { exchange_route_guard(); }
    std::atomic_bool callback_enqueued { false };
    std::uint32_t    debug_magic { debug_magic_value };
    const char*      debug_name { "io_waiter_base" };

    void set_debug_name(const char* name)
    {
        if (name)
            debug_name = name;
    }
    bool has_valid_magic() const { return debug_magic == debug_magic_value; }

    /**
     * @brief workqueue 投递回调：恢复协程执行。
     */
    static void resume_cb(struct worknode* w)
    {
        auto* self = static_cast<io_waiter_base*>(w);
        if (!self) {
            CO_WQ_CBQ_WARN("[io_waiter] resume_cb invoked with null self\n");
            return;
        }
        auto magic = self->debug_magic;
        auto name  = self->debug_name ? self->debug_name : "<null-name>";
        if (!self->h) {
            CO_WQ_CBQ_WARN("[io_waiter] resume_cb skipping null coroutine self=%p name=%s magic=%08x\n",
                           static_cast<void*>(self),
                           name,
                           magic);
            return;
        }
        void* haddr        = self->h.address();
        bool  already_done = self->h.done();
        if (already_done) {
            CO_WQ_CBQ_WARN("[io_waiter] resume_cb found done coroutine self=%p h=%p name=%s magic=%08x\n",
                           static_cast<void*>(self),
                           haddr,
                           name,
                           magic);
#if defined(_WIN32)
            __debugbreak();
#endif
            return;
        }
        CO_WQ_CBQ_TRACE("[io_waiter] resume_cb resuming self=%p h=%p name=%s magic=%08x\n",
                        static_cast<void*>(self),
                        haddr,
                        name,
                        magic);
        self->h.resume();
        CO_WQ_CBQ_TRACE("[io_waiter] resume_cb completed self=%p h=%p name=%s magic=%08x\n",
                        static_cast<void*>(self),
                        haddr,
                        name,
                        magic);
    }
};

// 通过 route_post（若设置）把节点投递到“串行回调队列”；否则直接投递到主执行队列。
template <class Lock> inline void post_via_route(workqueue<Lock>& exec, worknode& node)
{
    auto& b = static_cast<io_waiter_base&>(node);
    if (b.route_post)
        b.route_post(b.route_ctx, &node);
    else
        exec.post(node);
}

} // namespace co_wq::net
