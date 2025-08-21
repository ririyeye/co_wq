#include "workqueue.hpp"
#include "lock.h"
namespace co_wq {

void workqueue::post(struct worknode& pnode)
{
    lk.lock();
    list_del(&pnode.ws_node);
    list_add_tail(&pnode.ws_node, &ws_head);
    lk.unlock();
    if (trig) {
        trig(this);
    }
}

void workqueue::add_new_nolock(struct worknode& pnode)
{
    list_del(&pnode.ws_node);
    list_add_tail(&pnode.ws_node, &ws_head);
}

void workqueue::trig_once()
{
    if (trig) {
        trig(this);
    }
}

static struct worknode* get_work_node(struct workqueue& wq)
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

int workqueue::work_once()
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
}
