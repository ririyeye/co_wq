// tcp_listener.hpp - TCP 异步 accept
#pragma once
#ifdef __linux__
#include "epoll_reactor.hpp" // 默认 reactor
#include "fd_base.hpp"
#include "io_waiter.hpp"
#include "tcp_socket.hpp"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace co_wq::net {

/**
 * @brief TCP 监听器，提供 bind+listen 及异步 accept 能力。
 */
template <lockable lock, template <class> class Reactor = epoll_reactor> class tcp_listener {
public:
    explicit tcp_listener(workqueue<lock>& exec) : _exec(exec)
    {
        _fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (_fd < 0)
            throw std::runtime_error("listener socket failed");
        set_non_block();
        Reactor<lock>::instance(_exec).add_fd(_fd);
    }
    ~tcp_listener() { close(); }
    /**
     * @brief 绑定并监听。
     * @param host 监听地址("0.0.0.0" 或 空 字符串 表示 INADDR_ANY)。
     * @param port 端口。
     * @param backlog listen backlog。
     */
    void bind_listen(const std::string& host, uint16_t port, int backlog = 128)
    {
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        if (host.empty() || host == "0.0.0.0")
            addr.sin_addr.s_addr = INADDR_ANY;
        else if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0)
            throw std::runtime_error("inet_pton failed");
        int opt = 1;
        ::setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (::bind(_fd, (sockaddr*)&addr, sizeof(addr)) < 0)
            throw std::runtime_error("bind failed");
        if (::listen(_fd, backlog) < 0)
            throw std::runtime_error("listen failed");
    }
    /**
     * @brief 关闭监听 socket 并注销 reactor。
     */
    void close()
    {
        if (_fd >= 0) {
            Reactor<lock>::instance(_exec).remove_fd(_fd);
            ::close(_fd);
            _fd = -1;
        }
    }
    int native_handle() const { return _fd; }
    /**
     * @brief 异步 accept awaiter。
     * @return >=0 新连接 fd；-1 需等待（内部挂起）；-2 致命错误。
     */
    struct accept_awaiter : io_waiter_base {
        tcp_listener& lst;
        int           newfd { -1 };
        accept_awaiter(tcp_listener& l) : lst(l) { }
        bool await_ready() noexcept
        {
            newfd = try_accept();
            return newfd >= 0 || newfd == -2;
        }
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h = h;
            INIT_LIST_HEAD(&this->ws_node);
            Reactor<lock>::instance(lst._exec).add_waiter(lst._fd, EPOLLIN, this);
        }
        int await_resume() noexcept
        {
            if (newfd >= 0 || newfd == -2)
                return newfd;
            return try_accept();
        }
        int try_accept() noexcept // 单次非阻塞 accept4
        {
            sockaddr_in addr;
            socklen_t   alen = sizeof(addr);
            int         fd   = ::accept4(lst._fd, (sockaddr*)&addr, &alen, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (fd >= 0)
                return fd;
            if (fd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return -1; // 需等待
            return -2;     // 致命错误
        }
    };
    /**
     * @brief 获取 accept awaiter。
     */
    accept_awaiter accept() { return accept_awaiter(*this); }

private:
    void set_non_block()
    {
        int flags = ::fcntl(_fd, F_GETFL, 0);
        if (flags >= 0)
            ::fcntl(_fd, F_SETFL, flags | O_NONBLOCK);
    }
    workqueue<lock>& _exec;
    int              _fd { -1 };
};

template <lockable lock, template <class> class Reactor = epoll_reactor>
inline Task<int, Work_Promise<lock, int>> async_accept(tcp_listener<lock, Reactor>& lst)
{
    int fd = co_await lst.accept();
    co_return fd;
}

// 辅助: 若成功返回已创建的 tcp_socket，否则返回一个已关闭的占位 socket
template <lockable lock, template <class> class Reactor = epoll_reactor>
inline Task<tcp_socket<lock, Reactor>, Work_Promise<lock, tcp_socket<lock, Reactor>>>
async_accept_socket(fd_workqueue<lock, Reactor>& fwq, tcp_listener<lock, Reactor>& lst)
{
    int fd = co_await lst.accept();
    if (fd < 0) {
        // return an empty moved-from socket: construct then close
        tcp_socket<lock, Reactor> tmp = fwq.make_tcp_socket();
        tmp.close();
        co_return std::move(tmp);
    }
    co_return fwq.adopt_tcp_socket(fd);
}

} // namespace co_wq::net
#endif
