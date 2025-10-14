# QUIC 集成 TODO

## 1. 依赖与构建

- [ ] 在 `third_party/msquic` 引入 msquic（子模块或预编译包）
- [ ] 扩展 `xmake.lua`，新增 `USING_QUIC_MSQUIC` 选项及平台差异链接参数
- [ ] 编写 Linux（OpenSSL 后端）与 Windows（SChannel/OpenSSL）构建脚本
- [ ] 在 `README.md` 标明额外依赖包与环境变量

## 2. 初始化封装

- [ ] 新增 `net/quic/msquic_api.hpp/cpp`，封装 `MsQuicOpen/MsQuicClose` 与 TLS 证书配置
- [ ] 提供承载 ALPN、证书来源、日志偏好的配置结构
- [ ] 复用现有 `tls_context` 加载逻辑，支持 PEM / 系统证书库

## 3. 执行模型桥接

- [ ] 设计 `quic_connection_msquic`，持有 `HQUIC` 并把回调绑定到 `workqueue`
- [ ] 实现 `quic_stream_msquic`，提供 `send`/`send_all`/`recv`/`recv_all` awaiter
- [ ] 确保 MsQuic 回调即时 `post_to` 到对应的 `callback_wq`
- [ ] 将 `QUIC_STATUS` 转换为框架通用的负 errno

## 4. 生命周期与错误处理

- [ ] 为 `HQUIC`、传输参数、缓冲区定义 RAII 包装
- [ ] 实现优雅关闭路径，对齐 `basic_tls_socket::close` 语义
- [ ] 将 MsQuic tracing 对接仓库日志系统（`logs/`），可设置分类过滤

## 5. 测试与示例

- [ ] 在 `test/` 中加入 loopback 等集成测试
- [ ] 编写 `co_quic_echo` 示例，展示多 stream 场景
- [ ] CI/构建脚本在依赖满足时开启 QUIC 构建

## 6. 文档整理

- [ ] 撰写 `docs/quic_msquic.md`，说明架构与排障要点
- [ ] 在 `README.md` 标记 QUIC 功能开关与限制
- [ ] 记录尚未覆盖的能力（0-RTT、迁移、拥塞调优）供后续跟进
