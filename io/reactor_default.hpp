// reactor_default.hpp - choose default Reactor template by platform
#pragma once

#ifdef _WIN32
#define CO_WQ_DEFAULT_REACTOR iocp_reactor
#else
#define CO_WQ_DEFAULT_REACTOR epoll_reactor
#endif
