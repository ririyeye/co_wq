#pragma once
#include "lock.hpp"
#include "workqueue.hpp"
#include <coroutine>

namespace co_wq {

// 为了禁止直接实例化 Sem_req（只能通过 SemReqAwaiter 使用）：
// 1. 向前声明 SemReqAwaiter 模板。
// 2. 把 Sem_req 的构造/析构函数放到 protected，并声明 SemReqAwaiter 为友元。
// 这样 "Sem_req x;" 会编译失败；但 SemReqAwaiter 继承它仍可构造。
template <lockable lock> struct SemReqAwaiter; // forward declaration

struct Sem_req : worknode {
protected:
    Sem_req()                          = default;
    ~Sem_req()                         = default;
    Sem_req(const Sem_req&)            = delete;
    Sem_req& operator=(const Sem_req&) = delete;

    template <lockable lock> friend struct SemReqAwaiter; // 允许 awaiter 访问构造

public:
    enum req_sta {
        REQ_OK = 0,
        REQ_FAIL,
        REQ_TIME_OUT,
    } req_sta;
};

template <lockable lock> struct Semaphore : worknode {
private:
    workqueue<lock>& _executor;
    int              _cur_val;
    int              _max_val;
    list_head        acquire_list;

    void sem_chk_cb()
    {
        if (list_empty(&acquire_list)) {
            return; // nothing to do
        }

        worknode* pos;
        worknode* n;
        int       trig_flg = 0;

        _executor.lock();
        list_for_each_entry_safe (pos, n, &acquire_list, ws_node, worknode) {
            Sem_req* req = static_cast<Sem_req*>(pos);

            if (_cur_val > 0) {
                _cur_val--;
                list_del(&req->ws_node);
                req->req_sta = Sem_req::REQ_OK;
                _executor.add_new_nolock(*req);
                trig_flg = 1;
            }
        }
        _executor.unlock();

        if (trig_flg) {
            _executor.trig_once();
        }
    }

public:
    // Parameter name adjusted to avoid potential name hiding warnings (C4459)
    explicit Semaphore(workqueue<lock>& exec, int init_val, int max_val)
        : _executor(exec), _cur_val(init_val), _max_val(max_val)
    {
        INIT_LIST_HEAD(&ws_node);
        INIT_LIST_HEAD(&acquire_list);
        func = [](struct worknode* work) {
            Semaphore* tcq = static_cast<Semaphore*>(work);
            tcq->sem_chk_cb();
        };
    }

    ~Semaphore()
    {
        // Wake (fail) all pending acquire requests so their awaiting coroutines resume
        // preventing dangling awaiters referencing a destroyed semaphore.
        if (!list_empty(&acquire_list)) {
            worknode* pos;
            worknode* n;
            int       trig_flg = 0;

            _executor.lock();
            list_for_each_entry_safe (pos, n, &acquire_list, ws_node, worknode) {
                Sem_req* req = static_cast<Sem_req*>(pos);
                list_del(&req->ws_node);
                // mark as failed due to semaphore destruction
                req->req_sta = Sem_req::REQ_FAIL;
                _executor.add_new_nolock(*req);
                trig_flg = 1;
            }
            _executor.unlock();

            if (trig_flg) {
                _executor.trig_once();
            }
        }
    }

    void acquire(Sem_req& sem_req)
    {
        _executor.lock();
        if (_cur_val > 0) {
            _cur_val--;
            _executor.add_new_nolock(sem_req);
        } else {
            list_add_tail(&sem_req.ws_node, &acquire_list);
        }
        _executor.unlock();
    }

    bool try_acquire()
    {
        int succ = 0;

        _executor.lock();
        if (_cur_val > 0) {
            _cur_val--;
            succ = 1;
        }
        _executor.unlock();

        return succ;
    }

    void release()
    {
        int post_flg = 0;

        _executor.lock();
        if (_cur_val < _max_val) {
            _cur_val++;
            if (!list_empty(&acquire_list)) {
                post_flg = 1;
            }
        }
        _executor.unlock();

        if (post_flg) {
            _executor.post(*this);
        }
    }
};

template <lockable lock> struct SemReqAwaiter : Sem_req {

    explicit SemReqAwaiter(Semaphore<lock>& sem) : mSemaphore(sem) { }

    std::coroutine_handle<> mCoroutine;
    Semaphore<lock>&        mSemaphore;

    bool await_ready() const noexcept
    {
        if (mSemaphore.try_acquire()) {
            return true; // already acquired
        }
        return false;
    }

    void await_suspend(std::coroutine_handle<> coroutine) noexcept
    {
        mCoroutine = coroutine;
        func       = [](struct worknode* pws) {
            SemReqAwaiter* self = static_cast<SemReqAwaiter*>(pws);

            if (self->mCoroutine) {
                self->mCoroutine.resume();
            }
        };

        mSemaphore.acquire(*this);
    };
    void await_resume() const noexcept { }
};

template <lockable lock> inline auto wait_sem(Semaphore<lock>& sem)
{
    return SemReqAwaiter(sem);
}

}
