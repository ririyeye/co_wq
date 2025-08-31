// tcp_listener.hpp - async accept for TCP
#pragma once
#ifdef __linux__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string>
#include <stdexcept>
#include <errno.h>
#include <fcntl.h>

#include "epoll_reactor.hpp"
#include "io_waiter.hpp"
#include "tcp_socket.hpp"
#include "fd_base.hpp"

namespace co_wq::net {

template <lockable lock> class tcp_listener {
public:
    explicit tcp_listener(workqueue<lock>& exec) : _exec(exec)
    {
        _fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (_fd < 0) throw std::runtime_error("listener socket failed");
        set_non_block();
        epoll_reactor<lock>::instance(_exec).add_fd(_fd);
    }
    ~tcp_listener() { close(); }
    void bind_listen(const std::string& host, uint16_t port, int backlog = 128)
    {
        sockaddr_in addr {};
        addr.sin_family = AF_INET; addr.sin_port = htons(port);
        if (host.empty() || host == "0.0.0.0") addr.sin_addr.s_addr = INADDR_ANY; else if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) throw std::runtime_error("inet_pton failed");
        int opt = 1; ::setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (::bind(_fd, (sockaddr*)&addr, sizeof(addr)) < 0) throw std::runtime_error("bind failed");
        if (::listen(_fd, backlog) < 0) throw std::runtime_error("listen failed");
    }
    void close()
    {
        if (_fd >= 0) { epoll_reactor<lock>::instance(_exec).remove_fd(_fd); ::close(_fd); _fd = -1; }
    }
    int native_handle() const { return _fd; }
    struct accept_awaiter : io_waiter_base {
        tcp_listener& lst; int newfd { -1 };
        accept_awaiter(tcp_listener& l) : lst(l) {}
        bool await_ready() noexcept { newfd = try_accept(); return newfd >= 0 || newfd == -2; }
        void await_suspend(std::coroutine_handle<> h)
        { this->h = h; INIT_LIST_HEAD(&this->ws_node); epoll_reactor<lock>::instance(lst._exec).add_waiter(lst._fd, EPOLLIN, this); }
        int await_resume() noexcept { if (newfd >= 0 || newfd == -2) return newfd; return try_accept(); }
        int try_accept() noexcept
        {
            sockaddr_in addr; socklen_t alen = sizeof(addr);
            int fd = ::accept4(lst._fd, (sockaddr*)&addr, &alen, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (fd >= 0) return fd;
            if (fd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -1; // need wait
            return -2; // fatal
        }
    };
    accept_awaiter accept() { return accept_awaiter(*this); }
private:
    void set_non_block(){ int flags = ::fcntl(_fd, F_GETFL, 0); if (flags >= 0) ::fcntl(_fd, F_SETFL, flags | O_NONBLOCK); }
    workqueue<lock>& _exec; int _fd { -1 };
};

template <lockable lock>
inline Task<int, Work_Promise<lock, int>> async_accept(tcp_listener<lock>& lst)
{ int fd = co_await lst.accept(); co_return fd; }

// helper: return a ready tcp_socket if accept ok (fd>=0) else nullptr-like via optional pattern (simplified)
template <lockable lock>
inline Task<tcp_socket<lock>, Work_Promise<lock, tcp_socket<lock>>> async_accept_socket(fd_workqueue<lock>& fwq, tcp_listener<lock>& lst)
{
    int fd = co_await lst.accept();
    if (fd < 0) {
        // return an empty moved-from socket: construct then close
        tcp_socket<lock> tmp = fwq.make_tcp_socket();
        tmp.close();
        co_return std::move(tmp);
    }
    co_return fwq.adopt_tcp_socket(fd);
}

} // namespace co_wq::net
#endif
