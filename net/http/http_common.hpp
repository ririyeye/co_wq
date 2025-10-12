#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace co_wq::net::http {

using Headers = std::unordered_map<std::string, std::string>;

std::string to_lower(std::string_view input);
void        to_upper_inplace(std::string& input);
std::string trim_leading_spaces(std::string_view input);
bool        iequals(std::string_view a, std::string_view b);

} // namespace co_wq::net::http
