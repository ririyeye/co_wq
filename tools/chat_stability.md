# Chat Stability Test Tools

该目录包含用于驱动 `test/chat_server.cpp` 的辅助脚本，用来对 TCP/TLS 聊天通道做高并发与大负载回放测试。

- 协议细节见 `docs/chat_stability_protocol.md`。
- Python 客户端：`chat_stability_client.py`
- 配置示例：`chat_stability.sample.json`

## 构建 C++ 测试服务器

```powershell
python .\script\xmk.py build
xmake build co_chat
```

服务器支持以下参数：

- `--host`：监听地址，默认 `0.0.0.0`
- `--port`：TCP 端口，默认 `9100`
- `--tls-port`：可选 TLS 端口（默认沿用 `--port+1`；缺省证书时会自动查找 `certs/server.crt` 和 `certs/server.key`）
- `--max-payload`：单帧最大载荷，默认 4 MiB

示例：

```powershell
xmake run co_chat --host 0.0.0.0 --port 9100
```

TLS 模式（如已通过 `script/gen_selfsigned_cert.py` 生成默认证书，可省略 `--cert/--key`）：

```powershell
xmake run co_chat --port 9100 --tls-port 9443 --cert certs/server.crt --key certs/server.key
```

## Python 客户端

依赖：Python ≥ 3.10。

```powershell
python tools/chat_stability_client.py --config tools/chat_stability.sample.json
```

常用覆盖参数：

- `--host` / `--port`：覆盖配置中的地址与端口
- `--tls` / `--no-tls`：强制开启或关闭 TLS
- `--ca` / `--cert` / `--key`：TLS 证书配置
- `--max-concurrency`：并发连接数
- `--log-level`：输出级别（如 `DEBUG`、`INFO`）

配置文件字段概览：

- `transport`：连接信息（含 TLS 选项）
- `defaults`：机器人默认参数
- `workloads`：批量生成机器人，每项支持 `rooms`、`clients_per_room` 等
- `bots`：可选的显式机器人列表
- `runtime`：运行时控制，如并发与连接节奏

客户端会统计每个机器人发送/接收的帧数，并对照服务器 `summary` 信息，输出聚合结果。任何协议或计划偏差都会标记为失败，方便追踪稳定性问题。

## 调试技巧

- 在 Windows PowerShell 中调试时，如果需要让服务器进程在独立窗口运行、同时保留当前控制台用于执行客户端脚本，可以使用 `Start-Process`：

	```powershell
	Start-Process -FilePath d:\work\co_wq\build\windows\x64\debug\co_chat.exe -WorkingDirectory d:\work\co_wq
	```

	该命令会在单独的终端窗口中启动 `co_chat.exe`，避免可执行文件锁定当前会话，也无需结束服务器进程就能在原窗口继续执行 `python tools\chat_stability_client.py` 等测试命令。
