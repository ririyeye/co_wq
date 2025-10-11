#include "dns_resolver.hpp"

#include "workqueue.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <ws2tcpip.h>
#define CO_WQ_GAI_STRERROR ::gai_strerrorA
#else
#include <netdb.h>
#define CO_WQ_GAI_STRERROR ::gai_strerror
#endif

namespace co_wq::net::dns {

namespace {

    std::string strip_brackets(const std::string& input)
    {
        if (input.size() >= 2 && input.front() == '[' && input.back() == ']')
            return input.substr(1, input.size() - 2);
        return input;
    }

    void populate_ipv4_mapped(const sockaddr_in* v4, sockaddr_in6* v6)
    {
        std::memset(v6, 0, sizeof(sockaddr_in6));
        v6->sin6_family           = AF_INET6;
        v6->sin6_port             = v4->sin_port;
        v6->sin6_addr.s6_addr[10] = 0xFF;
        v6->sin6_addr.s6_addr[11] = 0xFF;
        std::memcpy(&v6->sin6_addr.s6_addr[12], &v4->sin_addr, sizeof(v4->sin_addr));
    }

    void resolve_core(const std::string& host, uint16_t port, const resolve_options& options, resolve_result& result)
    {
        result.success    = false;
        result.error_code = 0;
        result.error_message.clear();
        result.length = 0;
        result.endpoints.clear();
        result.selected_index = static_cast<size_t>(-1);

        std::string node = strip_brackets(host);
        if (node.empty()) {
            result.error_code    = EAI_NONAME;
            result.error_message = "empty host";
            return;
        }

        addrinfo hints {};
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_family   = options.family;

        if (options.family == AF_INET6 && options.allow_dual_stack)
            hints.ai_family = AF_UNSPEC;

        char service[16] {};
        std::snprintf(service, sizeof(service), "%u", static_cast<unsigned>(port));

        addrinfo* result_list = nullptr;
        int       rc          = ::getaddrinfo(node.c_str(), service, &hints, &result_list);
        if (rc != 0 || !result_list) {
            result.error_code    = rc;
            result.error_message = rc == 0 ? "no results" : CO_WQ_GAI_STRERROR(rc);
            if (result_list)
                ::freeaddrinfo(result_list);
            return;
        }

        std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard(result_list, ::freeaddrinfo);

        // 先收集所有候选地址（仅拷贝原始 sockaddr），便于调用方打印全部解析结果。
        for (auto* ai = result_list; ai; ai = ai->ai_next) {
            if (ai->ai_addr && ai->ai_addrlen > 0) {
                if (static_cast<size_t>(ai->ai_addrlen) <= sizeof(resolve_result::endpoint_entry {}.addr)) {
                    resolve_result::endpoint_entry ep {};
                    std::memcpy(&ep.addr, ai->ai_addr, ai->ai_addrlen);
                    ep.len = static_cast<socklen_t>(ai->ai_addrlen);
                    result.endpoints.push_back(ep);
                }
            }
        }

        auto copy_family = [&](const addrinfo* ai) {
            if (static_cast<size_t>(ai->ai_addrlen) > sizeof(result.storage))
                return false;
            std::memcpy(&result.storage, ai->ai_addr, ai->ai_addrlen);
            result.length  = static_cast<socklen_t>(ai->ai_addrlen);
            result.success = true;
            return true;
        };

        const addrinfo* selected = nullptr;
        size_t          index    = 0;
        for (auto* ai = result_list; ai; ai = ai->ai_next, ++index) {
            if (options.family == AF_UNSPEC) {
                selected              = ai;
                result.selected_index = index < result.endpoints.size() ? index : static_cast<size_t>(-1);
                break;
            }
            if (ai->ai_family == options.family) {
                selected              = ai;
                result.selected_index = index < result.endpoints.size() ? index : static_cast<size_t>(-1);
                break;
            }
            if (options.family == AF_INET6 && options.allow_dual_stack && ai->ai_family == AF_INET) {
                selected              = ai;
                result.selected_index = index < result.endpoints.size() ? index : static_cast<size_t>(-1);
                break;
            }
        }

        if (!selected) {
            result.error_code    = EAI_NONAME;
            result.error_message = "no matching family";
            return;
        }

        if (selected->ai_family == options.family || options.family == AF_UNSPEC) {
            if (!copy_family(selected)) {
                result.error_code    = EAI_FAIL;
                result.error_message = "address too large";
            }
            return;
        }

        if (options.family == AF_INET6 && options.allow_dual_stack && selected->ai_family == AF_INET) {
            auto* v4 = reinterpret_cast<sockaddr_in*>(selected->ai_addr);
            auto* v6 = reinterpret_cast<sockaddr_in6*>(&result.storage);
            populate_ipv4_mapped(v4, v6);
            result.length  = static_cast<socklen_t>(sizeof(sockaddr_in6));
            result.success = true;
            return;
        }

        result.error_code    = EAI_NONAME;
        result.error_message = "unsupported address family";
    }

} // namespace

resolve_result resolve_sync(const std::string& host, uint16_t port, const resolve_options& options)
{
    resolve_result result;
    result.submit_time = std::chrono::steady_clock::now();
    result.start_time  = result.submit_time;
    resolve_core(host, port, options, result);
    result.finish_time = std::chrono::steady_clock::now();
    return result;
}

struct async_resolver::Impl {
    explicit Impl(size_t worker_count)
    {
        auto hw = std::thread::hardware_concurrency();
        if (hw == 0)
            hw = 2;
        if (worker_count == 0)
            worker_count = std::clamp<size_t>(hw, size_t { 2 }, size_t { 8 });
        workers.reserve(worker_count);
        for (size_t i = 0; i < worker_count; ++i) {
            workers.emplace_back([this]() { worker_loop(); });
        }
    }

    ~Impl()
    {
        {
            std::lock_guard<std::mutex> guard(mutex);
            stop = true;
        }
        cv.notify_all();
        for (auto& worker : workers) {
            if (worker.joinable())
                worker.join();
        }
    }

    struct Job {
        std::string             host;
        uint16_t                port { 0 };
        resolve_options         options;
        workqueue<SpinLock>*    exec { nullptr };
        std::coroutine_handle<> continuation;
        resolve_result          result;
    };

    struct ResumeNode : worknode {
        std::shared_ptr<Job> job;
        static void          run(worknode* node)
        {
            auto                 self   = static_cast<ResumeNode*>(node);
            std::shared_ptr<Job> job    = std::move(self->job);
            auto                 handle = job->continuation;
            job->continuation           = std::coroutine_handle<> {};
            delete self;
            if (handle)
                handle.resume();
        }
    };

    void enqueue(std::shared_ptr<Job> job)
    {
        {
            std::lock_guard<std::mutex> guard(mutex);
            queue.push_back(std::move(job));
        }
        cv.notify_one();
    }

    void post_result(const std::shared_ptr<Job>& job)
    {
        if (job->exec == nullptr) {
            if (job->continuation)
                job->continuation.resume();
            return;
        }
        auto* node = new ResumeNode();
        node->job  = job;
        INIT_LIST_HEAD(&node->ws_node);
        node->func = &ResumeNode::run;
        job->exec->post(*node);
    }

    void worker_loop()
    {
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [this]() { return stop || !queue.empty(); });
                if (stop && queue.empty())
                    return;
                job = std::move(queue.front());
                queue.pop_front();
            }
            job->result.start_time = std::chrono::steady_clock::now();
            resolve_core(job->host, job->port, job->options, job->result);
            job->result.finish_time = std::chrono::steady_clock::now();
            post_result(job);
        }
    }

    std::vector<std::thread>         workers;
    std::mutex                       mutex;
    std::condition_variable          cv;
    std::deque<std::shared_ptr<Job>> queue;
    bool                             stop { false };
};

async_resolver::async_resolver(size_t worker_count) : _impl(std::make_unique<Impl>(worker_count)) { }

async_resolver::~async_resolver() = default;

Task<resolve_result, Work_Promise<SpinLock, resolve_result>> async_resolver::resolve(workqueue<SpinLock>&   exec,
                                                                                     const std::string&     host,
                                                                                     uint16_t               port,
                                                                                     const resolve_options& options)
{
    struct Awaitable {
        Impl&                      impl;
        workqueue<SpinLock>&       exec;
        std::string                host;
        uint16_t                   port;
        resolve_options            options;
        std::shared_ptr<Impl::Job> job;

        bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<> h)
        {
            job                     = std::make_shared<Impl::Job>();
            job->host               = std::move(host);
            job->port               = port;
            job->options            = options;
            job->exec               = &exec;
            job->continuation       = h;
            job->result.submit_time = std::chrono::steady_clock::now();
            impl.enqueue(job);
        }

        resolve_result await_resume()
        {
            resolve_result result = std::move(job->result);
            job.reset();
            return result;
        }
    };

    auto result = co_await Awaitable { *_impl, exec, host, port, options, nullptr };
    co_return result;
}

} // namespace co_wq::net::dns
