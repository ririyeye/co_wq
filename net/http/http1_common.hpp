#pragma once

#include <string>

namespace co_wq::net::http {

class Http1HeaderCollector {
public:
    Http1HeaderCollector()                                       = default;
    Http1HeaderCollector(const Http1HeaderCollector&)            = delete;
    Http1HeaderCollector& operator=(const Http1HeaderCollector&) = delete;
    Http1HeaderCollector(Http1HeaderCollector&&)                 = default;
    Http1HeaderCollector& operator=(Http1HeaderCollector&&)      = default;
    virtual ~Http1HeaderCollector()                              = default;

protected:
    void reset_header_state()
    {
        current_field_.clear();
        current_value_.clear();
    }

    void collect_header_field(const char* at, size_t length)
    {
        if (!current_value_.empty())
            emit_header();
        current_field_.append(at, length);
    }

    void collect_header_value(const char* at, size_t length) { current_value_.append(at, length); }

    void finalize_header_value() { emit_header(); }

    bool has_pending_header() const noexcept { return !current_field_.empty(); }

private:
    void emit_header()
    {
        if (current_field_.empty())
            return;
        auto field = std::move(current_field_);
        auto value = std::move(current_value_);
        current_field_.clear();
        current_value_.clear();
        handle_header(std::move(field), std::move(value));
    }

    virtual void handle_header(std::string&& name, std::string&& value) = 0;

private:
    std::string current_field_;
    std::string current_value_;
};

} // namespace co_wq::net::http
