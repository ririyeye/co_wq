#include "http_common.hpp"

#include <cctype>

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

} // namespace co_wq::net::http
