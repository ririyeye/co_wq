#include "http_router.hpp"

#include "http_common.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>

namespace co_wq::net::http {
namespace {

    std::string normalize_method(std::string method)
    {
        to_upper_inplace(method);
        return method;
    }

} // namespace

HttpRouter::HttpRouter()
{
    not_found_handler_ = [this](const HttpRequest& request) { return make_default_not_found(request); };
}

void HttpRouter::add_route(std::string method, std::string path, RouteHandler handler)
{
    if (!handler)
        return;
    routes_.push_back(
        Route { normalize_method(std::move(method)), std::move(path), std::move(handler), false, nullptr });
}

void HttpRouter::add_route_with_suffix(std::string method, std::string path, RouteHandler handler, bool allow_prefix)
{
    if (!handler)
        return;
    routes_.push_back(
        Route { normalize_method(std::move(method)), std::move(path), std::move(handler), allow_prefix, nullptr });
}

void HttpRouter::get(std::string path, RouteHandler handler)
{
    add_route("GET", std::move(path), std::move(handler));
}

void HttpRouter::get_prefix(std::string path, RouteHandler handler)
{
    add_route_with_suffix("GET", std::move(path), std::move(handler), true);
}

void HttpRouter::post(std::string path, RouteHandler handler)
{
    add_route("POST", std::move(path), std::move(handler));
}

void HttpRouter::put(std::string path, RouteHandler handler)
{
    add_route("PUT", std::move(path), std::move(handler));
}

void HttpRouter::del(std::string path, RouteHandler handler)
{
    add_route("DELETE", std::move(path), std::move(handler));
}

void HttpRouter::add_reverse_proxy(std::string prefix, std::string upstream_url)
{
    add_reverse_proxy(std::move(prefix), std::move(upstream_url), ReverseProxyOptions {});
}

void HttpRouter::add_reverse_proxy(std::string prefix, std::string upstream_url, ReverseProxyOptions options)
{
    auto normalized_prefix = normalize_prefix(std::move(prefix));
    auto normalized_method = std::string("*");

    std::optional<UrlParts> url = parse_url(upstream_url);
    if (!url)
        return;

    auto config           = std::make_shared<ReverseProxyConfig>();
    config->scheme        = std::move(url->scheme);
    config->host          = std::move(url->host);
    config->port          = url->port;
    config->upstream_path = ensure_leading_slash(std::move(url->path));
    config->strip_prefix  = options.strip_prefix;
    config->preserve_host = options.preserve_host;

    auto handler = [config](const HttpRequest& request) -> HttpResponse {
        HttpResponse response;
        response.set_status(502, "Bad Gateway");
        response.set_header("content-type", "text/plain; charset=utf-8");
        response.set_body("Reverse proxy not implemented\n");
        (void)request;
        (void)config;
        return response;
    };

    Route route;
    route.method        = std::move(normalized_method);
    route.path          = std::move(normalized_prefix);
    route.handler       = std::move(handler);
    route.allow_prefix  = true;
    route.reverse_proxy = std::move(config);
    routes_.push_back(std::move(route));
}

void HttpRouter::add_middleware(Middleware middleware)
{
    add_post_middleware(std::move(middleware));
}

void HttpRouter::add_post_middleware(PostMiddleware middleware)
{
    if (middleware)
        post_middlewares_.push_back(std::move(middleware));
}

void HttpRouter::add_pre_middleware(PreMiddleware middleware)
{
    if (middleware)
        pre_middlewares_.push_back(std::move(middleware));
}

void HttpRouter::clear_middlewares()
{
    post_middlewares_.clear();
    pre_middlewares_.clear();
}

void HttpRouter::add_forward_proxy_rule(ForwardProxyRule rule)
{
    forward_proxy_enabled_ = true;
    forward_proxy_rules_.push_back(std::move(rule));
}

void HttpRouter::enable_forward_proxy(std::vector<ForwardProxyRule> rules)
{
    forward_proxy_enabled_ = true;
    forward_proxy_rules_   = std::move(rules);
}

void HttpRouter::disable_forward_proxy()
{
    forward_proxy_enabled_ = false;
    forward_proxy_rules_.clear();
}

void HttpRouter::set_not_found_handler(RouteHandler handler)
{
    not_found_handler_ = std::move(handler);
    if (!not_found_handler_) {
        not_found_handler_ = [this](const HttpRequest& request) { return make_default_not_found(request); };
    }
}

HttpResponse HttpRouter::handle(const HttpRequest& request) const
{
    HttpResponse response;
    bool         short_circuit = false;

    for (const auto& middleware : pre_middlewares_) {
        if (!middleware)
            continue;
        if (!middleware(request, response)) {
            short_circuit = true;
            break;
        }
    }

    if (!short_circuit) {
        MatchResult match_result = match(request);
        if (match_result.matched && match_result.handler)
            response = match_result.handler(request);
        else
            response = not_found_handler_(request);
    }

    apply_post_middlewares(request, response);
    return response;
}

HttpRouter::MatchResult HttpRouter::match(const HttpRequest& request) const
{
    const std::string      normalized_method = normalize_method(request.method);
    const std::string_view path              = extract_path(request);

    for (const auto& route : routes_) {
        if (!method_matches(route.method, normalized_method))
            continue;
        if (!path_matches(route.path, path, route.allow_prefix))
            continue;
        return make_match_result(&route);
    }
    return MatchResult {};
}

const HttpRouter::ReverseProxyConfig* HttpRouter::resolve_reverse_proxy(const HttpRequest& request) const
{
    MatchResult result = match(request);
    return result.proxy;
}

std::string HttpRouter::map_reverse_proxy_path(const HttpRequest&        request,
                                               const ReverseProxyConfig& config,
                                               std::string_view          route_path,
                                               bool                      allow_prefix) const
{
    std::string_view raw_target = request.target.empty() ? std::string_view(request.path)
                                                         : std::string_view(request.target);
    if (raw_target.empty())
        raw_target = "/";

    std::string_view query_fragment;
    std::string_view path_part = raw_target;
    if (auto pos = raw_target.find_first_of("?#"); pos != std::string_view::npos) {
        path_part      = raw_target.substr(0, pos);
        query_fragment = raw_target.substr(pos);
    }

    if (path_part.empty())
        path_part = "/";

    auto ensure_base = [](std::string base) {
        if (base.empty())
            base = "/";
        if (base.front() != '/')
            base.insert(base.begin(), '/');
        while (base.size() > 1 && base.back() == '/')
            base.pop_back();
        return base;
    };

    std::string base = ensure_base(config.upstream_path);

    auto join = [](std::string base_path, std::string_view suffix) {
        if (suffix.empty())
            return base_path;
        if (base_path.back() != '/')
            base_path.push_back('/');
        if (!suffix.empty() && suffix.front() == '/')
            suffix.remove_prefix(1);
        base_path.append(suffix);
        return base_path;
    };

    std::string_view remainder;
    if (allow_prefix && path_part.size() >= route_path.size())
        remainder = path_part.substr(route_path.size());

    std::string result_path;
    if (config.strip_prefix)
        result_path = join(std::move(base), remainder);
    else
        result_path = join(std::move(base), path_part);

    result_path.append(query_fragment);
    return result_path;
}

HttpRouter::ForwardProxyDecision HttpRouter::authorize_forward_proxy_request(const HttpRequest& request) const
{
    std::string_view target = request.target.empty() ? std::string_view {} : std::string_view(request.target);
    if (!target.empty())
        return authorize_forward_proxy_target(target);
    return ForwardProxyDecision {};
}

HttpRouter::ForwardProxyDecision HttpRouter::authorize_forward_proxy_target(std::string_view absolute_target) const
{
    if (!forward_proxy_enabled_)
        return ForwardProxyDecision {};

    std::string target_copy(absolute_target);
    auto        parsed = parse_url(target_copy);
    if (!parsed)
        return build_forward_proxy_decision(std::move(target_copy), nullptr, false, false);

    for (const auto& rule : forward_proxy_rules_) {
        const bool scheme_ok = rule.scheme.empty() || iequals(rule.scheme, parsed->scheme);
        const bool host_ok   = rule.host.empty() || host_matches(rule.host, parsed->host, rule.allow_subdomains);
        const bool port_ok   = (rule.port == 0) || (rule.port == parsed->port);
        if (scheme_ok && host_ok && port_ok)
            return build_forward_proxy_decision(std::move(target_copy), &rule, true, true);
    }

    return build_forward_proxy_decision(std::move(target_copy), nullptr, false, true);
}

namespace {

    std::string_view guess_mime_type(const std::filesystem::path& path)
    {
        const std::string ext = to_lower(path.extension().string());
        if (ext == ".html" || ext == ".htm")
            return "text/html; charset=utf-8";
        if (ext == ".css")
            return "text/css; charset=utf-8";
        if (ext == ".js")
            return "application/javascript";
        if (ext == ".json")
            return "application/json";
        if (ext == ".png")
            return "image/png";
        if (ext == ".jpg" || ext == ".jpeg")
            return "image/jpeg";
        if (ext == ".gif")
            return "image/gif";
        if (ext == ".svg")
            return "image/svg+xml";
        if (ext == ".txt")
            return "text/plain; charset=utf-8";
        if (ext == ".wasm")
            return "application/wasm";
        if (ext == ".ico")
            return "image/x-icon";
        return "application/octet-stream";
    }

    std::filesystem::path ensure_absolute_normal(const std::filesystem::path& path)
    {
        std::error_code       ec;
        std::filesystem::path result = std::filesystem::absolute(path, ec);
        if (ec)
            result = path;
        result = result.lexically_normal();
        return result;
    }

    bool path_has_prefix(const std::filesystem::path& path, const std::filesystem::path& prefix)
    {
        auto prefix_it = prefix.begin();
        auto path_it   = path.begin();
        for (; prefix_it != prefix.end(); ++prefix_it, ++path_it) {
            if (path_it == path.end())
                return false;
            if (*prefix_it != *path_it)
                return false;
        }
        return true;
    }

} // namespace

bool HttpRouter::method_matches(std::string_view lhs, std::string_view rhs)
{
    return lhs == rhs || lhs == "*";
}

bool HttpRouter::path_matches(std::string_view route_path, std::string_view request_target, bool allow_prefix)
{
    if (allow_prefix) {
        if (route_path.empty())
            return true;
        if (request_target.size() < route_path.size())
            return false;
        if (request_target.compare(0, route_path.size(), route_path) != 0)
            return false;
        if (request_target.size() == route_path.size())
            return true;
        char next = request_target[route_path.size()];
        return next == '/' || next == '?' || next == '#';
    }
    return route_path == request_target;
}

std::string_view HttpRouter::extract_path(const HttpRequest& request)
{
    if (!request.path.empty())
        return request.path;
    if (!request.target.empty())
        return request.target;
    return std::string_view("/");
}

HttpRouter::MatchResult HttpRouter::make_match_result(const Route* route) const
{
    MatchResult result;
    result.matched = route != nullptr;
    if (route) {
        result.handler      = route->handler;
        result.allow_prefix = route->allow_prefix;
        result.proxy        = route->reverse_proxy.get();
        result.route_path   = route->path;
    }
    return result;
}

std::string HttpRouter::normalize_prefix(std::string prefix)
{
    if (prefix.empty())
        return "/";
    if (prefix.front() != '/')
        prefix.insert(prefix.begin(), '/');
    while (prefix.size() > 1 && prefix.back() == '/')
        prefix.pop_back();
    return prefix;
}

std::string HttpRouter::ensure_leading_slash(std::string path)
{
    if (path.empty())
        return "/";
    if (path.front() != '/')
        path.insert(path.begin(), '/');
    return path;
}

HttpResponse HttpRouter::make_default_not_found(const HttpRequest& request) const
{
    (void)request;
    HttpResponse response;
    response.set_status(404, "Not Found");
    response.set_header("content-type", "text/plain; charset=utf-8");
    response.set_body("Not Found\n");
    return response;
}

void HttpRouter::apply_middlewares(const HttpRequest& request, HttpResponse& response) const
{
    apply_post_middlewares(request, response);
}

void HttpRouter::serve_static(std::string prefix, std::filesystem::path root, std::string index_file)
{
    if (prefix.empty())
        prefix = "/";
    if (prefix.front() != '/')
        prefix.insert(prefix.begin(), '/');
    while (prefix.size() > 1 && prefix.back() == '/')
        prefix.pop_back();

    std::filesystem::path normalized_root = ensure_absolute_normal(root);
    if (normalized_root.empty())
        normalized_root = std::move(root);

    auto handler = [this, prefix_copy = prefix, base = std::move(normalized_root), index = std::move(index_file)](
                       const HttpRequest& request) -> HttpResponse {
        const std::string      method_upper = normalize_method(request.method);
        const bool             send_body    = method_upper != "HEAD";
        const std::string_view full_target  = extract_path(request);

        std::string clean_path(full_target);
        if (auto query_pos = clean_path.find_first_of("?#"); query_pos != std::string::npos)
            clean_path.resize(query_pos);
        if (clean_path.empty())
            clean_path = "/";

        if (clean_path.size() < prefix_copy.size() || clean_path.compare(0, prefix_copy.size(), prefix_copy) != 0)
            return make_default_not_found(request);

        std::string suffix;
        if (clean_path.size() > prefix_copy.size())
            suffix = clean_path.substr(prefix_copy.size());
        if (!suffix.empty() && suffix.front() == '/')
            suffix.erase(0, 1);
        if (!suffix.empty() && suffix.back() == '/')
            suffix.append(index);
        if (suffix.empty())
            suffix = index;

        std::filesystem::path candidate = base / std::filesystem::path(suffix).lexically_normal();
        candidate                       = ensure_absolute_normal(candidate);

        if (!path_has_prefix(candidate, base))
            return make_default_not_found(request);

        std::error_code status_ec;
        if (std::filesystem::is_directory(candidate, status_ec) && !status_ec) {
            candidate /= index;
            candidate = ensure_absolute_normal(candidate);
            if (!path_has_prefix(candidate, base))
                return make_default_not_found(request);
        }

        std::error_code exists_ec;
        if (!std::filesystem::exists(candidate, exists_ec) || exists_ec)
            return make_default_not_found(request);

        std::ifstream stream(candidate, std::ios::binary);
        if (!stream.is_open())
            return make_default_not_found(request);

        std::string body;
        if (send_body)
            body.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());

        std::error_code file_size_ec;
        const auto      file_size = std::filesystem::file_size(candidate, file_size_ec);

        HttpResponse response;
        response.set_status(200, "OK");
        response.set_header("content-type", std::string(guess_mime_type(candidate)));
        if (!file_size_ec)
            response.set_header("content-length", std::to_string(file_size));
        if (send_body)
            response.set_body(std::move(body));
        return response;
    };

    add_route_with_suffix("GET", prefix, handler, true);
    add_route_with_suffix("HEAD", prefix, std::move(handler), true);
}

void HttpRouter::apply_post_middlewares(const HttpRequest& request, HttpResponse& response) const
{
    for (const auto& middleware : post_middlewares_) {
        if (!middleware)
            continue;
        middleware(request, response);
    }
}

bool HttpRouter::host_matches(std::string_view pattern, std::string_view host, bool allow_subdomains)
{
    if (pattern == "*" || pattern.empty())
        return true;

    std::string pattern_lower = to_lower(pattern);
    std::string host_lower    = to_lower(host);

    if (pattern_lower == host_lower)
        return true;

    if (!allow_subdomains)
        return false;

    if (host_lower.size() <= pattern_lower.size())
        return false;

    if (!host_lower.ends_with(pattern_lower))
        return false;

    const size_t prefix_pos = host_lower.size() - pattern_lower.size();
    if (prefix_pos == 0)
        return true;
    return host_lower[prefix_pos - 1] == '.';
}

HttpRouter::ForwardProxyDecision HttpRouter::build_forward_proxy_decision(std::string             target,
                                                                          const ForwardProxyRule* rule,
                                                                          bool                    allowed,
                                                                          bool                    matched) const
{
    ForwardProxyDecision decision;
    decision.matched        = matched;
    decision.allowed        = allowed;
    decision.rule           = rule;
    decision.request_target = std::move(target);
    return decision;
}

} // namespace co_wq::net::http
