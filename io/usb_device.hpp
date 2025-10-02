#pragma once

#if defined(USING_USB)

#include "worker.hpp"

#include <libusb-1.0/libusb.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

namespace co_wq::net {

class usb_error : public std::runtime_error {
public:
    usb_error(std::string_view where, int code) : std::runtime_error(build_message(where, code)), _code(code) { }

    int code() const noexcept { return _code; }

private:
    static std::string build_message(std::string_view where, int code)
    {
        const char* err_name = libusb_error_name(code);
        std::string msg(where);
        msg.append(" failed: ");
        msg.append(err_name ? err_name : "unknown");
        msg.append(" (");
        msg.append(std::to_string(code));
        msg.push_back(')');
        return msg;
    }

    int _code;
};

class usb_context {
public:
    usb_context()
    {
        int rc = libusb_init(&_ctx);
        if (rc != 0)
            throw usb_error("libusb_init", rc);
    }

    ~usb_context()
    {
        if (_ctx)
            libusb_exit(_ctx);
    }

    usb_context(const usb_context&)            = delete;
    usb_context& operator=(const usb_context&) = delete;

    usb_context(usb_context&& other) noexcept : _ctx(other._ctx) { other._ctx = nullptr; }
    usb_context& operator=(usb_context&& other) noexcept
    {
        if (this != &other) {
            if (_ctx)
                libusb_exit(_ctx);
            _ctx       = other._ctx;
            other._ctx = nullptr;
        }
        return *this;
    }

    libusb_context* native_handle() const noexcept { return _ctx; }

    void set_debug(int level)
    {
        if (_ctx)
            libusb_set_option(_ctx, LIBUSB_OPTION_LOG_LEVEL, level);
    }

private:
    libusb_context* _ctx { nullptr };
};

inline std::unique_ptr<libusb_device_handle, decltype(&libusb_close)> make_device_handle(libusb_device_handle* handle)
{
    return std::unique_ptr<libusb_device_handle, decltype(&libusb_close)>(handle, &libusb_close);
}

template <lockable lock> class usb_device {
public:
    usb_device() = delete;

    usb_device(workqueue<lock>& exec, usb_context& ctx, libusb_device_handle* raw_handle)
        : _exec(&exec), _context(&ctx), _handle(make_device_handle(raw_handle))
    {
        if (!_handle)
            throw std::runtime_error("usb_device: null device handle");
    }

    usb_device(const usb_device&)            = delete;
    usb_device& operator=(const usb_device&) = delete;

    usb_device(usb_device&& other) noexcept
        : _exec(other._exec), _context(other._context), _handle(std::move(other._handle))
    {
    }

    usb_device& operator=(usb_device&& other) noexcept
    {
        if (this != &other) {
            _exec    = other._exec;
            _context = other._context;
            _handle  = std::move(other._handle);
        }
        return *this;
    }

    static usb_device open(workqueue<lock>& exec, usb_context& ctx, uint16_t vendor_id, uint16_t product_id)
    {
        libusb_device_handle* raw = libusb_open_device_with_vid_pid(ctx.native_handle(), vendor_id, product_id);
        if (!raw)
            throw std::runtime_error("libusb_open_device_with_vid_pid returned nullptr");
        return usb_device(exec, ctx, raw);
    }

    libusb_device_handle* native_handle() const noexcept { return _handle.get(); }

    workqueue<lock>& exec() { return *_exec; }

    int claim_interface(int interface_number)
    {
        std::lock_guard<lock> guard(_io_lock);
        return libusb_claim_interface(_handle.get(), interface_number);
    }

    int release_interface(int interface_number)
    {
        std::lock_guard<lock> guard(_io_lock);
        return libusb_release_interface(_handle.get(), interface_number);
    }

    bool kernel_driver_active(int interface_number)
    {
        std::lock_guard<lock> guard(_io_lock);
        int                   rc = libusb_kernel_driver_active(_handle.get(), interface_number);
        if (rc < 0)
            throw usb_error("libusb_kernel_driver_active", rc);
        return rc == 1;
    }

    int detach_kernel_driver(int interface_number)
    {
        std::lock_guard<lock> guard(_io_lock);
        return libusb_detach_kernel_driver(_handle.get(), interface_number);
    }

    Task<int, Work_Promise<lock, int>>
    bulk_transfer_in(unsigned char endpoint, unsigned char* buffer, int length, unsigned int timeout_ms = 0)
    {
        std::lock_guard<lock> guard(_io_lock);
        int                   transferred = 0;
        int rc = libusb_bulk_transfer(_handle.get(), endpoint, buffer, length, &transferred, timeout_ms);
        if (rc < 0)
            co_return rc;
        co_return transferred;
    }

    Task<int, Work_Promise<lock, int>>
    bulk_transfer_out(unsigned char endpoint, const unsigned char* data, int length, unsigned int timeout_ms = 0)
    {
        std::lock_guard<lock> guard(_io_lock);
        int                   transferred = 0;
        unsigned char*        mutable_buf = const_cast<unsigned char*>(data);
        int rc = libusb_bulk_transfer(_handle.get(), endpoint, mutable_buf, length, &transferred, timeout_ms);
        if (rc < 0)
            co_return rc;
        co_return transferred;
    }

    Task<int, Work_Promise<lock, int>> control_transfer(uint8_t        bmRequestType,
                                                        uint8_t        bRequest,
                                                        uint16_t       wValue,
                                                        uint16_t       wIndex,
                                                        unsigned char* data,
                                                        uint16_t       wLength,
                                                        unsigned int   timeout_ms = 0)
    {
        std::lock_guard<lock> guard(_io_lock);
        int                   rc = libusb_control_transfer(_handle.get(),
                                         bmRequestType,
                                         bRequest,
                                         wValue,
                                         wIndex,
                                         data,
                                         wLength,
                                         timeout_ms);
        co_return rc;
    }

private:
    workqueue<lock>*                                               _exec { nullptr };
    usb_context*                                                   _context { nullptr };
    std::unique_ptr<libusb_device_handle, decltype(&libusb_close)> _handle;
    lock                                                           _io_lock {};
};

} // namespace co_wq::net

#endif // USING_USB
