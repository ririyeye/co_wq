#pragma once

/**
 * @file udp_socket.hpp
 * @brief Windows UDP socket wrapper re-exporting cross-platform implementation.
 */
#pragma once

#ifdef _WIN32

#include "../linux/udp_socket.hpp" // IWYU pragma: export

#endif
