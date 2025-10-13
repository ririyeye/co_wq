#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace co_wq::net::http {

using Headers = std::unordered_map<std::string, std::string>;

std::string to_lower(std::string_view input);
void        to_upper_inplace(std::string& input);
std::string trim_leading_spaces(std::string_view input);
bool        iequals(std::string_view a, std::string_view b);

struct UrlParts {
    std::string scheme;
    std::string host;
    uint16_t    port { 0 };
    std::string path;
};

bool split_host_port(std::string_view input, uint16_t default_port, std::string& host_out, uint16_t& port_out);
std::optional<UrlParts> parse_url(const std::string& url);
std::string             resolve_redirect_url(const UrlParts& base, const std::string& location);

} // namespace co_wq::net::http
