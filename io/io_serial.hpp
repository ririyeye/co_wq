// io_serial.hpp - common serialized IO queue helpers for tcp/udp/file awaiters
#pragma once
/**
 * @file io_serial.hpp
 * @brief 通用 IO 串行化队列 & Awaiter 基础工具。
 *
 * 提供三层抽象：
 *  1. serial_queue: 简单的 FIFO 等待队列 + 锁占用标志。
 *  2. serial_slot_awaiter: 基础 CRTP，用于一次性获取“串行槽位”。
 *  3. two_phase_drain_awaiter: 在获取槽位后执行“多次尝试 -> 可能等待 -> 继续驱动”二阶段循环模式。
 *
 * 设计目标：统一 tcp/udp/file 等对象内部 send/recv/read/write 的串行化与驱动逻辑，消除重复的
 * lock_acquired_cb / drive_cb 模板样板代码，同时不强制具体锁类型（支持 SpinLock / nolock 等）。
 */
#include "io_waiter.hpp"
#include "workqueue.hpp"
#include <concepts>
#include <initializer_list>
#include <mutex>
#include <type_traits>

namespace co_wq::net {

// --- Concepts 限制: 执行器与串行 Owner 接口 ---
using co_wq::lockable; // 引入 lockable 概念名称

template <class Exec>
concept exec_like = requires(Exec e, worknode& n) {
    { e.post(n) } -> std::same_as<void>;
};

template <class Owner>
concept serial_owner = requires(Owner o, worknode& n) {
    // 需要提供 serial_lock() & exec() 接口
    { o.serial_lock() };
    { o.exec().post(n) } -> std::same_as<void>;
    // serial_lock() 返回类型需满足 lockable
    requires co_wq::lockable<std::remove_reference_t<decltype(o.serial_lock())>>;
};

// (不对 Derived 直接加 concept，因 CRTP 派生类型在定义时不完整，概念检查会失败)

/**
 * @brief 串行化等待队列（与单一逻辑槽位绑定）。
 *
 * locked = true 表示槽位当前被某个 awaiter 占用；
 * waiters 存放其他等待获得槽位的协程节点（worknode.ws_node 链表）。
 */
struct serial_queue {
    list_head waiters;          ///< FIFO 链表 (worknode::ws_node)
    bool      locked { false }; ///< 当前是否已被占用
};

/**
 * @brief 初始化串行队列节点。
 */
inline void serial_queue_init(serial_queue& q)
{
    INIT_LIST_HEAD(&q.waiters);
    q.locked = false;
}

/**
 * @brief 尝试立即获取串行槽位；否则进入等待队列。
 * @tparam Lock 任意满足 lockable 概念的锁类型（SpinLock, std::mutex 包装等）。
 * @tparam Exec workqueue 类型。
 * @param q 目标串行队列。
 * @param lock 同步锁。
 * @param exec 关联执行队列，用于立即 post 获得锁的节点。
 * @param node 待加入的工作节点（其 func 需已设置为 lock_acquired 回调）。
 * @return true 表示已立即获得槽位并已 post；false 已加入等待队列。
 */
template <co_wq::lockable Lock, exec_like Exec>
inline bool serial_acquire_or_enqueue(serial_queue& q, Lock& lock, Exec& exec, worknode& node)
{
    std::lock_guard<Lock> lk(lock);
    if (!q.locked && list_empty(&q.waiters)) {
        q.locked = true;
        exec.post(node); // immediate acquisition: post lock_acquired callback
        return true;
    }
    list_add_tail(&node.ws_node, &q.waiters);
    return false; // enqueued
}

/**
 * @brief 释放串行槽位；若有等待者则唤醒下一个。
 * @tparam Lock 锁类型（满足 lockable）。
 * @tparam Exec 执行队列类型。
 */
template <co_wq::lockable Lock, exec_like Exec> inline void serial_release(serial_queue& q, Lock& lock, Exec& exec)
{
    worknode* next = nullptr;
    {
        std::lock_guard<Lock> lk(lock);
        if (!list_empty(&q.waiters)) {
            auto* lh = q.waiters.next;
            auto* wn = list_entry(lh, worknode, ws_node);
            list_del(lh);
            next = wn;
        } else {
            q.locked = false;
        }
    }
    if (next)
        exec.post(*next);
}

/**
 * @brief 基础串行槽位获取 CRTP Awaiter。
 *
 * 要求 Owner 暴露：
 *  - workqueue<Lock>& exec();
 *  - Lock& serial_lock();
 *
 * Derived 需提供 static void lock_acquired_cb(worknode*).
 */
template <class Derived, serial_owner Owner> struct serial_slot_awaiter : io_waiter_base {
    Owner&        owner;
    serial_queue& q; // queue we serialize on
    serial_slot_awaiter(Owner& o, serial_queue& queue) : owner(o), q(queue) { }
    bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> awaiting)
    {
        this->h    = awaiting;
        this->func = &Derived::lock_acquired_cb;
        INIT_LIST_HEAD(&this->ws_node);
        serial_acquire_or_enqueue(q, owner.serial_lock(), owner.exec(), *this);
    }
    // Helper for derived to release slot when operation completes
    static void release(Derived* self) { serial_release(self->q, self->owner.serial_lock(), self->owner.exec()); }
};

/**
 * @brief 二阶段可重入驱动 Awaiter（获取槽位 -> 尝试执行 -> 可能等待事件 -> 继续驱动）。
 *
 * Derived 需实现：
 *  int attempt_once();  语义: >0 取得进展继续循环；=0 完成/终止；-1 需等待事件。
 *  static void register_wait(Derived* self, bool first); 在需等待时注册事件；first 表示是否首轮。
 */
template <class Derived, serial_owner Owner> struct two_phase_drain_awaiter : serial_slot_awaiter<Derived, Owner> {
    using base = serial_slot_awaiter<Derived, Owner>;
    using base::base;
    // attempt_once contract described above
    static void lock_acquired_cb(worknode* w)
    {
        static_assert(std::is_base_of_v<two_phase_drain_awaiter, Derived> || true,
                      "CRTP pattern: Derived should inherit two_phase_drain_awaiter<Derived, Owner>");
        auto* self = static_cast<Derived*>(w);
        while (true) {
            int r = self->attempt_once();
            if (r > 0)
                continue; // progress, loop again
            if (r == 0) { // done
                base::release(self);
                self->func = &io_waiter_base::resume_cb;
                if (self->h)
                    self->h.resume();
                return;
            }
            // would block
            self->func = &Derived::drive_cb;
            Derived::register_wait(self, true);
            return;
        }
    }
    static void drive_cb(worknode* w)
    {
        auto* self = static_cast<Derived*>(w);
        while (true) {
            int r = self->attempt_once();
            if (r > 0)
                continue; // keep draining
            if (r == 0) { // complete
                base::release(self);
                self->func = &io_waiter_base::resume_cb;
                if (self->h)
                    self->h.resume();
                return;
            }
            // still would block -> re-arm
            self->func = &Derived::drive_cb;
            Derived::register_wait(self, false);
            return;
        }
    }
};

// ---- 通用辅助：批量收集 & 投递串行队列中的等待者 ----
// 收集：在持锁情况下，将多个 serial_queue 的 waiters 节点搬运到 out_pending，并重置 locked 标志。
template <co_wq::lockable Lock>
inline void serial_collect_waiters(Lock& lk, std::initializer_list<serial_queue*> queues, list_head& out_pending)
{
    std::scoped_lock guard(lk);
    for (auto* q : queues) {
        while (!list_empty(&q->waiters)) {
            auto* lh = q->waiters.next;
            list_del(lh);
            list_add_tail(lh, &out_pending);
        }
        q->locked = false;
    }
}
// 批量投递：统一设置 resume_cb，然后一次性 splice 进执行队列（trig 一次）。
template <class Exec> inline void serial_post_pending(Exec& exec, list_head& pending)
{
    if (list_empty(&pending))
        return;
    list_head* pos;
    list_for_each (pos, &pending) {
        auto* wn = list_entry(pos, worknode, ws_node);
        wn->func = &io_waiter_base::resume_cb;
    }
    exec.post(pending);
}
} // namespace co_wq::net
