#include "syswork.hpp"
#include "workqueue.hpp"
#include <atomic>
#include <condition_variable>
struct executor_wq : co_wq::workqueue<co_wq::SpinLock> {
    std::atomic_int trig_cnt = 0;
    // 仅系统执行器需要的线程同步
    std::condition_variable_any cv;

    // 阻塞版本获取，由用户循环调用
    co_wq::worknode* wait_and_get(struct workqueue& wq)
    {
        // 注意：此函数由基类 work_once() 在已持有 lk 情况下调用，不能再次加锁，否则死锁。
        for (;;) {
            co_wq::worknode* n = co_wq::workqueue<co_wq::SpinLock>::get_work_node(wq); // 使用基类非阻塞获取逻辑
            if (n) {
                return n; // 解锁由 work_once() 负责
            }
            // 无任务则阻塞等待新任务到来；wait 内部会临时释放 lk 并在唤醒后重新加锁
            cv.wait(lk);
        }
    }

protected:
    // 覆写以在为空时不立即返回工作节点；这里仍复用父逻辑，无附加策略
    co_wq::worknode* get_work_node(co_wq::workqueue<co_wq::SpinLock>& wq) override { return wait_and_get(wq); }

public:
    explicit executor_wq()
    {
        trig = [](struct workqueue* wq) {
            executor_wq* ewq = static_cast<executor_wq*>(wq);
            ewq->trig_cnt++;
            // 触发时额外唤醒一个等待线程（post 已经 notify_one，这里保底 notify_all）
            ewq->cv.notify_all();
        };
    }
};

executor_wq                               executor;
co_wq::Timer_check_queue<co_wq::SpinLock> timer(executor);

co_wq::workqueue<co_wq::SpinLock>& get_sys_workqueue(void)
{
    return executor;
}

co_wq::Timer_check_queue<co_wq::SpinLock>& get_sys_timer(void)
{
    return timer;
}
