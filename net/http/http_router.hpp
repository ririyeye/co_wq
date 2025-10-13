#pragma once

#include "http_message.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace co_wq::net::http {

class HttpRouter {
public:
    using RouteHandler   = std::function<HttpResponse(const HttpRequest&)>;
    using Middleware     = std::function<void(const HttpRequest&, HttpResponse&)>;
    using PostMiddleware = Middleware;
    using PreMiddleware  = std::function<bool(const HttpRequest&, HttpResponse&)>;

    struct ReverseProxyOptions {
        bool strip_prefix { true };
        bool preserve_host { false };
    };

    struct ReverseProxyConfig {
        std::string scheme;
        std::string host;
        uint16_t    port { 0 };
        std::string upstream_path;
        bool        strip_prefix { true };
        bool        preserve_host { false };
    };

    struct MatchResult {
        bool                      matched { false };
        RouteHandler              handler;
        const ReverseProxyConfig* proxy { nullptr };
        std::string_view          route_path {};
        bool                      allow_prefix { false };
    };

    struct ForwardProxyRule {
        std::string scheme;
        std::string host;
        uint16_t    port { 0 };
        bool        allow_subdomains { false };
    };

    struct ForwardProxyDecision {
        bool                    matched { false };
        bool                    allowed { false };
        const ForwardProxyRule* rule { nullptr };
        std::string             request_target;
    };

    HttpRouter();

    void add_route(std::string method, std::string path, RouteHandler handler);
    void add_route_with_suffix(std::string method, std::string path, RouteHandler handler, bool allow_prefix);
    void get(std::string path, RouteHandler handler);
    void get_prefix(std::string path, RouteHandler handler);
    void post(std::string path, RouteHandler handler);
    void put(std::string path, RouteHandler handler);
    void del(std::string path, RouteHandler handler);
    void add_reverse_proxy(std::string prefix, std::string upstream_url, ReverseProxyOptions options);
    void add_reverse_proxy(std::string prefix, std::string upstream_url);

    void add_middleware(Middleware middleware);
    void add_post_middleware(PostMiddleware middleware);
    void add_pre_middleware(PreMiddleware middleware);
    void clear_middlewares();
    void add_forward_proxy_rule(ForwardProxyRule rule);
    void enable_forward_proxy(std::vector<ForwardProxyRule> rules);
    void disable_forward_proxy();
    void set_not_found_handler(RouteHandler handler);

    HttpResponse              handle(const HttpRequest& request) const;
    MatchResult               match(const HttpRequest& request) const;
    const ReverseProxyConfig* resolve_reverse_proxy(const HttpRequest& request) const;
    std::string               map_reverse_proxy_path(const HttpRequest&        request,
                                                     const ReverseProxyConfig& config,
                                                     std::string_view          route_path,
                                                     bool                      allow_prefix) const;
    ForwardProxyDecision      authorize_forward_proxy_request(const HttpRequest& request) const;
    ForwardProxyDecision      authorize_forward_proxy_target(std::string_view absolute_target) const;
    bool                      forward_proxy_enabled() const noexcept { return forward_proxy_enabled_; }
    void                      apply_middlewares(const HttpRequest& request, HttpResponse& response) const;

    void serve_static(std::string prefix, std::filesystem::path root, std::string index_file = "index.html");

private:
    struct Route {
        std::string                         method;
        std::string                         path;
        RouteHandler                        handler;
        bool                                allow_prefix { false };
        std::shared_ptr<ReverseProxyConfig> reverse_proxy;
    };

    std::vector<Route>            routes_;
    std::vector<PostMiddleware>   post_middlewares_;
    std::vector<PreMiddleware>    pre_middlewares_;
    std::vector<ForwardProxyRule> forward_proxy_rules_;
    bool                          forward_proxy_enabled_ { false };
    RouteHandler                  not_found_handler_;

    static bool method_matches(std::string_view lhs, std::string_view rhs);
    static bool path_matches(std::string_view route_path, std::string_view request_target, bool allow_prefix);
    static std::string_view extract_path(const HttpRequest& request);
    MatchResult             make_match_result(const Route* route) const;
    static std::string      normalize_prefix(std::string prefix);
    static std::string      ensure_leading_slash(std::string path);
    void                    apply_post_middlewares(const HttpRequest& request, HttpResponse& response) const;
    HttpResponse            make_default_not_found(const HttpRequest& request) const;
    static bool             host_matches(std::string_view pattern, std::string_view host, bool allow_subdomains);
    ForwardProxyDecision
    build_forward_proxy_decision(std::string target, const ForwardProxyRule* rule, bool allowed, bool matched) const;
};

} // namespace co_wq::net::http
