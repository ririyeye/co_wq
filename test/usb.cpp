#include "syswork.hpp"

#if !defined(USING_USB)
#error "USING_USB must be enabled to build usb example"
#endif

#include "usb_device.hpp"

#if __has_include(<libusb-1.0/libusb.h>)
#include <libusb-1.0/libusb.h>
#elif __has_include(<libusb.h>)
#include <libusb.h>
#else
#error "libusb headers not found"
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

using namespace co_wq;
using namespace co_wq::net;

namespace {

struct UsbOptions {
    bool                    enumerate { true };
    std::optional<int>      interface_number {};
    std::optional<uint16_t> vid {};
    std::optional<uint16_t> pid {};
    bool                    detach_kernel_driver { false };
    unsigned int            timeout_ms { 1000 };
    int                     debug_level { -1 };
};

uint16_t parse_hex_uint16(std::string_view text)
{
    std::string   tmp(text);
    unsigned long value = std::stoul(tmp, nullptr, 0);
    if (value > 0xFFFF)
        throw std::out_of_range("value exceeds uint16_t");
    return static_cast<uint16_t>(value);
}

UsbOptions parse_args(int argc, char* argv[])
{
    UsbOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            CO_WQ_LOG_INFO("Usage: co_usb [--no-list] [--vid HEX] [--pid HEX] [--interface NUM] [--detach] [--timeout "
                           "MS] [--debug LEVEL]");
            std::exit(0);
        } else if (arg == "--no-list") {
            opts.enumerate = false;
        } else if (arg == "--vid" && i + 1 < argc) {
            opts.vid = parse_hex_uint16(argv[++i]);
        } else if (arg == "--pid" && i + 1 < argc) {
            opts.pid = parse_hex_uint16(argv[++i]);
        } else if (arg == "--interface" && i + 1 < argc) {
            opts.interface_number = static_cast<int>(std::stoi(argv[++i]));
        } else if (arg == "--detach") {
            opts.detach_kernel_driver = true;
        } else if (arg == "--timeout" && i + 1 < argc) {
            opts.timeout_ms = static_cast<unsigned int>(std::stoul(argv[++i]));
        } else if (arg == "--debug" && i + 1 < argc) {
            opts.debug_level = std::stoi(argv[++i]);
        } else {
            CO_WQ_LOG_ERROR("Unknown argument: %.*s", static_cast<int>(arg.size()), arg.data());
            std::exit(1);
        }
    }
    return opts;
}

void print_device_info(libusb_device* dev)
{
    libusb_device_descriptor desc {};
    if (libusb_get_device_descriptor(dev, &desc) != 0)
        return;

    CO_WQ_LOG_INFO("Bus %03d Device %03d ID %04x:%04x class=%d max-packet=%d",
                   static_cast<int>(libusb_get_bus_number(dev)),
                   static_cast<int>(libusb_get_device_address(dev)),
                   static_cast<unsigned int>(desc.idVendor),
                   static_cast<unsigned int>(desc.idProduct),
                   static_cast<int>(desc.bDeviceClass),
                   static_cast<int>(desc.bMaxPacketSize0));
}

Task<void, Work_Promise<SpinLock, void>> usb_demo(workqueue<SpinLock>& exec, usb_context& ctx, const UsbOptions& opts)
{
    if (opts.debug_level >= 0)
        ctx.set_debug(opts.debug_level);

    if (opts.enumerate) {
        libusb_device** list  = nullptr;
        ssize_t         count = libusb_get_device_list(ctx.native_handle(), &list);
        if (count < 0) {
            CO_WQ_LOG_ERROR("[usb] libusb_get_device_list failed: %s", libusb_error_name(static_cast<int>(count)));
        } else {
            CO_WQ_LOG_INFO("[usb] detected %lld devices", static_cast<long long>(count));
            for (ssize_t i = 0; i < count; ++i)
                print_device_info(list[i]);
        }
        if (list)
            libusb_free_device_list(list, 1);
    }

    if (!opts.vid || !opts.pid) {
        co_return;
    }

    CO_WQ_LOG_INFO("[usb] opening device 0x%04x:0x%04x",
                   static_cast<unsigned int>(*opts.vid),
                   static_cast<unsigned int>(*opts.pid));

    std::optional<usb_device<SpinLock>> device_holder;
    try {
        device_holder.emplace(usb_device<SpinLock>::open(exec, ctx, *opts.vid, *opts.pid));
    } catch (const std::exception& ex) {
        CO_WQ_LOG_ERROR("[usb] failed to open VID:PID device: %s", ex.what());
        co_return;
    }

    auto& device            = *device_holder;
    bool  interface_claimed = false;

    if (opts.interface_number.has_value()) {
        int if_num = *opts.interface_number;
        if (opts.detach_kernel_driver) {
            try {
                if (device.kernel_driver_active(if_num)) {
                    int rc = device.detach_kernel_driver(if_num);
                    if (rc != 0)
                        CO_WQ_LOG_ERROR("[usb] detach_kernel_driver failed: %s", libusb_error_name(rc));
                    else
                        CO_WQ_LOG_INFO("[usb] detached kernel driver from interface %d", if_num);
                }
            } catch (const usb_error& ex) {
                CO_WQ_LOG_ERROR("[usb] %s", ex.what());
            }
        }
        int rc = device.claim_interface(if_num);
        if (rc != 0) {
            CO_WQ_LOG_ERROR("[usb] claim_interface failed: %s", libusb_error_name(rc));
        } else {
            interface_claimed = true;
            CO_WQ_LOG_INFO("[usb] claimed interface %d", if_num);
        }
    }

    std::array<unsigned char, LIBUSB_DT_DEVICE_SIZE> descriptor {};
    const uint8_t  bm_request_type = static_cast<uint8_t>(static_cast<uint8_t>(LIBUSB_ENDPOINT_IN)
                                                         | static_cast<uint8_t>(LIBUSB_REQUEST_TYPE_STANDARD)
                                                         | static_cast<uint8_t>(LIBUSB_RECIPIENT_DEVICE));
    const uint16_t w_value         = static_cast<uint16_t>(LIBUSB_DT_DEVICE << 8);
    int            rc              = co_await device.control_transfer(bm_request_type,
                                              LIBUSB_REQUEST_GET_DESCRIPTOR,
                                              w_value,
                                              0,
                                              descriptor.data(),
                                              static_cast<uint16_t>(descriptor.size()),
                                              opts.timeout_ms);
    if (rc < 0) {
        CO_WQ_LOG_ERROR("[usb] control_transfer failed: %s", libusb_error_name(rc));
    } else {
        CO_WQ_LOG_INFO("[usb] device descriptor length=%d", rc);
    }

    if (interface_claimed) {
        int if_num = *opts.interface_number;
        int rel    = device.release_interface(if_num);
        if (rel != 0)
            CO_WQ_LOG_ERROR("[usb] release_interface failed: %s", libusb_error_name(rel));
        else
            CO_WQ_LOG_INFO("[usb] released interface %d", if_num);
    }

    co_return;
}

} // namespace

int main(int argc, char* argv[])
{
    auto opts = parse_args(argc, argv);

    auto&       wq = get_sys_workqueue(0);
    usb_context ctx;

    std::atomic_bool finished { false };

    auto  task           = usb_demo(wq, ctx, opts);
    auto  coroutine      = task.get();
    auto& promise        = coroutine.promise();
    promise.mUserData    = &finished;
    promise.mOnCompleted = [](Promise_base& pb) {
        auto* flag = static_cast<std::atomic_bool*>(pb.mUserData);
        if (flag)
            flag->store(true, std::memory_order_release);
    };

    post_to(task, wq);

    sys_wait_until(finished);

    return 0;
}
