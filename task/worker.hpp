#include "task.hpp"
#include "workqueue.hpp"
#include <coroutine>

#pragma once
namespace co_wq {

template <lockable lock> struct Work_promise_base : worknode {
    virtual ~Work_promise_base() = default;

    workqueue<lock>*             _excutor             = nullptr;
    bool                         _running_on_executor = false;
    bool                         _completion_posted   = false;
    Promise_base::on_completed_t _user_on_completed   = nullptr;

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

        promise->_running_on_executor = true;
        if (!coro.done()) {
            coro.resume();
        }
        promise->_running_on_executor = false;

        if (coro.done()) {
            promise->_completion_posted = false;
            promise->_user_on_completed = nullptr;
            coro.destroy();
        }
    }

    Work_promise_base& operator=(Work_promise_base&&) = delete;
};

template <lockable lock, class T = void> struct Work_Promise : Promise<T>, Work_promise_base<lock> {
    auto get_return_object() { return std::coroutine_handle<Work_Promise>::from_promise(*this); }

    void return_value(T&& ret) { Promise<T>::return_value(std::move(ret)); }

    void return_value(T const& ret) { Promise<T>::return_value(ret); }

    static void destroy_on_executor(worknode* work)
    {
        auto* self = static_cast<Work_Promise*>(work);
        auto  coro = std::coroutine_handle<Work_Promise>::from_promise(*self);
        coro.destroy();
    }

    static void on_completed_adapter(Promise_base& pb)
    {
        auto& self = static_cast<Work_Promise&>(pb);
        if (self._user_on_completed) {
            self._user_on_completed(pb);
        }
        if (!self._completion_posted && self._excutor && !self._running_on_executor) {
            self._completion_posted = true;
            self.func               = &destroy_on_executor;
            self.post(self._excutor);
        }
    }

    static void prepare_for_post(Work_Promise& promise)
    {
        if (promise.mOnCompleted != &on_completed_adapter) {
            promise._user_on_completed = promise.mOnCompleted;
            promise.mOnCompleted       = &on_completed_adapter;
        }
        promise._completion_posted = false;
    }
};

template <lockable lock> struct Work_Promise<lock, void> : Promise<void>, Work_promise_base<lock> {
    auto get_return_object() { return std::coroutine_handle<Work_Promise>::from_promise(*this); }

    void return_void() noexcept { }

    static void destroy_on_executor(worknode* work)
    {
        auto* self = static_cast<Work_Promise*>(work);
        auto  coro = std::coroutine_handle<Work_Promise>::from_promise(*self);
        coro.destroy();
    }

    static void on_completed_adapter(Promise_base& pb)
    {
        auto& self = static_cast<Work_Promise&>(pb);
        if (self._user_on_completed) {
            self._user_on_completed(pb);
        }
        if (!self._completion_posted && self._excutor && !self._running_on_executor) {
            self._completion_posted = true;
            self.func               = &destroy_on_executor;
            self.post(self._excutor);
        }
    }

    static void prepare_for_post(Work_Promise& promise)
    {
        if (promise.mOnCompleted != &on_completed_adapter) {
            promise._user_on_completed = promise.mOnCompleted;
            promise.mOnCompleted       = &on_completed_adapter;
        }
        promise._completion_posted = false;
    }
};

template <lockable lock, class T, class Alloc>
static inline void post_to(Task<T, Work_Promise<lock, T>, Alloc>& tk, workqueue<lock>& executor)
{
    auto& promise = tk.mCoroutine.promise();
    INIT_LIST_HEAD(&promise.ws_node);
    Work_Promise<lock, T>::prepare_for_post(promise);
    tk.detach(); // 放弃所有权，防止局部变量销毁时析构Task
    promise.post(&executor);
}
}
