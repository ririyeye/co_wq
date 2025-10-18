#include "header_utils.hpp"
#include "http_common.hpp"

#include <algorithm>

namespace co_wq::net::http {

bool header_exists(const std::vector<HeaderEntry>& headers, std::string_view name)
{
    for (const auto& h : headers) {
        if (iequals(h.name, name))
            return true;
    }
    return false;
}

void remove_content_headers(std::vector<HeaderEntry>& headers)
{
    headers.erase(std::remove_if(headers.begin(),
                                 headers.end(),
                                 [](const HeaderEntry& h) {
                                     return iequals(h.name, "content-type") || iequals(h.name, "content-length")
                                         || iequals(h.name, "transfer-encoding");
                                 }),
                  headers.end());
}

} // namespace co_wq::net::http
