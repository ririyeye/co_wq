#pragma once

#include <cerrno>
#include <cstddef>
#include <mutex>
#include <stdexcept>

#ifdef _WIN32
#include <BaseTsd.h>
#include <mstcpip.h>
#include <vector>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>


#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef SHUT_RD
#define SHUT_RD SD_RECEIVE
#endif
#ifndef SHUT_WR
#define SHUT_WR SD_SEND
#endif
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0
#endif
#ifndef F_GETFL
#define F_GETFL 1
#endif
#ifndef F_SETFL
#define F_SETFL 2
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x4000
#endif

namespace co_wq::net::os {

using fd_t    = SOCKET;
using ssize_t = SSIZE_T;

#ifndef IOVEC_DEFINED_CO_WQ
struct iovec {
    void*  iov_base;
    size_t iov_len;
};
#define IOVEC_DEFINED_CO_WQ 1
#endif

inline fd_t invalid_fd() noexcept
{
    return INVALID_SOCKET;
}

inline void set_errno_from_wsa(int err)
{
    switch (err) {
    case WSAEWOULDBLOCK:
        errno = EWOULDBLOCK;
        break;
    case WSAEINPROGRESS:
        errno = EINPROGRESS;
        break;
    case WSAEALREADY:
        errno = EALREADY;
        break;
    case WSAENOTSOCK:
        errno = ENOTSOCK;
        break;
    case WSAEDESTADDRREQ:
        errno = EDESTADDRREQ;
        break;
    case WSAEMSGSIZE:
        errno = EMSGSIZE;
        break;
    case WSAEPROTOTYPE:
        errno = EPROTOTYPE;
        break;
    case WSAENOPROTOOPT:
        errno = ENOPROTOOPT;
        break;
    case WSAEPROTONOSUPPORT:
        errno = EPROTONOSUPPORT;
        break;
    case WSAEOPNOTSUPP:
        errno = EOPNOTSUPP;
        break;
    case WSAEAFNOSUPPORT:
        errno = EAFNOSUPPORT;
        break;
    case WSAEADDRINUSE:
        errno = EADDRINUSE;
        break;
    case WSAEADDRNOTAVAIL:
        errno = EADDRNOTAVAIL;
        break;
    case WSAENETDOWN:
        errno = ENETDOWN;
        break;
    case WSAENETUNREACH:
        errno = ENETUNREACH;
        break;
    case WSAENETRESET:
        errno = ENETRESET;
        break;
    case WSAECONNABORTED:
        errno = ECONNABORTED;
        break;
    case WSAECONNRESET:
        errno = ECONNRESET;
        break;
    case WSAENOBUFS:
        errno = ENOBUFS;
        break;
    case WSAEISCONN:
        errno = EISCONN;
        break;
    case WSAENOTCONN:
        errno = ENOTCONN;
        break;
    case WSAETIMEDOUT:
        errno = ETIMEDOUT;
        break;
    case WSAEHOSTUNREACH:
        errno = EHOSTUNREACH;
        break;
    case WSAEINVAL:
        errno = EINVAL;
        break;
    case WSAESHUTDOWN:
        errno = EPIPE;
        break;
    case WSAEMFILE:
        errno = EMFILE;
        break;
    default:
        errno = err;
        break;
    }
}

inline void ensure_wsa()
{
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data {};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            set_errno_from_wsa(WSAGetLastError());
            throw std::runtime_error("WSAStartup failed");
        }
    });
}

inline fd_t create_socket(int domain, int type, int protocol)
{
    ensure_wsa();
    DWORD flags = WSA_FLAG_OVERLAPPED;
#ifdef WSA_FLAG_NO_HANDLE_INHERIT
    flags |= WSA_FLAG_NO_HANDLE_INHERIT;
#endif
    SOCKET s = WSASocketW(domain, type, protocol, nullptr, 0, flags);
    if (s == INVALID_SOCKET) {
        set_errno_from_wsa(WSAGetLastError());
        return invalid_fd();
    }
    SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
    return s;
}

inline int close_fd(fd_t fd)
{
    if (fd == invalid_fd())
        return 0;
    if (closesocket(fd) == SOCKET_ERROR) {
        set_errno_from_wsa(WSAGetLastError());
        return -1;
    }
    return 0;
}

inline int set_non_block(fd_t fd)
{
    u_long mode = 1;
    if (ioctlsocket(fd, FIONBIO, &mode) == SOCKET_ERROR) {
        set_errno_from_wsa(WSAGetLastError());
        return -1;
    }
    return 0;
}

inline int connect(fd_t fd, const sockaddr* addr, socklen_t len)
{
    if (::connect(fd, addr, len) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        set_errno_from_wsa(err);
        return -1;
    }
    return 0;
}

inline int getsockopt(fd_t fd, int level, int optname, void* optval, socklen_t* optlen)
{
    int len = optlen ? static_cast<int>(*optlen) : 0;
    int rc  = ::getsockopt(fd, level, optname, static_cast<char*>(optval), &len);
    if (rc == SOCKET_ERROR) {
        set_errno_from_wsa(WSAGetLastError());
        return -1;
    }
    if (optlen)
        *optlen = static_cast<socklen_t>(len);
    return 0;
}

inline int setsockopt(fd_t fd, int level, int optname, const void* optval, socklen_t optlen)
{
    if (::setsockopt(fd, level, optname, static_cast<const char*>(optval), static_cast<int>(optlen)) == SOCKET_ERROR) {
        set_errno_from_wsa(WSAGetLastError());
        return -1;
    }
    return 0;
}

inline int shutdown(fd_t fd, int how)
{
    if (::shutdown(fd, how) == SOCKET_ERROR) {
        set_errno_from_wsa(WSAGetLastError());
        return -1;
    }
    return 0;
}

inline int getsockname(fd_t fd, sockaddr* addr, socklen_t* len)
{
    int alen = len ? static_cast<int>(*len) : 0;
    int rc   = ::getsockname(fd, addr, len ? &alen : nullptr);
    if (rc == SOCKET_ERROR) {
        set_errno_from_wsa(WSAGetLastError());
        return -1;
    }
    if (len)
        *len = static_cast<socklen_t>(alen);
    return 0;
}

inline int listen(fd_t fd, int backlog)
{
    if (::listen(fd, backlog) == SOCKET_ERROR) {
        set_errno_from_wsa(WSAGetLastError());
        return -1;
    }
    return 0;
}

inline fd_t accept(fd_t fd, sockaddr* addr, socklen_t* len, int flags)
{
    int    alen = len ? static_cast<int>(*len) : 0;
    SOCKET s    = ::accept(fd, addr, len ? &alen : nullptr);
    if (s == INVALID_SOCKET) {
        set_errno_from_wsa(WSAGetLastError());
        return invalid_fd();
    }
    if (flags & SOCK_NONBLOCK)
        set_non_block(s);
    if (flags & SOCK_CLOEXEC)
        SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
    if (len)
        *len = static_cast<socklen_t>(alen);
    return s;
}

inline ssize_t recv(fd_t fd, void* buf, size_t len, int flags)
{
    int wflags = flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL);
    int rc     = ::recv(fd, static_cast<char*>(buf), static_cast<int>(len), wflags);
    if (rc == SOCKET_ERROR) {
        set_errno_from_wsa(WSAGetLastError());
        return -1;
    }
    return rc;
}

inline ssize_t recvfrom(fd_t fd, void* buf, size_t len, int flags, sockaddr* addr, socklen_t* addrlen)
{
    int alen = addrlen ? static_cast<int>(*addrlen) : 0;
    int rc   = ::recvfrom(fd, static_cast<char*>(buf), static_cast<int>(len), flags, addr, addrlen ? &alen : nullptr);
    if (rc == SOCKET_ERROR) {
        set_errno_from_wsa(WSAGetLastError());
        return -1;
    }
    if (addrlen)
        *addrlen = static_cast<socklen_t>(alen);
    return rc;
}

inline ssize_t send(fd_t fd, const void* buf, size_t len, int flags)
{
    int wflags = flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL);
    int rc     = ::send(fd, static_cast<const char*>(buf), static_cast<int>(len), wflags);
    if (rc == SOCKET_ERROR) {
        set_errno_from_wsa(WSAGetLastError());
        return -1;
    }
    return rc;
}

inline ssize_t sendto(fd_t fd, const void* buf, size_t len, int flags, const sockaddr* addr, socklen_t addrlen)
{
    int rc = ::sendto(fd, static_cast<const char*>(buf), static_cast<int>(len), flags, addr, addrlen);
    if (rc == SOCKET_ERROR) {
        set_errno_from_wsa(WSAGetLastError());
        return -1;
    }
    return rc;
}

inline ssize_t writev(fd_t fd, const struct iovec* iov, int iovcnt)
{
    std::vector<WSABUF> bufs(static_cast<size_t>(iovcnt));
    for (int i = 0; i < iovcnt; ++i) {
        bufs[static_cast<size_t>(i)].buf = static_cast<char*>(iov[static_cast<size_t>(i)].iov_base);
        bufs[static_cast<size_t>(i)].len = static_cast<ULONG>(iov[static_cast<size_t>(i)].iov_len);
    }
    DWORD sent = 0;
    if (WSASend(fd, bufs.data(), static_cast<DWORD>(bufs.size()), &sent, 0, nullptr, nullptr) == SOCKET_ERROR) {
        set_errno_from_wsa(WSAGetLastError());
        return -1;
    }
    return static_cast<ssize_t>(sent);
}

inline ssize_t
sendto_vec(fd_t fd, const struct iovec* iov, int iovcnt, const sockaddr* addr, socklen_t addrlen, int flags)
{
    std::vector<WSABUF> bufs(static_cast<size_t>(iovcnt));
    for (int i = 0; i < iovcnt; ++i) {
        bufs[static_cast<size_t>(i)].buf = static_cast<char*>(iov[static_cast<size_t>(i)].iov_base);
        bufs[static_cast<size_t>(i)].len = static_cast<ULONG>(iov[static_cast<size_t>(i)].iov_len);
    }
    DWORD sent    = 0;
    DWORD dwFlags = static_cast<DWORD>(flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL));
    if (WSASendTo(fd, bufs.data(), static_cast<DWORD>(bufs.size()), &sent, dwFlags, addr, addrlen, nullptr, nullptr)
        == SOCKET_ERROR) {
        set_errno_from_wsa(WSAGetLastError());
        return -1;
    }
    return static_cast<ssize_t>(sent);
}

inline int last_error()
{
    int err = WSAGetLastError();
    set_errno_from_wsa(err);
    return errno;
}

inline int bind(fd_t fd, const sockaddr* addr, socklen_t len)
{
    if (::bind(fd, addr, len) == SOCKET_ERROR) {
        set_errno_from_wsa(WSAGetLastError());
        return -1;
    }
    return 0;
}

} // namespace co_wq::net::os

#else

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>


namespace co_wq::net::os {

using fd_t    = int;
using ssize_t = ::ssize_t;

inline fd_t invalid_fd() noexcept
{
    return -1;
}

inline fd_t create_socket(int domain, int type, int protocol)
{
    fd_t fd = ::socket(domain, type | SOCK_CLOEXEC, protocol);
    if (fd < 0)
        return invalid_fd();
    return fd;
}

inline int close_fd(fd_t fd)
{
    if (fd < 0)
        return 0;
    return ::close(fd);
}

inline int set_non_block(fd_t fd)
{
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

inline int connect(fd_t fd, const sockaddr* addr, socklen_t len)
{
    return ::connect(fd, addr, len);
}

inline int getsockopt(fd_t fd, int level, int optname, void* optval, socklen_t* optlen)
{
    return ::getsockopt(fd, level, optname, optval, optlen);
}

inline int setsockopt(fd_t fd, int level, int optname, const void* optval, socklen_t optlen)
{
    return ::setsockopt(fd, level, optname, optval, optlen);
}

inline int shutdown(fd_t fd, int how)
{
    return ::shutdown(fd, how);
}

inline int getsockname(fd_t fd, sockaddr* addr, socklen_t* len)
{
    return ::getsockname(fd, addr, len);
}

inline int listen(fd_t fd, int backlog)
{
    return ::listen(fd, backlog);
}

inline fd_t accept(fd_t fd, sockaddr* addr, socklen_t* len, int flags)
{
    return ::accept4(fd, addr, len, flags | SOCK_CLOEXEC | SOCK_NONBLOCK);
}

inline ssize_t recv(fd_t fd, void* buf, size_t len, int flags)
{
    return ::recv(fd, buf, len, flags);
}

inline ssize_t recvfrom(fd_t fd, void* buf, size_t len, int flags, sockaddr* addr, socklen_t* addrlen)
{
    return ::recvfrom(fd, buf, len, flags, addr, addrlen);
}

inline ssize_t send(fd_t fd, const void* buf, size_t len, int flags)
{
    return ::send(fd, buf, len, flags);
}

inline ssize_t sendto(fd_t fd, const void* buf, size_t len, int flags, const sockaddr* addr, socklen_t addrlen)
{
    return ::sendto(fd, buf, len, flags, addr, addrlen);
}

inline ssize_t writev(fd_t fd, const struct iovec* iov, int iovcnt)
{
    return ::writev(fd, iov, iovcnt);
}

inline ssize_t
sendto_vec(fd_t fd, const struct iovec* iov, int iovcnt, const sockaddr* addr, socklen_t addrlen, int flags)
{
    msghdr msg {};
    msg.msg_name       = const_cast<sockaddr*>(addr);
    msg.msg_namelen    = addrlen;
    msg.msg_iov        = const_cast<struct iovec*>(iov);
    msg.msg_iovlen     = static_cast<size_t>(iovcnt);
    msg.msg_control    = nullptr;
    msg.msg_controllen = 0;
    msg.msg_flags      = 0;
    return ::sendmsg(fd, &msg, flags);
}

inline int last_error()
{
    return errno;
}

inline int bind(fd_t fd, const sockaddr* addr, socklen_t len)
{
    return ::bind(fd, addr, len);
}

} // namespace co_wq::net::os

#endif
