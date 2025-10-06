#include "lock.hpp"
#include "timer.hpp"
#include "workqueue.hpp"
#include <atomic>

// 获取系统全局工作队列；threads 参数:
//  >0 : 指定工作线程数量 (仅第一次调用生效)
//   0 : 自动检测 (std::thread::hardware_concurrency, 至少 1)
//  后续再次调用忽略 threads 参数，返回同一个实例。
co_wq::workqueue<co_wq::SpinLock>&         get_sys_workqueue(int threads = 0);
co_wq::Timer_check_queue<co_wq::SpinLock>& get_sys_timer(void);

// 等待某个原子完成标志为 true（由协程完成回调置位），避免外部直接调用 work_once。
// 内部使用条件变量或轻度 sleep 轮询。
void sys_wait_until(std::atomic_bool& finished);
