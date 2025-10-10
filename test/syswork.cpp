#include "syswork.hpp"
#include "workqueue.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace {
struct executor_wq : co_wq::workqueue<co_wq::SpinLock> {
    std::atomic_int             trig_cnt = 0;
    std::condition_variable_any cv;
    std::atomic_bool            stopping { false };
    // worker 线程集合
    std::vector<std::thread> workers;

    co_wq::worknode* wait_and_get(struct workqueue& wq)
    {
        for (;;) {
            if (stopping.load(std::memory_order_acquire)) {
                return nullptr;
            }
            co_wq::worknode* n = co_wq::workqueue<co_wq::SpinLock>::get_work_node(wq);
            if (n) {
                return n;
            }
            cv.wait(lk, [&]() {
                return stopping.load(std::memory_order_acquire) || !co_wq::list_empty(&wq.ws_head); // 有任务继续
            });
        }
    }

protected:
    co_wq::worknode* get_work_node(co_wq::workqueue<co_wq::SpinLock>& wq) override { return wait_and_get(wq); }

public:
    executor_wq()
    {
        trig = [](struct workqueue* wq) {
            executor_wq* ewq = static_cast<executor_wq*>(wq);
            ewq->trig_cnt.fetch_add(1, std::memory_order_relaxed);
            ewq->cv.notify_one();
        };
    }

    void start_workers(int n)
    {
        if (n <= 0) {
            unsigned hc = std::thread::hardware_concurrency();
            if (hc == 0)
                hc = 1;
            n = (int)hc;
        }
        if (!workers.empty())
            return; // 已经启动
        workers.reserve((size_t)n);
        for (int i = 0; i < n; ++i) {
            workers.emplace_back([this]() {
                while (!stopping.load(std::memory_order_acquire)) {
                    // 直接调用基类的 work_once() 处理一个任务
                    (void)this->work_once();
                }
            });
        }
    }

    void stop_and_join()
    {
        stopping.store(true, std::memory_order_release);
        cv.notify_all();
        for (auto& t : workers) {
            if (t.joinable())
                t.join();
        }
    }

    ~executor_wq() { stop_and_join(); }
};

executor_wq& get_executor()
{
    static executor_wq exec;
    return exec;
}

struct timer_holder {
    co_wq::Timer_check_queue<co_wq::SpinLock>* ptr;
    std::atomic_bool                           ticker_running { false };
    std::atomic_bool                           ticker_stop { false };
    std::thread                                ticker_thread;

    timer_holder() : ptr(nullptr) { }

    void ensure_ticker()
    {
        if (ticker_running.load(std::memory_order_acquire))
            return;
        bool expected = false;
        if (!ticker_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            return;
        ticker_stop.store(false, std::memory_order_release);
        ticker_thread = std::thread([this]() {
            while (!ticker_stop.load(std::memory_order_acquire)) {
                auto* queue = ptr;
                if (queue)
                    queue->tick_update();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }

    ~timer_holder()
    {
        ticker_stop.store(true, std::memory_order_release);
        if (ticker_thread.joinable())
            ticker_thread.join();
    }
};
timer_holder& get_timer_holder()
{
    static timer_holder th;
    return th;
}

std::once_flag g_init_flag;
int            g_threads = 0; // 记录初始化线程数
} // namespace

co_wq::workqueue<co_wq::SpinLock>& get_sys_workqueue(int threads)
{
    executor_wq& exec = get_executor();
    std::call_once(g_init_flag, [&]() {
        g_threads = threads;
        exec.start_workers(threads);
        // 初始化 timer 队列
        get_timer_holder().ptr = new co_wq::Timer_check_queue<co_wq::SpinLock>(exec);
        get_timer_holder().ensure_ticker();
    });
    return exec;
}

co_wq::Timer_check_queue<co_wq::SpinLock>& get_sys_timer(void)
{
    if (!get_timer_holder().ptr) {
        // 若用户先调用 timer，隐式初始化
        (void)get_sys_workqueue(0);
    }
    get_timer_holder().ensure_ticker();
    return *get_timer_holder().ptr;
}

void sys_wait_until(std::atomic_bool& finished)
{
    // 简单阻塞轮询：等待 finished 变为 true。因为 wq 的 worker 线程已自动消费任务，无需外部调用 work_once()
    while (!finished.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// timer 不删除以避免静态析构顺序问题（可选清理）
