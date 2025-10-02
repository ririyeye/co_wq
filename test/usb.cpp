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
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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
            std::cout << "Usage: co_usb [--no-list] [--vid HEX] [--pid HEX] [--interface NUM] [--detach] [--timeout "
                         "MS] [--debug LEVEL]\n";
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
            std::cerr << "Unknown argument: " << arg << "\n";
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

    std::cout << "Bus " << std::setw(3) << std::setfill('0') << static_cast<int>(libusb_get_bus_number(dev))
              << " Device " << std::setw(3) << static_cast<int>(libusb_get_device_address(dev)) << " ID " << std::hex
              << std::setw(4) << std::setfill('0') << desc.idVendor << ':' << std::setw(4) << desc.idProduct << std::dec
              << std::setfill(' ') << " class=" << static_cast<int>(desc.bDeviceClass)
              << " max-packet=" << static_cast<int>(desc.bMaxPacketSize0) << '\n';
}

Task<void, Work_Promise<SpinLock, void>> usb_demo(workqueue<SpinLock>& exec, usb_context& ctx, const UsbOptions& opts)
{
    if (opts.debug_level >= 0)
        ctx.set_debug(opts.debug_level);

    if (opts.enumerate) {
        libusb_device** list  = nullptr;
        ssize_t         count = libusb_get_device_list(ctx.native_handle(), &list);
        if (count < 0) {
            std::cerr << "[usb] libusb_get_device_list failed: " << libusb_error_name(static_cast<int>(count)) << '\n';
        } else {
            std::cout << "[usb] detected " << count << " devices" << '\n';
            for (ssize_t i = 0; i < count; ++i)
                print_device_info(list[i]);
        }
        if (list)
            libusb_free_device_list(list, 1);
    }

    if (!opts.vid || !opts.pid) {
        co_return;
    }

    std::cout << std::hex << std::showbase;
    std::cout << "[usb] opening device " << static_cast<int>(*opts.vid) << ':' << static_cast<int>(*opts.pid) << '\n';
    std::cout << std::dec << std::noshowbase;

    std::optional<usb_device<SpinLock>> device_holder;
    try {
        device_holder.emplace(usb_device<SpinLock>::open(exec, ctx, *opts.vid, *opts.pid));
    } catch (const std::exception& ex) {
        std::cerr << "[usb] failed to open VID:PID device: " << ex.what() << '\n';
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
                        std::cerr << "[usb] detach_kernel_driver failed: " << libusb_error_name(rc) << '\n';
                    else
                        std::cout << "[usb] detached kernel driver from interface " << if_num << '\n';
                }
            } catch (const usb_error& ex) {
                std::cerr << "[usb] " << ex.what() << '\n';
            }
        }
        int rc = device.claim_interface(if_num);
        if (rc != 0) {
            std::cerr << "[usb] claim_interface failed: " << libusb_error_name(rc) << '\n';
        } else {
            interface_claimed = true;
            std::cout << "[usb] claimed interface " << if_num << '\n';
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
        std::cerr << "[usb] control_transfer failed: " << libusb_error_name(rc) << '\n';
    } else {
        std::cout << "[usb] device descriptor length=" << rc << '\n';
    }

    if (interface_claimed) {
        int if_num = *opts.interface_number;
        int rel    = device.release_interface(if_num);
        if (rel != 0)
            std::cerr << "[usb] release_interface failed: " << libusb_error_name(rel) << '\n';
        else
            std::cout << "[usb] released interface " << if_num << '\n';
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
