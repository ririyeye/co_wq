/**
 * @file iocp_reactor.hpp
 * @brief Windows 平台 IOCP Reactor，实现 epoll_reactor 接口子集以复用跨平台模板代码。
 *
 * 设计要点:
 *  - 使用 IOCP (I/O Completion Port) 完成通知模型，区别于 Linux 的就绪模型；
 *  - 复用 epoll_reactor 名称（模板默认值不分平台），并提供 iocp_reactor 别名增强可读性；
 *  - add_waiter / add_waiter_custom 直接投递（post），因为 IOCP 不需要“注册可读/可写”事件；
 *  - Overlapped 完成由独立线程 loop() 捕获，再通过 workqueue 投递协程恢复，避免在 IOCP 线程直接 resume。
 *
 * 线程模型:
 *  - 构造时创建 1 条后台线程阻塞在 GetQueuedCompletionStatus；
 *  - 完成事件 -> 提取 iocp_ovl.waiter -> exec.post(waiter) -> workqueue 线程调度协程。
 */
// iocp_reactor.hpp - Windows IOCP-based reactor implementing epoll_reactor interface subset
#pragma once
#ifdef _WIN32
#include "io_waiter.hpp"
#include "workqueue.hpp"
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")

/**
 * @brief 扩展 Overlapped，携带等待的 io_waiter_base 指针。
 */
struct iocp_ovl : OVERLAPPED {
    co_wq::net::io_waiter_base* waiter { nullptr };
};

namespace co_wq::net {

/**
 * @class epoll_reactor
 * @brief Windows 下 IOCP 后端；名称复用以简化跨平台模板。
 */
template <lockable lock> class epoll_reactor { // reuse name under Windows as IOCP backend
public:
    /**
     * @brief 构造并初始化 IOCP，启动后台轮询线程。
     * @param exec 关联的执行队列。
     */
    explicit epoll_reactor(workqueue<lock>& exec) : _exec(exec)
    {
        ensure_wsa();
        _iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
        if (!_iocp)
            throw std::runtime_error("CreateIoCompletionPort failed");
        _running.store(true, std::memory_order_relaxed);
        _thr = std::thread([this] { loop(); });
    }
    epoll_reactor(const epoll_reactor&)            = delete;
    epoll_reactor& operator=(const epoll_reactor&) = delete;
    /**
     * @brief 析构：停止后台线程并关闭 IOCP。
     */
    ~epoll_reactor()
    {
        _running.store(false, std::memory_order_relaxed);
        if (_iocp)
            PostQueuedCompletionStatus(_iocp, 0, 0, nullptr);
        if (_thr.joinable())
            _thr.join();
        if (_iocp)
            CloseHandle(_iocp);
    }
    /** @brief 将 socket 关联到 IOCP。*/
    void add_fd(int fd) { CreateIoCompletionPort((HANDLE)_get_socket(fd), _iocp, (ULONG_PTR)fd, 0); }
    /** @brief 添加等待者(直接投递)。*/
    void add_waiter(int, uint32_t, io_waiter_base* waiter) { _exec.post(*waiter); }
    /** @brief 自定义添加等待者(与 add_waiter 相同保持接口对齐)。*/
    void add_waiter_custom(int fd, uint32_t mask, io_waiter_base* waiter) { add_waiter(fd, mask, waiter); }
    /** @brief 移除 socket (关闭)；未完成 Overlapped 将以失败形式完成。*/
    void remove_fd(int fd) { closesocket(_get_socket(fd)); }
    /** @brief 手动投递一个完成。*/
    void post_completion(io_waiter_base* w) { _exec.post(*w); }
    /** @brief 获取 IOCP 句柄。*/
    HANDLE iocp_handle() const noexcept { return _iocp; }

private:
    static SOCKET _get_socket(int fd) { return (SOCKET)fd; }
    /** @brief 后台循环：等待 IO 完成并投递 waiter。*/
    void loop()
    {
        DWORD        bytes = 0;
        ULONG_PTR    key   = 0;
        LPOVERLAPPED povl  = nullptr;
        while (_running.load(std::memory_order_relaxed)) {
            BOOL ok = GetQueuedCompletionStatus(_iocp, &bytes, &key, &povl, 50); // 50ms timeout
            if (!ok && povl == nullptr) {                                        // timeout or wake
                continue;
            }
            if (povl) {
                auto* ovl = reinterpret_cast<iocp_ovl*>(povl);
                if (ovl->waiter) {
                    // stash byte count in waiter->ws_node.prev (hack) is messy; better store in derived awaiter
                    _exec.post(*ovl->waiter);
                }
            }
        }
    }
    /** @brief 确保 WSA 启动 (线程安全一次)。*/
    static void ensure_wsa()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
                throw std::runtime_error("WSAStartup failed");
        });
    }
    HANDLE           _iocp { nullptr };
    std::atomic_bool _running { false };
    std::thread      _thr;
    workqueue<lock>& _exec;
};

} // namespace co_wq::net

/** @brief iocp_reactor 显式别名，提升直观性。*/
namespace co_wq::net {
template <lockable lock> using iocp_reactor = epoll_reactor<lock>;
}
#endif // _WIN32
