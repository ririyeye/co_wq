# co_wq

协程友好的工作队列与网络原语集合，基于 C++20 coroutine 实现跨平台（Linux/Windows）任务调度、同步与 IO 封装。本库核心围绕 `co_wq::workqueue`、`co_wq::Task` 以及一组可组合的同步/网络 awaiter，帮助你用接近同步的代码风格组织异步逻辑。

## 核心特性
- **零依赖、轻量化**：纯头文件实现，除标准库外不依赖第三方组件。
- **可插拔执行器**：`workqueue<lock>` 支持自定义锁类型，适配不同并发模型。
- **丰富 Awaiter**：信号量、定时器、串行化 IO、TCP/UDP/UDS 套接字等 awaiter 开箱即用。
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
脚本默认以 `releasedbg` 模式启用网络/TLS/USB 模块与全部示例，执行流程：
1. 以 `releasedbg` 模式写入配置，将输出目录设置为 `build/`；
2. 根据“full”预设打开 `USING_NET/USING_SSL/USE_BUNDLED_LLHTTP/USING_USB/USING_EXAMPLE`；
3. 生成 `compile_commands.json`；
4. 构建并安装产物到 `install/`。

若仅需最小核心（仅 `task/` 与 `sync/`），可执行：

```bash
$ bash script/xmk-local.sh --core
```

使用 `--help` 查看可用模式说明。

### Windows（MSVC 工具链，本机）
在包含 Visual Studio 工具链的 PowerShell 提示符中执行：
```powershell
PS> .\script\xmsvc.bat
```
批处理默认执行与 `xmk-local.sh` 相同的“full”配置（启用网络/TLS/USB/示例），流程包括：
1. 以 `releasedbg` 模式写入配置并使用 `build/` 目录；
2. 生成 `compile_commands.json`；
3. 构建并安装到 `install/`。

若只需核心库，可附加 `--core` 参数：

```powershell
PS> .\script\xmsvc.bat --core
```

同样可通过 `--help` 查看模式说明。

### Windows（Wine + MSVC 工具链）
参考 `script/xmk-wine-msvc.sh`：
```bash
$ bash script/xmk-wine-msvc.sh
```
脚本默认以 `releasedbg` 模式仅构建核心库（关闭 `USING_NET/USING_SSL/USING_USB/USING_EXAMPLE`），步骤包括：
1. 写入 `--sdk=/opt/toolchain/msvc-wine/msvc` 等配置并使用 `build/` 作为输出目录；
2. 构建并安装 `co_wq` 静态库；
3. 生成 `compile_commands.json` 给 IDE 使用。

若需要完整网络/TLS/USB + 示例，可追加 `--full`：

```bash
$ bash script/xmk-wine-msvc.sh --full
```

脚本也提供 `--help` 查看全部参数说明。

> ℹ️ 首次执行任一构建脚本时，xmake 会通过内置包管理器下载依赖（如 llhttp、openssl、libusb），请确保网络连通。

> 💡 编译卡住时可运行 `wineserver -k` 重启 Wine。详细工具链搭建请参考脚本内联注释。

### 清理构建缓存
```bash
$ bash script/clean.sh
```
该脚本会移除 `.xmake/`、`~/.xmake/`、`build/`、`install/` 及 `.cache/`。

Windows 可执行：
```powershell
PS> .\script\clean.bat
```
会同样清理工作区内的构建产物，并额外尝试删除 `%USERPROFILE%\.xmake` 缓存。

## 手动使用 xmake
默认配置仅输出核心组件（`task/` 与 `sync/`），无额外第三方依赖。可通过下表开关选择性启用模块：

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `USING_NET` | `false` | 启用网络相关头文件与依赖（TCP/UDP/TLS/WebSocket 等） |
| `USING_SSL` | `false` | 链接 OpenSSL，暴露 TLS/DTLS/socket TLS awaiter |
| `USE_BUNDLED_LLHTTP` | `true` | 使用内置 `llhttp` 包处理 HTTP/WebSocket 升级（仅在 `USING_NET=y` 时生效） |
| `USING_USB` | `false` | 启用基于 libusb 的 USB 协程封装 |
| `USING_EXAMPLE` | `false` | 构建 `test/` 目录下示例程序（需要配合 `USING_NET=y`） |

最小化构建示例：

```bash
xmake f -y -m releasedbg -o build
xmake -vD
xmake install -o install
```

启用全部网络 + 示例的配置可手动写为：

```bash
xmake f -y -m releasedbg -o build \
  --USING_NET=y --USING_SSL=y --USE_BUNDLED_LLHTTP=y \
  --USING_USB=y --USING_EXAMPLE=y
xmake -vD
xmake install -o install
```

`co_wq` 目标为静态库（包含至少一个占位源文件 `task/empty.cpp` 以便安装流程），公共头文件通过 `add_includedirs(..., {public = true})` 暴露。

## TLS/SSL 支持

`co_wq` 内置 OpenSSL 驱动的 `net::tls_socket`。核心构建默认不启用 SSL，若需要 TLS/DTLS，可在配置阶段添加 `--USING_NET=y --USING_SSL=y`（或直接运行 `script/xmk-local.sh`/`xmsvc.bat` 默认的 “full” 模式）。此外还提供 `net::dtls_socket` 封装，基于 UDP (`net::udp_socket`) 组合实现 DTLS 握手与读写 awaiter，适用于低时延场景（需保证底层 UDP 已绑定/连接到对端）。

## USB IO 支持

若需在协程内访问 USB 设备，可在配置时开启 `USING_USB`（核心配置默认关闭，以避免拉取 libusb）：

```bash
xmake f -y --USING_USB=y
xmake build co_wq
```

启用后，头文件 `io/usb_device.hpp` 暴露基于 [libusb](https://libusb.info/) 的 RAII 封装：

- `co_wq::net::usb_context`：管理 `libusb_context` 生命周期，可选设置调试级别；
- `co_wq::net::usb_device<lock>`：结合 `workqueue<lock>` 管理设备句柄，提供串行化的协程方法：
  - `bulk_transfer_in/out()`：返回成功传输的字节数，错误时返回 libusb 负值；
  - `control_transfer()`：直接返回 libusb 状态码；
  - 接口辅助：`claim_interface` / `release_interface` / `detach_kernel_driver` 等。

示例（假设运行在 `Work_Promise<SpinLock, int>` 协程内）：

```cpp
#include "usb_device.hpp"

co_wq::net::usb_context ctx;
auto dev = co_wq::net::usb_device(exec, ctx, libusb_open_device_with_vid_pid(ctx.native_handle(), vid, pid));
co_await dev.claim_interface(0);
std::array<std::uint8_t, 64> buf{};
int received = co_await dev.bulk_transfer_in(0x81, buf.data(), buf.size(), 1000);
```

> ⚠️ 当前实现使用 libusb 的同步 API，会在协程所在的工作线程内阻塞直到操作完成；如果需要完整的非阻塞事件驱动，可在此封装基础上扩展 libusb Transfer + reactor 集成。

若已启用 `USING_EXAMPLE=y`，可构建示例程序 `co_usb`：

```bash
xmake f -y --USING_EXAMPLE=y --USING_USB=y
xmake build co_usb
xmake run co_usb --help
```

默认会列出当前总线上的设备，可通过 `--vid/--pid` 指定目标并尝试执行一次标准控制传输（读取设备描述符）。

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
xmake f -y -m releasedbg -o build \
  --USING_NET=y --USING_SSL=y --USE_BUNDLED_LLHTTP=y --USING_EXAMPLE=y
xmake build co_http
xmake run co_http --host 0.0.0.0 --port 8443 --cert certs/server.crt --key certs/server.key
```

> ✅ 已执行 `script/xmk-local.sh` 或 `script/xmsvc.bat` 默认的 “full” 模式时，可跳过上述 `xmake f` 配置步骤。

随后可通过浏览器或 `curl` 访问：

```bash
curl -k https://127.0.0.1:8443/
```

> ⚠️ 自签证书仅用于测试；生产环境请使用受信任的 CA 证书，并在服务端配置 `SSL_CTX_load_verify_locations` 等细节。

## 示例程序
以下示例假设已通过 `script/xmk-local.sh`（默认 full）或手动执行 `xmake f` 使 `USING_NET=y --USE_BUNDLED_LLHTTP=y --USING_EXAMPLE=y`（以及按需开启 `USING_SSL/USING_USB`）。完成配置后，可尝试 `test/echo.cpp` 内的 TCP/UDP echo 服务器与客户端：
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

### HTTP Forward Proxy 示例

`co_http_proxy` 展示了如何基于 `llhttp` 解析器实现带 CONNECT 支持的正向代理。示例在解析请求后会重新构造 origin-form 请求并回源，同时为 `CONNECT` 方法建立双向隧道。

```bash
xmake run co_http_proxy --host 127.0.0.1 --port 18080
```

然后通过 curl 验证：

```bash
curl --proxy http://127.0.0.1:18080 http://example.com
curl --proxy http://127.0.0.1:18080 https://example.com --proxytunnel
```

程序默认开启调试日志（输出到 `stderr`），包含时间戳、线程 ID 以及关键事件，例如“收到请求”“上游连接结果”“CONNECT 隧道关闭”等。可使用标准重定向将日志落地：

```bash
xmake run co_http_proxy --host 127.0.0.1 --port 18080 2>proxy.log
```

如需关闭日志，可在源码中将 `g_debug_logging` 初始化为 `false`，或依据自身需求扩展命令行开关。

  ### WebSocket Echo 示例

  `net/websocket.hpp` 提供基于 llhttp 的握手辅助与帧收发工具函数，`co_ws` 示例展示了如何在协程中构建 WebSocket 服务：

  ```bash
  xmake run co_ws --host 0.0.0.0 --port 9000
  ```

  可使用浏览器或常见客户端（如 [`wscat`](https://github.com/websockets/wscat)）连接并发送文本/二进制消息，服务器会自动回显：

  > ℹ️ 自 v0.x 修复后，服务器对在握手阶段主动断开的客户端会静默忽略，无额外 400 日志；握手成功后若对端复位连接，会记录一条 “peer closed connection” 信息便于排查。

  ```bash
  wscat -c ws://127.0.0.1:9000
  ```

  示例涵盖：
  - 通过 `websocket::accept` 完成 HTTP Upgrade 握手并可选匹配子协议；
  - 使用 `websocket::read_message` 自动处理分片、Ping/Pong 与 Close 帧；
  - 借助 `websocket::send_text` / `send_binary` / `send_close` 回写响应。

### Unix Domain Socket 示例

`co_uds` 展示了基于 `unix_listener/unix_socket` 的本地 IPC echo 逻辑：

```bash
xmake f -y -m releasedbg -o build --USING_NET=y --USING_EXAMPLE=y
xmake build co_uds
xmake run co_uds --path /tmp/co_wq_uds.sock --message "ping uds"
```

运行上述命令会同时启动服务器与客户端，客户端发送一条消息后退出。若只想常驻服务器，可执行：

```bash
xmake run co_uds --server --path /tmp/co_wq_uds.sock --max-conn 0
```

路径以 `@` 开头时会切换到 Linux 抽象命名空间（不会在文件系统生成条目），例如 `--path @co_wq_demo`。

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
- `detail::stream_listener_base` / `detail::stream_socket_base` / `detail::datagram_socket_base`：Linux 网络监听/流式/数据报基类，统一封装 fd 生命周期、串行化 send/recv 与 reactor 交互，现已补充中文 Doxygen 注释，方便生成参考文档。
- `tcp_socket<lock, Reactor>`：非阻塞 TCP 封装，支持：
  - `connect(host, port)`：异步连接；
  - `recv(buf, len, full)` / `send(buf, len, full)`：单次或聚合收发；
  - `recv_vectored` / `send_vectored`：`iovec` 批量操作；
  - `shutdown_tx()` / `close()` / `native_handle()`；
  - 状态查询 `rx_eof()`、`tx_shutdown()`。
- `tcp_listener<lock, Reactor>`：监听/接受连接，提供 `bind_listen()` 与 `accept()` awaiter。
- `udp_socket<lock, Reactor>`：支持 `send_to/recv_from`、可选 `connect()`。
- `unix_socket<lock, Reactor>` / `unix_listener<lock, Reactor>`：协程化 Unix Domain Stream 套接字（Linux），
  支持文件路径或以 `@` 开头的抽象命名空间，API 与 TCP 版本保持一致（`connect/recv/send`、`bind_listen/accept`）。
- Windows 目录下提供 IOCP 版本，实现接口一致，便于跨平台编译：
  - `net/win/tcp_listener.hpp` / `tcp_socket.hpp` 现补充了详尽的中文 Doxygen 注释，便于生成跨平台 API 文档；
  - `tcp_socket::recv` 在缺省 Reactor 场景下会自动回退到工作队列投递，避免协程悬挂。

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


