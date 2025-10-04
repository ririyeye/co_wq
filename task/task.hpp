#pragma once

#include "previous_awaiter.hpp"
#include "uninitialized.hpp"
#include <coroutine>
#include <cstdlib>
#include <utility>

struct task_stat {
    int malloc_cnt = 0;
    int free_cnt   = 0;
};

inline task_stat sys_sta;

namespace co_wq {

#if USE_EXCEPTION
#include <exception>
#endif

struct Promise_base {
    using on_completed_t = void (*)(Promise_base&);

    auto initial_suspend() noexcept { return std::suspend_always(); }

    struct FinalAwaiter {
        Promise_base*           self;
        bool                    await_ready() const noexcept { return false; }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<>) const noexcept
        {
            auto previous     = self->mPrevious;
            auto on_completed = self->mOnCompleted;
            if (on_completed) {
                on_completed(*self);
            }
            return previous ? previous : std::noop_coroutine();
        }
        void await_resume() const noexcept { }
    };

    auto final_suspend() noexcept { return FinalAwaiter { this }; }

    void unhandled_exception() noexcept
    {
#if USE_EXCEPTION
        mException = std::current_exception();
#endif
    }
    std::coroutine_handle<> mPrevious {};
    on_completed_t          mOnCompleted { nullptr };
    void*                   mUserData { nullptr }; // for generic control blocks
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

template <typename T>
concept taskalloc = requires(T a, void* ptr, std::size_t size) {
    { a.alloc(size) } -> std::same_as<void*>;
    { a.dealloc(ptr, size) } -> std::same_as<void>;
};

struct sys_taskalloc {

public:
    void* alloc(std::size_t size) { return malloc(size); }
    void  dealloc(void* ptr, std::size_t sz) noexcept
    {
        (void)sz;
        free(ptr);
    }
};

template <class BasePromise, taskalloc Alloc> struct promise_with_alloc : BasePromise {
    static void* operator new(std::size_t sz)
    {
        ++sys_sta.malloc_cnt;
        return Alloc {}.alloc(sz);
    }
    static void operator delete(void* p, std::size_t sz) noexcept
    {
        ++sys_sta.free_cnt;
        Alloc {}.dealloc(p, sz);
    }
    static void operator delete(void* p) noexcept
    {
        ++sys_sta.free_cnt;
        Alloc {}.dealloc(p, 0);
    }
    auto get_return_object() { return std::coroutine_handle<promise_with_alloc>::from_promise(*this); }
};

template <class T = void, class P = Promise<T>, taskalloc Alloc = sys_taskalloc> struct [[nodiscard]] Task {
    using promise_type = promise_with_alloc<P, Alloc>; // wrap original promise with allocator

    Task(std::coroutine_handle<promise_type> coroutine = nullptr) noexcept : mCoroutine(coroutine) { }

    Task(Task&& that) noexcept : mCoroutine(that.mCoroutine) { that.mCoroutine = nullptr; }

    Task& operator=(Task&& that) noexcept
    {
        std::swap(mCoroutine, that.mCoroutine);
        return *this;
    }

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
#if 0
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
#endif
} // namespace co_wq
