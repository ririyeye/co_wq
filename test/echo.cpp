
// echo.cpp
#include "syswork.hpp"
#include <iostream>

#if defined(USING_NET)
#include "fd_base.hpp"
#include "tcp_listener.hpp"
#include "tcp_socket.hpp"
#include "udp_socket.hpp"
#include <atomic>
#include <chrono>
#ifndef _WIN32
#include <csignal>
#endif
#include <cstring>
#include <string>

#ifdef _WIN32
#include <basetsd.h>
using ssize_t = SSIZE_T;
#endif

using namespace co_wq;

// ---- Server global control & stats ----
static std::atomic_bool                      g_stop { false };
static std::atomic<uint64_t>                 g_conn_count { 0 };
static std::atomic<uint64_t>                 g_bytes_echoed { 0 };
static std::atomic<int>                      g_listener_fd { -1 }; // for forced close
static std::chrono::steady_clock::time_point g_server_start;

#ifdef _WIN32
#include <windows.h>
static BOOL WINAPI console_ctrl_handler(DWORD type)
{
    if (type == CTRL_C_EVENT) {
        g_stop.store(true, std::memory_order_release);
        int fd = g_listener_fd.exchange(-1, std::memory_order_acq_rel);
        if (fd != -1)
            ::closesocket((SOCKET)fd);
        return TRUE;
    }
    return FALSE;
}
#else
static void sigint_handler(int)
{
    g_stop.store(true, std::memory_order_release);
    int fd = g_listener_fd.exchange(-1, std::memory_order_acq_rel);
    if (fd != -1)
        ::close(fd);
}
#endif

// 简单 echo 客户端协程: 连接 127.0.0.1:12345 发送 "hello" 并读取回显
// fd_workqueue 由外部构造并传入，它自身绑定 reactor 线程；协程继续运行在主系统 workqueue 上
// connection handler: echo back whatever is received
static Task<void, Work_Promise<SpinLock, void>> echo_conn(net::tcp_socket<SpinLock> sock)
{
    char buf[512];
    while (true) {
        ssize_t n = co_await sock.recv(buf, sizeof(buf));
        if (n <= 0)
            break;
        g_bytes_echoed.fetch_add((uint64_t)n, std::memory_order_relaxed);
        ssize_t m = co_await sock.send(buf, (size_t)n);
        if (m <= 0)
            break;
    }
    co_return;
}

// server coroutine: listen on 127.0.0.1:12345 and accept a single client then exit
static Task<void, Work_Promise<SpinLock, void>>
echo_server(net::fd_workqueue<SpinLock>& fdwq, std::string host, uint16_t port, int max_conn)
{
    net::tcp_listener<SpinLock> lst(fdwq.base(), fdwq.reactor());
    lst.bind_listen(host, port, 16);
    g_server_start = std::chrono::steady_clock::now();
    g_listener_fd.store(lst.native_handle(), std::memory_order_release);
    int accepted = 0;
    while ((max_conn <= 0 || accepted < max_conn) && !g_stop.load(std::memory_order_acquire)) {
        int fd = co_await lst.accept();
        if (fd < 0) {
            // fatal error or shutdown
            break;
        }
        ++accepted;
        g_conn_count.fetch_add(1, std::memory_order_relaxed);
        auto sock = fdwq.adopt_tcp_socket(fd);
        auto t    = echo_conn(std::move(sock));
        post_to(t, fdwq.base());
    }
    lst.close(); // server exits after reaching max_conn (if specified)
    g_listener_fd.store(-1, std::memory_order_release);
    // Print statistics on unlimited mode exit via Ctrl+C or after finishing limited accepts.
    auto     dur   = std::chrono::steady_clock::now() - g_server_start;
    double   sec   = std::chrono::duration_cast<std::chrono::duration<double>>(dur).count();
    uint64_t bytes = g_bytes_echoed.load(std::memory_order_relaxed);
    uint64_t conns = g_conn_count.load(std::memory_order_relaxed);
    double   mbps  = sec > 0 ? (bytes / (1024.0 * 1024.0)) / sec : 0.0;
    std::cout << "[server] connections=" << conns << " bytes=" << bytes << " elapsed(s)=" << sec
              << " throughput(MiB/s)=" << mbps << "\n";
    co_return;
}

static Task<void, Work_Promise<SpinLock, void>>
echo_client(net::fd_workqueue<SpinLock>& fdwq, std::string host, uint16_t port)
{
    auto sock = fdwq.make_tcp_socket();
    int  rc   = co_await sock.connect(host, port);
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

struct EchoOptions {
    bool        run_server { true };
    bool        run_client { true };
    bool        run_udp_server { false }; // 新增：UDP server
    bool        run_udp_client { false }; // 新增：UDP client
    std::string host { "127.0.0.1" };
    uint16_t    port { 12345 };
    uint16_t    udp_port { 12346 }; // UDP 默认端口（避免与 TCP 冲突）
    int         max_conn { 0 };     // 0 or negative => unlimited
};

static void print_usage(const char* prog)
{
    std::cout << "Usage: " << prog
              << " [--server|--client|--both] [--host HOST] [--port TCP_PORT] [--udp-server] [--udp-client] "
                 "[--udp-port UDP_PORT] [--max-conn N]\n";
}

static EchoOptions parse_args(int argc, char* argv[])
{
    EchoOptions opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--server") {
            opt.run_server = true;
            opt.run_client = false;
        } else if (a == "--client") {
            opt.run_server = false;
            opt.run_client = true;
        } else if (a == "--both") {
            opt.run_server = true;
            opt.run_client = true;
        } else if (a == "--host" && i + 1 < argc) {
            opt.host = argv[++i];
        } else if (a == "--port" && i + 1 < argc) {
            opt.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (a == "--max-conn" && i + 1 < argc) {
            opt.max_conn = std::stoi(argv[++i]);
        } else if (a == "--udp-server") {
            opt.run_udp_server = true;
        } else if (a == "--udp-client") {
            opt.run_udp_client = true;
        } else if (a == "--udp-port" && i + 1 < argc) {
            opt.udp_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (a == "--help" || a == "-h") {
            print_usage(argv[0]);
        }
    }
    return opt;
}

int main(int argc, char* argv[])
{
    auto options = parse_args(argc, argv);
    // Install Ctrl+C handler for server infinite mode
    if (options.run_server && options.max_conn <= 0) {
#ifdef _WIN32
        SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
        std::signal(SIGINT, sigint_handler);
#endif
    }
    // 0 => 自动检测线程数
    auto&                                    wq = get_sys_workqueue(0);
    net::fd_workqueue<SpinLock>              fdwq(wq); // 外部创建并传入协程
    Task<void, Work_Promise<SpinLock, void>> server_task { nullptr };
    Task<void, Work_Promise<SpinLock, void>> client_task { nullptr };
    Task<void, Work_Promise<SpinLock, void>> udp_server_task { nullptr };
    Task<void, Work_Promise<SpinLock, void>> udp_client_task { nullptr };
    if (options.run_server)
        server_task = echo_server(fdwq, options.host, options.port, options.max_conn);
    if (options.run_client)
        client_task = echo_client(fdwq, options.host, options.port);
    if (options.run_udp_server) {
        // 简单 UDP echo server：收 -> 发回
        udp_server_task = [&fdwq, options]() -> Task<void, Work_Promise<SpinLock, void>> {
            auto usock = fdwq.make_udp_socket();
            // 绑定
            sockaddr_in addr {};
            addr.sin_family      = AF_INET;
            addr.sin_addr.s_addr = inet_addr(options.host.c_str());
            addr.sin_port        = htons(options.udp_port);
            if (::bind(usock.native_handle(), (sockaddr*)&addr, sizeof(addr)) != 0) {
                std::cout << "UDP bind failed\n";
                co_return;
            }
            std::cout << "[udp server] listening on " << options.host << ":" << options.udp_port << "\n";
            char        buf[1500];
            sockaddr_in peer;
            socklen_t   plen = sizeof(peer);
            while (!g_stop.load(std::memory_order_acquire)) {
                ssize_t n = co_await usock.recv_from(buf, sizeof(buf), &peer, &plen);
                if (n <= 0)
                    continue; // ignore errors
                co_await usock.send_to(buf, (size_t)n, peer);
            }
            co_return;
        }();
    }
    if (options.run_udp_client) {
        udp_client_task = [&fdwq, options]() -> Task<void, Work_Promise<SpinLock, void>> {
            auto usock = fdwq.make_udp_socket();
            // 可选 connect (方便后面直接 recv)
            if (co_await usock.connect(options.host, options.udp_port) != 0) {
                std::cout << "UDP connect failed (non-fatal, fallback to send_to)\n";
            }
            sockaddr_in peer {};
            peer.sin_family      = AF_INET;
            peer.sin_addr.s_addr = inet_addr(options.host.c_str());
            peer.sin_port        = htons(options.udp_port);
            const char* msg      = "ping-udp";
            for (int i = 0; i < 5; ++i) {
                co_await usock.send_to(msg, strlen(msg), peer);
                char        rbuf[256];
                sockaddr_in from;
                socklen_t   flen = sizeof(from);
                ssize_t     rn   = co_await usock.recv_from(rbuf, sizeof(rbuf), &from, &flen);
                if (rn > 0)
                    std::cout << "[udp client] recv: " << std::string_view(rbuf, (size_t)rn) << "\n";
            }
            co_return;
        }();
    }
    // choose a promise to wait on (prefer client, else server)
    // 选择一个用来同步退出；优先客户端(UDP/TCP)
    auto             ch      = (options.run_udp_client
                                    ? udp_client_task.get()
                                    : (options.run_client ? client_task.get()
                                                          : (options.run_udp_server ? udp_server_task.get() : server_task.get())));
    auto&            promise = ch.promise();
    std::atomic_bool finished { false };
    promise.mUserData    = &finished;
    promise.mOnCompleted = [](Promise_base& pb) {
        auto* f = static_cast<std::atomic_bool*>(pb.mUserData);
        if (f)
            f->store(true, std::memory_order_release);
    };
    // 投递到执行队列
    if (options.run_server) {
        post_to(server_task, wq);
    }
    if (options.run_client) {
        post_to(client_task, wq);
    }
    if (options.run_udp_server) {
        post_to(udp_server_task, wq);
    }
    if (options.run_udp_client) {
        post_to(udp_client_task, wq);
    }

    // 事件循环: 处理工作项，等待协程完成
    sys_wait_until(finished);
    return 0;
}

#else // USING_NET
int main()
{
    std::cout << "co_echo disabled (requires Linux + USING_NET)\n";
    return 0;
}
#endif
