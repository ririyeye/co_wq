// tcp_socket.hpp - TCP socket coroutine primitives
#pragma once
#ifdef __linux__
#include "epoll_reactor.hpp" // default Reactor
#include "io_waiter.hpp"
#include "worker.hpp"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace co_wq::net {

// Forward decl for fd_workqueue with reactor parameter
template <lockable lock, template <class> class Reactor> class fd_workqueue;

// tcp_socket now parameterized by Reactor backend (must provide static instance(exec) returning reactor with
// add_fd/remove_fd/add_waiter)
template <lockable lock, template <class> class Reactor = epoll_reactor> class tcp_socket {
public:
    tcp_socket()                             = delete;
    tcp_socket(const tcp_socket&)            = delete;
    tcp_socket& operator=(const tcp_socket&) = delete;
    tcp_socket(tcp_socket&& o) noexcept : _exec(o._exec), _fd(o._fd), _rx_eof(o._rx_eof), _tx_shutdown(o._tx_shutdown)
    {
        o._fd          = -1;
        o._rx_eof      = false;
        o._tx_shutdown = false;
    }
    tcp_socket& operator=(tcp_socket&& o) noexcept
    {
        if (this != &o) {
            close();
            _exec          = o._exec;
            _fd            = o._fd;
            _rx_eof        = o._rx_eof;
            _tx_shutdown   = o._tx_shutdown;
            o._fd          = -1;
            o._rx_eof      = false;
            o._tx_shutdown = false;
        }
        return *this;
    }
    ~tcp_socket() { close(); }
    void close()
    {
        if (_fd >= 0) {
            if (_reactor)
                _reactor->remove_fd(_fd);
            ::close(_fd);
            _fd = -1;
        }
    }
    void shutdown_tx()
    {
        if (_fd >= 0 && !_tx_shutdown) {
            ::shutdown(_fd, SHUT_WR);
            _tx_shutdown = true;
        }
    }
    bool tx_shutdown() const noexcept { return _tx_shutdown; }
    bool rx_eof() const noexcept { return _rx_eof; }
    int  native_handle() const { return _fd; }
    struct connect_awaiter : io_waiter_base {
        tcp_socket& sock;
        std::string host;
        uint16_t    port;
        int         ret { 0 };
        connect_awaiter(tcp_socket& s, std::string h, uint16_t p) : sock(s), host(std::move(h)), port(p) { }
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h = h;
            INIT_LIST_HEAD(&this->ws_node);
            sockaddr_in addr {};
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(port);
            if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
                ret = -1;
                sock._exec.post(*this);
                return;
            }
            int r = ::connect(sock._fd, (sockaddr*)&addr, sizeof(addr));
            if (r == 0) {
                ret = 0;
                sock._exec.post(*this);
                return;
            }
            if (r < 0 && errno != EINPROGRESS) {
                ret = -1;
                sock._exec.post(*this);
                return;
            }
            sock._reactor->add_waiter(sock._fd, EPOLLOUT, this);
        }
        int await_resume() noexcept
        {
            if (ret == 0) {
                int       err = 0;
                socklen_t len = sizeof(err);
                if (::getsockopt(sock._fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
                    return 0;
                return -1;
            }
            return ret;
        }
    };
    connect_awaiter connect(const std::string& host, uint16_t port) { return connect_awaiter(*this, host, port); }
    struct recv_awaiter : io_waiter_base {
        tcp_socket& sock;
        void*       buf;
        size_t      len;
        ssize_t     nread { 0 };
        recv_awaiter(tcp_socket& s, void* b, size_t l) : sock(s), buf(b), len(l) { }
        bool await_ready() noexcept
        {
            nread = ::recv(sock._fd, buf, len, MSG_DONTWAIT);
            if (nread >= 0)
                return true;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                nread = -1;
                return false;
            }
            return true;
        }
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h = h;
            INIT_LIST_HEAD(&this->ws_node);
            sock._reactor->add_waiter(sock._fd, EPOLLIN, this);
        }
        ssize_t await_resume() noexcept
        {
            if (nread >= 0) {
                if (nread == 0)
                    sock._rx_eof = true;
                return nread;
            }
            ssize_t r = ::recv(sock._fd, buf, len, MSG_DONTWAIT);
            if (r == 0)
                sock._rx_eof = true;
            return r;
        }
    };
    recv_awaiter recv(void* buf, size_t len) { return recv_awaiter(*this, buf, len); }
    struct send_awaiter : io_waiter_base {
        tcp_socket& sock;
        const void* buf;
        size_t      len;
        size_t      sent { 0 };
        send_awaiter(tcp_socket& s, const void* b, size_t l) : sock(s), buf(b), len(l) { }
        bool await_ready() noexcept
        {
            ssize_t n = ::send(sock._fd, (char*)buf + sent, len - sent, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n >= 0) {
                sent += (size_t)n;
                if (sent == len)
                    return true;
                return false;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return false;
            return true;
        }
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h = h;
            INIT_LIST_HEAD(&this->ws_node);
            sock._reactor->add_waiter(sock._fd, EPOLLOUT, this);
        }
        ssize_t await_resume() noexcept
        {
            if (sent == len)
                return (ssize_t)sent;
            while (sent < len) {
                ssize_t n = ::send(sock._fd, (char*)buf + sent, len - sent, MSG_DONTWAIT | MSG_NOSIGNAL);
                if (n > 0) {
                    sent += (size_t)n;
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    break;
                if (n < 0 && (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN))
                    sock._tx_shutdown = true;
                return n;
            }
            return (ssize_t)sent;
        }
    };
    send_awaiter send(const void* buf, size_t len) { return send_awaiter(*this, buf, len); }

private:
    friend class fd_workqueue<lock, Reactor>;
    explicit tcp_socket(workqueue<lock>& exec, Reactor<lock>& reactor) : _exec(exec), _reactor(&reactor)
    {
        _fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (_fd < 0)
            throw std::runtime_error("socket failed");
        set_non_block();
        _reactor->add_fd(_fd);
    }
    tcp_socket(int fd, workqueue<lock>& exec, Reactor<lock>& reactor) : _exec(exec), _reactor(&reactor), _fd(fd)
    {
        set_non_block();
        _reactor->add_fd(_fd);
    }
    void set_non_block()
    {
        int flags = ::fcntl(_fd, F_GETFL, 0);
        if (flags >= 0)
            ::fcntl(_fd, F_SETFL, flags | O_NONBLOCK);
    }
    workqueue<lock>& _exec;
    Reactor<lock>*   _reactor { nullptr };
    int              _fd { -1 };
    bool             _rx_eof { false };
    bool             _tx_shutdown { false };
};

// wrappers
template <lockable lock>
inline Task<int, Work_Promise<lock, int>> async_connect(tcp_socket<lock>& s, const std::string host, uint16_t port)
{
    co_return co_await s.connect(host, port);
}
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>> async_send_all(tcp_socket<lock>& s, const void* buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = co_await s.send((const char*)buf + sent, len - sent);
        if (n <= 0)
            co_return (sent > 0) ? (ssize_t)sent : n;
        sent += (size_t)n;
    }
    co_return (ssize_t) sent;
}
template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>> async_recv_some(tcp_socket<lock>& s, void* buf, size_t len)
{
    ssize_t n = co_await s.recv(buf, len);
    co_return n;
}

} // namespace co_wq::net
#endif
