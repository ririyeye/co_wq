#pragma once

#include <coroutine>

namespace co_wq {

struct PreviousAwaiter {
    std::coroutine_handle<> mPrevious = nullptr;

    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<>) const noexcept
    {
        return mPrevious ? mPrevious : std::noop_coroutine();
    }

    void await_resume() const noexcept { }
};

}
