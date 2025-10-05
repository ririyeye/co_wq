// callback_wq.hpp - per-owner ordered callback dispatcher on top of main workqueue
#pragma once
#include "io_waiter.hpp"
#include "workqueue.hpp"
#include <mutex>

namespace co_wq::net {

/**
 * @brief 面向“拥有者”的回调分发器，在共享的 workqueue 之上保证回调按 FIFO 顺序执行。
 *
 * 为什么
 *  - 在多线程执行器中，来自 epoll/IOCP 的直接投递可能在不同线程间交错。
 *  - 对于同一个 socket/file 拥有者，回调应当按照投递顺序执行，避免微妙的竞态。
 *
 * 如何实现
 *  - 使用 worknode 内置的 intrusive list_head 作为回调队列（无需额外分配）。
 *  - 向主工作队列仅调度一个 runner 节点，按序依次排空待执行的回调。
 *  - 线程安全：用一把小锁保护队列与调度状态；回调在拥有者的执行队列上运行。
 *
 * 集成方式
 *  - io_waiter_base 暴露了 route_ctx/route_post。将其设置为 (&_cbq, &callback_wq::post_adapter)。
 *  - 反应器需通过 post_via_route(exec, node) 投递，使回调经过 _cbq 路由。
 *
 * 能力与保证
 *  - 对同一 callback_wq 实例，严格 FIFO；任意时刻至多一个 runner 在运行；入队/出队摊销 O(1)；无堆分配。
 *
 * 生命周期
 *  - 拥有者必须比所有在途 awaiter 更长寿。该队列不拥有节点，仅负责链接/脱链。
 */
template <lockable lock> class callback_wq {
public:
    explicit callback_wq(workqueue<lock>& exec) : _exec(exec)
    {
        _runner.owner = this;
        _runner.func  = &callback_wq::runner_cb;
        INIT_LIST_HEAD(&_runner.ws_node);
        INIT_LIST_HEAD(&_pending);
    }
    // 适配到 io_waiter_base 的路由回调
    static void post_adapter(void* ctx, worknode* node)
    {
        auto* self = static_cast<callback_wq*>(ctx);
        self->post(*node);
    }
    // 入队一个回调节点（不重入执行，按顺序在主 wq 中执行）
    void post(worknode& node)
    {
        bool need_sched = false;
        {
            std::lock_guard<lock> g(_lk);
            auto*                 waiter = static_cast<io_waiter_base*>(&node);
            if (waiter->callback_enqueued.exchange(true, std::memory_order_acq_rel))
                return; // already enqueued, skip duplicate
            if (node.ws_node.next != &node.ws_node)
                list_del(&node.ws_node); // 脱离任何旧链表，避免损坏
            // 使用内置 intrusive 链表节点，避免额外分配
            list_add_tail(&node.ws_node, &_pending);
            if (!_running) {
                _running   = true; // 进入 RUNNING 状态（runner 已经或即将调度）
                need_sched = true;
            }
        }
        if (need_sched) {
            _exec.post(_runner);
        }
    }

private:
    struct runner_node : worknode {
        callback_wq* owner { nullptr };
    };
    static void runner_cb(worknode* w)
    {
        auto* r = static_cast<runner_node*>(w);
        r->owner->drain();
    }
    // 从队列中逐个取出并调用 worknode::func，直到为空；确保与 post() 互斥
    void drain()
    {
        // 单 runner 模式：_running==true 表示 runner 已入队或正在处理。
        // 采用批量提取，减少锁竞争；允许在执行回调时继续 post 新节点。
        for (;;) {
            list_head local { &local, &local };
            {
                std::lock_guard<lock> g(_lk);
                if (list_empty(&_pending)) {
                    // 队列空 => 退出 RUNNING 状态。
                    _running = false;
                    break;
                }
                // splice _pending -> local
                local.next       = _pending.next;
                local.prev       = _pending.prev;
                local.next->prev = &local;
                local.prev->next = &local;
                INIT_LIST_HEAD(&_pending);
            }
            // 批量处理 local 中的所有节点
            while (!list_empty(&local)) {
                list_head* lh = local.next;
                list_del(lh);
                worknode* n = list_entry(lh, worknode, ws_node);
                if (n)
                    static_cast<io_waiter_base*>(n)->callback_enqueued.store(false, std::memory_order_release);
                if (n && n->func)
                    n->func(n);
            }
            // 回到循环再检查是否有新 post
        }
    }

    workqueue<lock>& _exec;
    lock             _lk;
    list_head        _pending { &_pending, &_pending };
    bool             _running { false }; // RUNNING 包含“已调度 / 正在执行”两个阶段
    runner_node      _runner;
};

} // namespace co_wq::net
