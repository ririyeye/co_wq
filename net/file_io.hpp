// file_io.hpp - async file read/write (non-blocking + epoll) with internal serialization
#pragma once
#ifdef __linux__
#include "epoll_reactor.hpp"
#include "io_waiter.hpp"
#include <errno.h>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>
#include <vector>

namespace co_wq::net {

template <lockable lock, template <class> class Reactor> class fd_workqueue; // fwd decl

template <lockable lock, template <class> class Reactor = epoll_reactor> class file_handle {
public:
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
    void close()
    {
        if (_fd < 0)
            return;
        _closed = true;
        if (_reactor)
            _reactor->remove_fd(_fd);
        std::vector<worknode*> pending;
        {
            std::scoped_lock lk(_io_serial_lock);
            while (!list_empty(&_read_waiters)) {
                auto* lh = _read_waiters.next;
                auto* wn = list_entry(lh, worknode, ws_node);
                list_del(lh);
                pending.push_back(wn);
            }
            _read_locked = false;
            while (!list_empty(&_write_waiters)) {
                auto* lh = _write_waiters.next;
                auto* wn = list_entry(lh, worknode, ws_node);
                list_del(lh);
                pending.push_back(wn);
            }
            _write_locked = false;
        }
        for (auto* wn : pending) {
            wn->func = &io_waiter_base::resume_cb;
            _exec.post(*wn);
        }
        ::close(_fd);
        _fd = -1;
    }

    struct read_awaiter : io_waiter_base {
        file_handle& fh;
        void*        buf;
        size_t       len;
        ssize_t      nrd { -1 };
        off_t*       pofs { nullptr };
        bool         use_offset { false };
        bool         have_lock { false };
        read_awaiter(file_handle& f, void* b, size_t l) : fh(f), buf(b), len(l) { }
        read_awaiter(file_handle& f, void* b, size_t l, off_t& ofs)
            : fh(f), buf(b), len(l), pofs(&ofs), use_offset(true)
        {
        }
        static void lock_acquired_cb(worknode* w)
        {
            auto* self      = static_cast<read_awaiter*>(w);
            self->have_lock = true;
            self->nrd       = self->try_read();
            if (self->nrd >= 0 || (self->nrd < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                self->fh.release_read_slot();
                self->func = &io_waiter_base::resume_cb;
                if (self->h)
                    self->h.resume();
                return;
            }
            self->nrd  = -1;
            self->func = &read_awaiter::drive_cb;
            self->fh._reactor->add_waiter_custom(self->fh._fd, EPOLLIN, self);
        }
        static void drive_cb(worknode* w)
        {
            auto* self = static_cast<read_awaiter*>(w);
            self->nrd  = self->try_read();
            self->fh.release_read_slot();
            self->func = &io_waiter_base::resume_cb;
            if (self->h)
                self->h.resume();
        }
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h    = h;
            this->func = &read_awaiter::lock_acquired_cb;
            INIT_LIST_HEAD(&this->ws_node);
            std::scoped_lock lk(fh._io_serial_lock);
            if (!fh._read_locked && list_empty(&fh._read_waiters)) {
                fh._read_locked = true;
                fh._exec.post(*this);
                return;
            }
            list_add_tail(&this->ws_node, &fh._read_waiters);
        }
        ssize_t await_resume() noexcept
        {
            if (nrd < 0 && fh._closed) {
                errno = ECANCELED;
                return -1;
            }
            return nrd;
        }
        ssize_t try_read()
        {
            if (fh._fd < 0)
                return -1;
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

    struct write_awaiter : io_waiter_base {
        file_handle& fh;
        const void*  buf;
        size_t       len;
        size_t       done { 0 };
        off_t*       pofs { nullptr };
        bool         use_offset { false };
        bool         have_lock { false };
        write_awaiter(file_handle& f, const void* b, size_t l) : fh(f), buf(b), len(l) { }
        write_awaiter(file_handle& f, const void* b, size_t l, off_t& ofs)
            : fh(f), buf(b), len(l), pofs(&ofs), use_offset(true)
        {
        }
        static void lock_acquired_cb(worknode* w)
        {
            auto* self      = static_cast<write_awaiter*>(w);
            self->have_lock = true;
            if (!self->try_write()) {
                self->func = &write_awaiter::drive_cb;
                self->fh._reactor->add_waiter_custom(self->fh._fd, EPOLLOUT, self);
                return;
            }
            self->fh.release_write_slot();
            self->func = &io_waiter_base::resume_cb;
            if (self->h)
                self->h.resume();
        }
        static void drive_cb(worknode* w)
        {
            auto* self = static_cast<write_awaiter*>(w);
            if (!self->try_write()) {
                self->func = &write_awaiter::drive_cb;
                self->fh._reactor->add_waiter_custom(self->fh._fd, EPOLLOUT, self);
                return;
            }
            self->fh.release_write_slot();
            self->func = &io_waiter_base::resume_cb;
            if (self->h)
                self->h.resume();
        }
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h)
        {
            this->h    = h;
            this->func = &write_awaiter::lock_acquired_cb;
            INIT_LIST_HEAD(&this->ws_node);
            std::scoped_lock lk(fh._io_serial_lock);
            if (!fh._write_locked && list_empty(&fh._write_waiters)) {
                fh._write_locked = true;
                fh._exec.post(*this);
                return;
            }
            list_add_tail(&this->ws_node, &fh._write_waiters);
        }
        ssize_t await_resume() noexcept
        {
            if (done == 0 && fh._closed) {
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
    file_handle(workqueue<lock>& e, Reactor<lock>& r, int fd) : _exec(e), _reactor(&r), _fd(fd)
    {
        if (_fd < 0)
            throw std::runtime_error("invalid fd");
        int flags = ::fcntl(_fd, F_GETFL, 0);
        if (flags >= 0)
            ::fcntl(_fd, F_SETFL, flags | O_NONBLOCK);
        _reactor->add_fd(_fd);
        INIT_LIST_HEAD(&_read_waiters);
        INIT_LIST_HEAD(&_write_waiters);
    }
    workqueue<lock>& _exec;
    Reactor<lock>*   _reactor { nullptr };
    int              _fd { -1 };
    lock             _io_serial_lock;
    list_head        _read_waiters {};
    bool             _read_locked { false };
    list_head        _write_waiters {};
    bool             _write_locked { false };
    bool             _closed { false };
    void             release_read_slot()
    {
        worknode* next = nullptr;
        {
            std::scoped_lock lk(_io_serial_lock);
            if (!list_empty(&_read_waiters)) {
                auto* lh = _read_waiters.next;
                auto* aw = list_entry(lh, worknode, ws_node);
                list_del(lh);
                next = aw;
            } else {
                _read_locked = false;
            }
        }
        if (next)
            _exec.post(*next);
    }
    void release_write_slot()
    {
        worknode* next = nullptr;
        {
            std::scoped_lock lk(_io_serial_lock);
            if (!list_empty(&_write_waiters)) {
                auto* lh = _write_waiters.next;
                auto* aw = list_entry(lh, worknode, ws_node);
                list_del(lh);
                next = aw;
            } else {
                _write_locked = false;
            }
        }
        if (next)
            _exec.post(*next);
    }
};

template <lockable lock, template <class> class Reactor = epoll_reactor>
inline file_handle<lock, Reactor> make_file_handle(workqueue<lock>& exec, Reactor<lock>& r, int fd)
{
    return file_handle<lock, Reactor>(exec, r, fd);
}

} // namespace co_wq::net
#endif
