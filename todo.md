# QUIC 多连接支持改造 TODO

1. QUIC Dispatcher 设计评审 ✅
   - 明确目标：单 UDP fd 上支持多个 QUIC 连接，集中收发并按 Connection ID 路由。
   - 调研 OpenSSL QUIC API：`SSL_handle_events`、`SSL_provide_quic_data`、`SSL_fetch_quic_write_buf`、`SSL_get0_connection_id` 等使用方式。
   - 形成架构草图：`QuicDispatcher` 管理 UDP I/O、连接注册表、收/发队列、回调接口。

2. Dispatcher 核心组件实现
   - 在 `net/` 新增 `quic_dispatcher.hpp/.cpp`，封装：
     - 单线程 `recvfrom` 循环（挂在 `fd_workqueue` 上）。
     - 数据报解析并根据 DCID 调用对应 `SSL` 的 `SSL_provide_quic_data`→`SSL_handle_events`。
     - 上行发送：集中读取连接待发送 buffer，再调用 `sendto`。
   - 定义连接描述结构，保存 `SSL*`、所属执行器、回调句柄等元信息。

3. Listener 与会话接入改造 ✅
   - ✅ `run_quic_server`：只创建一个 UDP socket 和 dispatcher，循环 `SSL_accept_connection` 得到新 `SSL` 后注册到 dispatcher。
   - ✅ `handle_quic_server_session`：通过 dispatcher 驱动 handshake/IO（当前仍在示例内部，尚未提升到通用库）。
   - ✅ 客户端路径已通过 dispatcher 验证交互（`co_quic` chat 模式跑通并可正常结束）。

4. QUIC Socket 抽象重构（完成）
   - ✅ 库级 `net::quic_socket` 内建 dispatcher，默认附加自定义 BIO 并驱动 UDP I/O。
   - ✅ 提取 `quic_dispatcher`/`quic_accept_awaiter` 至 `net/quic_dispatcher.hpp`，示例改用公共实现。
   - ✅ 将示例内 `QuicServerSession` 抽出为 `net/quic_stream_session.hpp`，统一握手/收发逻辑。
   - 待移植 dispatcher IO 到公共封装：
      - ✅ 移除 `BIO_new_dgram(fd)`，实现 dispatcher 专用 BIO：对外仍走 BIO 接口，内部通过 channel 管道调用 `SSL_inject_net_dgram` 并分发待发送数据。
         - ✅ 在 dispatcher 侧维护 per-connection 入出站队列与唤醒，BIO 的 `read/write` 映射到 channel，socket 通过 `drive_events`/`wait_io` 与 dispatcher 协作。
         - ✅ `channel_bio` 自定义 BIO 骨架与 dispatcher I/O 已贯通。
         - ✅ 借鉴 OpenSSL 非阻塞示例，事件循环使用 `SSL_handle_events` + `SSL_net_{read,write}_desired` + `SSL_get_event_timeout` 信息驱动 channel 唤醒。
            - ✅ `quic_dispatcher.hpp` 引入 `channel` 注册与数据队列骨架。
      - ✅ `recv/send` awaiter 走 dispatcher 事件通知并复用 `serial_queue`，确保完成后正确 release/flush。
   - ✅ 近期目标 1：让 `quic_stream_session` 的收发 awaiter 直接复用 `basic_tls_socket` 的串行队列封装，减少双份实现。
      - ✅ 近期目标 2：demo 中 `quic_socket` 改为通过 `channel_bio` 绑定 dispatcher，客户端路径验证通过后再推广到库层。
   - ✅ 兼容模式不再保留：`quic_socket` 统一走 dispatcher，客户端示例注释更新同步。

### 后续性能优化设想

- 多执行器拆分：QUIC/TLS/DTLS/HTTP socket 当前默认与 reactor 共享同一 workqueue；后续可在稳定后引入“会话执行器”抽象，允许为每个连接指定独立的 executor，实现 dispatcher 单线程 + 会话多线程。
- 统一串行层改造：在 `basic_tls_socket`、`quic_stream_session` 等处抽象 `_exec` 绑定流程，确保 `serial_queue` 与 `callback_wq` 能安全跨 workqueue 投递，补齐生命周期与线程安全。
- API 设计：为上层组件（HTTP server/router、websocket、代理）提供可选的 executor 配置项，便于更灵活的线程调度策略。
- 测试计划：在性能测试阶段评估拆分 executor 对延迟与吞吐的收益，再决定是否作为默认行为或可选优化开关。

5. 资源与生命周期管理
   - Dispatcher 管理连接注册/注销，确保关闭时：
     - 解除所有 `SSL` 引用，flush 待发送队列，关闭 UDP fd。
   - 会话结束流程：通知 dispatcher、等待 pending 数据完成、释放 `SSL`。

6. 测试计划
   - 编写单元/集成测试：
     - 多客户端并发连接、发送/接收验证（至少 3+ 个连接）。
     - 模拟连接重置/超时/迁移场景，确认 dispatcher 正常回收。
   - CLI 示例：`co_quic` 启用 dispatcher 后保持单连接兼容模式，提供压力测试脚本。

7. 调试记录（2025-10-16）
    - 现象：服务端空闲时持续刷日志、握手完成后立即退出交互、服务器 Ctrl+C 时客户端报 `Bad file descriptor`。
    - 排查步骤：
       - 给 `QuicAcceptAwaiter` 增加 5s 节流和掩码去重，结合 `-v` 观察定时唤醒；
       - 在 `socket_to_stdout_task` 打印 `SSL_get_shutdown()`、错误栈，定位 `protocol is shutdown` 和 `quic network error`；
       - 应用层 `QuicServerSession::recv/send_all`、库层 `net::tls.hpp` 中引入 `SSL_ERROR_ZERO_RETURN`/`SSL_R_QUIC_NETWORK_ERROR` 的细分逻辑，多次在客户端重现+分析。
    - 结论：
       - OpenSSL QUIC 默认流可能在无数据时返回 0，但需等到 `SSL_RECEIVED_SHUTDOWN` 置位才视为 EOF；
       - `quic network error`/`-EBADF` 需当作远端已断开而非硬错误，清理错误栈后优雅退出；
       - 日志节流必需，否则 200ms 超时轮询会淹没有效信息。
    - 注意事项：
       - 调试时确保 `ERR_clear_error()` 在将错误视作暂态后调用，避免旧错误串扰；
       - 使用 `shutdown_flags` 辅助定位 QUIC 状态机；
       - 收发协程调用 `SSL_handle_events` 前保持 dispatcher 驱动一致性，并在等待前更新 `wait_mask`。
       - 参考官方示例的 `handle_io_failure()` 模式，继续细化 `SSL_ERROR_ZERO_RETURN`、`SSL_ERROR_SSL` 的处理与队列唤醒路径，减少误判断连。
    - ✅ 2025-10-17：dispatcher 增加管道唤醒，Ctrl+C 或自定义取消可立刻恢复正在等待的 accept/IO。
    - ✅ 2025-10-17：调研 OpenSSL 3.5.1 QUIC API，仅提供 `SSL_inject_net_dgram`/`SSL_net_{read,write}_desired` 等入口；确认没有公开的 `SSL_fetch_quic_write_buf`，后续改造需走自定义 BIO。
   - ✅ 2025-10-17：定位客户端握手卡死原因为 dispatcher 的 `wait_with_timeout` 覆盖 handshake awaiter 的自定义回调，导致协程 resume 未触发；注册等待后恢复原函数指针并补充日志验证，重新运行 `script/run_quic_sequence.sh` 握手顺利完成。
   - ✅ 2025-10-18：`co_quic` 增加 `--auto` 模式，客户端/服务端自动往返两轮消息后保持连接待退出，便于流水线验证 dispatcher + awaiter 行为。
   - ✅ 2025-10-18：调整 `callback_wq` drain 流程，跳过已失效 waiter 的 `callback_enqueued` 重置；`script/run_quic_sequence.sh --timeout` 验证握手全程未再出现 “invalid magic” 警告。
   - ⏳ 计划：通过 `co_quic --tls-trace` 触发 `SSL_set_msg_callback`/`OSSL_trace_set_callback` 输出握手与 alert 细节，便于定位 QUIC/TLS 协议交互问题。
