// io_waiter.hpp - common waiter base for fd-based async operations
#pragma once
#include "workqueue.hpp"
#include <coroutine>

namespace co_wq::net {

struct io_waiter_base : worknode {
    std::coroutine_handle<> h; // coroutine to resume
    static void             resume_cb(struct worknode* w)
    {
        auto* self = static_cast<io_waiter_base*>(w);
        if (self->h)
            self->h.resume();
    }
};

} // namespace co_wq::net
