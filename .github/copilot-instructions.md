# co_wq Copilot Instructions

以后都用中文回答。

## 项目概览
- `co_wq` 提供基于 C++20 协程的跨平台异步框架：`task/` 内实现 `Task`/`Promise`，`sync/` 封装信号量、定时器，`io/` 与 `net/` 提供 epoll/IOCP reactor 与 TCP/UDP/TLS/Unix Socket/文件/USB awaiter。
- 核心执行器 `co_wq::workqueue<Lock>` 驱动所有回调。投递节点必须初始化 `worknode::func` 并保持生命周期 > 在途任务。
- Linux 默认依赖 `epoll`，Windows 使用 MSVC 构建接入 IOCP。`USING_NET/USING_SSL/USING_USB/USING_EXAMPLE` 控制可选模块。

## 构建与脚本
- 优先运行 `python script/xmk.py build`：`xmake` 配置 releasedbg 模式、启用 examples、生成 `compile_commands.json`，并将产物安装到 `install/`。
- Windows 环境请使用 MSVC 工具链执行 `python script/xmk.py build`。
- 清理使用 `python script/xmk.py clean`（会删 `.xmake/ build/ install/ .cache/`，可附带 `--remove-global-cache` 删除 `~/.xmake`）。
- 手动 xmake：`xmake f -y -m releasedbg --USING_EXAMPLE=y -o build` → `xmake -vD` → `xmake install -o install`。
- 注意 `xmake` 目标：`co_wq` 静态库 +（启用 examples 时）`test/xmake.lua` 下的 `co_echo/co_http/...` 可执行文件。

## 运行时执行模型
- `co_wq::Task<T>` 使用 `promise_with_alloc` 统计分配；默认分配器见 `task/task.hpp` (`sys_taskalloc`)。
- 惯例：创建任务后用 `post_to(task, workqueue)` 将协程绑定执行器并 detach；不要手动调用 `resume()`。
- 异步 IO 通过 `fd_workqueue<Lock>` 聚合 reactor + socket/file 构造。TCP/UDP/unix socket 均直接使用 `net/` 根目录下的跨平台实现与 `detail::stream_socket_base` 协作。
- 所有 IO awaiter继承 `io_waiter_base`，经 `post_via_route()`（或 `callback_wq`) 保证同一资源回调顺序。
- 定时器：创建 `Timer_check_queue`（通常见 `test/syswork.cpp` 中的全局实例），使用 `post_delayed_work()` 或 `delay_ms()`；外部需要周期调用 `tick_update()` 或安排自唤醒。

## 关键目录惯例
- `task/`：`workqueue.hpp` 定义 `worknode` intrusive list；调试状态可能调用 `wq_debug_check_func_addr` 弱符号。
- `sync/`：`semaphore.hpp` / `timer.hpp` / `inotify.hpp` 等均返回自定义 awaiter，注意它们通过 `cancel_waiter` 处理超时。
- `io/`：`io_serial.hpp` 提供串行化 helper；`callback_wq.hpp` 负责 per-owner FIFO 回调；`file_io.hpp` 采用 two-phase awaiter 模式。
- `net/`：跨平台 header 直接位于 `net/` 根目录，`tls.hpp` 基于 OpenSSL 组合 TCP socket，实现 handshake + send/recv awaiter。
- `test/`：示例程序演示常用组合。`test/syswork.*` 暴露 `get_sys_workqueue`/`get_sys_timer` 供 demo 使用。

## 常见模式与注意事项
- **生命周期**：所有被 post 的 `worknode` 必须在完成前保持有效；通常由 promise 继承 `worknode`。
- **串行化 IO**：socket、file、TLS awaiter通过 `serial_queue` 保证单通道顺序；完成后需调用 `serial_release`（在 helper 中自动处理）。
- **回调路由**：IO awaiter将 `route_ctx` 设为 `callback_wq`，以避免多线程执行器导致的竞态。
- **超时处理**：`SemReqTimeoutAwaiter` / `NotifyReqTimeoutAwaiter` 利用 `Timer_check_queue` 的 `TimeoutNode`，只有在 `armed` 标志仍为真时才 resume，防止竞态。
- **USB**：`io/usb_device.hpp` 启用 `USING_USB` 后可用；调用 libusb 同步 API，需要调用者自行放入后台线程或注意阻塞。
- **示例执行**：示例任务通过 `post_to` 投递到 `get_sys_workqueue()`；`sys_wait_until` 轮询标志等待完成。
- **HTTP 代理日志**：`co_http_proxy` 默认将调试级日志写入 `logs/proxy.log`（启动时自动截断）并同步到控制台，可用 `--log-file` 覆盖路径、`--log-append/--no-log-truncate` 关闭截断、`--no-verbose`/`--quiet` 下调日志级别；隧道两端的 EOF/错误会在日志中标明具体流向（`client->upstream` 等）。
- **代理探针脚本**：`tools/proxy_probe.py` 执行 HTTP/HTTPS/CONNECT 测试时，会对每次失败立即打印 `[failure] target=... reason=... local_port=... latency=...`，同时在汇总统计中保留最多 10 条样本，便于与代理日志或抓包结果对照。

## 推荐工作流
- 添加新 awaiter 时参考 `io/file_io.hpp` 的 two-phase 模式：尝试->注册等待->复用同一节点。
- 引入新网络原语需在 `xmake.lua` 中 gated by `USING_NET/USING_SSL/USING_USB`，并更新对应 includes。
- 若增添示例程序，记得在 `test/xmake.lua` 注册目标并受 `USING_EXAMPLE` 控制。
- 修改构建脚本后运行 `python script/xmk.py build` 以刷新 `compile_commands.json`。

## 调试与验证
- 判断内存泄漏：查看 `task/sys_sta.malloc_cnt/free_cnt`（例如在 demo 中打印）。
- Reactor 问题：确认 `fd_workqueue` 构造时 `add_fd` 成功；`epoll_reactor` 在析构时会移除 fd 并唤醒所有 waiter。
- Windows 构建：在 VS 工具链环境中直接用 `python script/xmk.py build`。
