
#include "syswork.hpp"
#ifdef USING_NET
#include "tcp.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

using namespace co_wq;

// 简单 echo 客户端协程: 连接 127.0.0.1:12345 发送 "hello" 并读取回显
static Task<void, Work_Promise<nolock, void>> echo_client()
{
    auto&                     wq = get_sys_workqueue();
    net::fd_workqueue<nolock> fdwq(wq);
    auto                      sock = fdwq.make_tcp_socket();
    int                       rc   = co_await sock.connect("127.0.0.1", 12345);
    if (rc != 0) {
        std::cout << "connect failed\n";
        co_return;
    }
    char buf[256];
    while (true) {
        ssize_t n = co_await sock.recv(buf, sizeof(buf));
        if (n <= 0) {
            std::cout << "recv end: " << n << "\n";
            break;
        }
        std::cout << "recv: " << std::string_view(buf, (size_t)n) << "\n";
        ssize_t sent = co_await sock.send(buf, (size_t)n);
        std::cout << "sent: " << sent << " bytes\n";
        if (sent <= 0) {
            std::cout << "send error: " << sent << "\n";
            break;
        }
    }
    co_return;
}
#endif

int main()
{
#ifdef USING_NET
    auto&            wq      = get_sys_workqueue();
    auto             tk      = echo_client(); // Task<void>
    auto             coro    = tk.get();
    auto&            promise = coro.promise();
    std::atomic_bool finished { false };
    promise.mUserData    = &finished;
    promise.mOnCompleted = [](Promise_base& pb) {
        auto* f = static_cast<std::atomic_bool*>(pb.mUserData);
        if (f)
            f->store(true, std::memory_order_release);
    };
    // 投递到执行队列
    post_to(tk, wq);

    // 事件循环: 处理工作项，等待协程完成
    while (!finished.load(std::memory_order_acquire)) {
        while (wq.work_once()) {
            // drain queue fully each tick
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#endif
    return 0;
}
