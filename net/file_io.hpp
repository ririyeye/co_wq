// file_io.hpp - async file read/write using epoll (O_NONBLOCK) + coroutine
#pragma once
#ifdef __linux__
#include "epoll_reactor.hpp" // default reactor
#include "io_waiter.hpp"
#include <errno.h>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

namespace co_wq::net {

template <lockable lock, template <class> class Reactor> class fd_workqueue; // fwd

/**
 * @brief 异步文件句柄 (非阻塞 + epoll)；支持 read/pread 与 write/pwrite 协程等待。
 */
template <lockable lock, template <class> class Reactor = epoll_reactor> class file_handle {
public:
    file_handle()                              = delete;
    file_handle(const file_handle&)            = delete;
    file_handle& operator=(const file_handle&) = delete;
    file_handle(file_handle&& o) noexcept : _exec(o._exec), _fd(o._fd) { o._fd = -1; }
    file_handle& operator=(file_handle&& o) noexcept
    {
        if (this != &o) {
            close();
            _exec = o._exec;
            _fd   = o._fd;
            o._fd = -1;
        }
        return *this;
    }
    ~file_handle() { close(); }
    int native_handle() const { return _fd; }
    /**
     * @brief 关闭文件并从 reactor 注销。
     */
    void close()
    {
        if (_fd >= 0) {
            Reactor<lock>::instance(_exec).remove_fd(_fd);
            ::close(_fd);
            _fd = -1;
        }
    }
    /**
     * @brief 读取 awaiter；支持顺序 read 和带偏移的 pread（自动维护 ofs）。
     */
    struct read_awaiter : io_waiter_base {
        file_handle& fh;
        void*        buf;
        size_t       len;
        ssize_t      nrd { 0 };
        off_t*       pofs;
        bool         use_offset;
        read_awaiter(file_handle& f, void* b, size_t l) : fh(f), buf(b), len(l), pofs(nullptr), use_offset(false) { }
        read_awaiter(file_handle& f, void* b, size_t l, off_t& ofs)
            : fh(f), buf(b), len(l), pofs(&ofs), use_offset(true)
        {
        }
        bool await_ready() noexcept
        {
            nrd = try_read();
            if (nrd >= 0)
                return true;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                nrd = -1;
                return false;
            }
            return true;
        }
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h = h;
            INIT_LIST_HEAD(&this->ws_node);
            Reactor<lock>::instance(fh._exec).add_waiter(fh._fd, EPOLLIN, this);
        }
        ssize_t await_resume() noexcept
        {
            if (nrd >= 0)
                return nrd;
            nrd = try_read();
            return nrd;
        }
        ssize_t try_read()
        {
            if (use_offset) {
                ssize_t r = ::pread(fh._fd, buf, len, *pofs);
                if (r > 0)
                    *pofs += r;
                return r;
            }
            return ::read(fh._fd, buf, len);
        }
    };
    read_awaiter read(void* buf, size_t len) { return read_awaiter(*this, buf, len); }
    read_awaiter pread(void* buf, size_t len, off_t& ofs) { return read_awaiter(*this, buf, len, ofs); }
    /**
     * @brief 写入 awaiter；支持顺序 write 和带偏移 pwrite，内部自旋写到 EAGAIN。
     */
    struct write_awaiter : io_waiter_base {
        file_handle& fh;
        const void*  buf;
        size_t       len;
        size_t       done { 0 };
        off_t*       pofs;
        bool         use_offset;
        write_awaiter(file_handle& f, const void* b, size_t l) : fh(f), buf(b), len(l), pofs(nullptr), use_offset(false)
        {
        }
        write_awaiter(file_handle& f, const void* b, size_t l, off_t& ofs)
            : fh(f), buf(b), len(l), pofs(&ofs), use_offset(true)
        {
        }
        bool await_ready() noexcept { return try_write(); }
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h = h;
            INIT_LIST_HEAD(&this->ws_node);
            Reactor<lock>::instance(fh._exec).add_waiter(fh._fd, EPOLLOUT, this);
        }
        ssize_t await_resume() noexcept
        {
            if (done == len)
                return (ssize_t)done;
            try_write();
            return (ssize_t)done;
        }
        bool try_write()
        {
            while (done < len) {
                ssize_t n;
                if (use_offset) {
                    n = ::pwrite(fh._fd, (char*)buf + done, len - done, *pofs);
                    if (n > 0)
                        *pofs += n;
                } else {
                    n = ::write(fh._fd, (char*)buf + done, len - done);
                }
                if (n > 0) {
                    done += (size_t)n;
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    return false;
                return true;
            }
            return true;
        }
    };
    write_awaiter write(const void* buf, size_t len) { return write_awaiter(*this, buf, len); }
    write_awaiter pwrite(const void* buf, size_t len, off_t& ofs) { return write_awaiter(*this, buf, len, ofs); }

private:
    friend class fd_workqueue<lock, Reactor>;
    file_handle(workqueue<lock>& e, int fd) : _exec(e), _fd(fd)
    {
        if (_fd < 0)
            throw std::runtime_error("invalid fd");
        int flags = ::fcntl(_fd, F_GETFL, 0);
        if (flags >= 0)
            ::fcntl(_fd, F_SETFL, flags | O_NONBLOCK);
        Reactor<lock>::instance(_exec).add_fd(_fd);
    }
    workqueue<lock>& _exec;
    int              _fd { -1 };
};

template <lockable lock, template <class> class Reactor = epoll_reactor>
inline file_handle<lock, Reactor> adopt_file(workqueue<lock>& exec, int fd)
{
    return file_handle<lock, Reactor>(exec, fd);
}

} // namespace co_wq::net
#endif
