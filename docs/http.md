# HTTP 模块设计概览

本文记录 `co_wq::net::http` 模块的整体设计、关键组件与常见用法，帮助新加入的开发者快速理解项目结构并扩展相关功能。

## 1. 目标与边界

HTTP 子系统围绕以下目标设计：

1. 提供统一的 HTTP/1.1 与 HTTP/2 协议抽象，屏蔽底层解析库差异。
2. 复用协程执行模型（`workqueue`），保证异步 IO 与应用逻辑的串行语义。
3. 支撑客户端与服务端场景：从命令行工具、反向代理到内嵌 HTTP API。
4. 允许按需裁剪：`USING_NET`、`USING_SSL`、`USING_EXAMPLE` 通过 xmake 控制可选模块。

HTTP3/QUIC 暂未纳入规划；WebSocket 与代理扩展作为独立子模块维护。

## 2. 协议抽象层

### 2.1 `IParser`

- 定义统一接口 `reset/feed/finish`，承载解析状态。
- `Http1Parser` 与 `Http2Parser` 均实现该接口，通过组合模式重用回调逻辑。
- 调用方负责消息对象（`HttpRequest` / `HttpResponse`）生命周期，可复用以避免频繁分配。

### 2.2 消息对象

- 核心类型：`HttpMessage`（公共字段）、`HttpRequest`、`HttpResponse`。
- Header 存储使用无序容器，并在访问接口上做大小写标准化（参考 RFC 7230）。
- `set_header/remove_header/has_header/header` 等操作全部大小写不敏感，通过内部 canonical key 存储实现。

## 3. 客户端会话

### 3.1 `HttpEasyClient`

- 封装连接、TLS、协议协商（ALPN）与请求重试。
- 默认支持 HTTP/2，底层依赖 `nghttp2`；若协商失败或服务器不支持，则自动回退 HTTP/1.1。
- 支持多地址重试：解析出的多个 A/AAAA 记录会在失败时轮询重连。
- 示例参考 `test/co_curl.cpp`，`perform_request` 调用 `HttpEasyClient::request` 在协程中发起请求：

```cpp
net::http::HttpEasyClient client(fdwq);
auto response = co_await client.request(options);
if (response && response->status_code == 200)
  std::printf("%s\n", response->body.c_str());
```

示例通过命令行参数填充 `options`（见 `http_cli.hpp`），构建完成后直接打印响应体。

### 3.2 `Http2ClientSession`

- 面向高并发场景，允许单连接多 stream 并发。
- 管理 `nghttp2_session` 生命周期及回调，暴露流级别的发送/接收 awaiter。

## 4. 服务端组件

### 4.1 `HttpRouter`

- 集成路由匹配、静态文件、代理功能。
- 支持中间件链（pre/post），用于注入鉴权、公共响应头或请求日志。
- 通过 `serve_static` 暴露静态目录，并在 Windows 与 Linux 使用统一的路径规范。

### 4.2 `HttpEasyServer`

- 最小示例可参考 `test/co_http_server.cpp` 与 `test/co_bilibili_ranking.cpp`，以下片段展示基本使用方式：

```cpp
net::http::HttpServerWorkqueue fdwq(get_sys_workqueue(0));
net::http::HttpEasyServer server(fdwq);

server.router().get("/", [](const HttpRequest&) {
  HttpResponse resp;
  resp.set_status(200, "OK");
  resp.set_body("hello\n");
  return resp;
});

HttpEasyServer::Config cfg;
cfg.host = "0.0.0.0";
cfg.port = 8080;

if (!server.start(cfg))
  return 1;

server.wait();
```

- `Config` 支持 HTTP/2、TLS、自定义日志开关；示例工程展示了路由、中间件、静态目录和信号处理的组合写法。

### 4.3 代理模式

- 正向代理：用于 `co_http_proxy`、`co_socks_proxy` 示例。
- 反向代理：支持前缀重写、上游集群，配合 TLS 终止模块工作。
- 日志系统默认写入 `logs/proxy.log`，支持 CLI 参数调整。

## 5. CLI 工具与共享结构

命令行工具（`co_curl`、`co_http_proxy` 等）共享位于 `http_cli.hpp/.cpp` 的解析逻辑：

- `CommandLineOptions`：描述目标 URL、方法、头部、自定义 TLS 参数等。
- `RequestState`：跟踪解析结果与请求体状态。
- `parse_common_arguments`：统一解析 `argc/argv`，填充结构体并处理验证（host、端口、协议）。

`test/co_curl.cpp` 现已完全依赖该通用解析器，其他示例在后续迭代中逐步迁移，确保选项语义一致。

## 6. Header 策略

### 6.1 大小写与规范化

- Header 在内部存储时采用 canonical key（首字母大写、其余小写），所有对外接口执行大小写不敏感比较。
- `HttpMessage` 提供 `set_header/remove_header/has_header/header`，均基于 canonical key。
- `header_exists` 辅助函数用于 `HeaderList` 内的快速判断。

### 6.2 默认值与覆盖

- `cli::build_http_request` 在构造客户端请求时自动注入：
  - `Host`
  - `User-Agent`（默认 `co_wq/co_curl`）
  - `Accept`（默认 `*/*`）
  - `Content-Length`（当存在请求体且未指定 `Transfer-Encoding` 时）
- 若命令行显式传入同名 header，将覆盖默认值。
- `RequestState::drop_content_headers` 针对 `--no-body` 等场景移除 `Content-Length/Type`，避免违反协议。

### 6.3 单元测试

`test/http_headers_test.cpp` 覆盖以下边界：

1. `HttpResponse` header 操作的大小写不敏感行为。
2. `build_http_request` 默认 header 注入及与 `Transfer-Encoding` 的互斥。
3. `drop_content_headers` 在禁用请求体时正确清理相关 header。

测试入口可通过 `xmake` 构建，或在 Windows 平台执行 `python script/xmk.py build` 生成目标后直接运行。

## 7. 运行时与执行模型

- 所有客户端/服务端 IO 通过 `fd_workqueue`/`callback_wq` 保证串行回调，规避多线程竞争。
- Timer 相关功能复用 `Timer_check_queue`，必要时通过 `post_delayed_work` 实现超时控制。
- `task::promise_with_alloc` 跟踪内存使用，便于调试协程泄漏。

## 8. 构建与依赖

- 默认依赖 `llhttp`、`nghttp2`、`OpenSSL`（Windows 集成 wepoll 子模块）。
- `python script/xmk.py build` 会执行以下步骤：
  1. `xmake f -m releasedbg --USING_EXAMPLE=y`
  2. 生成/更新 `compile_commands.json`
  3. 安装产物到 `install/`
- 清理命令：`python script/xmk.py clean`，可附带 `--remove-global-cache`。

## 9. 常见扩展点

1. **新增 awaiter**：参考 `io/file_io.hpp` two-phase 模式，注册等待节点并确保完成后 `serial_release`。
2. **引入新协议特性**：在 `http/common` 层添加能力（例如 gzip），再由 `HttpRouter`、客户端封装各自集成。
3. **新增 CLI 工具**：复用 `http_cli` 解析，写入 `test/xmake.lua` 以便示例编译。

## 10. Roadmap

- 完成其它示例向共享 CLI 的迁移，减少重复解析逻辑。
- 引入自动化回归脚本，结合 `tools/proxy_probe.py` 做端到端验证。
- 评估 HTTP/2 Push、WebSocket TLS 透传等增强特性。

## 11. 与 libhv 的差异

- **执行模型**：co_wq 以协程驱动的 `workqueue` 为核心，所有 awaiter 与 HTTP 组件默认运行在单线程串行执行器上；libhv 采用线程池 + 事件循环，更偏向回调式编程。
- **协议抽象**：co_wq 将 HTTP/1 与 HTTP/2 解析统一到 `IParser` 接口，`HttpEasyServer/Client` 屏蔽底层差异；libhv 中 HTTP/1 与 HTTP/2 模块相对独立，需要手工选择 API。
- **可裁剪性**：co_wq 的构建选项（`USING_NET/USING_SSL/USING_EXAMPLE` 等）由 `xmake` 与 `script/xmk.py` 集成，便于按需启用；libhv 更侧重全家桶式构建，裁剪需自定义 CMake 选项。
- **示例覆盖**：本文档提到的 `co_http_server.cpp`、`co_bilibili_ranking.cpp`、`co_curl.cpp` 等示例涵盖 HTTP 路由、静态资源、TLS 与多路复用测试场景，有助于验证协程封装在真实业务中的可行性。

如有疑问，可在 `docs/chat_stability_protocol.md` 留存的流程基础上补充实践记录，或直接联系当前维护者。
