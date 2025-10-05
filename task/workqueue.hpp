#pragma once

#include "lock.hpp"
#include "usrlist.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#ifndef CO_WQ_ENABLE_CALLBACK_WQ_TRACE
#define CO_WQ_ENABLE_CALLBACK_WQ_TRACE 0
#endif

#ifndef CO_WQ_ENABLE_CALLBACK_WQ_WARN
#define CO_WQ_ENABLE_CALLBACK_WQ_WARN 1
#endif

#if CO_WQ_ENABLE_CALLBACK_WQ_TRACE
#define CO_WQ_CBQ_TRACE(...) std::fprintf(stderr, __VA_ARGS__)
#else
#define CO_WQ_CBQ_TRACE(...) ((void)0)
#endif

#if CO_WQ_ENABLE_CALLBACK_WQ_WARN
#define CO_WQ_CBQ_WARN(...) std::fprintf(stderr, __VA_ARGS__)
#else
#define CO_WQ_CBQ_WARN(...) ((void)0)
#endif

// 可选的调试 Hook（弱符号，C 链接）：应用可在自身代码中定义以捕获队列中函数指针地址
// 形参为指针大小的整数，可在 32/64 位平台安全使用；若未定义，该符号在 MSVC 下会别名到一个空实现。
namespace co_wq {
struct worknode;
}

#if defined(_MSC_VER) && !defined(__clang__)
extern "C" void wq_debug_check_func_addr(std::uintptr_t addr);
extern "C" void wq_debug_check_func_addr_default(std::uintptr_t addr);
extern "C" void wq_debug_null_func(co_wq::worknode* node);
extern "C" void wq_debug_null_func_default(co_wq::worknode* node);
#else
extern "C" void wq_debug_check_func_addr(std::uintptr_t addr) __attribute__((weak));
extern "C" void wq_debug_null_func(co_wq::worknode* node) __attribute__((weak));
#endif

namespace co_wq {

#define USING_WQ_NAME 0

struct worknode {
    typedef void (*work_func_t)(struct worknode* work);

    struct list_head ws_node;
    work_func_t      func;
};

// Helper: detect common MSVC debug poison patterns for pointers to catch
// uninitialized or freed memory being used as function pointers.
inline bool __wq_is_debug_poison_ptr_uintptr(std::uintptr_t p)
{
#if defined(_MSC_VER)
#if INTPTR_MAX == INT64_MAX
    constexpr std::uintptr_t POISON_CD = 0xcdcdcdcdcdcdcdcdULL; // uninitialized heap
    constexpr std::uintptr_t POISON_CC = 0xccccccccccccccccULL; // uninitialized stack
    constexpr std::uintptr_t POISON_FE = 0xfeeefeeefeeefeeeULL; // freed heap
    constexpr std::uintptr_t POISON_AB = 0xababababababababULL; // no-init heap (some CRTs)
#else
    constexpr std::uintptr_t POISON_CD = 0xcdcdcdcdU;
    constexpr std::uintptr_t POISON_CC = 0xccccccccU;
    constexpr std::uintptr_t POISON_FE = 0xfeeefeeeU;
    constexpr std::uintptr_t POISON_AB = 0xababababU;
#endif
    return p == POISON_CD || p == POISON_CC || p == POISON_FE || p == POISON_AB;
#else
    (void)p;
    return false;
#endif
}

inline bool __wq_is_debug_poison_func_ptr(worknode::work_func_t fn)
{
    return __wq_is_debug_poison_ptr_uintptr(reinterpret_cast<std::uintptr_t>(fn));
}

inline worknode* __wq_node_to_worknode(struct list_head* node)
{
    return reinterpret_cast<worknode*>(reinterpret_cast<char*>(node) - offsetof(worknode, ws_node));
}

inline void __wq_maybe_invoke_debug_hook(std::uintptr_t addr)
{
#if defined(_MSC_VER) && !defined(__clang__)
    if (wq_debug_check_func_addr != nullptr && wq_debug_check_func_addr != wq_debug_check_func_addr_default) {
        wq_debug_check_func_addr(addr);
    }
#else
    if (wq_debug_check_func_addr != nullptr) {
        wq_debug_check_func_addr(addr);
    }
#endif
}

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
            auto fn = pnod->func;
            if (fn) {
#ifndef NDEBUG
                // Detect and guard against calling invalid debug poison addresses like 0xCDCDCDCD...
                if (__wq_is_debug_poison_func_ptr(fn)) {
                    assert(("worknode.func is invalid (debug poison pattern like 0xCDCD...), likely uninitialized",
                            false));
                    return 1; // skip calling to avoid crash in debug
                }
#endif
                fn(pnod);
            } else {
                CO_WQ_CBQ_WARN("[workqueue] warning: null func for node %p\n", static_cast<void*>(pnod));
            }
            return 1;
        }
        return 0;
    }
    void post(struct worknode& pnode)
    {
#ifndef NDEBUG
        // Validate func before enqueue
        auto fn = pnode.func;
        // 调试 hook：由应用侧（例如 app/main.cpp）可选实现
        __wq_maybe_invoke_debug_hook(reinterpret_cast<std::uintptr_t>(fn));
        if (fn == nullptr) {
#if defined(_MSC_VER) && !defined(__clang__)
            if (wq_debug_null_func != nullptr && wq_debug_null_func != wq_debug_null_func_default) {
                wq_debug_null_func(&pnode);
            }
#else
            if (wq_debug_null_func != nullptr) {
                wq_debug_null_func(&pnode);
            }
#endif
        }
        assert(fn != nullptr && "worknode.func must not be null when enqueuing");
        assert(!__wq_is_debug_poison_func_ptr(fn)
               && "worknode.func is invalid (debug poison pattern like 0xCDCD...), likely uninitialized");
#else
        __wq_maybe_invoke_debug_hook(reinterpret_cast<std::uintptr_t>(pnode.func));
#endif

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
#ifndef NDEBUG
        // Validate all func pointers in batch before enqueue
        for (list_head* pos = batch_head.next; pos != &batch_head; pos = pos->next) {
            worknode* wn = __wq_node_to_worknode(pos);
            auto      fn = wn->func;
            __wq_maybe_invoke_debug_hook(reinterpret_cast<std::uintptr_t>(fn));
            if (fn == nullptr) {
#if defined(_MSC_VER) && !defined(__clang__)
                if (wq_debug_null_func != nullptr && wq_debug_null_func != wq_debug_null_func_default) {
                    wq_debug_null_func(wn);
                }
#else
                if (wq_debug_null_func != nullptr) {
                    wq_debug_null_func(wn);
                }
#endif
            }
            assert(fn != nullptr && "worknode.func must not be null when enqueuing (batch)");
            assert(!__wq_is_debug_poison_func_ptr(fn)
                   && "worknode.func is invalid (debug poison pattern like 0xCDCD...), likely uninitialized (batch)");
        }
#else
        for (list_head* pos = batch_head.next; pos != &batch_head; pos = pos->next) {
            worknode* wn = __wq_node_to_worknode(pos);
            __wq_maybe_invoke_debug_hook(reinterpret_cast<std::uintptr_t>(wn->func));
        }
#endif
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
#ifndef NDEBUG
        auto fn = pnode.func;
        __wq_maybe_invoke_debug_hook(reinterpret_cast<std::uintptr_t>(fn));
        if (fn == nullptr) {
#if defined(_MSC_VER) && !defined(__clang__)
            if (wq_debug_null_func != nullptr && wq_debug_null_func != wq_debug_null_func_default) {
                wq_debug_null_func(&pnode);
            }
#else
            if (wq_debug_null_func != nullptr) {
                wq_debug_null_func(&pnode);
            }
#endif
        }
        assert(fn != nullptr && "worknode.func must not be null when enqueuing (nolock)");
        assert(!__wq_is_debug_poison_func_ptr(fn)
               && "worknode.func is invalid (debug poison pattern like 0xCDCD...), likely uninitialized (nolock)");
#else
        __wq_maybe_invoke_debug_hook(reinterpret_cast<std::uintptr_t>(pnode.func));
#endif
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
