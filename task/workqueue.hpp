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

template <lockable lock> struct workqueue {
    explicit workqueue() { }
    typedef void (*wq_trig)(struct workqueue* work);

    struct list_head ws_head;
    wq_trig          trig;
    lock             lk;
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

private:
    struct worknode* get_work_node(struct workqueue& wq)
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
