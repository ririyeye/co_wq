// tcp.hpp - minimal coroutine-based TCP socket helpers integrated with workqueue
// Linux epoll implementation (header-only). Designed to work with Task/Work_Promise.
// Usage outline:
//   auto& wq = get_sys_workqueue();
//   co_wq::net::init_tcp(wq); // once
//   co_wq::net::tcp_socket<co_wq::nolock> sock;
//   co_await sock.connect("127.0.0.1", 8080);
//   std::string msg = "hello";
//   co_await sock.send(msg.data(), msg.size());
//   char buf[128]; auto n = co_await sock.recv(buf, sizeof(buf));
// All resumes happen on the provided workqueue.

#pragma once

#ifdef __linux__

#include <arpa/inet.h>
#include <atomic>
#include <coroutine>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "worker.hpp"    // Work_Promise & Task
#include "workqueue.hpp" // workqueue

namespace co_wq::net {

// Forward reactor singleton accessor
template <lockable lock> class epoll_reactor;

// Internal waiter node posted back to workqueue to resume coroutine
struct io_waiter_base : worknode {
    std::coroutine_handle<> h; // coroutine to resume
    static void             resume_cb(struct worknode* w)
    {
        io_waiter_base* self = static_cast<io_waiter_base*>(w);
        if (self->h)
            self->h.resume();
    }
};

template <lockable lock> class epoll_reactor {
public:
    explicit epoll_reactor(workqueue<lock>& exec) : _exec(exec)
    {
        _epfd = ::epoll_create1(EPOLL_CLOEXEC);
        if (_epfd < 0)
            throw std::runtime_error("epoll_create1 failed");
        _running.store(true, std::memory_order_relaxed);
        _thr = std::thread([this] { this->run_loop(); });
    }

    ~epoll_reactor()
    {
        _running.store(false, std::memory_order_relaxed);
        if (_epfd >= 0) {
            ::epoll_ctl(_epfd, EPOLL_CTL_ADD, -1, nullptr); // no-op to wake? best-effort
        }
        if (_thr.joinable())
            _thr.join();
        if (_epfd >= 0)
            ::close(_epfd);
    }

    void add_fd(int fd)
    {
        std::scoped_lock lk(_mtx);
        epoll_event      ev {};
        ev.data.fd = fd;
        ev.events  = EPOLLIN | EPOLLOUT | EPOLLET; // edge-trigger for both; we filter later
        if (::epoll_ctl(_epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            if (errno == EEXIST) {
                ::epoll_ctl(_epfd, EPOLL_CTL_MOD, fd, &ev);
            }
        }
    }

    void add_waiter(int fd, uint32_t evmask, io_waiter_base* waiter)
    {
        std::scoped_lock lk(_mtx);
        waiter->func = &io_waiter_base::resume_cb;
        INIT_LIST_HEAD(&waiter->ws_node);
        _waiters[fd].push_back({ evmask, waiter });
        // ensure fd tracked by epoll
        epoll_event ev {};
        ev.data.fd = fd;
        // always monitor both IN and OUT (edge trigger) to simplify
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLERR | EPOLLHUP;
        ::epoll_ctl(_epfd, EPOLL_CTL_MOD, fd, &ev);
    }

    static epoll_reactor& instance(workqueue<lock>& exec)
    {
        static epoll_reactor* inst = nullptr;
        static std::mutex     imtx;
        if (!inst) {
            std::scoped_lock lk(imtx);
            if (!inst) {
                inst = new epoll_reactor(exec);
            }
        }
        return *inst;
    }

private:
    struct waiter_item {
        uint32_t        mask;
        io_waiter_base* waiter;
    };

    void run_loop()
    {
        constexpr int            MAX_EVENTS = 32;
        std::vector<epoll_event> evs(MAX_EVENTS);
        while (_running.load(std::memory_order_relaxed)) {
            int n = ::epoll_wait(_epfd, evs.data(), MAX_EVENTS, 50); // 50ms timeout
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (n == 0)
                continue;
            for (int i = 0; i < n; ++i) {
                int                      fd    = evs[i].data.fd;
                uint32_t                 flags = evs[i].events;
                std::vector<waiter_item> to_resume;
                {
                    std::scoped_lock lk(_mtx);
                    auto             it = _waiters.find(fd);
                    if (it != _waiters.end()) {
                        auto& vec = it->second;
                        // collect those whose mask satisfied
                        auto new_end = std::remove_if(vec.begin(), vec.end(), [&](waiter_item& wi) {
                            if ((flags & wi.mask) || (flags & (EPOLLERR | EPOLLHUP))) {
                                to_resume.push_back(wi);
                                return true; // remove
                            }
                            return false;
                        });
                        vec.erase(new_end, vec.end());
                        if (vec.empty()) {
                            _waiters.erase(it);
                        }
                    }
                }
                for (auto& wi : to_resume) {
                    _exec.post(*wi.waiter); // will resume on workqueue loop
                }
            }
        }
    }

    // resume_cb provided in io_waiter_base

    int                                               _epfd { -1 };
    std::atomic_bool                                  _running { false };
    std::thread                                       _thr;
    workqueue<lock>&                                  _exec;
    std::mutex                                        _mtx;
    std::unordered_map<int, std::vector<waiter_item>> _waiters; // fd -> waiters
};

// tcp_socket: non-owning of reactor, owns fd.
template <lockable lock> class tcp_socket {
public:
    explicit tcp_socket(workqueue<lock>& exec) : _exec(exec)
    {
        _fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (_fd < 0)
            throw std::runtime_error("socket failed");
        set_non_block();
        epoll_reactor<lock>::instance(_exec).add_fd(_fd);
    }

    tcp_socket(int existing_fd, workqueue<lock>& exec) : _exec(exec), _fd(existing_fd)
    {
        set_non_block();
        epoll_reactor<lock>::instance(_exec).add_fd(_fd);
    }

    ~tcp_socket()
    {
        if (_fd >= 0)
            ::close(_fd);
    }

    int native_handle() const { return _fd; }

    // Awaitable connect
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
            // resolve & initiate
            sockaddr_in addr {};
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(port);
            if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
                ret = -1;
                sock._exec.post(*this); // resume with error
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
            // wait for writability
            epoll_reactor<lock>::instance(sock._exec).add_waiter(sock._fd, EPOLLOUT, this);
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
            return true; // error case propagate immediately
        }
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h = h;
            INIT_LIST_HEAD(&this->ws_node);
            epoll_reactor<lock>::instance(sock._exec).add_waiter(sock._fd, EPOLLIN, this);
        }
        ssize_t await_resume() noexcept
        {
            if (nread >= 0)
                return nread;
            // try again after readiness
            return ::recv(sock._fd, buf, len, MSG_DONTWAIT);
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
                    return true; // done
                return false;    // still remaining; wait for writable
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return false;
            }
            // error -> complete now
            return true;
        }
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h = h;
            INIT_LIST_HEAD(&this->ws_node);
            epoll_reactor<lock>::instance(sock._exec).add_waiter(sock._fd, EPOLLOUT, this);
        }
        ssize_t await_resume() noexcept
        {
            if (sent == len)
                return (ssize_t)sent;
            // try to flush remaining
            while (sent < len) {
                ssize_t n = ::send(sock._fd, (char*)buf + sent, len - sent, MSG_DONTWAIT | MSG_NOSIGNAL);
                if (n > 0) {
                    sent += (size_t)n;
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    // need another suspension: create a fresh awaiter by returning partial (not typical). For
                    // simplicity return bytes sent.
                    break;
                }
                return n; // error
            }
            return (ssize_t)sent;
        }
    };

    send_awaiter send(const void* buf, size_t len) { return send_awaiter(*this, buf, len); }

private:
    void set_non_block()
    {
        int flags = ::fcntl(_fd, F_GETFL, 0);
        if (flags >= 0) {
            ::fcntl(_fd, F_SETFL, flags | O_NONBLOCK);
        }
    }

    workqueue<lock>& _exec;
    int              _fd { -1 };
};

// Convenience coroutine wrappers returning Task types (using Work_Promise so they execute on workqueue)
template <lockable lock>
inline Task<int, Work_Promise<lock, int>> async_connect(tcp_socket<lock>& sock, const std::string host, uint16_t port)
{
    co_return co_await sock.connect(host, port);
}

template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>> async_send_all(tcp_socket<lock>& sock, const void* buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = co_await sock.send((const char*)buf + sent, len - sent);
        if (n <= 0) {
            co_return (sent > 0) ? (ssize_t)sent : n; // return bytes sent or error
        }
        sent += (size_t)n;
    }
    co_return (ssize_t) sent;
}

template <lockable lock>
inline Task<ssize_t, Work_Promise<lock, ssize_t>> async_recv_some(tcp_socket<lock>& sock, void* buf, size_t len)
{
    ssize_t n = co_await sock.recv(buf, len);
    co_return n;
}

// Helper to initialize reactor explicitly (optional as first socket does it implicitly)
template <lockable lock> inline void init_tcp(workqueue<lock>& exec)
{
    (void)epoll_reactor<lock>::instance(exec);
}

} // namespace co_wq::net

#endif // __linux__
