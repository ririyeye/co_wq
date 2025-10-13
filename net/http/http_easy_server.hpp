#pragma once

#include "http_easy_client.hpp"
#include "http_router.hpp"
#include "http_server.hpp"

#include "../os_compat.hpp"
#include "../tcp_listener.hpp"
#include "../tcp_socket.hpp"

#if defined(USING_SSL)
#include "../tls.hpp"
#endif

#include "../../task/task.hpp"
#include "../../task/workqueue.hpp"

#include <atomic>
#include <condition_variable>
#include <optional>
#include <string>
#include <string_view>

namespace co_wq::net::http {

using HttpServerWorkqueue = net::fd_workqueue<SpinLock, epoll_reactor>;

class HttpEasyServer {
public:
    struct TlsConfig {
        std::string cert_file;
        std::string key_file;
    };

    struct Config {
        std::string              host { "0.0.0.0" };
        uint16_t                 port { 8080 };
        bool                     enable_http2 { false };
        bool                     verbose_logging { false };
        std::size_t              backlog { 128 };
        std::optional<TlsConfig> tls;
    };

    using TaskType = Task<void, Work_Promise<SpinLock, void>>;

    explicit HttpEasyServer(HttpServerWorkqueue& wq);

    HttpRouter&       router() noexcept { return router_; }
    const HttpRouter& router() const noexcept { return router_; }

    bool start(const Config& config);
    void request_stop();
    void wait();

    bool running() const noexcept { return running_.load(std::memory_order_acquire); }

private:
    static void on_server_completed(Promise_base& promise);
#if defined(USING_SSL)
    static int select_http_alpn(SSL*                  ssl,
                                const unsigned char** out,
                                unsigned char*        outlen,
                                const unsigned char*  in,
                                unsigned int          inlen,
                                void*                 arg);
#endif
    TaskType run_server(Config config_copy);

    template <typename Socket> TaskType handle_http_connection(Socket sock, bool is_tls, std::string peer_desc);

    template <typename Socket>
    TaskType handle_http1_connection(Socket sock, std::string initial_data, bool is_tls, const std::string& peer_desc);

    template <typename Socket> TaskType handle_http2_connection(Socket sock, std::string initial_data);

    std::string describe_peer(int fd) const;

    HttpServerWorkqueue& server_wq_;
    HttpRouter           router_;
    Config               config_ {};

#if defined(USING_SSL)
    std::optional<net::tls_context> tls_context_;
#endif

    TaskType                                      server_task_ { nullptr };
    std::coroutine_handle<TaskType::promise_type> coroutine_ { nullptr };
    std::atomic_bool                              running_ { false };
    std::atomic_bool                              stop_requested_ { false };
    std::atomic<net::os::fd_t>                    listener_fd_ { net::os::invalid_fd() };

    std::mutex              mutex_;
    std::condition_variable cv_;
    bool                    finished_ { true };
};

} // namespace co_wq::net::http
