#pragma once
#include "lock.hpp"
#include "timer.hpp"
#include "workqueue.hpp"
#include <atomic>
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
    // 标记该请求是否已被取消（例如超时），供事件处理在持锁下跳过并清理
    std::atomic_bool cancelled { false };
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

            // 若请求已取消（如超时），在持锁下移除但不消耗令牌
            if (req->cancelled.load(std::memory_order_acquire)) {
                list_del(&req->ws_node);
                continue;
            }

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

    // 取消某个挂起的 acquire 请求，如果其仍在等待队列中
    bool cancel_waiter(Sem_req& sem_req)
    {
        bool removed = false;
        _executor.lock();
        sem_req.cancelled.store(true, std::memory_order_release);
        if (!list_empty(&acquire_list)) {
            worknode* pos;
            worknode* n;
            list_for_each_entry_safe (pos, n, &acquire_list, ws_node, worknode) {
                if (pos == &sem_req) {
                    list_del(&sem_req.ws_node);
                    removed = true;
                    break;
                }
            }
        }
        _executor.unlock();
        return removed;
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

// 带超时的信号量等待 awaiter，await_resume 返回 bool（true=获取到，false=超时/失败）
template <lockable lock> struct SemReqTimeoutAwaiter : Sem_req {
    struct TimeoutNode : Timer_worknode<lock> {
        // 放在 TimeoutNode 内部，回调在失败路径不触碰 awaiter，避免 UAF
        std::atomic_bool        armed { false };
        std::coroutine_handle<> handle {};
        Semaphore<lock>*        sem { nullptr };
        Sem_req*                req { nullptr }; // 指向本 awaiter（仅在赢得竞态时访问）

        static void on_timer(worknode* p)
        {
            auto* tn = static_cast<TimeoutNode*>(p);
            // 尝试赢得恢复权
            if (!tn->armed.exchange(false, std::memory_order_acq_rel)) {
                return; // 已被事件路径夺走或已处理
            }
            if (tn->sem && tn->req) {
                // 在持锁逻辑下移除等待者并标记取消，避免令牌丢失
                tn->sem->cancel_waiter(*tn->req);
                tn->req->req_sta = Sem_req::REQ_TIME_OUT;
            }
            if (tn->handle) {
                tn->handle.resume();
            }
        }
    };

    explicit SemReqTimeoutAwaiter(Semaphore<lock>& sem, Timer_check_queue<lock>& tq, uint32_t ms)
        : mSemaphore(sem), mTimerQ(tq), mTimeoutMs(ms)
    {
        // 初始化定时器节点
        mTimeout.func = &TimeoutNode::on_timer;
    }

    ~SemReqTimeoutAwaiter()
    {
        // 析构时取消定时器（若仍挂起）
        mTimerQ.cancel(&mTimeout);
    }

    std::coroutine_handle<>  mCoroutine;
    Semaphore<lock>&         mSemaphore;
    Timer_check_queue<lock>& mTimerQ;
    uint32_t                 mTimeoutMs { 0 };
    TimeoutNode              mTimeout {};
    // mArmed 迁移至 TimeoutNode.armed，避免回调在失败路径访问 awaiter
    mutable bool mImmediateAcquired { false };

    bool await_ready() const noexcept
    {
        // 尝试立即获取
        if (const_cast<Semaphore<lock>&>(mSemaphore).try_acquire()) {
            mImmediateAcquired = true;
            return true; // 不挂起
        }
        return false;
    }

    void await_suspend(std::coroutine_handle<> coroutine) noexcept
    {
        mCoroutine      = coroutine;
        mTimeout.handle = coroutine;
        mTimeout.sem    = &mSemaphore;
        mTimeout.req    = this;
        mTimeout.armed.store(true, std::memory_order_release);

        // Ensure worknode links are in a clean initial state before enqueueing
        INIT_LIST_HEAD(&this->ws_node);
        INIT_LIST_HEAD(&mTimeout.ws_node);
        mTimeout.func = &TimeoutNode::on_timer;

        // 当信号量可用时被 post，检查是否仍有效；有效则取消定时器并恢复协程
        func = [](struct worknode* pws) {
            auto* self = static_cast<SemReqTimeoutAwaiter*>(pws);
            if (!self->mTimeout.armed.exchange(false, std::memory_order_acq_rel)) {
                // 已被超时等路径处理，忽略
                return;
            }
            // 正常获取成功（req_sta 已由 Semaphore 设置为 REQ_OK；
            // 若是销毁触发，将为 REQ_FAIL，这里不覆盖）
            // 取消定时器
            self->mTimerQ.cancel(&self->mTimeout);
            if (self->mTimeout.handle) {
                self->mTimeout.handle.resume();
            }
        };

        // 安排超时（若为无限超时，则不注册定时器）
        if (mTimeoutMs != 0xffffffffu) {
            mTimerQ.post_delayed_work(&mTimeout, mTimeoutMs);
        }
        // 入队等待
        mSemaphore.acquire(*this);
    }

    bool await_resume() const noexcept
    {
        if (mImmediateAcquired)
            return true;
        return req_sta == Sem_req::REQ_OK;
    }
};

template <lockable lock>
inline auto wait_sem_for(Semaphore<lock>& sem, Timer_check_queue<lock>& tq, uint32_t timeout_ms)
{
    return SemReqTimeoutAwaiter<lock>(sem, tq, timeout_ms);
}

// 立即返回（零超时）：不挂起，直接尝试获取信号量，返回是否成功
template <lockable lock> struct SemTryAwaiter {
    explicit SemTryAwaiter(Semaphore<lock>& s) : sem(s) { }
    Semaphore<lock>& sem;
    bool             await_ready() const noexcept { return true; }
    void             await_suspend(std::coroutine_handle<>) const noexcept { }
    bool             await_resume() const noexcept { return const_cast<Semaphore<lock>&>(sem).try_acquire(); }
};

template <lockable lock> inline auto wait_sem_try(Semaphore<lock>& sem)
{
    return SemTryAwaiter<lock>(sem);
}

// 无限等待（友好封装）：基于 wait_sem_for，传入 0xffffffff 跳过定时器注册，返回 bool（成功后为 true）
template <lockable lock> inline auto wait_sem_forever(Semaphore<lock>& sem, Timer_check_queue<lock>& tq)
{
    return wait_sem_for(sem, tq, 0xffffffffu);
}

}
