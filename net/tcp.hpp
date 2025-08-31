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
        if (_fds.find(fd) == _fds.end()) {
            _fds.emplace(fd, fd_state {});
        }
        // initially not interested in anything until waiter added
    }

    void add_waiter(int fd, uint32_t evmask, io_waiter_base* waiter)
    {
        std::scoped_lock lk(_mtx);
        waiter->func = &io_waiter_base::resume_cb;
        INIT_LIST_HEAD(&waiter->ws_node);
        auto& st = _fds[fd];
        st.waiters.push_back({ evmask, waiter });
        update_fd_interest_unlocked(fd, st);
    }

    void remove_fd(int fd)
    {
        std::vector<waiter_item> to_resume;
        {
            std::scoped_lock lk(_mtx);
            auto             it = _fds.find(fd);
            if (it != _fds.end()) {
                to_resume.swap(it->second.waiters);
                _fds.erase(it);
            }
        }
        ::epoll_ctl(_epfd, EPOLL_CTL_DEL, fd, nullptr);
        for (auto& wi : to_resume) {
            _exec.post(*wi.waiter);
        }
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

    struct fd_state {
        std::vector<waiter_item> waiters;        // pending waiters
        uint32_t                 interest { 0 }; // current epoll interest mask (IN|OUT subset)
    };

    void update_fd_interest_unlocked(int fd, fd_state& st)
    {
        uint32_t new_interest = 0;
        for (auto& wi : st.waiters)
            new_interest |= wi.mask;
        if (new_interest == st.interest) {
            return; // no change
        }
        st.interest = new_interest;
        if (st.interest == 0) {
            // no waiters left -> remove interest
            ::epoll_ctl(_epfd, EPOLL_CTL_DEL, fd, nullptr);
            return;
        }
        epoll_event ev {};
        ev.data.fd = fd;
        ev.events  = st.interest | EPOLLERR | EPOLLHUP | EPOLLET;
        if (::epoll_ctl(_epfd, EPOLL_CTL_MOD, fd, &ev) < 0) {
            if (errno == ENOENT) {
                ::epoll_ctl(_epfd, EPOLL_CTL_ADD, fd, &ev);
            }
        }
    }

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
                    auto             it = _fds.find(fd);
                    if (it != _fds.end()) {
                        auto& st      = it->second;
                        auto& vec     = st.waiters;
                        auto  new_end = std::remove_if(vec.begin(), vec.end(), [&](waiter_item& wi) {
                            if ((flags & wi.mask) || (flags & (EPOLLERR | EPOLLHUP))) {
                                to_resume.push_back(wi);
                                return true;
                            }
                            return false;
                        });
                        vec.erase(new_end, vec.end());
                        update_fd_interest_unlocked(fd, st); // may drop interest or modify
                    }
                }
                for (auto& wi : to_resume) {
                    _exec.post(*wi.waiter);
                }
            }
        }
    }

    // resume_cb provided in io_waiter_base

    int                               _epfd { -1 };
    std::atomic_bool                  _running { false };
    std::thread                       _thr;
    workqueue<lock>&                  _exec;
    std::mutex                        _mtx;
    std::unordered_map<int, fd_state> _fds; // fd -> state (waiters + interest)
};

// tcp_socket: non-owning of reactor, owns fd.
template <lockable lock> class fd_workqueue; // forward

template <lockable lock> class tcp_socket {
public:
    tcp_socket()                             = delete;
    tcp_socket(const tcp_socket&)            = delete;
    tcp_socket& operator=(const tcp_socket&) = delete;
    tcp_socket(tcp_socket&& other) noexcept
        : _exec(other._exec), _fd(other._fd), _rx_eof(other._rx_eof), _tx_shutdown(other._tx_shutdown)
    {
        other._fd          = -1;
        other._rx_eof      = false;
        other._tx_shutdown = false;
    }
    tcp_socket& operator=(tcp_socket&& other) noexcept
    {
        if (this != &other) {
            close();
            _exec              = other._exec;
            _fd                = other._fd;
            _rx_eof            = other._rx_eof;
            _tx_shutdown       = other._tx_shutdown;
            other._fd          = -1;
            other._rx_eof      = false;
            other._tx_shutdown = false;
        }
        return *this;
    }

    ~tcp_socket() { close(); }

    void close()
    {
        if (_fd >= 0) {
            // notify reactor to drop and wake waiters
            epoll_reactor<lock>::instance(_exec).remove_fd(_fd);
            ::close(_fd);
            _fd = -1;
        }
    }

    // half-close (shutdown write side)
    void shutdown_tx()
    {
        if (_fd >= 0 && !_tx_shutdown) {
            ::shutdown(_fd, SHUT_WR);
            _tx_shutdown = true;
        }
    }

    bool tx_shutdown() const noexcept { return _tx_shutdown; }
    bool rx_eof() const noexcept { return _rx_eof; }

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
                if (nread == 0) {
                    sock._rx_eof = true; // peer closed write
                }
            return nread;
            // try again after readiness
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
                if (n < 0 && (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN)) {
                    sock._tx_shutdown = true; // peer closed or connection reset
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

    // only factory (fd_workqueue) can construct
    friend class fd_workqueue<lock>;
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

    workqueue<lock>& _exec;
    int              _fd { -1 };
    bool             _rx_eof { false };
    bool             _tx_shutdown { false };
};

// fd_workqueue: abstraction owning a base workqueue reference for fd-based IO
template <lockable lock> class fd_workqueue {
public:
    explicit fd_workqueue(workqueue<lock>& base) : _base(base) { }

    workqueue<lock>& base() { return _base; }

    tcp_socket<lock> make_tcp_socket() { return tcp_socket<lock>(_base); }
    tcp_socket<lock> adopt_tcp_socket(int fd) { return tcp_socket<lock>(fd, _base); }

private:
    workqueue<lock>& _base;
};

// tcp_listener: accept incoming connections
template <lockable lock> class tcp_listener {
public:
    explicit tcp_listener(workqueue<lock>& exec) : _exec(exec)
    {
        _fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (_fd < 0)
            throw std::runtime_error("listener socket failed");
        set_non_block();
        epoll_reactor<lock>::instance(_exec).add_fd(_fd);
    }
    ~tcp_listener() { close(); }

    void bind_listen(const std::string& host, uint16_t port, int backlog = 128)
    {
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        if (host.empty() || host == "0.0.0.0") {
            addr.sin_addr.s_addr = INADDR_ANY;
        } else if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
            throw std::runtime_error("inet_pton failed");
        }
        int opt = 1;
        ::setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (::bind(_fd, (sockaddr*)&addr, sizeof(addr)) < 0)
            throw std::runtime_error("bind failed");
        if (::listen(_fd, backlog) < 0)
            throw std::runtime_error("listen failed");
    }

    void close()
    {
        if (_fd >= 0) {
            epoll_reactor<lock>::instance(_exec).remove_fd(_fd);
            ::close(_fd);
            _fd = -1;
        }
    }

    int native_handle() const { return _fd; }

    struct accept_awaiter : io_waiter_base {
        tcp_listener& lst;
        int           newfd { -1 };
        accept_awaiter(tcp_listener& l) : lst(l) { }
        bool await_ready() noexcept
        {
            newfd = try_accept();
            if (newfd >= 0 || newfd == -2) // -2 indicates fatal error
                return true;
            return false; // EAGAIN -> suspend
        }
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h = h;
            INIT_LIST_HEAD(&this->ws_node);
            epoll_reactor<lock>::instance(lst._exec).add_waiter(lst._fd, EPOLLIN, this);
        }
        int await_resume() noexcept
        {
            if (newfd >= 0 || newfd == -2)
                return newfd;
            // try again
            return try_accept();
        }
        int try_accept() noexcept
        {
            sockaddr_in addr;
            socklen_t   alen = sizeof(addr);
            int         fd   = ::accept4(lst._fd, (sockaddr*)&addr, &alen, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (fd >= 0)
                return fd;
            if (fd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return -1; // need wait
            return -2;     // fatal error indicator
        }
    };

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

template <lockable lock> inline Task<int, Work_Promise<lock, int>> async_accept(tcp_listener<lock>& lst)
{
    int fd = co_await lst.accept();
    co_return fd;
}

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
