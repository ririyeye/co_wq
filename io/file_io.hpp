// file_io.hpp - async file read/write (non-blocking + epoll) with internal serialization
#pragma once
#ifdef __linux__
#include "epoll_reactor.hpp"
#include "io_serial.hpp"
#include "io_waiter.hpp"
#include <errno.h>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

namespace co_wq::net {

template <lockable lock, template <class> class Reactor> class fd_workqueue; // fwd decl

template <lockable lock, template <class> class Reactor = epoll_reactor> class file_handle {
public:
    // Helper alias to simplify two-phase awaiter declarations
    template <class D> using tp_base           = two_phase_drain_awaiter<D, file_handle>;
    file_handle()                              = delete;
    file_handle(const file_handle&)            = delete;
    file_handle& operator=(const file_handle&) = delete;
    file_handle(file_handle&& o) noexcept : _exec(o._exec), _reactor(o._reactor), _fd(o._fd), _closed(o._closed)
    {
        o._fd     = -1;
        o._closed = true;
    }
    file_handle& operator=(file_handle&& o) noexcept
    {
        if (this != &o) {
            close();
            _exec     = o._exec;
            _reactor  = o._reactor;
            _fd       = o._fd;
            _closed   = o._closed;
            o._fd     = -1;
            o._closed = true;
        }
        return *this;
    }
    ~file_handle() { close(); }
    int  native_handle() const { return _fd; }
    bool closed() const { return _closed || _fd < 0; }
    // Accessors for serial_slot_awaiter
    workqueue<lock>& exec() { return _exec; }
    lock&            serial_lock() { return _io_serial_lock; }
    Reactor<lock>*   reactor() { return _reactor; }
    void             close()
    {
        if (_fd < 0)
            return;
        _closed = true;
        if (_reactor)
            _reactor->remove_fd(_fd);
        // 批量收集并一次性投递串行等待节点
        list_head pending;
        INIT_LIST_HEAD(&pending);
        serial_collect_waiters(_io_serial_lock, { &_read_q, &_write_q }, pending);
        serial_post_pending(_exec, pending);
        ::close(_fd);
        _fd = -1;
    }

    struct read_awaiter : tp_base<read_awaiter> {
        void*   buf;
        size_t  len;
        ssize_t nrd { -1 };
        off_t*  pofs { nullptr };
        bool    use_offset { false };
        read_awaiter(file_handle& f, void* b, size_t l) : tp_base<read_awaiter>(f, f._read_q), buf(b), len(l) { }
        read_awaiter(file_handle& f, void* b, size_t l, off_t& ofs)
            : tp_base<read_awaiter>(f, f._read_q), buf(b), len(l), pofs(&ofs), use_offset(true)
        {
        }
        int attempt_once()
        {
            nrd = try_read();
            if (nrd >= 0)
                return 0; // done (success or 0=EOF)
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return -1; // would block
            return 0;      // error -> done
        }
        static void register_wait(read_awaiter* self, bool /*first*/)
        {
            self->owner.reactor()->add_waiter_custom(self->owner.native_handle(), EPOLLIN, self);
        }
        bool    await_ready() noexcept { return false; }
        ssize_t await_resume() noexcept
        {
            if (nrd < 0 && this->owner.closed()) {
                errno = ECANCELED;
                return -1;
            }
            return nrd;
        }
        ssize_t try_read()
        {
            if (this->owner.native_handle() < 0)
                return -1;
            if (use_offset) {
                ssize_t r = ::pread(this->owner.native_handle(), buf, len, *pofs);
                if (r > 0)
                    *pofs += r;
                return r;
            }
            return ::read(this->owner.native_handle(), buf, len);
        }
    };
    read_awaiter read(void* buf, size_t len) { return read_awaiter(*this, buf, len); }
    read_awaiter pread(void* buf, size_t len, off_t& ofs) { return read_awaiter(*this, buf, len, ofs); }

    struct write_awaiter : tp_base<write_awaiter> {
        const void* buf;
        size_t      len;
        size_t      done { 0 };
        off_t*      pofs { nullptr };
        bool        use_offset { false };
        write_awaiter(file_handle& f, const void* b, size_t l) : tp_base<write_awaiter>(f, f._write_q), buf(b), len(l)
        {
        }
        write_awaiter(file_handle& f, const void* b, size_t l, off_t& ofs)
            : tp_base<write_awaiter>(f, f._write_q), buf(b), len(l), pofs(&ofs), use_offset(true)
        {
        }
        int attempt_once()
        {
            if (try_write())
                return 0; // done or error
            return -1;    // would block
        }
        static void register_wait(write_awaiter* self, bool /*first*/)
        {
            self->owner.reactor()->add_waiter_custom(self->owner.native_handle(), EPOLLOUT, self);
        }
        bool    await_ready() noexcept { return false; }
        ssize_t await_resume() noexcept
        {
            if (done == 0 && this->owner.closed()) {
                errno = ECANCELED;
                return -1;
            }
            return (ssize_t)done;
        }
        bool try_write()
        {
            while (done < len) {
                ssize_t n;
                if (use_offset) {
                    n = ::pwrite(this->owner.native_handle(), (char*)buf + done, len - done, *pofs);
                    if (n > 0)
                        *pofs += n;
                } else {
                    n = ::write(this->owner.native_handle(), (char*)buf + done, len - done);
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
    file_handle(workqueue<lock>& e, Reactor<lock>& r, int fd) : _exec(e), _reactor(&r), _fd(fd)
    {
        if (_fd < 0)
            throw std::runtime_error("invalid fd");
        int flags = ::fcntl(_fd, F_GETFL, 0);
        if (flags >= 0)
            ::fcntl(_fd, F_SETFL, flags | O_NONBLOCK);
        _reactor->add_fd(_fd);
        serial_queue_init(_read_q);
        serial_queue_init(_write_q);
    }
    workqueue<lock>& _exec;
    Reactor<lock>*   _reactor { nullptr };
    int              _fd { -1 };
    lock             _io_serial_lock;
    serial_queue     _read_q;
    serial_queue     _write_q;
    bool             _closed { false };
    // release handled via serial_slot_awaiter
};

template <lockable lock, template <class> class Reactor = epoll_reactor>
inline file_handle<lock, Reactor> make_file_handle(workqueue<lock>& exec, Reactor<lock>& r, int fd)
{
    return file_handle<lock, Reactor>(exec, r, fd);
}

} // namespace co_wq::net
#elif defined(_WIN32)
#include "io_waiter.hpp"
#include "iocp_reactor.hpp"
#include <basetsd.h>
#include <stdexcept>
#include <windows.h>

#ifndef _SSIZE_T_DEFINED
using ssize_t = SSIZE_T;
#define _SSIZE_T_DEFINED
#endif

namespace co_wq::net {

template <lockable lock, template <class> class Reactor> class fd_workqueue; // fwd

template <lockable lock, template <class> class Reactor = epoll_reactor> class file_handle {
public:
    file_handle()                              = delete;
    file_handle(const file_handle&)            = delete;
    file_handle& operator=(const file_handle&) = delete;
    file_handle(file_handle&& o) noexcept : _exec(o._exec), _reactor(o._reactor), _h(o._h)
    {
        o._h = INVALID_HANDLE_VALUE;
    }
    file_handle& operator=(file_handle&& o) noexcept
    {
        if (this != &o) {
            close();
            _exec    = o._exec;
            _reactor = o._reactor;
            _h       = o._h;
            o._h     = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    ~file_handle() { close(); }
    void close()
    {
        if (_h != INVALID_HANDLE_VALUE) {
            _reactor->remove_fd((int)(intptr_t)_h);
            ::CloseHandle(_h);
            _h = INVALID_HANDLE_VALUE;
        }
    }
    bool             closed() const { return _h == INVALID_HANDLE_VALUE; }
    int              native_handle() const { return (int)(intptr_t)_h; }
    workqueue<lock>& exec() { return _exec; }
    Reactor<lock>*   reactor() { return _reactor; }
    struct read_awaiter : io_waiter_base {
        file_handle& fh;
        void*        buf;
        size_t       len;
        ssize_t      nrd { -1 };
        iocp_ovl     ovl;
        read_awaiter(file_handle& f, void* b, size_t l) : fh(f), buf(b), len(l)
        {
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.waiter = this;
        }
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> coro)
        {
            this->h    = coro;
            this->func = &io_waiter_base::resume_cb;
            INIT_LIST_HEAD(&this->ws_node);
            DWORD tr = 0;
            BOOL  ok = ReadFile(fh._h, buf, (DWORD)len, &tr, &ovl);
            if (ok) {
                nrd = (ssize_t)tr;
                fh._reactor->post_completion(this);
                return;
            }
            if (GetLastError() != ERROR_IO_PENDING) {
                nrd = -1;
                fh._reactor->post_completion(this);
            }
        }
        ssize_t await_resume() noexcept
        {
            if (nrd == -1) {
                DWORD tr = 0;
                if (GetOverlappedResult(fh._h, &ovl, &tr, FALSE))
                    nrd = (ssize_t)tr;
            }
            return nrd;
        }
    };
    read_awaiter read(void* buf, size_t len) { return read_awaiter(*this, buf, len); }
    struct write_awaiter : io_waiter_base {
        file_handle& fh;
        const void*  buf;
        size_t       len;
        ssize_t      nwr { -1 };
        iocp_ovl     ovl;
        write_awaiter(file_handle& f, const void* b, size_t l) : fh(f), buf(b), len(l)
        {
            ZeroMemory(&ovl, sizeof(ovl));
            ovl.waiter = this;
        }
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> coro)
        {
            this->h    = coro;
            this->func = &io_waiter_base::resume_cb;
            INIT_LIST_HEAD(&this->ws_node);
            DWORD tr = 0;
            BOOL  ok = WriteFile(fh._h, buf, (DWORD)len, &tr, &ovl);
            if (ok) {
                nwr = (ssize_t)tr;
                fh._reactor->post_completion(this);
                return;
            }
            if (GetLastError() != ERROR_IO_PENDING) {
                nwr = -1;
                fh._reactor->post_completion(this);
            }
        }
        ssize_t await_resume() noexcept
        {
            if (nwr == -1) {
                DWORD tr = 0;
                if (GetOverlappedResult(fh._h, &ovl, &tr, FALSE))
                    nwr = (ssize_t)tr;
            }
            return nwr;
        }
    };
    write_awaiter write(const void* buf, size_t len) { return write_awaiter(*this, buf, len); }

private:
    friend class fd_workqueue<lock, Reactor>;
    file_handle(workqueue<lock>& e, Reactor<lock>& r, HANDLE handle) : _exec(e), _reactor(&r), _h(handle)
    {
        if (handle == INVALID_HANDLE_VALUE)
            throw std::runtime_error("invalid file handle");
        _reactor->add_fd((int)(intptr_t)_h);
    }
    workqueue<lock>& _exec;
    Reactor<lock>*   _reactor { nullptr };
    HANDLE           _h { INVALID_HANDLE_VALUE };
};

} // namespace co_wq::net
#endif
