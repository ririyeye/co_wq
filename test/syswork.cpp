#include "syswork.hpp"
#include "workqueue.hpp"
#include <atomic>
struct executor_wq : co_wq::workqueue<co_wq::nolock> {
    std::atomic_int trig_cnt = 0;

    explicit executor_wq()
    {
        trig = [](struct workqueue* wq) {
            executor_wq* ewq = static_cast<executor_wq*>(wq);
            ewq->trig_cnt++;
        };
    }
};

executor_wq                             executor;
co_wq::Timer_check_queue<co_wq::nolock> timer(executor);

co_wq::workqueue<co_wq::nolock>& get_sys_workqueue(void)
{
    return executor;
}

co_wq::Timer_check_queue<co_wq::nolock>& get_sys_timer(void)
{
    return timer;
}
