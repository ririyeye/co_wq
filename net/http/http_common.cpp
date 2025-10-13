#include "http_common.hpp"

#include <cctype>
#include <cstdint>
#include <limits>

namespace {

bool parse_port(std::string_view input, uint16_t& port_out)
{
    if (input.empty() || input.size() > 5)
        return false;

    uint32_t value = 0;
    for (unsigned char ch : input) {
        if (!std::isdigit(ch))
            return false;
        value = value * 10 + static_cast<uint32_t>(ch - '0');
        if (value > std::numeric_limits<uint16_t>::max())
            return false;
    }

    port_out = static_cast<uint16_t>(value);
    return true;
}

} // namespace

namespace co_wq::net::http {

std::string to_lower(std::string_view input)
{
    std::string out;
    out.reserve(input.size());
    for (char ch : input) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

void to_upper_inplace(std::string& input)
{
    for (char& ch : input) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
}

std::string trim_leading_spaces(std::string_view input)
{
    size_t pos = 0;
    while (pos < input.size() && (input[pos] == ' ' || input[pos] == '\t')) {
        ++pos;
    }
    return std::string(input.substr(pos));
}

bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb))
            return false;
    }
    return true;
}

bool split_host_port(std::string_view input, uint16_t default_port, std::string& host_out, uint16_t& port_out)
{
    host_out.clear();
    port_out = default_port;

    if (input.empty())
        return false;

    if (input.front() == '[') {
        auto closing = input.find(']');
        if (closing == std::string_view::npos)
            return false;
        host_out.assign(input.substr(1, closing - 1));
        if (closing + 1 == input.size())
            return true;
        if (input[closing + 1] != ':')
            return false;
        std::string_view port_part = input.substr(closing + 2);
        uint16_t         port      = 0;
        if (!parse_port(port_part, port))
            return false;
        port_out = port;
        return true;
    }

    auto first_colon = input.find(':');
    auto last_colon  = input.rfind(':');
    if (first_colon != std::string_view::npos && first_colon == last_colon) {
        std::string_view host_part = input.substr(0, first_colon);
        std::string_view port_part = input.substr(first_colon + 1);
        if (host_part.empty())
            return false;
        uint16_t port = 0;
        if (!parse_port(port_part, port))
            return false;
        host_out.assign(host_part);
        port_out = port;
        return true;
    }

    host_out.assign(input);
    return true;
}

std::optional<UrlParts> parse_url(const std::string& url)
{
    auto scheme_pos = url.find("://");
    if (scheme_pos == std::string::npos)
        return std::nullopt;

    UrlParts result;
    result.scheme = to_lower(url.substr(0, scheme_pos));
    if (result.scheme != "http" && result.scheme != "https")
        return std::nullopt;

    size_t authority_start = scheme_pos + 3;
    if (authority_start >= url.size())
        return std::nullopt;

    auto        slash_pos = url.find('/', authority_start);
    std::string authority = slash_pos == std::string::npos ? url.substr(authority_start)
                                                           : url.substr(authority_start, slash_pos - authority_start);
    result.path           = slash_pos == std::string::npos ? std::string("/") : url.substr(slash_pos);
    if (authority.empty())
        return std::nullopt;

    uint16_t default_port = (result.scheme == "https") ? 443 : 80;
    if (!split_host_port(authority, default_port, result.host, result.port))
        return std::nullopt;
    if (result.host.empty())
        return std::nullopt;

    return result;
}

std::string resolve_redirect_url(const UrlParts& base, const std::string& location)
{
    if (location.empty())
        return {};
    if (location.find("http://") == 0 || location.find("https://") == 0)
        return location;

    auto build_authority = [&]() {
        std::string value           = base.scheme + "://" + base.host;
        const bool  is_https        = base.scheme == "https";
        const bool  is_default_port = (is_https && base.port == 443) || (!is_https && base.port == 80);
        if (!is_default_port) {
            value.push_back(':');
            value.append(std::to_string(base.port));
        }
        return value;
    };

    if (location.front() == '/')
        return build_authority() + location;

    std::string result     = build_authority();
    auto        last_slash = base.path.find_last_of('/');
    if (last_slash == std::string::npos) {
        result.push_back('/');
    } else {
        result.append(base.path.substr(0, last_slash + 1));
    }
    result.append(location);
    return result;
}

} // namespace co_wq::net::http
