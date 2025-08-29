#include "lock.hpp"
#include "timer.hpp"

co_wq::workqueue<co_wq::nolock>&         get_sys_workqueue(void);
co_wq::Timer_check_queue<co_wq::nolock>& get_sys_timer(void);
