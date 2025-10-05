# callback_wq Missing `func` Incident Report

## 概要
- **日期**：2025-10-05
- **模块**：`io/callback_wq.hpp`
- **症状**：`drain()` 过程中命中断点，日志提示 `worknode.func` 为空（"missing func"）。
- **影响**：协程恢复回调被跳过，潜在导致等待任务卡死或状态紊乱。

## 事件时间线
1. 压测 HTTP 代理示例时，`callback_wq` 日志出现 `missing func` 警告，随后触发 `__debugbreak()`。
2. 日志显示对应节点 `debug_name` 正常（例如 `tcp_socket::recv`），但 `func` 指针为 `nullptr`。
3. 初步判断 awaiter 在队列中复用时未重新设置 `func`。

## 根因分析
- `worknode::func` 是 `workqueue` 调度回调的唯一入口。
- 某些 awaiter 在完成后重置了 `func`（或被析构），但在再次入队前没有重新赋值。
- 因此 `callback_wq` 在 `drain()` 时取到的节点 `func == nullptr`，触发断言。

## 最新排查进展（2025-10-05 晚）
- 追加在 `callback_wq::state::post/drain`、`workqueue::post`、`io_waiter_base::resume_cb` 等关键路径的详细日志，捕获到问题节点多数来自 Windows 平台 TCP awaiter。
- 压测期间多次复现：当 IOCP 回调在 `CancelIoEx` 之后抵达，awaiter 已析构并将 `func` 复位为 `nullptr`，但仍残留在回调队列中，最终在 `drain()` 中触发断点。
- 为保证队列寿命一致性，所有 socket/file/timer awaiter 现统一存储 `callback_wq` 的守卫引用（`retain_guard()`），并在销毁时显式释放，避免队列先于 awaiter 析构导致悬挂指针。
- 在 `workqueue` 层补充 `wq_debug_null_func` hook 与 poison pattern 检测，可在断点前打印相关协程、route guard 用量与调用栈，辅助定位复现路径。

## 追加事故处理（2025-10-05 深夜）
- `co_http_proxy` 在 Windows 上运行时再次崩溃（`completion without waiter` → AV @ `callback_wq::state::post`）。
- 原因：部分 Windows TCP awaiter（`recv`/`recv_all`/`send`）在迁移日志宏后，`route_ctx` 仍指向 `callback_wq` 对象本身而非内部 `state`，导致 `post_adapter` 将无效指针解释为 `state*` 并写入损坏的 pending 链表。
- 修复：这些 awaiter 现在与 Linux 路径保持一致——在构造时：
   1. `retain_guard()` 并缓存到 `io_waiter_base`，
   2. 使用 `callback_queue().context()` 传入真实的 `state*`，
   3. 为每次 IO 投递显式维护 `iocp_ovl.cancelled` 标志，避免完成回调在 `waiter` 置空后仍被误判为错误。
- 复验：`xmake build co_http_proxy` + `xmake run co_http_proxy --port 18100` 通过，未再触发 `state::post` 崩溃，仅手动 Ctrl+C 触发正常退出。

## 解决思路
1. 保留并强化 `callback_wq` 对 `func` 的断言，确保问题立即暴露。
2. 回溯具体 awaiter（通过 `debug_name`）到对应实现，确认其生命周期：
   - 是否在 `finish()` 或析构阶段清空了 `func`；
   - 是否在下一次复用前重新设置为正确回调（例如 `resume_cb`）。
3. 修复 awaiter：保证每次投递前 `func` 都被设为有效函数指针。
4. 复测同样场景，确认断点不再触发。

## 后续行动
- 审查所有 IO awaiter（TCP/UDP/TLS/文件/USB）是否有复用节点的逻辑，确保设置 `func` 的路径完整。
- 针对关键 awaiter 增加构造/析构日志，便于确认生命周期。
- 在压力测试脚本中保留该场景作为回归用例。
- 为避免调试日志淹没业务输出，新增 `CO_WQ_ENABLE_CALLBACK_WQ_TRACE` 宏开关，默认关闭详细打印，仅在复现/压测时开启。

## 经验总结
- `worknode` 复用需要严格管理状态；若协程对象在运行过程中改变 `func`，必须谨慎在下一次使用前复位。
- 提前加入断言和详细日志是定位此类内存/状态问题的关键。
- `callback_wq` 的 guard 仅保护队列生命周期，不负责节点自身的一致性，节点状态仍需 awaiter 自行维护。
