#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace co_wq::net::http {

struct HeaderEntry {
    std::string name;
    std::string value;
};

bool header_exists(const std::vector<HeaderEntry>& headers, std::string_view name);
void remove_content_headers(std::vector<HeaderEntry>& headers);

} // namespace co_wq::net::http
