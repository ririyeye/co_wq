#pragma once

#include "http1_parser.hpp"
#include "http2_client_session.hpp"
#include "http_client.hpp"
#include "http_message.hpp"

#include "../dns_resolver.hpp"
#include "../tcp_socket.hpp"
#if defined(USING_SSL)
#include "../tls.hpp"
#endif

#include "../../io/epoll_reactor.hpp"
#include "../../io/fd_base.hpp"
#include "../../task/lock.hpp"
#include "../../task/task.hpp"
#include "../../task/worker.hpp"

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace co_wq::net::http {

using HttpClientWorkqueue = net::fd_workqueue<SpinLock, epoll_reactor>;

struct EasyRequest {
    std::string              url;
    std::string              method { "GET" };
    std::vector<HeaderEntry> headers;
    std::string              body;
    bool                     drop_content_headers { false };
    bool                     verbose { false };
    bool                     include_headers { false };
    bool                     buffer_body { false };
    bool                     prefer_http2 { false };
    bool                     insecure { false };
    std::string              user_agent { "co_curl/1.0" };
    std::FILE*               output { nullptr };
};

struct EasyResponse {
    int                                          status_code { 0 };
    std::string                                  reason;
    std::vector<HeaderEntry>                     headers;
    std::unordered_map<std::string, std::string> header_lookup;
    bool                                         used_http2 { false };
    bool                                         headers_complete { false };
    bool                                         message_complete { false };
    std::string                                  body;

    void                       reset();
    std::optional<std::string> header(std::string_view name) const;
};

class HttpEasyClient {
public:
    using TaskInt = Task<int, Work_Promise<SpinLock, int>>;

    explicit HttpEasyClient(HttpClientWorkqueue& fdwq);

    TaskInt perform(const EasyRequest& request, EasyResponse& response);

private:
    HttpClientWorkqueue& fdwq_;
};

} // namespace co_wq::net::http
