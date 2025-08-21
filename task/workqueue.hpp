#pragma once

#include "lock.h"
#include "usrlist.hpp"

namespace co_wq {

#define USING_WQ_NAME 0

struct worknode {
    typedef void (*work_func_t)(struct worknode* work);

    struct list_head ws_node;
    work_func_t      func;
    SpinLock         lk;
};

struct workqueue {
    typedef void (*wq_trig)(struct workqueue* work);

    struct list_head ws_head;
    wq_trig          trig;
    SpinLock         lk;
#if USING_WQ_NAME
    char names[16];
#endif

    int  work_once();
    void post(struct worknode& pnode);
    void add_new_nolock(struct worknode& pnode);
    void trig_once();
};

}
