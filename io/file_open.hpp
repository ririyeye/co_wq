// file_open.hpp - cross-platform helper to open files and return file_handle
#pragma once
#include "fd_base.hpp" // for fd_workqueue make_file variants
#include "file_io.hpp" // for file_handle
#include <optional>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace co_wq::net {

enum class open_mode {
    read_only,
    write_trunc, // create or truncate for write
};

template <lockable lock, template <class> class Reactor>
std::optional<file_handle<lock, Reactor>>
open_file_handle(fd_workqueue<lock, Reactor>& fdwq, const char* path, open_mode mode)
{
#ifdef _WIN32
    DWORD access = 0;
    DWORD share  = 0;
    DWORD disp   = 0;
    switch (mode) {
    case open_mode::read_only:
        access = GENERIC_READ;
        share  = FILE_SHARE_READ;
        disp   = OPEN_EXISTING;
        break;
    case open_mode::write_trunc:
        access = GENERIC_WRITE;
        share  = 0; // exclusive
        disp   = CREATE_ALWAYS;
        break;
    }
    HANDLE h = ::CreateFileA(path, access, share, nullptr, disp, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    return fdwq.make_file(h);
#else
    int flags = 0;
    switch (mode) {
    case open_mode::read_only:
        flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
        break;
    case open_mode::write_trunc:
        flags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NONBLOCK;
        break;
    }
    int fd = ::open(path, flags, 0644);
    if (fd < 0) {
        return std::nullopt;
    }
    return fdwq.make_file(fd);
#endif
}

} // namespace co_wq::net
