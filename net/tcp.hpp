// Umbrella header: include modular components
#pragma once
#ifdef __linux__
#include "epoll_reactor.hpp"
#include "fd_base.hpp"
#include "fd_wait.hpp"
#include "file_io.hpp"
#include "io_waiter.hpp"
#include "tcp_listener.hpp"
#include "tcp_socket.hpp"

namespace co_wq::net {
template <lockable lock> inline void init_tcp(workqueue<lock>& exec)
{
    init_reactor<lock>(exec);
}
} // namespace co_wq::net
#endif
