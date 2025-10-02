#include "syswork.hpp"
#include <iostream>
#include <string>

#if defined(USING_NET) && !defined(_WIN32)
#include "fd_base.hpp"
#include "unix_listener.hpp"
#include "unix_socket.hpp"
#include <atomic>
#include <cerrno>
#include <cstring>
#include <string_view>

using namespace co_wq;

struct UdsOptions {
    bool        run_server { true };
    bool        run_client { true };
    std::string path { "/tmp/co_wq_uds.sock" };
    std::string message { "hello from co_wq UDS client" };
    int         max_conn { 1 };
};

static void print_usage(const char* prog)
{
    std::cout << "Usage: " << prog
              << " [--server|--client|--both] [--path PATH] [--message MSG] [--max-conn N]\n";
    std::cout << "\nBy default both server and client run together once.\n";
    std::cout << "Pass a path starting with '@' to use Linux abstract namespace.\n";
}

static UdsOptions parse_args(int argc, char* argv[])
{
    UdsOptions opt;
    bool       mode_explicit = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--server") {
            opt.run_server   = true;
            opt.run_client   = false;
            mode_explicit    = true;
        } else if (a == "--client") {
            opt.run_server   = false;
            opt.run_client   = true;
            mode_explicit    = true;
        } else if (a == "--both") {
            opt.run_server   = true;
            opt.run_client   = true;
            mode_explicit    = true;
        } else if (a == "--path" && i + 1 < argc) {
            opt.path = argv[++i];
        } else if (a == "--message" && i + 1 < argc) {
            opt.message = argv[++i];
        } else if (a == "--max-conn" && i + 1 < argc) {
            opt.max_conn = std::stoi(argv[++i]);
        } else if (a == "--help" || a == "-h") {
            print_usage(argv[0]);
        }
    }
    if (mode_explicit && opt.run_server && !opt.run_client && opt.max_conn == 1)
        opt.max_conn = 0; // server-only 默认无限制
    return opt;
}

static Task<void, Work_Promise<SpinLock, void>> uds_echo_conn(net::unix_socket<SpinLock> sock)
{
    char buffer[512];
    while (true) {
        ssize_t n = co_await sock.recv(buffer, sizeof(buffer));
        if (n <= 0)
            break;
        co_await sock.send_all(buffer, (size_t)n);
    }
    co_return;
}

static Task<void, Work_Promise<SpinLock, void>>
uds_server(net::fd_workqueue<SpinLock>& fdwq, const std::string path, int max_conn)
{
    net::unix_listener<SpinLock> lst(fdwq.base(), fdwq.reactor());
    lst.bind_listen(path, 16);
    std::cout << "[uds server] listening on " << path << "\n";
    int accepted = 0;
    while (max_conn <= 0 || accepted < max_conn) {
        int fd = co_await lst.accept();
        if (fd < 0)
            break;
        ++accepted;
        auto sock = fdwq.adopt_unix_socket(fd);
        auto task = uds_echo_conn(std::move(sock));
        post_to(task, fdwq.base());
    }
    lst.close();
    std::cout << "[uds server] exit\n";
    co_return;
}

static Task<void, Work_Promise<SpinLock, void>>
uds_client(net::fd_workqueue<SpinLock>& fdwq, const std::string path, const std::string message)
{
    auto sock = fdwq.make_unix_socket();
    if (co_await sock.connect(path) != 0) {
        std::cout << "[uds client] connect failed: " << std::strerror(errno) << "\n";
        co_return;
    }
    std::cout << "[uds client] connected to " << path << "\n";
    co_await sock.send_all(message.data(), message.size());
    sock.shutdown_tx();
    char buffer[512];
    ssize_t n = co_await sock.recv(buffer, sizeof(buffer));
    if (n > 0)
        std::cout << "[uds client] reply: " << std::string_view(buffer, (size_t)n) << "\n";
    else
        std::cout << "[uds client] no reply (" << n << ")\n";
    co_return;
}

int main(int argc, char* argv[])
{
    auto options = parse_args(argc, argv);
    if (!options.run_server && !options.run_client) {
        std::cout << "Nothing to do. Use --server, --client or --both.\n";
        return 0;
    }
    auto&                       wq = get_sys_workqueue(0);
    net::fd_workqueue<SpinLock> fdwq(wq);
    Task<void, Work_Promise<SpinLock, void>> server_task { nullptr };
    Task<void, Work_Promise<SpinLock, void>> client_task { nullptr };
    if (options.run_server)
        server_task = uds_server(fdwq, options.path, options.max_conn);
    if (options.run_client)
        client_task = uds_client(fdwq, options.path, options.message);

    Task<void, Work_Promise<SpinLock, void>>* chosen = nullptr;
    if (options.run_client)
        chosen = &client_task;
    else if (options.run_server)
        chosen = &server_task;

    std::atomic_bool finished { false };
    if (chosen && chosen->get()) {
        auto& promise          = chosen->get().promise();
        promise.mUserData      = &finished;
        promise.mOnCompleted   = [](Promise_base& pb) {
            auto* flag = static_cast<std::atomic_bool*>(pb.mUserData);
            if (flag)
                flag->store(true, std::memory_order_release);
        };
    }

    if (options.run_server)
        post_to(server_task, wq);
    if (options.run_client)
        post_to(client_task, wq);

    if (chosen)
        sys_wait_until(finished);
    else
        std::cout << "No tasks scheduled.\n";
    return 0;
}

#else
int main()
{
    std::cout << "co_uds disabled (requires Linux + USING_NET)\n";
    return 0;
}
#endif
