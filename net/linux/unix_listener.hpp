// unix_listener.hpp - Unix Domain socket 异步 accept
#pragma once

#include "epoll_reactor.hpp"
#include "io_waiter.hpp"
#include "unix_socket.hpp"
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace co_wq::net {

template <lockable lock, template <class> class Reactor> class fd_workqueue;

inline constexpr int k_accept_fatal = -2;

template <lockable lock, template <class> class Reactor = epoll_reactor> class unix_listener {
public:
    explicit unix_listener(workqueue<lock>& exec, Reactor<lock>& reactor) : _exec(exec), _reactor(&reactor)
    {
        _fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (_fd < 0)
            throw std::runtime_error("listener socket failed");
        set_non_block();
        _reactor->add_fd(_fd);
    }
    ~unix_listener() { close(); }

    void bind_listen(const std::string& path, int backlog = 128, bool unlink_existing = true, mode_t mode = 0666)
    {
        if (_bound)
            throw std::runtime_error("listener already bound");
        sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        bool abstract   = !path.empty() && path[0] == '@';
        socklen_t slen  = 0;
        if (abstract) {
            size_t len = path.size();
            if (len <= 1 || len - 1 >= sizeof(addr.sun_path))
                throw std::runtime_error("unix path too long");
            addr.sun_path[0] = '\0';
            std::memcpy(addr.sun_path + 1, path.data() + 1, len - 1);
            slen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + len);
        } else {
            if (path.size() >= sizeof(addr.sun_path))
                throw std::runtime_error("unix path too long");
            std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
            addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
            slen                                     = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + std::strlen(addr.sun_path) + 1);
            if (unlink_existing)
                ::unlink(addr.sun_path);
        }
        if (::bind(_fd, reinterpret_cast<sockaddr*>(&addr), slen) < 0)
            throw std::runtime_error("bind failed");
        if (!abstract)
            ::chmod(addr.sun_path, mode);
        if (::listen(_fd, backlog) < 0)
            throw std::runtime_error("listen failed");
        _path      = path;
        _abstract  = abstract;
        _bound     = true;
    }

    void close()
    {
        if (_fd >= 0) {
            if (_reactor)
                _reactor->remove_fd(_fd);
            ::close(_fd);
            _fd = -1;
        }
        if (_bound) {
            if (!_abstract && !_path.empty()) {
                ::unlink(_path.c_str());
                _path.clear();
            }
            _bound = false;
        }
    }

    int native_handle() const { return _fd; }

    struct accept_awaiter : io_waiter_base {
        unix_listener& lst;
        int            newfd { -1 };
        accept_awaiter(unix_listener& l) : lst(l) { }
        bool await_ready() noexcept
        {
            newfd = try_accept();
            return newfd >= 0 || newfd == k_accept_fatal;
        }
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h = h;
            INIT_LIST_HEAD(&this->ws_node);
            if (lst._reactor)
                lst._reactor->add_waiter(lst._fd, EPOLLIN, this);
        }
        int await_resume() noexcept
        {
            if (newfd >= 0 || newfd == k_accept_fatal)
                return newfd;
            return try_accept();
        }
        int try_accept() noexcept
        {
            sockaddr_un addr;
            socklen_t   alen = sizeof(addr);
            int         fd   = ::accept4(lst._fd, reinterpret_cast<sockaddr*>(&addr), &alen, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (fd >= 0)
                return fd;
            if (fd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return -1;
            return k_accept_fatal;
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
    Reactor<lock>*   _reactor { nullptr };
    int              _fd { -1 };
    std::string      _path;
    bool             _abstract { false };
    bool             _bound { false };
};

template <lockable lock, template <class> class Reactor = epoll_reactor>
inline Task<int, Work_Promise<lock, int>> async_accept(unix_listener<lock, Reactor>& lst)
{
    int fd = co_await lst.accept();
    co_return fd;
}

template <lockable lock, template <class> class Reactor = epoll_reactor>
inline Task<unix_socket<lock, Reactor>, Work_Promise<lock, unix_socket<lock, Reactor>>>
async_accept_socket(fd_workqueue<lock, Reactor>& fwq, unix_listener<lock, Reactor>& lst)
{
    int fd = co_await lst.accept();
    if (fd < 0) {
        auto tmp = fwq.make_unix_socket();
        tmp.close();
        co_return std::move(tmp);
    }
    co_return fwq.adopt_unix_socket(fd);
}

} // namespace co_wq::net
