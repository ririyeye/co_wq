#pragma once
#include "concepts.hpp"
#include "task.hpp"
#include <coroutine>
#include <tuple>

namespace co_wq {

struct WhenAllCtlBlock {
    std::size_t             count;
    std::coroutine_handle<> previous {};
};

template <class... Ts>
concept AllVoidAwaitable = (... && Awaitable<Ts>) && (... && std::is_void_v<typename AwaitableTraits<Ts>::RetType>);

// 直接可 co_await 的聚合等待器：不再构造一个额外的 when_all 协程帧，减少一次分配。
// 用法： co_await when_all_awaitable(t1, t2, ...);
// 与返回 Task 的 when_all 区别：本对象只在调用方协程栈上生成（通常是编译器优化成若干寄存器/局部变量），
// 不需要动态分配协程帧，适合短生命周期、只需等待全部完成的场景。
template <AllVoidAwaitable... Ts> struct WhenAllAwaiter {
    static_assert(sizeof...(Ts) > 0, "when_all_awaitable requires at least one task");
    WhenAllCtlBlock    ctl { sizeof...(Ts), {} };
    std::tuple<Ts&...> tasks; // 引用子任务

    explicit WhenAllAwaiter(Ts&... ts) : tasks(ts...) { }

    static void on_completed(Promise_base& base) noexcept
    {
        auto* c = static_cast<WhenAllCtlBlock*>(base.mUserData);
        if (--c->count == 0) {
            if (c->previous)
                c->previous.resume();
        }
    }

    bool                    await_ready() const noexcept { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept
    {
        ctl.previous = h;
        std::apply(
            [this](auto&... t) {
                (([this](auto& child) {
                     auto hchild = child.get();
                     if (!hchild)
                         return;
                     auto& p        = hchild.promise();
                     p.mPrevious    = std::noop_coroutine();
                     p.mUserData    = &ctl;
                     p.mOnCompleted = &on_completed;
                     hchild.resume();
                 })(t),
                 ...);
            },
            tasks);
        return std::noop_coroutine(); // 不挂到其它，立即返回，等待子协程完成后回跳
    }
    void await_resume() const noexcept { }
};

template <AllVoidAwaitable... Ts> WhenAllAwaiter<Ts...> when_all_awaitable(Ts&... ts)
{
    return WhenAllAwaiter<Ts...>(ts...);
}

// 增加 Alloc 模板参数，使 when_all 创建的协程帧也可使用自定义分配器（统计内存或替换分配策略）。
template <taskalloc Alloc = sys_taskalloc, AllVoidAwaitable... Ts> Task<void, Promise<void>, Alloc> when_all(Ts&... ts)
{
    // 复用零帧 Awaiter，保持语义一致，同时允许选择使用 Task 包裹（需要分配器/链式操作时）。
    co_await when_all_awaitable(ts...);
    co_return;
}

} // namespace co_wq
