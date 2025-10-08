# Chat Stability Test Protocol

该协议用于 `co_wq` 聊天稳定性测试服务器与 Python 压测客户端之间的双向通信，主要关注 TCP/TLS 大量长连接与固定大小消息的吞吐、内存占用及稳健性。

## 帧编码

所有消息都采用统一的帧头格式：

- 1 字节 `frame_type`
- 4 字节大端序 `payload_length`
- `payload_length` 字节有效载荷

目前约定的 `frame_type`：

| 值 | 含义 | 载荷内容 |
| --- | --- | --- |
| `0x01` | 控制帧 | UTF-8 JSON 文本，描述握手、状态或控制信息 |
| `0x02` | 数据帧 | 二进制消息，首 8 字节为大端序序号，其余为纯负载 |

发送端必须一次写出完整帧，接收端必须循环读取直至得到完整帧或连接关闭。

## 控制消息

控制帧 payload 是带 `type` 字段的 JSON 对象：

### `hello`（客户端→服务器）

```json
{
  "type": "hello",
  "version": 1,
  "room": "room-0001",
  "client_id": "bot-0001",
  "send_plan": 1000,
  "payload_bytes": 512,
  "expect_receive": 1000,
  "server_push": 0,
  "shutdown_after_send": true,
  "wait_timeout_ms": 30000
}
```

字段说明：

- `version`：协议版本，当前固定 `1`。
- `room`：配对房间名。服务器会将同一房间的两个客户端在同一流上互联；每个房间同时最多保留一对客户端。
- `client_id`：客户端自定义标识，用于日志与 `ready` 回包。
- `send_plan`：本连接计划发送的数据帧数量（用于限流与统计）。
- `payload_bytes`：数据帧除序号之外的负载长度，服务器用于校验。
- `expect_receive`：期望收到的转发帧数量（通常等于对端的 `send_plan`）。
- `server_push`：服务器在配对成功后主动向该客户端额外推送的帧数量；用于压测单向推送场景。
- `shutdown_after_send`：若为 `true`，当计划发送与接收均完成时服务器会半关闭连接并发送总结帧。
- `wait_timeout_ms`：可选，等待对端入房的最长时间，默认 30000 毫秒。

### `hello_ack`（服务器→客户端）

服务器在解析握手后立即回应：

```json
{
  "type": "hello_ack",
  "room": "room-0001",
  "status": "waiting"
}
```

当同房间第二个客户端到达并配对成功后，会收到 `ready` 信息。

### `ready`（服务器→客户端）

```json
{
  "type": "ready",
  "room": "room-0001",
  "peer_id": "bot-0002",
  "peer_send_plan": 1000,
  "peer_payload_bytes": 512,
  "server_push": 0
}
```

客户端在收到 `ready` 后才应开始发送数据帧。

### `summary`（服务器→客户端）

在计划收发完成、连接主动关闭或出现错误时发送：

```json
{
  "type": "summary",
  "room": "room-0001",
  "sent_from_client": 1000,
  "forwarded_to_peer": 1000,
  "received_from_peer": 1000,
  "server_generated": 0,
  "duration_ms": 1234,
  "peer_active": false,
  "error": null
}
```

### `error`（服务器→客户端）

当解析失败或违反约束时发送，并随后关闭连接。

### `done`（客户端→服务器，可选）

客户端在完成自身 `send_plan` 后可发送：

```json
{"type": "done", "sent": 1000}
```

服务器会记录并在总结里体现。

## 数据帧结构

`frame_type` 为 `0x02` 时 payload 结构如下：

1. 8 字节大端序无符号整数，表示序号 `seq`。
2. `payload_bytes` 字节原始数据，通常由客户端填充重复字节以控制负载大小。

服务器透传完整 payload（包括序号）给对端，且会统计：

- 实际收到的帧数是否与 `send_plan` 一致；
- 转发给对端的帧数与 `expect_receive` 是否匹配；
- 每帧负载长度是否等于 `payload_bytes`。

## 典型交互流程

1. 客户端连接（TCP 或 TLS），发送 `hello`。
2. 服务器回应 `hello_ack`，将客户端挂入对应房间等待队列。
3. 第二个客户端进入同一房间，双方分别收到 `ready`。
4. 双方按照 `send_plan` 发送数据帧，服务器直接转发。
5. 数据发送完成后客户端可发送 `done` 通知。
6. 当发送与接收满足计划、或任一端错误/断开时，服务器发送 `summary` 并关闭（或半关闭）连接。

## 兼容性说明

- 所有控制字段均允许未来扩展；未识别字段会被忽略。
- `payload_bytes` 为 `0` 时允许发送纯序号数据。
- 如未在 `expect_receive` 内收齐帧，服务器会在 `summary.error` 中指出未满足计划。
- 若 `server_push > 0`，服务器在 `ready` 之后以同样的数据帧格式主动下发对应数量的消息。

该协议既适用于纯 TCP，也适用于 TLS（握手完成后逻辑一致）。客户端可通过 JSON 配置批量生成不同房间、不同消息规模的“机器人”以进行压力与稳定性测试。

## 配置与运行特性

### 多 workload 与传输模式

`tools/chat_stability_client.py` 支持在配置文件的 `workloads` 数组中声明任意数量的子任务。每个 workload 可以：

- 指定独立的 `name`/`label`，以便在日志中识别来源；
- 通过 `transport` 小节选择协议与端口，例如：
  - `{ "mode": "tcp", "port": 9100 }`
  - `{ "mode": "tls", "port": 9101, "tls": { "verify": false } }`
- 设置房间/客户端生成规则（`clients`、`client_id_prefix` 等）。

运行期会根据 workload 内部的计划生成对应数量的机器人，并把目标端口与协议写入日志（失败行会标注 `host:port(proto)`）。

### 范围型计划参数

`send_plan`、`payload_bytes`、`expect_receive`、`server_push` 均接受以下形式的范围描述：

- 区间字典：`{"min": 4, "max": 16}` 或 `{"range": [4, 16]}`；
- 单值：`64` 或 `{"value": 64}`；
- 字符串区间：`"4~16"`。

客户端会在每个机器人启动时随机抽取具体数值（同一 workload 内的不同机器人彼此独立），并把随机后的结果带入 `hello` 握手。

### Base64 随机载荷

为了覆盖更多负载模式，客户端生成数据帧时会把随机字节经 Base64 编码后裁剪到目标长度。这样可以在相同大小前提下模拟高熵内容，而服务器仍然按照 `payload_bytes` 校验长度。若配置值不是 4 的倍数，客户端会自动向上补齐，以避免 Base64 填充字符导致长度偏差。

## 服务器端口与 TLS 证书

- 缺省情况下，`co_chat` 会监听 `--port`（默认 9100）提供 TCP 服务，并尝试在 `--tls-port`（默认 9101）上启动 TLS。
- 如果用户没有传入 `--tls-port`，程序会把 TLS 端口保持在 `TCP+1`；若显式覆盖了 `--port`，TLS 端口会同步调整。
- 当 TLS 端口启用但缺少 `--cert`、`--key` 时，程序会尝试在当前目录或工程根目录下的 `certs/server.crt`、`certs/server.key` 中自动加载证书；若未找到：
  - 如果端口是默认推导出的，程序会记录警告并自动关闭 TLS；
  - 如果用户显式指定 `--tls-port`，程序会报错退出。
- 一旦 TLS 启动成功，客户端可以在 workload 中开启 `mode: "tls"`（并酌情关闭证书校验或提供 CA）。

示例配置可参考 `tools/chat_stability.sample.json`，其中同时包含 TCP 小包、TCP 大包和 TLS 混合流量三种 workload，用于展示以上特性。