// io_waiter.hpp - common waiter base for fd-based async operations
#pragma once
#include "workqueue.hpp"
#include <atomic>
#include <coroutine>

namespace co_wq::net {

/**
 * @brief 所有 IO awaiter 的公共基类：继承 worknode，提供 resume 回调。
 */
struct io_waiter_base : worknode {
    std::coroutine_handle<> h; ///< 目标协程句柄
    // 可选：指定一个“回调队列”路由，保证在多线程执行器下按队列顺序执行
    void* route_ctx { nullptr };
    void (*route_post)(void* ctx, worknode* node) { nullptr };
    std::atomic_bool callback_enqueued { false };
    /**
     * @brief workqueue 投递回调：恢复协程执行。
     */
    static void resume_cb(struct worknode* w)
    {
        auto* self = static_cast<io_waiter_base*>(w);
        if (self->h)
            self->h.resume();
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
