#pragma once
#include "lock.hpp"
#include "workqueue.hpp"
#include <chrono>
#include <coroutine>

namespace co_wq {

template <lockable lock> struct Timer_check_queue; // forward

template <lockable lock> struct Timer_worknode : worknode {
    std::chrono::steady_clock::time_point expire;
    Timer_worknode*                       child       = nullptr;
    Timer_worknode*                       sibling     = nullptr;
    Timer_worknode*                       parent      = nullptr; // for removal
    Timer_check_queue<lock>*              owner_queue = nullptr; // set when enqueued

    ~Timer_worknode();
};

template <lockable lock> struct Timer_check_queue : worknode {
private:
    workqueue<lock>&      _executor;
    Timer_worknode<lock>* heap_root;

    /**
     * @brief 合并两个最小堆（pairing heap）的根节点，返回新的根节点
     *
     * 算法说明：
     * 1. 如果任一堆为空，则直接返回另一个堆根。
     * 2. 比较 a->expire 和 b->expire，较小（更早到期）的节点作为新根。
     * 3. 将另一节点作为子堆插入到新根的 child 链表首位：
     *    - 当 a 是新根时，b->sibling 指向 a->child，a->child 更新为 b。
     *    - 当 b 是新根时，a->sibling 指向 b->child，b->child 更新为 a。
     * 4. 返回合并后的新根，保持 pairing heap 的结构。
     */
    static Timer_worknode<lock>* heap_meld(Timer_worknode<lock>* a, Timer_worknode<lock>* b) noexcept
    {
        if (!a)
            return b;
        if (!b)
            return a;
        if (a->expire <= b->expire) {
            b->sibling = a->child;
            a->child   = b;
            b->parent  = a;
            // siblings already have correct parent (a)
            return a;
        } else {
            a->sibling = b->child;
            b->child   = a;
            a->parent  = b;
            return b;
        }
    }

    /**
     * @brief 对 pairing heap 的子节点链表执行两遍合并（two-pass merge），返回新的子堆根
     *
     * 算法说明：
     * 1. 如果节点为空或没有 sibling，则该链表只有一个子堆，直接返回该节点。
     * 2. 将链表分为三部分：a（第一个节点）、b（第二个节点）、rest（从第三个节点开始的链表）。
     * 3. 将 a->sibling 和 b->sibling 清空，使它们成为独立的单节点堆。
     * 4. 先用 heap_meld 合并 a 和 b，得到中间堆 root1；
     * 5. 递归对 rest 调用 two_pass_merge，得到中间堆 root2；
     * 6. 最后用 heap_meld 合并 root1 和 root2，并返回最终堆根。
     */
    static Timer_worknode<lock>* two_pass_merge(Timer_worknode<lock>* node) noexcept
    {
        if (!node || !node->sibling)
            return node;
        Timer_worknode<lock>* a    = node;
        Timer_worknode<lock>* b    = a->sibling;
        Timer_worknode<lock>* rest = b->sibling;
        a->sibling                 = nullptr;
        b->sibling                 = nullptr;
        // detach parents since they become roots of temporary heaps
        a->parent = nullptr;
        b->parent = nullptr;
        return heap_meld(heap_meld(a, b), two_pass_merge(rest));
    }

    // 删除最小元素（根）并返回新的堆
    static Timer_worknode<lock>* delete_min(Timer_worknode<lock>* root) noexcept
    {
        Timer_worknode<lock>* nr = two_pass_merge(root->child);
        if (nr)
            nr->parent = nullptr;
        return nr;
    }

    void tim_chk_cb()
    {
        auto now      = std::chrono::steady_clock::now();
        int  trig_flg = 0;
        _executor.lock();
        while (heap_root && heap_root->expire <= now) {
            Timer_worknode<lock>* dpos = heap_root;
            heap_root                  = delete_min(heap_root);
            // detach popped node from heap so its destructor won't try to cancel again
            dpos->owner_queue = nullptr;
            dpos->parent      = nullptr;
            dpos->child       = nullptr;
            dpos->sibling     = nullptr;
            _executor.add_new_nolock(*dpos);
            trig_flg = 1;
        }
        _executor.unlock();
        if (trig_flg) {
            _executor.trig_once();
        }
    }

public:
    explicit Timer_check_queue(workqueue<lock>& executor) : _executor(executor)
    {
        INIT_LIST_HEAD(&ws_node);
        heap_root = nullptr;
        func      = [](struct worknode* work) {
            Timer_check_queue* tcq = static_cast<Timer_check_queue*>(work);
            tcq->tim_chk_cb();
        };
    }

    void post_delayed_work(Timer_worknode<lock>* dwork, uint32_t ms)
    {
        auto now           = std::chrono::steady_clock::now();
        dwork->expire      = now + std::chrono::milliseconds(ms);
        dwork->child       = nullptr;
        dwork->sibling     = nullptr;
        dwork->parent      = nullptr;
        dwork->owner_queue = this;

        _executor.lock();
        heap_root = heap_meld(heap_root, dwork);
        if (heap_root)
            heap_root->parent = nullptr; // root parent must stay null
        _executor.unlock();
    }

    void tick_update()
    {
        if (!heap_root) {
            return;
        }
        _executor.post(*this);
    }

    // 取消（移除）指定的定时节点（如果存在于当前队列）
    void cancel(Timer_worknode<lock>* node)
    {
        _executor.lock();
        if (!node || node->owner_queue != this) {
            _executor.unlock();
            return; // not in this queue
        }

        // Case: root removal
        if (node == heap_root) {
            heap_root = delete_min(heap_root);
        } else {
            // detach from parent's child list
            Timer_worknode<lock>* parent = node->parent;
            if (parent) {
                Timer_worknode<lock>* cur  = parent->child;
                Timer_worknode<lock>* prev = nullptr;
                while (cur) {
                    if (cur == node) {
                        if (prev)
                            prev->sibling = cur->sibling;
                        else
                            parent->child = cur->sibling;
                        break;
                    }
                    prev = cur;
                    cur  = cur->sibling;
                }
            }
            // merge node's children back into heap
            if (node->child) {
                Timer_worknode<lock>* merged = two_pass_merge(node->child);
                if (merged)
                    merged->parent = nullptr; // new root of subheap
                heap_root = heap_meld(heap_root, merged);
                if (heap_root)
                    heap_root->parent = nullptr;
            }
        }

        // clear node links
        node->child       = nullptr;
        node->sibling     = nullptr;
        node->parent      = nullptr;
        node->owner_queue = nullptr;
        _executor.unlock();
    }
};

template <lockable lock> struct DelayAwaiter : Timer_worknode<lock> {

    explicit DelayAwaiter(Timer_check_queue<lock>& delay_queue, uint32_t ms) : _delay_queue(delay_queue)
    {
        wait_ms = ms;
    }

    std::coroutine_handle<>  mCoroutine;
    uint32_t                 wait_ms;
    Timer_check_queue<lock>& _delay_queue;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> coroutine) noexcept
    {
        mCoroutine = coroutine;
        INIT_LIST_HEAD(&this->ws_node);
        this->func = timer_callback;
        _delay_queue.post_delayed_work(this, wait_ms);
    }

    void await_resume() const noexcept { }

    static void timer_callback(struct worknode* pws)
    {
        DelayAwaiter* self = static_cast<DelayAwaiter*>(pws);

        if (self->mCoroutine) {
            self->mCoroutine.resume();
        }
    }
};

template <lockable lock> inline auto delay_ms(struct Timer_check_queue<lock>& dly_wkq, uint32_t ms)
{
    return DelayAwaiter(dly_wkq, ms);
}

// Timer_worknode destructor (after Timer_check_queue defined)
template <lockable lock> inline Timer_worknode<lock>::~Timer_worknode()
{
    if (owner_queue) {
        owner_queue->cancel(this);
    }
}
}
