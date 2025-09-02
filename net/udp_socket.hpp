// udp_socket.hpp - UDP socket coroutine primitives
#pragma once
#ifdef __linux__
#include "epoll_reactor.hpp"
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

template <lockable lock, template <class> class Reactor> class fd_workqueue; // fwd

template <lockable lock, template <class> class Reactor = epoll_reactor> class udp_socket {
public:
    udp_socket()                             = delete;
    udp_socket(const udp_socket&)            = delete;
    udp_socket& operator=(const udp_socket&) = delete;
    udp_socket(udp_socket&& o) noexcept : _exec(o._exec), _reactor(o._reactor), _fd(o._fd) { o._fd = -1; }
    udp_socket& operator=(udp_socket&& o) noexcept
    {
        if (this != &o) {
            close();
            _exec    = o._exec;
            _reactor = o._reactor;
            _fd      = o._fd;
            o._fd    = -1;
        }
        return *this;
    }
    ~udp_socket() { close(); }
    void close()
    {
        if (_fd >= 0) {
            if (_reactor)
                _reactor->remove_fd(_fd);
            ::close(_fd);
            _fd = -1;
        }
    }
    int native_handle() const { return _fd; }

    // Optional connect to set default peer
    struct connect_awaiter : io_waiter_base {
        udp_socket& sock;
        std::string host;
        uint16_t    port;
        int         ret { 0 };
        connect_awaiter(udp_socket& s, std::string h, uint16_t p) : sock(s), host(std::move(h)), port(p) { }
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

    struct recvfrom_awaiter : io_waiter_base {
        udp_socket&  sock;
        void*        buf;
        size_t       len;
        sockaddr_in* out_addr;
        socklen_t*   out_len;
        ssize_t      nread { 0 };
        recvfrom_awaiter(udp_socket& s, void* b, size_t l, sockaddr_in* oa, socklen_t* ol)
            : sock(s), buf(b), len(l), out_addr(oa), out_len(ol)
        {
        }
        bool await_ready() noexcept
        {
            nread = ::recvfrom(sock._fd, buf, len, MSG_DONTWAIT, (sockaddr*)out_addr, out_len);
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
            if (nread >= 0)
                return nread;
            return ::recvfrom(sock._fd, buf, len, MSG_DONTWAIT, (sockaddr*)out_addr, out_len);
        }
    };
    recvfrom_awaiter recv_from(void* buf, size_t len, sockaddr_in* addr, socklen_t* addrlen)
    {
        return recvfrom_awaiter(*this, buf, len, addr, addrlen);
    }

    struct sendto_awaiter : io_waiter_base {
        udp_socket&        sock;
        const void*        buf;
        size_t             len;
        const sockaddr_in* dest;
        size_t             sent { 0 };
        sendto_awaiter(udp_socket& s, const void* b, size_t l, const sockaddr_in* d) : sock(s), buf(b), len(l), dest(d)
        {
        }
        bool await_ready() noexcept
        {
            ssize_t n = ::sendto(sock._fd,
                                 (const char*)buf + sent,
                                 len - sent,
                                 MSG_DONTWAIT | MSG_NOSIGNAL,
                                 (const sockaddr*)dest,
                                 sizeof(*dest));
            if (n >= 0) {
                sent += (size_t)n;
                return sent == len;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return false;
            return true; // error
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
                ssize_t n = ::sendto(sock._fd,
                                     (const char*)buf + sent,
                                     len - sent,
                                     MSG_DONTWAIT | MSG_NOSIGNAL,
                                     (const sockaddr*)dest,
                                     sizeof(*dest));
                if (n > 0) {
                    sent += (size_t)n;
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    break; // wait again
                return n;  // error
            }
            return (ssize_t)sent;
        }
    };
    sendto_awaiter send_to(const void* buf, size_t len, const sockaddr_in& dest)
    {
        return sendto_awaiter(*this, buf, len, &dest);
    }

private:
    friend class fd_workqueue<lock, Reactor>;
    udp_socket(workqueue<lock>& exec, Reactor<lock>& reactor) : _exec(exec), _reactor(&reactor)
    {
        _fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (_fd < 0)
            throw std::runtime_error("udp socket failed");
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
};

// Convenience async wrappers
template <lockable lock, template <class> class Reactor>
inline Task<ssize_t, Work_Promise<lock, ssize_t>>
async_udp_recv_from(udp_socket<lock, Reactor>& s, void* buf, size_t len, sockaddr_in* addr, socklen_t* alen)
{
    co_return co_await s.recv_from(buf, len, addr, alen);
}

template <lockable lock, template <class> class Reactor>
inline Task<ssize_t, Work_Promise<lock, ssize_t>>
async_udp_send_to(udp_socket<lock, Reactor>& s, const void* buf, size_t len, const sockaddr_in& dest)
{
    co_return co_await s.send_to(buf, len, dest);
}

} // namespace co_wq::net
#endif
