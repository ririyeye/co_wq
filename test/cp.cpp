#include "fd_base.hpp"
#include "file_io.hpp"
#include "file_open.hpp"
#include "syswork.hpp"
#include <atomic>
#include <cstring>
#include <iostream>
#include <sys/stat.h>
#include <vector>


using namespace co_wq;

#ifdef USING_NET
// 简单文件复制协程：使用 file_handle read/write awaiter 串行非阻塞复制
static Task<size_t, Work_Promise<SpinLock, size_t>>
file_copy_task(net::fd_workqueue<SpinLock>& fdwq, const char* src, const char* dst)
{
    auto s_opt = net::open_file_handle(fdwq, src, net::open_mode::read_only);
    if (!s_opt.has_value()) {
        co_return (size_t) 0;
    }
    auto d_opt = net::open_file_handle(fdwq, dst, net::open_mode::write_trunc);
    if (!d_opt.has_value()) {
        co_return (size_t) 0;
    }
    auto sf = std::move(*s_opt);
    auto df = std::move(*d_opt);

    constexpr size_t  BUF_SZ = 64 * 1024;
    std::vector<char> buf(BUF_SZ);
    size_t            total = 0;
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
    auto&                       wq = get_sys_workqueue();
    net::fd_workqueue<SpinLock> fdwq(wq);
    auto                        tk      = file_copy_task(fdwq, argv[1], argv[2]);
    auto                        coro    = tk.get();
    auto&                       promise = coro.promise();
    std::atomic_bool            finished { false };
    promise.mUserData    = &finished;
    promise.mOnCompleted = [](Promise_base& pb) {
        auto* f = static_cast<std::atomic_bool*>(pb.mUserData);
        f->store(true, std::memory_order_release);
    };
    post_to(tk, wq);
    while (!finished.load(std::memory_order_acquire)) {
        while (wq.work_once()) { }
    }
    size_t copied = coro.promise().result();
    std::cout << "copied bytes: " << copied << "\n";
    return 0;
#endif
}
