#pragma once

#include "lock.hpp"
#include "usrlist.hpp"

namespace co_wq {

#define USING_WQ_NAME 0

struct worknode {
    typedef void (*work_func_t)(struct worknode* work);

    struct list_head ws_node;
    work_func_t      func;
};

template <lockable Lock> struct workqueue {
    explicit workqueue() { }
    typedef void (*wq_trig)(struct workqueue* work);

    struct list_head ws_head;
    wq_trig          trig;
    Lock             lk;
#if USING_WQ_NAME
    char names[16];
#endif

    int work_once()
    {
        lk.lock();
        struct worknode* pnod = get_work_node(*this);
        lk.unlock();

        if (pnod) {
            if (pnod->func) {
                pnod->func(pnod);
            }
            return 1;
        }
        return 0;
    }
    void post(struct worknode& pnode)
    {
        lk.lock();
        list_del(&pnode.ws_node);
        list_add_tail(&pnode.ws_node, &ws_head);
        lk.unlock();
        if (trig) {
            trig(this);
        }
    }
    // 批量投递：batch_head 为一个临时链表头，里面挂着若干 worknode.ws_node。
    // 所有节点整体串到队列尾部，只触发一次 trig。
    void post(struct list_head& batch_head)
    {
        if (list_empty(&batch_head))
            return; // nothing to do
        lk.lock();
        // splice tail: insert [first..last] before ws_head
        list_head* first = batch_head.next;
        list_head* last  = batch_head.prev;
        // 连接到现有队列尾部
        first->prev        = ws_head.prev;
        ws_head.prev->next = first;
        last->next         = &ws_head;
        ws_head.prev       = last;
        // 重新初始化 batch 头（清空）
        INIT_LIST_HEAD(&batch_head);
        lk.unlock();
        if (trig) {
            trig(this);
        }
    }
    void add_new_nolock(struct worknode& pnode)
    {
        list_del(&pnode.ws_node);
        list_add_tail(&pnode.ws_node, &ws_head);
    }
    void trig_once()
    {
        if (trig) {
            trig(this);
        }
    }

    // Expose the underlying lock so users can directly call workqueue.lock()/unlock().
    // This simply forwards to the contained lock instance `lk`.
    void lock() { lk.lock(); }
    void unlock() { lk.unlock(); }

protected:
    virtual struct worknode* get_work_node(struct workqueue& wq)
    {
        struct worknode* pnod = NULL;
        if (list_empty(&wq.ws_head)) {
            pnod = NULL;
        } else {
            pnod = list_first_entry(&wq.ws_head, worknode, ws_node);
            list_del(&pnod->ws_node);
        }
        return pnod;
    }
};
}
