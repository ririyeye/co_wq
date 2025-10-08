// unified fd_base.hpp (placed under io/) - platform specific parts separated by macros
#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <basetsd.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef _SSIZE_T_DEFINED
using ssize_t = SSIZE_T;
#define _SSIZE_T_DEFINED
#endif
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "file_io.hpp" // file_handle (platform-specific inside)
#include "reactor_default.hpp"
#include "tcp_socket.hpp" // per-platform (search path provides correct one)
#include "udp_socket.hpp" // may be unused on some platforms but harmless
#if !defined(_WIN32)
#include "unix_socket.hpp"
#endif
#if defined(USING_SSL)
#include "tls.hpp"
#endif

#include <stdexcept>
#include <utility>

namespace co_wq::net {

template <lockable lock, template <class> class Reactor> class fd_workqueue; // fwd

template <lockable lock, template <class> class Reactor = CO_WQ_DEFAULT_REACTOR> class fd_object {
public:
    fd_object(const fd_object&)            = delete;
    fd_object& operator=(const fd_object&) = delete;
    fd_object(fd_object&& other) noexcept : _exec(other._exec), _reactor(other._reactor), _fd(other._fd)
    {
        other._fd      = invalid_fd_value;
        other._reactor = nullptr;
    }
    fd_object& operator=(fd_object&& other) noexcept
    {
        if (this != &other) {
            close();
            _exec          = other._exec;
            _reactor       = other._reactor;
            _fd            = other._fd;
            other._fd      = invalid_fd_value;
            other._reactor = nullptr;
        }
        return *this;
    }
    ~fd_object() { close(); }

    int native_handle() const { return _fd; }

    void close()
    {
        if (!is_valid())
            return;
        if (_reactor)
            _reactor->remove_fd(_fd);
        close_native(_fd);
        _fd = invalid_fd_value;
    }

protected:
    explicit fd_object(workqueue<lock>& exec, Reactor<lock>& reactor, int fd) : _exec(exec), _reactor(&reactor), _fd(fd)
    {
        if (!is_valid())
            throw std::runtime_error(invalid_fd_message());
        set_non_block();
        _reactor->add_fd(_fd);
    }

    void set_non_block()
    {
#if defined(_WIN32)
        u_long m = 1;
        ioctlsocket((SOCKET)_fd, FIONBIO, &m);
#else
        int flags = ::fcntl(_fd, F_GETFL, 0);
        if (flags >= 0)
            ::fcntl(_fd, F_SETFL, flags | O_NONBLOCK);
#endif
    }

    bool is_valid() const { return _fd != invalid_fd_value; }

    static const char* invalid_fd_message()
    {
#if defined(_WIN32)
        return "invalid socket";
#else
        return "invalid fd";
#endif
    }

    static void close_native(int fd)
    {
#if defined(_WIN32)
        ::closesocket((SOCKET)fd);
#else
        ::close(fd);
#endif
    }

    workqueue<lock>& _exec;
    Reactor<lock>*   _reactor { nullptr };
    int              _fd { invalid_fd_value };

private:
#if defined(_WIN32)
    static constexpr int invalid_fd_value = (int)INVALID_SOCKET;
#else
    static constexpr int invalid_fd_value = -1;
#endif

    friend class fd_workqueue<lock, Reactor>;
};

template <lockable lock, template <class> class Reactor = CO_WQ_DEFAULT_REACTOR> class fd_workqueue {
public:
    explicit fd_workqueue(workqueue<lock>& base) : _base(base), _reactor(base) { }

    workqueue<lock>& base() { return _base; }
    Reactor<lock>&   reactor() { return _reactor; }

    tcp_socket<lock, Reactor> make_tcp_socket(int family = AF_INET, bool dual_stack = false)
    {
        return tcp_socket<lock, Reactor>(_base, _reactor, family, dual_stack);
    }

    tcp_socket<lock, Reactor> adopt_tcp_socket(int fd) { return tcp_socket<lock, Reactor>(fd, _base, _reactor); }

#if defined(USING_SSL)
    tls_socket<lock, Reactor> make_tls_socket(tls_context ctx, tls_mode mode)
    {
        return tls_socket<lock, Reactor>(_base, _reactor, make_tcp_socket(), std::move(ctx), mode);
    }

    tls_socket<lock, Reactor> adopt_tls_socket(int fd, tls_context ctx, tls_mode mode)
    {
        return tls_socket<lock, Reactor>(_base, _reactor, adopt_tcp_socket(fd), std::move(ctx), mode);
    }
#endif

    udp_socket<lock, Reactor> make_udp_socket() { return udp_socket<lock, Reactor>(_base, _reactor); }

#if defined(_WIN32)
    file_handle<lock, Reactor> make_file(HANDLE h) { return file_handle<lock, Reactor>(_base, _reactor, h); }
#else
    int open_file(const char* path, int flags, mode_t mode = 0644)
    {
        int fd = ::open(path, flags | O_CLOEXEC | O_NONBLOCK, mode);
        if (fd < 0)
            throw std::runtime_error("open failed");
        _reactor.add_fd(fd);
        return fd;
    }

    unix_socket<lock, Reactor> make_unix_socket() { return unix_socket<lock, Reactor>(_base, _reactor); }
    unix_socket<lock, Reactor> adopt_unix_socket(int fd) { return unix_socket<lock, Reactor>(fd, _base, _reactor); }
    file_handle<lock, Reactor> make_file(int fd) { return file_handle<lock, Reactor>(_base, _reactor, fd); }
#endif

private:
    workqueue<lock>& _base;
    Reactor<lock>    _reactor;
};

} // namespace co_wq::net
