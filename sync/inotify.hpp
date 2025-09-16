#pragma once
#include "lock.hpp"
#include "timer.hpp"
#include "workqueue.hpp"
#include <atomic>
#include <coroutine>

namespace co_wq {

struct notify_req_base {
    list_head notify_node;
    // 取消标记（如超时），事件侧在持锁下跳过并清理
    std::atomic_bool cancelled { false };
};

struct notify_req : worknode, notify_req_base { };

template <lockable lock> struct Notify : worknode {
private:
    workqueue<lock>& _executor;
    list_head        acquire_list;
    std::atomic_int  mNotify_one = 0;
    std::atomic_int  mNotify_all = 0;

    // -1表示不限
    int inotify_num(int req_cnt)
    {
        notify_req_base* pos;
        notify_req_base* n;
        int              ret = 0;
        _executor.lock();
        int curcnt = req_cnt;
        list_for_each_entry_safe (pos, n, &acquire_list, notify_node, notify_req_base) {
            // 已取消的请求仅跳过，不占用通知名额；由协程恢复后的析构统一移除
            if (pos->cancelled.load(std::memory_order_acquire)) {
                continue;
            }
            if (req_cnt > 0) {
                if (curcnt <= 0) {
                    break; // reach the limit
                }
                curcnt--;
            }

            notify_req* req = static_cast<notify_req*>(pos);
            _executor.post(*req);
            ret++;
        }
        _executor.unlock();

        return ret;
    }

    void inotify_cb()
    {
        if (list_empty(&acquire_list)) {
            return; // nothing to do
        }

        int cnt = 0;
        if (mNotify_all > 0) {

            cnt = inotify_num(-1);

            mNotify_all = 0;
            mNotify_one = 0;
        } else {
            cnt         = inotify_num(mNotify_one);
            mNotify_one = 0;
        }

        if (cnt > 0) {
            _executor.trig_once();
        }
    }

public:
    // Parameter name adjusted to avoid potential name hiding warnings (C4459)
    explicit Notify(workqueue<lock>& exec) : _executor(exec)
    {
        INIT_LIST_HEAD(&ws_node);
        INIT_LIST_HEAD(&acquire_list);
        func = [](struct worknode* work) {
            Notify* tcq = static_cast<Notify*>(work);
            tcq->inotify_cb();
        };
    }

    void notify_all()
    {
        mNotify_all++;
        _executor.post(*this);
    }

    void notify_one()
    {
        mNotify_one++;
        _executor.post(*this);
    }

    void regist(notify_req& req)
    {
        _executor.lock();
        list_add_tail(&req.notify_node, &acquire_list);
        _executor.unlock();
    }

    void unregist(notify_req& req)
    {
        _executor.lock();
        list_del(&req.notify_node);
        _executor.unlock();
    }
};

template <lockable lock> struct NotifyReqAwaiter : notify_req {

    explicit NotifyReqAwaiter(Notify<lock>& inotify) : mInotify(inotify) { }

    ~NotifyReqAwaiter() { mInotify.unregist(*this); }

    std::coroutine_handle<> mCoroutine;
    Notify<lock>&           mInotify;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> coroutine) noexcept
    {
        mCoroutine = coroutine;
        INIT_LIST_HEAD(&ws_node);
        func = [](struct worknode* pws) {
            NotifyReqAwaiter* self = static_cast<NotifyReqAwaiter*>(pws);

            if (self->mCoroutine) {
                self->mCoroutine.resume();
            }
        };

        mInotify.regist(*this);
    };
    void await_resume() const noexcept { }
};

template <lockable lock>

inline auto wait_inotify(Notify<lock>& sem)
{
    return NotifyReqAwaiter(sem);
}

// 带超时的 inotify 等待，await_resume 返回 bool（true=收到通知，false=超时）
template <lockable lock> struct NotifyReqTimeoutAwaiter : notify_req {
    struct TimeoutNode : Timer_worknode<lock> {
        std::atomic_bool        armed { false };
        std::coroutine_handle<> handle {};
        Notify<lock>*           ino { nullptr };
        notify_req_base*        req { nullptr };

        static void on_timer(worknode* p)
        {
            auto* tn = static_cast<TimeoutNode*>(p);
            if (!tn->armed.exchange(false, std::memory_order_acq_rel)) {
                return;
            }
            if (tn->ino && tn->req) {
                tn->req->cancelled.store(true, std::memory_order_release);
                // 不在定时器回调中移除，避免与事件侧/析构重复删除
            }
            if (tn->handle) {
                tn->handle.resume();
            }
        }
    };

    explicit NotifyReqTimeoutAwaiter(Notify<lock>& ino, Timer_check_queue<lock>& tq, uint32_t ms)
        : mInotify(ino), mTimerQ(tq), mTimeoutMs(ms)
    {
        mTimeout.func = &TimeoutNode::on_timer;
    }

    ~NotifyReqTimeoutAwaiter()
    {
        // 避免悬挂定时器
        mTimerQ.cancel(&mTimeout);
        // 若仍在通知队列上，则移除
        if (mRegistered) {
            mInotify.unregist(*this);
            mRegistered = false;
        }
    }

    std::coroutine_handle<>  mCoroutine;
    Notify<lock>&            mInotify;
    Timer_check_queue<lock>& mTimerQ;
    uint32_t                 mTimeoutMs { 0 };
    TimeoutNode              mTimeout {};
    bool                     mTimedOut { false };
    bool                     mRegistered { false };

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> coroutine) noexcept
    {
        mCoroutine      = coroutine;
        mTimeout.handle = coroutine;
        mTimeout.ino    = &mInotify;
        mTimeout.req    = this;
        mTimeout.armed.store(true, std::memory_order_release);
        mTimedOut = false;
        INIT_LIST_HEAD(&ws_node);
        func = [](struct worknode* pws) {
            auto* self = static_cast<NotifyReqTimeoutAwaiter*>(pws);
            if (!self->mTimeout.armed.exchange(false, std::memory_order_acq_rel)) {
                return; // 已被超时处理
            }
            // 收到通知
            self->mTimerQ.cancel(&self->mTimeout);
            if (self->mCoroutine) {
                self->mCoroutine.resume();
            }
        };
        // 安排超时（若为无限超时则不注册定时器），并注册
        if (mTimeoutMs != 0xffffffffu) {
            mTimerQ.post_delayed_work(&mTimeout, mTimeoutMs);
        }
        mInotify.regist(*this);
        mRegistered = true;
    }

    bool await_resume() const noexcept { return !cancelled.load(std::memory_order_acquire); }
};

template <lockable lock>
inline auto wait_inotify_for(Notify<lock>& ino, Timer_check_queue<lock>& tq, uint32_t timeout_ms)
{
    return NotifyReqTimeoutAwaiter<lock>(ino, tq, timeout_ms);
}

// 立即返回（零超时尝试）：不挂起，直接返回 false（因为没有同步队列拉取即时通知的通道）
// 如需要“已缓存事件”语义，可在 Notify 中扩展计数；此处保持无状态，等到协程真正挂起再等通知。
template <lockable lock> struct NotifyTryAwaiter {
    bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) const noexcept { }
    bool await_resume() const noexcept { return false; }
};

template <lockable lock> inline auto wait_inotify_try(Notify<lock>& /*ino*/)
{
    return NotifyTryAwaiter<lock>();
}

// 无限等待（友好封装）：基于 wait_inotify_for，传入 0xffffffff 跳过定时器注册，返回 bool（收到通知为 true）
template <lockable lock> inline auto wait_inotify_forever(Notify<lock>& ino, Timer_check_queue<lock>& tq)
{
    return wait_inotify_for(ino, tq, 0xffffffffu);
}
}
