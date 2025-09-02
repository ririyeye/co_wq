#include "syswork.hpp"
#include "file_io.hpp"
#include "fd_base.hpp"
#include <atomic>
#include <cstring>
#include <iostream>
#include <vector>
#include <sys/stat.h>

using namespace co_wq;

#ifdef USING_NET
// 简单文件复制协程：使用 file_handle read/write awaiter 串行非阻塞复制
static Task<size_t, Work_Promise<SpinLock, size_t>> file_copy_task(net::fd_workqueue<SpinLock>& fdwq, const char* src, const char* dst)
{
    // 打开源/目标文件 (源 O_RDONLY, 目标 O_WRONLY|O_CREAT|O_TRUNC)
    int sfd = ::open(src, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (sfd < 0) {
        co_return (size_t)0;
    }
    int dfd = ::open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NONBLOCK, 0644);
    if (dfd < 0) {
        ::close(sfd);
        co_return (size_t)0;
    }
    // 将 fd 注册到 reactor 并采用 file_handle（不可直接调用私有构造函数，使用辅助 adopt 方法）
    auto& reactor = fdwq.reactor();
    reactor.add_fd(sfd);
    reactor.add_fd(dfd);
    auto sf = fdwq.make_file(sfd);
    auto df = fdwq.make_file(dfd);

    constexpr size_t BUF_SZ = 64 * 1024;
    std::vector<char> buf(BUF_SZ);
    size_t total = 0;
    for (;;) {
        ssize_t n = co_await sf.read(buf.data(), BUF_SZ);
        if (n <= 0) {
            break; // EOF or error
        }
        size_t off = 0;
        while (off < (size_t)n) {
            ssize_t wn = co_await df.write(buf.data() + off, (size_t)n - off);
            if (wn <= 0) {
                std::cerr << "write error\n";
                co_return total;
            }
            off += (size_t)wn;
            total += (size_t)wn;
        }
    }
    co_return total;
}
#endif

int main(int argc, char** argv)
{
#ifndef USING_NET
    std::cout << "USING_NET not enabled (define it to build this test)\n";
    return 0;
#else
    if (argc < 3) {
        std::cout << "usage: co_cp <src> <dst>\n";
        return 0;
    }
    auto& wq = get_sys_workqueue();
    net::fd_workqueue<SpinLock> fdwq(wq);
    auto tk = file_copy_task(fdwq, argv[1], argv[2]);
    auto coro = tk.get();
    auto& promise = coro.promise();
    std::atomic_bool finished{false};
    promise.mUserData = &finished;
    promise.mOnCompleted = [](Promise_base& pb){
        auto* f = static_cast<std::atomic_bool*>(pb.mUserData);
        f->store(true, std::memory_order_release);
    };
    post_to(tk, wq);
    while(!finished.load(std::memory_order_acquire)) {
        while (wq.work_once()) {}
    }
    size_t copied = coro.promise().result();
    std::cout << "copied bytes: " << copied << "\n";
    return 0;
#endif
}
