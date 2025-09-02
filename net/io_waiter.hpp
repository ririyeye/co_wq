// io_waiter.hpp - common waiter base for fd-based async operations
#pragma once
#include "workqueue.hpp"
#include <coroutine>

namespace co_wq::net {

/**
 * @brief 所有 IO awaiter 的公共基类：继承 worknode，提供 resume 回调。
 */
struct io_waiter_base : worknode {
    std::coroutine_handle<> h; ///< 目标协程句柄
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

} // namespace co_wq::net
