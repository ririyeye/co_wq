#include "task.hpp"
#include "workqueue.hpp"
#include <coroutine>

#pragma once
namespace co_wq {

template <lockable lock> struct Work_promise_base : worknode {
    virtual ~Work_promise_base() = default;

    workqueue<lock>* _excutor = nullptr;

    void post(workqueue<lock>* wq)
    {
        _excutor = wq;
        func     = wk_cb;
        if (wq) {
            wq->post(*this);
        }
    }

    void post(void) { post(this->_excutor); }

    static void wk_cb(struct worknode* work)
    {
        auto* promise = static_cast<Work_promise_base*>(work);
        auto  coro    = std::coroutine_handle<Work_promise_base>::from_promise(*promise);

        if (!coro.done()) {
            coro.resume();
        }

        if (coro.done()) {
            coro.destroy();
        }
    }

    Work_promise_base& operator=(Work_promise_base&&) = delete;
};

template <lockable lock, class T = void> struct Work_Promise : Promise<T>, Work_promise_base<lock> {
    auto get_return_object() { return std::coroutine_handle<Work_Promise>::from_promise(*this); }

    void return_value(T&& ret)
    {
        Work_promise_base<lock>::post();
        Promise<T>::return_value(std::move(ret));
    }

    void return_value(T const& ret)
    {
        Work_promise_base<lock>::post();
        Promise<T>::return_value(ret);
    }
};

template <lockable lock> struct Work_Promise<void, lock> : Promise<void>, Work_promise_base<lock> {
    auto get_return_object() { return std::coroutine_handle<Work_Promise>::from_promise(*this); }

    void return_void() noexcept { Work_promise_base<lock>::post(); }
};

template <lockable lock, class T> static inline void post_to(Task<T, Work_Promise<T>>& tk, workqueue<lock>& executor)
{
    auto& promise = tk.mCoroutine.promise();
    INIT_LIST_HEAD(&promise.ws_node);
    tk.detach(); // 放弃所有权，防止局部变量销毁时析构Task
    promise.post(&executor);
}
}
