#pragma once
#include "lock.hpp"
#include "workqueue.hpp"
#include <coroutine>

namespace co_wq {

struct notify_req_base {
    list_head notify_node;
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
}
