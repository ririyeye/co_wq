// fd_base.hpp - generic fd abstraction and factory (fd_workqueue)
#pragma once

#include "iocp_reactor.hpp" // provides epoll_reactor alias
#include "tcp_socket.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdexcept>
namespace co_wq::net {

template <lockable lock, template <class> class Reactor> class fd_workqueue; // fwd

template <lockable lock, template <class> class Reactor = epoll_reactor> class fd_object {
public:
    fd_object(const fd_object&) = delete; fd_object& operator=(const fd_object&) = delete;
    fd_object(fd_object&& o) noexcept : _exec(o._exec), _reactor(o._reactor), _fd(o._fd) { o._fd = (int)INVALID_SOCKET; }
    fd_object& operator=(fd_object&& o) noexcept { if (this!=&o){ close(); _exec=o._exec; _reactor=o._reactor; _fd=o._fd; o._fd=(int)INVALID_SOCKET;} return *this; }
    ~fd_object(){ close(); }
    int native_handle() const { return _fd; }
    void close(){ if (_fd!=(int)INVALID_SOCKET){ if (_reactor) _reactor->remove_fd(_fd); ::closesocket((SOCKET)_fd); _fd=(int)INVALID_SOCKET; }}
protected:
    explicit fd_object(workqueue<lock>& exec, Reactor<lock>& reactor, int fd) : _exec(exec), _reactor(&reactor), _fd(fd)
    {
        if (_fd==(int)INVALID_SOCKET) throw std::runtime_error("invalid socket");
        set_non_block();
        _reactor->add_fd(_fd);
    }
    void set_non_block(){ u_long m=1; ioctlsocket((SOCKET)_fd, FIONBIO, &m); }
    workqueue<lock>& _exec; Reactor<lock>* _reactor {nullptr}; int _fd {(int)INVALID_SOCKET};
    friend class fd_workqueue<lock, Reactor>;
};

template <lockable lock, template <class> class Reactor = epoll_reactor> class fd_workqueue {
public:
    explicit fd_workqueue(workqueue<lock>& base) : _base(base), _reactor(base){}
    workqueue<lock>& base(){ return _base; }
    Reactor<lock>& reactor(){ return _reactor; }
    tcp_socket<lock, Reactor> make_tcp_socket(){ return tcp_socket<lock, Reactor>(_base, _reactor); }
    tcp_socket<lock, Reactor> adopt_tcp_socket(int fd){ return tcp_socket<lock, Reactor>(fd, _base, _reactor); }
private:
    workqueue<lock>& _base; Reactor<lock> _reactor;
};

} // namespace co_wq::net

