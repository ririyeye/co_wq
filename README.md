# co_wq

协程友好的工作队列与网络原语集合，基于 C++20 coroutine 实现跨平台（Linux/Windows）任务调度、同步与 IO 封装。本库核心围绕 `co_wq::workqueue`、`co_wq::Task` 以及一组可组合的同步/网络 awaiter，帮助你用接近同步的代码风格组织异步逻辑。

## 核心特性
- **零依赖、轻量化**：纯头文件实现，除标准库外不依赖第三方组件。
- **可插拔执行器**：`workqueue<lock>` 支持自定义锁类型，适配不同并发模型。
- **丰富 Awaiter**：信号量、定时器、串行化 IO、TCP/UDP 套接字等 awaiter 开箱即用。
- **跨平台网络栈**：Linux 采用 `epoll`，Windows 通过 Wine+MSVC 配置使用 IOCP 封装。
- **工具完善**：提供 `script/` 下的 xmake 构建脚本，自动生成 `compile_commands.json` 便于 IDE 使用。

## 目录总览
- `task/`：协程任务、promise、工作队列等基础设施。
- `sync/`：信号量、定时器、inotify 等同步原语。
- `io/`：反应器、串行化调度、文件与网络 IO 封装。
- `net/`：面向 Linux/Windows 的 TCP/UDP 高层接口。
- `test/`：示例程序（如 `echo` 服务），需启用 `USING_EXAMPLE` 构建选项。
- `script/`：常用构建脚本（本地、Wine-MSVC、清理）。
- `xmake.lua`：项目构建入口，包含平台/可选功能开关。
- `third_party/llhttp/`：HTTP 解析器子模块（来源于 [nodejs/llhttp](https://github.com/nodejs/llhttp.git)）。
- `script/gen-selfsigned-cert.sh`：快速生成 TLS 自签证书的辅助脚本。

## 快速开始

### 依赖
- C++20 编译器（GCC 12+/Clang 15+/MSVC 19.36+）
- [xmake](https://xmake.io/) 2.7+

### Linux/WSL 构建
```bash
$ bash script/xmk-local.sh
```
脚本会执行：
1. `xmake g --network=private` 初始化配置；
2. 以 `releasedbg` 模式开启 examples；
3. 生成 `compile_commands.json`；
4. 构建并安装产物到 `install/`。

### Windows（Wine + MSVC 工具链）
参考 `script/xmk-wine-msvc.sh`：
```bash
$ bash script/xmk-wine-msvc.sh
```
脚本会：
1. 设置 `--sdk=/opt/toolchain/msvc-wine/msvc`；
2. 切换到 `windows x64` 目标；
3. 明确构建 `co_wq` 静态库并安装；
4. 生成 `compile_commands.json` 给 IDE 使用。

> 💡 编译卡住时可运行 `wineserver -k` 重启 Wine。详细工具链搭建请参考脚本内联注释。

### 清理构建缓存
```bash
$ bash script/clean.sh
```
该脚本会移除 `.xmake/`、`~/.xmake/`、`build/`、`install/` 及 `.cache/`。

## 手动使用 xmake
`xmake.lua` 提供两个可选开关：

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `USING_NET` | `true` | 启用网络相关头文件与依赖 |
| `USING_EXAMPLE` | `false` | 构建 `test/` 下示例程序 |
| `USING_SSL` | `true` | 构建时链接 OpenSSL 并暴露 TLS 支持 |

示例命令：
```bash
xmake g --network=private
xmake f -y -m releasedbg --USING_NET=y --USING_EXAMPLE=n
xmake -vD
xmake install -o install
```

`co_wq` 目标为静态库（包含至少一个占位源文件 `task/empty.cpp` 以便安装流程），公共头文件通过 `add_includedirs(..., {public = true})` 暴露。

## TLS/SSL 支持

`co_wq` 内置 OpenSSL 驱动的 `net::tls_socket`。默认构建脚本已开启 `USING_SSL`，若需关闭可传入 `xmake f --USING_SSL=n`。此外还提供 `net::dtls_socket` 封装，基于 UDP (`net::udp_socket`) 组合实现 DTLS 握手与读写 awaiter，适用于低时延场景（需保证底层 UDP 已绑定/连接到对端）。

### 快速生成测试证书

使用脚本生成自签证书：

```bash
./script/gen-selfsigned-cert.sh -o certs -CN localhost
```

输出目录将包含：

- `certs/server.key`：RSA 私钥
- `certs/server.crt`：自签证书

### 启动 TLS HTTP 示例

编译并运行：

```bash
xmake f -y --USING_EXAMPLE=y
xmake build co_http
xmake run co_http --host 0.0.0.0 --port 8443 --cert certs/server.crt --key certs/server.key
```

随后可通过浏览器或 `curl` 访问：

```bash
curl -k https://127.0.0.1:8443/
```

> ⚠️ 自签证书仅用于测试；生产环境请使用受信任的 CA 证书，并在服务端配置 `SSL_CTX_load_verify_locations` 等细节。

## 示例程序
启用 `USING_EXAMPLE=y` 后，可尝试 `test/echo.cpp` 内的 TCP/UDP echo 服务器与客户端：
```bash
xmake run echo --both --host 127.0.0.1 --port 12345
```
示例中演示了：
- `net::tcp_listener` 接受连接、`net::tcp_socket` 协程化收发；
- `post_to()` 将协程投递到主工作队列；
- `net::udp_socket` 的 `send_to/recv_from` awaiter；
- 跨平台信号处理、统计信息输出。

### HTTP JSON 测试

`co_http` 示例引入了 [nlohmann/json](https://github.com/nlohmann/json) 以处理 JSON 负载，并新增 `POST /echo-json` 端点回显请求体。

1. 以明文模式运行：

  ```bash
  xmake run co_http --host 0.0.0.0 --port 8080
  ```

2. 发送 JSON 请求并观察响应：

  ```bash
  curl -X POST http://127.0.0.1:8080/echo-json \
      -H 'Content-Type: application/json' \
      -d '{"message":"hello co_wq"}'
  ```

  服务器会返回 `application/json`，包含请求方法、路径以及原始 payload：

  ```json
  {"status":"ok","method":"POST","path":"/echo-json","request":{"message":"hello co_wq"},"request_content_type":"application/json"}
  ```

## API 文档

### 协程任务与执行器（`task/`）
- `co_wq::Task<T, P, Alloc>`：泛型协程返回类型，默认结合 `promise_with_alloc` 提供定制分配器，支持 `detach()/release()/operator co_await()`。
- `co_wq::Promise_base` / `Promise<T>` / `Promise<void>`：统一的协程 promise 基类，内置 `previous_awaiter` 链接以便返回上游协程，支持异常透传（`USE_EXCEPTION`）。
- `co_wq::promise_with_alloc<BasePromise, Alloc>`：对 promise 进行自定义 `operator new/delete` 包装，用于统计 `sys_sta.malloc_cnt/free_cnt`。
- `co_wq::Work_Promise<lock, T>`：继承 `Work_promise_base` 与 `Promise<T>`，自动把协程 resume 投递到指定 `workqueue<lock>`。
- `co_wq::post_to(Task<T, Work_Promise<lock, T>, Alloc>&, workqueue<lock>&)`：将协程任务绑定执行队列并启动。

### 工作队列（`task/workqueue.hpp`）
- `co_wq::workqueue<lock>`：核心执行器，维护 `list_head` 链表。
  - `post(worknode&)` / `post(list_head&)`：单个或批量投递任务，支持调试 hook `wq_debug_check_func_addr`。
  - `work_once()`：取出并执行一个任务，返回执行次数。
  - `add_new_nolock(worknode&)`：在已持锁情况下入队，避免重复加锁。
  - `trig` 回调：队列非空时触发外部事件源（例如唤醒线程）。
  - `lock()/unlock()`：直接暴露内部锁，便于高级用法。

### 同步原语（`sync/`）
- `Semaphore<lock>`：计数信号量，支持 `acquire`、`try_acquire`、`release`，内部基于 `workqueue` 投递唤醒。
  - `wait_sem(sem)`：挂起直到获取令牌。
  - `wait_sem_for(sem, timer_q, timeout_ms)`：支持超时，返回 `bool` 表示是否成功。
  - `wait_sem_try(sem)` / `wait_sem_forever(sem, timer_q)`：非阻塞与无限等待封装。
  - `cancel_waiter()`：在超时/取消路径下正确清理等待者。
- `Timer_check_queue<lock>`：配对堆实现的定时任务队列。
  - `post_delayed_work(node, ms)`：注册定时任务。
  - `tick_update()`：触发检查，通常由外部定时器驱动。
  - `cancel(node)`：安全地移除定时节点。
- `DelayAwaiter`：协程级延时 awaiter，`co_await delay_ms(queue, ms)` 恢复后无返回值。
- `Notify<lock>`（`inotify.hpp`）：Linux inotify 事件封装，配合 `NotifyReqAwaiter` 和 `wait_inotify_*` awaiter 使用。

### IO 模块（`io/`）
- `callback_wq<lock>`：保证同一拥有者回调 FIFO 执行的工作队列路由器，常与 `io_waiter_base` 搭配。
- `io_serial`：提供 `serial_queue` 与 `serial_acquire` 等工具，保证同一资源的串行访问。
- `fd_workqueue<lock, Reactor>`：文件描述符执行器，管理底层 reactor、提供 `make_tcp_socket()`、`adopt_tcp_socket()`、`make_udp_socket()` 等。
- `epoll_reactor<lock>` / `iocp_reactor<lock>`：平台化事件循环后端，分别封装 `epoll` 与 IOCP，实现 `add_fd/remove_fd/add_waiter[_custom]` 接口。

### 网络原语（`net/`）
- `tcp_socket<lock, Reactor>`：非阻塞 TCP 封装，支持：
  - `connect(host, port)`：异步连接；
  - `recv(buf, len, full)` / `send(buf, len, full)`：单次或聚合收发；
  - `recv_vectored` / `send_vectored`：`iovec` 批量操作；
  - `shutdown_tx()` / `close()` / `native_handle()`；
  - 状态查询 `rx_eof()`、`tx_shutdown()`。
- `tcp_listener<lock, Reactor>`：监听/接受连接，提供 `bind_listen()` 与 `accept()` awaiter。
- `udp_socket<lock, Reactor>`：支持 `send_to/recv_from`、可选 `connect()`。
- Windows 目录下提供 IOCP 版本，实现接口一致，便于跨平台编译。

## 设计与最佳实践
- **锁策略**：默认锁类型为 `SpinLock`，如需与多线程配合可传入自定义互斥量（需满足 `lockable` 概念）。
- **调试辅助**：在 Debug 模式下，`workqueue` 会检测函数指针是否落在常见的“毒值”范围，尽早暴露未初始化问题。
- **异常处理**：定义 `USE_EXCEPTION=1` 后，`Promise` 可捕获并重新抛出协程内异常。
- **资源管理**：所有 awaiter 使用 intrusive 链表节点，避免额外分配；注意协程生命周期需长于在途任务。

## 常见问题
- **找不到网络相关头文件**：确保 `xmake f --USING_NET=y`。
- **示例未构建**：手动打开 `USING_EXAMPLE` 选项，或使用 `script/xmk-local.sh`。
- **Wine 编译失败**：确认脚本中的 MSVC SDK 路径正确，并提前安装 `msvc-wine` 项目依赖。
- **自定义锁死循环**：若自定义锁实现使用阻塞等待（如 `std::mutex`），请确保在多线程环境中不会阻塞 reactor 线程。

## 许可证
（请在此添加或确认项目的实际许可证信息。）


