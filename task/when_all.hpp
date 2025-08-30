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

template <AllVoidAwaitable... Ts> Task<void> when_all(Ts&... ts)
{
    static_assert(sizeof...(Ts) > 0, "when_all requires at least one task");
    WhenAllCtlBlock control { sizeof...(Ts) };
    struct Awaiter {
        WhenAllCtlBlock&   ctl;
        std::tuple<Ts&...> tasks; // references to child tasks
        static void        on_completed(Promise_base& base) noexcept
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
            return std::noop_coroutine();
        }
        void await_resume() const noexcept { }
    } awaiter { control, std::tuple<Ts&...>(ts...) };
    co_await awaiter;
    co_return;
}

} // namespace co_wq
