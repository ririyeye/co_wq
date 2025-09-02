// fd_base.hpp - generic fd abstraction and factory (fd_workqueue)
#pragma once
#ifdef __linux__
#include "epoll_reactor.hpp" // default reactor
#include "tcp_socket.hpp"    // for factory methods (now templated)
#include "udp_socket.hpp"
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

namespace co_wq::net {

template <lockable lock, template <class> class Reactor> class fd_workqueue; // fwd

/**
 * @brief 通用 fd 封装基类，负责: 非阻塞设置 + 注册到 reactor + RAII 关闭。
 */
template <lockable lock, template <class> class Reactor = epoll_reactor> class fd_object {
public:
    fd_object(const fd_object&)            = delete;
    fd_object& operator=(const fd_object&) = delete;
    fd_object(fd_object&& other) noexcept : _exec(other._exec), _fd(other._fd) { other._fd = -1; }
    fd_object& operator=(fd_object&& other) noexcept
    {
        if (this != &other) {
            close();
            _exec     = other._exec;
            _fd       = other._fd;
            other._fd = -1;
        }
        return *this;
    }
    ~fd_object() { close(); }
    int native_handle() const { return _fd; }
    /**
     * @brief 关闭 fd 并从 reactor 注销（可安全多次调用）。
     */
    void close()
    {
        if (_fd >= 0) {
            if (_reactor) {
                _reactor->remove_fd(_fd);
            }
            ::close(_fd);
            _fd = -1;
        }
    }

protected:
    explicit fd_object(workqueue<lock>& exec, Reactor<lock>& reactor, int fd) : _exec(exec), _reactor(&reactor), _fd(fd)
    {
        if (_fd < 0)
            throw std::runtime_error("invalid fd");
        set_non_block();
        _reactor->add_fd(_fd);
    }
    void set_non_block()
    {
        int flags = ::fcntl(_fd, F_GETFL, 0);
        if (flags >= 0)
            ::fcntl(_fd, F_SETFL, flags | O_NONBLOCK);
    }
    workqueue<lock>& _exec;
    Reactor<lock>*   _reactor { nullptr };
    int              _fd { -1 };
    friend class fd_workqueue<lock, Reactor>;
};

/**
 * @brief 封装一个基础 workqueue + 内嵌 Reactor，用于创建/接管各种 fd 对象 (TCP / UDP / 文件)。
 * @note Reactor 作为成员，生命周期与 fd_workqueue 绑定；避免单独堆分配。
 */
template <lockable lock, template <class> class Reactor = epoll_reactor> class fd_workqueue {
public:
    explicit fd_workqueue(workqueue<lock>& base) : _base(base), _reactor(base) { }
    workqueue<lock>& base() { return _base; }
    /**
     * @brief 打开文件 (O_NONBLOCK | O_CLOEXEC) 并纳入 reactor 管理。
     * @throws runtime_error 打开失败。
     * @return 文件描述符。
     */
    int open_file(const char* path, int flags, mode_t mode = 0644)
    {
        int fd = ::open(path, flags | O_CLOEXEC | O_NONBLOCK, mode);
        if (fd < 0)
            throw std::runtime_error("open failed");
        _reactor->add_fd(fd);
        return fd;
    }
    Reactor<lock>&            reactor() { return _reactor; } ///< 访问内部 reactor
    tcp_socket<lock, Reactor> make_tcp_socket() { return tcp_socket<lock, Reactor>(_base, _reactor); } ///< 创建新 TCP
    tcp_socket<lock, Reactor> adopt_tcp_socket(int fd)
    {
        return tcp_socket<lock, Reactor>(fd, _base, _reactor);
    }                                                                                                  ///< 接管现有 fd
    udp_socket<lock, Reactor> make_udp_socket() { return udp_socket<lock, Reactor>(_base, _reactor); } ///< 创建新 UDP

private:
    workqueue<lock>& _base;
    Reactor<lock>    _reactor; // 内嵌 reactor (无堆分配)
};

} // namespace co_wq::net
#endif
