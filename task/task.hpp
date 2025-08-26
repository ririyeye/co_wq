#pragma once

#include "previous_awaiter.hpp"
#include "uninitialized.hpp"
#include <coroutine>
#include <utility>

namespace co_wq {

#if USE_EXCEPTION
#include <exception>
#endif

struct Promise_base {

    auto initial_suspend() noexcept { return std::suspend_always(); }

    auto final_suspend() noexcept { return PreviousAwaiter(mPrevious); }

    void unhandled_exception() noexcept
    {
#if USE_EXCEPTION
        mException = std::current_exception();
#endif
    }
    std::coroutine_handle<> mPrevious;
#if USE_EXCEPTION
    std::exception_ptr mException {};
#endif
};

template <class T> struct Promise : Promise_base {

    void return_value(T&& ret) { mResult.emplace(std::move(ret)); }

    void return_value(T const& ret) { mResult.emplace(ret); }

    T result()
    {
#if USE_EXCEPTION
        if (mException) [[unlikely]] {
            std::rethrow_exception(mException);
        }
#endif
        return mResult.move();
    }

    auto get_return_object() { return std::coroutine_handle<Promise>::from_promise(*this); }

    Uninitialized<T> mResult; // destructed??

    Promise& operator=(Promise&&) = delete;
};

template <> struct Promise<void> : Promise_base {
    void return_void() noexcept { }

    void result()
    {
#if USE_EXCEPTION
        if (mException) [[unlikely]] {
            std::rethrow_exception(mException);
        }
#endif
    }

    auto get_return_object() { return std::coroutine_handle<Promise>::from_promise(*this); }

    Promise& operator=(Promise&&) = delete;
};

template <class T = void, class P = Promise<T>> struct [[nodiscard]] Task {
    using promise_type = P;

    Task(std::coroutine_handle<promise_type> coroutine = nullptr) noexcept : mCoroutine(coroutine) { }

    Task(Task&& that) noexcept : mCoroutine(that.mCoroutine) { that.mCoroutine = nullptr; }

    Task& operator=(Task&& that) noexcept { std::swap(mCoroutine, that.mCoroutine); }

    ~Task()
    {
        if (mCoroutine)
            mCoroutine.destroy();
    }

    std::coroutine_handle<promise_type> detach() noexcept { return std::exchange(mCoroutine, nullptr); }

    std::coroutine_handle<promise_type> get() const noexcept { return mCoroutine; }

    std::coroutine_handle<promise_type> release() noexcept { return std::exchange(mCoroutine, nullptr); }

    struct Awaiter {
        bool await_ready() const noexcept { return false; }

        std::coroutine_handle<promise_type> await_suspend(std::coroutine_handle<> coroutine) const noexcept
        {
            promise_type& promise = mCoroutine.promise();
            promise.mPrevious     = coroutine;
            return mCoroutine;
        }

        T await_resume() const { return mCoroutine.promise().result(); }

        std::coroutine_handle<promise_type> mCoroutine;
    };

    auto operator co_await() const noexcept { return Awaiter(mCoroutine); }

    operator std::coroutine_handle<promise_type>() const noexcept { return mCoroutine; }

    // private:
    std::coroutine_handle<promise_type> mCoroutine;
};

template <class Loop, class T, class P> T run_task(Loop& loop, Task<T, P> const& t)
{
    auto a = t.operator co_await();
    a.await_suspend(std::noop_coroutine()).resume();
    while (loop.run())
        ;
    return a.await_resume();
}

template <class T, class P> void spawn_task(Task<T, P> const& t)
{
    auto a = t.operator co_await();
    a.await_suspend(std::noop_coroutine()).resume();
}

} // namespace co_wq
