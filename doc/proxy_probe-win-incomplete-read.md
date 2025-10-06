# Windows Proxy HTTPS IncompleteRead Incident Report

## 概要
- **日期**：2025-10-06
- **模块**：`test/co_http_proxy`（Windows 平台 IOCP 路径）
- **症状**：HTTPS 代理压测中出现 `http_error: IncompleteRead`，客户端收到的响应长度小于 `Content-Length` 或 chunked 编码完整体。
- **影响**：跨平台代理稳定性下降，Windows 环境下 60 次请求中 10 次失败（16.67%）。Linux 代理或 Windows 直连均 100% 成功，定位为 Windows 代理特有问题。

## 测试快照
- **压测工具**：`tools/proxy_probe.py`
- **模式**：HTTPS 正向代理
- **代理地址**：`192.168.30.32:18100`（Windows 主机）
- **目标站点**：`www.baidu.com`, `www.qq.com`, `www.taobao.com`, `www.jd.com`, `www.tmall.com`, `www.bilibili.com`
- **请求总数**：6 × 10 = 60
- **统计**：
  - 成功：50（83.33%）
  - 失败：10（全部为 `IncompleteRead`）
  - 超时/连接/发送/接收/TLS/其他错误：0
  - 平均延迟：129.65 ms；最大延迟：327.82 ms
- **典型失败样例**：
  - `https://www.baidu.com/ → IncompleteRead(34016 bytes read, 620381 more expected)`
  - `https://www.qq.com/ → IncompleteRead(10737 bytes read, 112405 more expected)`
  - `https://www.taobao.com/ → IncompleteRead(64361 bytes read, chunk 剩余未知)`

## 现象详解
- 失败均来自大体量响应（`Content-Length` 数十 KB 以上），怀疑走 HTTP body relay 时提前终止。
- 失败分布集中在 `baidu`/`qq`，其他站点偶发；响应普遍使用 TLS + chunked/压缩编码。
- Windows 代理路径使用 IOCP + TLS unwrap + HTTP 隧道转发；Linux 代理（epoll + 同一业务逻辑）未见问题。

## 已知背景
- 压测同场景在 Linux 主机运行 `co_http_proxy` 可 100% 成功。
- Windows 客户端直接访问目标站点无 `IncompleteRead`，说明目标服务正常。
- 近期修复过 `callback_wq` / IOCP 路由相关缺陷（见 `callback_wq-missing-func.md`），当前问题可能与相邻代码或缓存大小策略有关。

## 初步假设
1. **IOCP 读写竞态**：某些情况下 `recv`/`send` awaiter 过早认为消息完成，提前关闭下游连接。
2. **TLS 解包残留**：`tls_socket` 在 Windows 路径下可能丢失尾包或未将缓冲区全部写出。
3. **转发环节截断**：`proxy` 在复制远端数据到客户端时，Windows 指定的 `send` 返回成功但实际发送字节 < 期望值，后续未继续补发。
4. **流量过滤/防火墙**：Windows 平台（或本地防病毒）对大包做了截断，需要抓包确认。

## 建议调查步骤
1. **复现确认**
   - 保留压测命令与日志，重复运行确认问题稳定复现。
   - 对比使用相同代理二进制在 Linux 主机运行的统计，确认仅 Windows 受影响。
2. **开启详细日志**
   - 设置 `CO_WQ_ENABLE_CALLBACK_WQ_TRACE` 与 proxy 专用调试宏，记录每次 `recv`/`send` 的长度与返回码。
   - 在 `co_http_proxy` 中打印隧道两端累计转发字节（上游/下游），检测不对称。
3. **抓包分析**
   - 使用 Wireshark/Ettercap 在 Windows 主机抓取出口与入口流量，比较是否存在 FIN/RST 或 TLS Alert 导致提前结束。
   - 若抓包无明显异常，再在 Linux 机器抓包对照。
4. **插桩排查**
   - 在 Windows `net/win/tcp_socket.hpp` 的 `recv`/`send` 完成回调中记录 `bytes_transferred` 与 `status`。
   - 检查 TLS awaiter：确认 `unwrap` 后剩余数据是否全部 push 到 `callback_wq`。
   - 在代理逻辑中为每条隧道维护 `expected_bytes` vs `forwarded_bytes` 计数。
5. **Stress/Timeout 复测**
   - 针对复现站点提高重试次数与并发，观察失败是否与并发或时间窗口相关。

## 可能的修复方向
- **补强重试/补发**：在 `send` 返回值 < 期望时继续投递，避免误判完成。
- **改进缓冲区复用**：检查 Windows TLS 缓冲区是否被提前清零或复用导致丢包。
- **回退最近更改**：逐步回退与 `callback_wq`/IOCP 相关修改，锁定引入缺陷的提交。

## 后续行动项
- [ ] 在 Windows 上重跑 `proxy_probe.py` 并保存原始日志、抓包文件。
- [ ] 实现隧道方向的字节统计日志，确认截断位置（服务器→代理或代理→客户端）。
- [ ] 审查 Windows `tls_socket` 发送路径，验证 chunked 结尾是否正确写出 `0\r\n\r\n`。
- [ ] 若定位到 IOCP awaiter 逻辑问题，补充单元/压力测试用例覆盖。
- [ ] 复测通过后更新回归文档，纳入常规压测场景。

## 附注
- 本事件归档于 `doc/proxy_probe-win-incomplete-read.md`，请与 `callback_wq-missing-func.md` 联动追踪（同属 Windows IOCP 路径相关问题）。
- 优先级：中（影响生产环境代理稳定性，但存在 Linux 备选方案）。
