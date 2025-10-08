// callback_wq.hpp - per-owner ordered callback dispatcher on top of main workqueue
#pragma once
#include "io_waiter.hpp"
#include "workqueue.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#if defined(_WIN32)
#include <windows.h>
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#endif

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
    struct state : std::enable_shared_from_this<state> {
        static std::atomic<std::uint64_t> next_id;
        std::uint64_t                     id { 0 };
        workqueue<lock>&                  exec;
        lock                              lk;
        list_head                         pending { &pending, &pending };
        bool                              running { false };

        struct runner_node : worknode {
            state*                 owner { nullptr };
            std::shared_ptr<state> guard {};
        } runner;

        struct pending_entry {
            list_head       link;
            io_waiter_base* waiter { nullptr };
            pending_entry() { INIT_LIST_HEAD(&link); }
        };

        explicit state(workqueue<lock>& e) : exec(e)
        {
            id           = next_id.fetch_add(1, std::memory_order_relaxed) + 1;
            runner.owner = this;
            runner.func  = &state::runner_cb;
            INIT_LIST_HEAD(&runner.ws_node);
            INIT_LIST_HEAD(&pending);
            CO_WQ_CBQ_TRACE("[callback_wq] state ctor id=%llu this=%p exec=%p\n",
                            static_cast<unsigned long long>(id),
                            static_cast<void*>(this),
                            static_cast<void*>(&exec));
        }

        struct waiter_snapshot {
            bool          captured { false };
            const char*   debug_name { "<null-name>" };
            std::uint32_t debug_magic { 0 };
            void*         route_ctx { nullptr };
            void*         func_ptr { nullptr };
            void*         haddr { nullptr };
            long          guard_use { 0 };
            void*         guard_ptr { nullptr };
            bool          already_enqueued { false };
        };

        static waiter_snapshot capture_waiter(io_waiter_base* waiter)
        {
            waiter_snapshot snap {};
            if (!waiter)
                return snap;
            if (!is_pointer_plausible(waiter))
                return snap;
            snap.debug_name       = waiter->debug_name ? waiter->debug_name : "<null-name>";
            snap.debug_magic      = waiter->debug_magic;
            snap.route_ctx        = waiter->route_ctx;
            snap.func_ptr         = reinterpret_cast<void*>(waiter->func);
            snap.haddr            = waiter->h ? waiter->h.address() : nullptr;
            snap.already_enqueued = waiter->callback_enqueued.load(std::memory_order_acquire);
            auto guard            = waiter->load_route_guard();
            if (guard) {
                snap.guard_use = io_waiter_base::route_guard_use_count(guard);
                snap.guard_ptr = guard.get();
            }
            snap.captured = true;
            return snap;
        }

        static bool exchange_enqueued(io_waiter_base* waiter, bool desired, bool& previous)
        {
            if (!waiter)
                return false;
            previous = waiter->callback_enqueued.exchange(desired, std::memory_order_acq_rel);
            return true;
        }

        static bool store_enqueued(io_waiter_base* waiter, bool value)
        {
            if (!waiter)
                return false;
            waiter->callback_enqueued.store(value, std::memory_order_release);
            return true;
        }

        static bool reset_guard(io_waiter_base* waiter)
        {
            if (!waiter)
                return false;
            waiter->exchange_route_guard();
            return true;
        }

        ~state()
        {
            while (!list_empty(&pending)) {
                auto* lh = pending.next;
                list_del(lh);
                auto* entry = list_entry(lh, pending_entry, link);
                delete entry;
            }
            [[maybe_unused]] long  guard_use = runner.guard ? static_cast<long>(runner.guard.use_count()) : 0;
            [[maybe_unused]] void* guard_ptr = runner.guard ? runner.guard.get() : nullptr;
            CO_WQ_CBQ_TRACE(
                "[callback_wq] state dtor id=%llu this=%p pending_empty=%d running=%d guard_use=%ld guard_ptr=%p\n",
                static_cast<unsigned long long>(id),
                static_cast<void*>(this),
                list_empty(&pending) ? 1 : 0,
                running ? 1 : 0,
                guard_use,
                guard_ptr);
        }

        static bool is_pointer_plausible(const void* ptr)
        {
            if (!ptr)
                return false;
#if defined(_WIN32)
            MEMORY_BASIC_INFORMATION mbi {};
            SIZE_T                   queried = VirtualQuery(ptr, &mbi, sizeof(mbi));
            if (queried == 0)
                return false;
            if (!(mbi.State & MEM_COMMIT))
                return false;
            DWORD protect = mbi.Protect;
            if (protect & PAGE_NOACCESS)
                return false;
            if (protect & PAGE_GUARD)
                return false;
#else
            (void)ptr;
#endif
            return true;
        }

        static void runner_cb(worknode* w)
        {
            auto* r = static_cast<runner_node*>(w);
            r->owner->drain();
        }

        void post(worknode& node)
        {
            auto* waiter = static_cast<io_waiter_base*>(&node);
            auto  snap   = capture_waiter(waiter);
            if (!snap.captured) {
                CO_WQ_CBQ_WARN("[callback_wq] post skip unusable waiter node=%p waiter=%p func=%p\n",
                               static_cast<void*>(&node),
                               static_cast<void*>(waiter),
                               reinterpret_cast<void*>(node.func));
                return;
            }
            bool magic_ok = snap.debug_magic == io_waiter_base::debug_magic_value;
            if (!magic_ok) {
                bool        guard_present = snap.guard_ptr != nullptr && snap.guard_use > 0;
                const char* severity      = guard_present ? "warning" : "info";
                CO_WQ_CBQ_WARN("[callback_wq] %s: post skip invalid magic node=%p waiter=%p func=%p magic=%08x "
                               "guard_use=%ld guard_ptr=%p route_ctx=%p name=%s already=%d h=%p\n",
                               severity,
                               static_cast<void*>(&node),
                               static_cast<void*>(waiter),
                               reinterpret_cast<void*>(node.func),
                               snap.debug_magic,
                               snap.guard_use,
                               snap.guard_ptr,
                               snap.route_ctx,
                               snap.debug_name,
                               snap.already_enqueued ? 1 : 0,
                               snap.haddr);
                return;
            }
            if (!node.func) {
                CO_WQ_CBQ_WARN("[callback_wq] warning: posting null func node=%p owner=%p state_id=%llu route_ctx=%p "
                               "name=%s magic=%08x guard_use=%ld already=%d h=%p\n",
                               static_cast<void*>(&node),
                               static_cast<void*>(this),
                               static_cast<unsigned long long>(id),
                               snap.route_ctx,
                               snap.debug_name,
                               snap.debug_magic,
                               snap.guard_use,
                               snap.already_enqueued ? 1 : 0,
                               snap.haddr);
#if defined(_MSC_VER)
                __debugbreak();
#endif
            }

            std::unique_ptr<pending_entry> entry_holder;
            entry_holder         = std::make_unique<pending_entry>();
            entry_holder->waiter = waiter;

            bool need_sched = false;
            {
                std::lock_guard<lock> g(lk);
                bool                  already;
                if (!exchange_enqueued(waiter, true, already)) {
                    CO_WQ_CBQ_WARN("[callback_wq] post abort exchange failure node=%p waiter=%p state_id=%llu\n",
                                   static_cast<void*>(&node),
                                   static_cast<void*>(waiter),
                                   static_cast<unsigned long long>(id));
                    return;
                }
                if (already) {
                    CO_WQ_CBQ_TRACE("[callback_wq] post skip already-enqueued node=%p owner=%p state_id=%llu\n",
                                    static_cast<void*>(&node),
                                    static_cast<void*>(this),
                                    static_cast<unsigned long long>(id));
                    return;
                }

                pending_entry* entry = entry_holder.release();
                if (!entry) {
                    entry         = new pending_entry();
                    entry->waiter = waiter;
                }
                list_add_tail(&entry->link, &pending);
                CO_WQ_CBQ_TRACE("[callback_wq] post linked entry=%p waiter=%p node=%p pending_first=%p pending_last=%p "
                                "state_id=%llu\n",
                                static_cast<void*>(entry),
                                static_cast<void*>(waiter),
                                static_cast<void*>(&node),
                                static_cast<void*>(pending.next),
                                static_cast<void*>(pending.prev),
                                static_cast<unsigned long long>(id));

                if (!running) {
                    running                                = true;
                    runner.guard                           = this->shared_from_this();
                    [[maybe_unused]] long guard_use_runner = runner.guard ? static_cast<long>(runner.guard.use_count())
                                                                          : 0;
                    CO_WQ_CBQ_TRACE(
                        "[callback_wq] runner guard retain owner=%p state_id=%llu guard_use=%ld guard_ptr=%p\n",
                        static_cast<void*>(this),
                        static_cast<unsigned long long>(id),
                        static_cast<long>(guard_use_runner),
                        runner.guard ? static_cast<void*>(runner.guard.get()) : nullptr);
                    need_sched = true;
                }
            }

            CO_WQ_CBQ_TRACE(
                "[callback_wq] post: node=%p func=%p owner=%p state_id=%llu route_ctx=%p h=%p name=%s magic=%08x "
                "already=%d guard_use=%ld\n",
                static_cast<void*>(&node),
                reinterpret_cast<void*>(node.func),
                static_cast<void*>(this),
                static_cast<unsigned long long>(id),
                snap.route_ctx,
                snap.haddr,
                snap.debug_name,
                snap.debug_magic,
                snap.already_enqueued ? 1 : 0,
                snap.guard_use);

            if (need_sched) {
                CO_WQ_CBQ_TRACE("[callback_wq] post scheduling runner owner=%p state_id=%llu\n",
                                static_cast<void*>(this),
                                static_cast<unsigned long long>(id));
                exec.post(runner);
            }
        }

        void drain()
        {
            std::shared_ptr<state> keep_alive    = runner.guard;
            bool                   release_guard = false;
            for (;;) {
                list_head local { &local, &local };
                {
                    std::lock_guard<lock> g(lk);
                    if (list_empty(&pending)) {
                        running       = false;
                        release_guard = true;
                        break;
                    }
                    list_head*                  pending_first = pending.next;
                    list_head*                  pending_last  = pending.prev;
                    bool                        first_ok      = is_pointer_plausible(pending_first);
                    bool                        last_ok       = is_pointer_plausible(pending_last);
                    list_head*                  first_prev    = first_ok ? pending_first->prev : nullptr;
                    [[maybe_unused]] list_head* first_next    = first_ok ? pending_first->next : nullptr;
                    [[maybe_unused]] list_head* last_prev     = last_ok ? pending_last->prev : nullptr;
                    list_head*                  last_next     = last_ok ? pending_last->next : nullptr;
                    bool                        first_prev_ok = first_ok ? is_pointer_plausible(first_prev) : false;
                    bool                        last_next_ok  = last_ok ? is_pointer_plausible(last_next) : false;
                    CO_WQ_CBQ_TRACE(
                        "[callback_wq] drain batch state owner=%p state_id=%llu pending_first=%p pending_last=%p "
                        "first_prev=%p first_next=%p last_prev=%p last_next=%p first_ok=%d last_ok=%d first_prev_ok=%d "
                        "last_next_ok=%d\n",
                        static_cast<void*>(this),
                        static_cast<unsigned long long>(id),
                        static_cast<void*>(pending_first),
                        static_cast<void*>(pending_last),
                        static_cast<void*>(first_prev),
                        static_cast<void*>(first_next),
                        static_cast<void*>(last_prev),
                        static_cast<void*>(last_next),
                        first_ok ? 1 : 0,
                        last_ok ? 1 : 0,
                        first_prev_ok ? 1 : 0,
                        last_next_ok ? 1 : 0);
                    if (!first_ok || !last_ok || !first_prev_ok || !last_next_ok || first_prev != &pending
                        || last_next != &pending) {
                        CO_WQ_CBQ_WARN(
                            "[callback_wq] warning: pending list corruption detected owner=%p state_id=%llu first=%p "
                            "last=%p first_prev=%p last_next=%p\n",
                            static_cast<void*>(this),
                            static_cast<unsigned long long>(id),
                            static_cast<void*>(pending_first),
                            static_cast<void*>(pending_last),
                            static_cast<void*>(first_prev),
                            static_cast<void*>(last_next));
#if defined(_MSC_VER)
                        __debugbreak();
#endif
                        running = false;
                        INIT_LIST_HEAD(&pending);
                        release_guard = true;
                        break;
                    }
                    local.next       = pending_first;
                    local.prev       = pending_last;
                    local.next->prev = &local;
                    local.prev->next = &local;
                    INIT_LIST_HEAD(&pending);
                }
                while (!list_empty(&local)) {
                    list_head* lh = local.next;
                    list_del(lh);

                    auto*           entry    = list_entry(lh, pending_entry, link);
                    io_waiter_base* n_waiter = entry ? entry->waiter : nullptr;
                    worknode*       n        = n_waiter ? static_cast<worknode*>(n_waiter) : nullptr;

                    if (!entry || !n_waiter) {
                        CO_WQ_CBQ_WARN("[callback_wq] warning: null waiter entry=%p owner=%p state_id=%llu\n",
                                       static_cast<void*>(entry),
                                       static_cast<void*>(this),
                                       static_cast<unsigned long long>(id));
                        delete entry;
                        continue;
                    }

                    auto snap = capture_waiter(n_waiter);
                    if (!snap.captured) {
                        CO_WQ_CBQ_WARN(
                            "[callback_wq] drain skip unusable waiter node=%p owner=%p state_id=%llu entry=%p\n",
                            static_cast<void*>(n),
                            static_cast<void*>(this),
                            static_cast<unsigned long long>(id),
                            static_cast<void*>(entry));
                        delete entry;
                        continue;
                    }

                    bool magic_ok = snap.debug_magic == io_waiter_base::debug_magic_value;
                    store_enqueued(n_waiter, false);
                    bool func_ok = n && n->func;

                    if (!func_ok) {
                        CO_WQ_CBQ_WARN(
                            "[callback_wq] warning: missing func node=%p owner=%p state_id=%llu route_ctx=%p name=%s "
                            "magic=%08x guard_use=%ld entry=%p\n",
                            static_cast<void*>(n),
                            static_cast<void*>(this),
                            static_cast<unsigned long long>(id),
                            snap.route_ctx,
                            snap.debug_name,
                            snap.debug_magic,
                            snap.guard_use,
                            static_cast<void*>(entry));
#if defined(_MSC_VER)
                        __debugbreak();
#endif
                        delete entry;
                        continue;
                    }
                    if (!magic_ok) {
                        CO_WQ_CBQ_WARN(
                            "[callback_wq] warning: invalid magic node=%p func=%p owner=%p state_id=%llu route_ctx=%p "
                            "h=%p name=%s magic=%08x guard_use=%ld entry=%p\n",
                            static_cast<void*>(n),
                            reinterpret_cast<void*>(n->func),
                            static_cast<void*>(this),
                            static_cast<unsigned long long>(id),
                            snap.route_ctx,
                            snap.haddr,
                            snap.debug_name,
                            snap.debug_magic,
                            snap.guard_use,
                            static_cast<void*>(entry));
                        if (snap.guard_use > 0) {
                            CO_WQ_CBQ_WARN(
                                "[callback_wq] invalid magic guard reset attempt node=%p guard_use=%ld guard_ptr=%p\n",
                                static_cast<void*>(n),
                                snap.guard_use,
                                snap.guard_ptr);
                            reset_guard(n_waiter);
                        }
                        delete entry;
                        continue;
                    }

                    CO_WQ_CBQ_TRACE("[callback_wq] exec: node=%p func=%p owner=%p state_id=%llu route_ctx=%p h=%p "
                                    "name=%s magic=%08x "
                                    "entry=%p\n",
                                    static_cast<void*>(n),
                                    reinterpret_cast<void*>(n->func),
                                    static_cast<void*>(this),
                                    static_cast<unsigned long long>(id),
                                    snap.route_ctx,
                                    snap.haddr,
                                    snap.debug_name,
                                    snap.debug_magic,
                                    static_cast<void*>(entry));
                    n->func(n);
                    delete entry;
                }
            }
            if (release_guard && runner.guard.get() == keep_alive.get())
                runner.guard.reset();
            if (release_guard) {
                CO_WQ_CBQ_TRACE("[callback_wq] runner guard release owner=%p state_id=%llu\n",
                                static_cast<void*>(this),
                                static_cast<unsigned long long>(id));
            }
        }
    };

public:
    explicit callback_wq(workqueue<lock>& exec) : _state(std::make_shared<state>(exec))
    {
        CO_WQ_CBQ_TRACE("[callback_wq] ctor this=%p state=%p state_id=%llu exec=%p\n",
                        static_cast<void*>(this),
                        static_cast<void*>(_state.get()),
                        static_cast<unsigned long long>(_state ? _state->id : 0),
                        static_cast<void*>(&exec));
    }

    ~callback_wq()
    {
        CO_WQ_CBQ_TRACE("[callback_wq] dtor this=%p state=%p state_id=%llu use_count=%ld\n",
                        static_cast<void*>(this),
                        static_cast<void*>(_state.get()),
                        static_cast<unsigned long long>(_state ? _state->id : 0),
                        static_cast<long>(_state.use_count()));
    }

    static void post_adapter(void* ctx, worknode* node)
    {
        auto* st = static_cast<state*>(ctx);
        if (st)
            st->post(*node);
    }

    void post(worknode& node) { _state->post(node); }

    void* context() const noexcept { return _state.get(); }

    std::shared_ptr<void> retain_guard() const noexcept
    {
        auto guard = std::shared_ptr<void>(_state);
        CO_WQ_CBQ_TRACE("[callback_wq] retain_guard state=%p state_id=%llu guard_use=%ld guard_ptr=%p\n",
                        static_cast<void*>(_state.get()),
                        static_cast<unsigned long long>(_state ? _state->id : 0),
                        static_cast<long>(guard.use_count()),
                        static_cast<void*>(guard.get()));
        return guard;
    }

    workqueue<lock>& exec() { return _state->exec; }

private:
    std::shared_ptr<state> _state;
};

template <co_wq::lockable Lock> std::atomic<std::uint64_t> co_wq::net::callback_wq<Lock>::state::next_id { 0 };

} // namespace co_wq::net
