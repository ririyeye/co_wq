#pragma once

#if defined(USING_USB)

#include "callback_wq.hpp"
#include "io_waiter.hpp"
#include "worker.hpp"

#if defined(_WIN32)
#include <basetsd.h>
#include <winsock2.h>

#ifdef ssize_t
#undef ssize_t
#endif
#endif

#include <libusb-1.0/libusb.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#if !defined(_WIN32)
#include <sys/time.h>
#endif
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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
        start_event_thread();
    }

    ~usb_context()
    {
        stop_event_thread();
        if (_ctx)
            libusb_exit(_ctx);
    }

    usb_context(const usb_context&)            = delete;
    usb_context& operator=(const usb_context&) = delete;
    usb_context(usb_context&&)                 = delete;
    usb_context& operator=(usb_context&&)      = delete;

    libusb_context* native_handle() const noexcept { return _ctx; }

    void set_debug(int level)
    {
        if (_ctx)
            libusb_set_option(_ctx, LIBUSB_OPTION_LOG_LEVEL, level);
    }

private:
    void start_event_thread()
    {
        _running.store(true, std::memory_order_release);
        _event_thread = std::thread([this] { event_loop(); });
    }

    void stop_event_thread()
    {
        _running.store(false, std::memory_order_release);
        if (_event_thread.joinable())
            _event_thread.join();
    }

    void event_loop()
    {
        while (_running.load(std::memory_order_acquire)) {
            if (!_ctx)
                break;
            timeval tv { 0, 100000 };
            int     rc = libusb_handle_events_timeout(_ctx, &tv);
            if (rc == LIBUSB_ERROR_INTERRUPTED)
                continue;
            if (rc < 0 && _running.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    libusb_context*   _ctx { nullptr };
    std::atomic<bool> _running { false };
    std::thread       _event_thread;
};

inline std::unique_ptr<libusb_device_handle, decltype(&libusb_close)> make_device_handle(libusb_device_handle* handle)
{
    return std::unique_ptr<libusb_device_handle, decltype(&libusb_close)>(handle, &libusb_close);
}

template <lockable lock> class usb_device {
public:
    struct config {
        std::size_t tx_slots { 8 };
        std::size_t rx_slots { 2 };
    };

    usb_device() = delete;

    usb_device(workqueue<lock>& exec, usb_context& ctx, libusb_device_handle* raw_handle, config cfg = {})
        : _exec(&exec), _context(&ctx), _handle(make_device_handle(raw_handle))
    {
        if (!_handle)
            throw std::runtime_error("usb_device: null device handle");
        if (cfg.tx_slots == 0 || cfg.rx_slots == 0)
            throw std::invalid_argument("usb_device: slot count must be greater than zero");

        _cbq     = std::make_unique<callback_wq<lock>>(exec);
        _tx_pool = std::make_unique<transfer_pool>(*this, cfg.tx_slots);
        _rx_pool = std::make_unique<transfer_pool>(*this, cfg.rx_slots);
    }

    usb_device(const usb_device&)            = delete;
    usb_device& operator=(const usb_device&) = delete;

    usb_device(usb_device&& other) noexcept
        : _exec(other._exec)
        , _context(other._context)
        , _handle(std::move(other._handle))
        , _cbq(std::move(other._cbq))
        , _tx_pool(std::move(other._tx_pool))
        , _rx_pool(std::move(other._rx_pool))
    {
        other._exec    = nullptr;
        other._context = nullptr;
    }

    usb_device& operator=(usb_device&& other) noexcept
    {
        if (this != &other) {
            _handle.reset();
            _cbq.reset();
            _tx_pool.reset();
            _rx_pool.reset();

            _exec    = other._exec;
            _context = other._context;
            _handle  = std::move(other._handle);
            _cbq     = std::move(other._cbq);
            _tx_pool = std::move(other._tx_pool);
            _rx_pool = std::move(other._rx_pool);

            other._exec    = nullptr;
            other._context = nullptr;
        }
        return *this;
    }

    static usb_device
    open(workqueue<lock>& exec, usb_context& ctx, uint16_t vendor_id, uint16_t product_id, config cfg = {})
    {
        libusb_device_handle* raw = libusb_open_device_with_vid_pid(ctx.native_handle(), vendor_id, product_id);
        if (!raw)
            throw std::runtime_error("libusb_open_device_with_vid_pid returned nullptr");
        return usb_device(exec, ctx, raw, cfg);
    }

    libusb_device_handle* native_handle() const noexcept { return _handle.get(); }

    workqueue<lock>& exec()
    {
        if (!_exec)
            throw std::runtime_error("usb_device: executor not available");
        return *_exec;
    }

    int claim_interface(int interface_number)
    {
        std::lock_guard<lock> guard(_libusb_lock);
        return libusb_claim_interface(_handle.get(), interface_number);
    }

    int release_interface(int interface_number)
    {
        std::lock_guard<lock> guard(_libusb_lock);
        return libusb_release_interface(_handle.get(), interface_number);
    }

    bool kernel_driver_active(int interface_number)
    {
        std::lock_guard<lock> guard(_libusb_lock);
        int                   rc = libusb_kernel_driver_active(_handle.get(), interface_number);
        if (rc < 0)
            throw usb_error("libusb_kernel_driver_active", rc);
        return rc == 1;
    }

    int detach_kernel_driver(int interface_number)
    {
        std::lock_guard<lock> guard(_libusb_lock);
        return libusb_detach_kernel_driver(_handle.get(), interface_number);
    }

    Task<int, Work_Promise<lock, int>>
    bulk_transfer_in(unsigned char endpoint, unsigned char* buffer, int length, unsigned int timeout_ms = 0)
    {
        if (!_rx_pool)
            co_return LIBUSB_ERROR_NO_DEVICE;
        bulk_transfer_awaiter awaiter(*this, *_rx_pool, endpoint, buffer, length, timeout_ms, false);
        co_return co_await awaiter;
    }

    Task<int, Work_Promise<lock, int>>
    bulk_transfer_out(unsigned char endpoint, const unsigned char* data, int length, unsigned int timeout_ms = 0)
    {
        if (!_tx_pool)
            co_return LIBUSB_ERROR_NO_DEVICE;
        unsigned char*        mutable_buf = const_cast<unsigned char*>(data);
        bulk_transfer_awaiter awaiter(*this, *_tx_pool, endpoint, mutable_buf, length, timeout_ms, true);
        co_return co_await awaiter;
    }

    Task<int, Work_Promise<lock, int>> control_transfer(uint8_t        bmRequestType,
                                                        uint8_t        bRequest,
                                                        uint16_t       wValue,
                                                        uint16_t       wIndex,
                                                        unsigned char* data,
                                                        uint16_t       wLength,
                                                        unsigned int   timeout_ms = 0)
    {
        std::lock_guard<lock> guard(_libusb_lock);
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
    struct transfer_slot;
    class transfer_pool;
    class bulk_transfer_awaiter;

    static int translate_status(const libusb_transfer& transfer)
    {
        switch (transfer.status) {
        case LIBUSB_TRANSFER_COMPLETED:
            return transfer.actual_length;
        case LIBUSB_TRANSFER_TIMED_OUT:
            return LIBUSB_ERROR_TIMEOUT;
        case LIBUSB_TRANSFER_STALL:
            return LIBUSB_ERROR_PIPE;
        case LIBUSB_TRANSFER_NO_DEVICE:
            return LIBUSB_ERROR_NO_DEVICE;
        case LIBUSB_TRANSFER_OVERFLOW:
            return LIBUSB_ERROR_OVERFLOW;
        case LIBUSB_TRANSFER_CANCELLED:
            return LIBUSB_ERROR_INTERRUPTED;
        case LIBUSB_TRANSFER_ERROR:
        default:
            return LIBUSB_ERROR_IO;
        }
    }

    workqueue<lock>*                                               _exec { nullptr };
    usb_context*                                                   _context { nullptr };
    std::unique_ptr<libusb_device_handle, decltype(&libusb_close)> _handle;
    lock                                                           _libusb_lock {};
    std::unique_ptr<callback_wq<lock>>                             _cbq;
    std::unique_ptr<transfer_pool>                                 _tx_pool;
    std::unique_ptr<transfer_pool>                                 _rx_pool;
};

template <lockable lock> struct usb_device<lock>::transfer_slot {
    explicit transfer_slot(transfer_pool& pool) : _pool(pool)
    {
        transfer = libusb_alloc_transfer(0);
        if (!transfer)
            throw std::bad_alloc();
        transfer->user_data = this;
    }

    ~transfer_slot()
    {
        if (transfer)
            libusb_free_transfer(transfer);
    }

    transfer_slot(const transfer_slot&)            = delete;
    transfer_slot& operator=(const transfer_slot&) = delete;

    int submit(bulk_transfer_awaiter& awaiter)
    {
        auto& dev       = _pool.owner();
        transfer->flags = 0;
        if (awaiter._add_zero_packet)
            transfer->flags |= LIBUSB_TRANSFER_ADD_ZERO_PACKET;
        libusb_fill_bulk_transfer(transfer,
                                  dev._handle.get(),
                                  awaiter._endpoint,
                                  awaiter._buffer,
                                  awaiter._length,
                                  &transfer_slot::transfer_cb,
                                  this,
                                  awaiter._timeout);
        owner  = &awaiter;
        status = state::in_flight;
        int rc = libusb_submit_transfer(transfer);
        if (rc < 0) {
            owner  = nullptr;
            status = state::idle;
        }
        return rc;
    }

    void cancel()
    {
        if (status == state::in_flight && transfer)
            libusb_cancel_transfer(transfer);
    }

    static void LIBUSB_CALL transfer_cb(libusb_transfer* xfer)
    {
        auto* slot = static_cast<transfer_slot*>(xfer->user_data);
        slot->on_transfer_complete(*xfer);
    }

    void on_transfer_complete(const libusb_transfer& xfer)
    {
        status        = state::completed;
        auto* awaiter = owner;
        owner         = nullptr;
        int result    = usb_device<lock>::translate_status(xfer);
        if (awaiter)
            awaiter->on_transfer_complete(result, this);
    }

    enum class state { idle, reserved, in_flight, completed };

    transfer_pool&         _pool;
    libusb_transfer*       transfer { nullptr };
    bulk_transfer_awaiter* owner { nullptr };
    list_head              link { &link, &link };
    state                  status { state::idle };
};

template <lockable lock> class usb_device<lock>::transfer_pool {
public:
    transfer_pool(usb_device& dev, std::size_t slot_count) : _dev(dev)
    {
        INIT_LIST_HEAD(&_free_slots);
        INIT_LIST_HEAD(&_waiting);
        _slots.reserve(slot_count);
        for (std::size_t i = 0; i < slot_count; ++i) {
            auto slot = std::make_unique<transfer_slot>(*this);
            list_add_tail(&slot->link, &_free_slots);
            _slots.push_back(std::move(slot));
        }
    }

    usb_device& owner() { return _dev; }

    transfer_slot* acquire_or_enqueue(bulk_transfer_awaiter& waiter)
    {
        std::lock_guard<lock> guard(_lock);
        if (!list_empty(&_free_slots)) {
            auto* node = _free_slots.next;
            list_del(node);
            auto* slot   = list_entry(node, transfer_slot, link);
            slot->status = transfer_slot::state::reserved;
            return slot;
        }
        list_add_tail(&waiter.ws_node, &_waiting);
        waiter.on_enqueued_locked();
        return nullptr;
    }

    void release_slot(transfer_slot* slot)
    {
        bulk_transfer_awaiter* next = nullptr;
        {
            std::lock_guard<lock> guard(_lock);
            slot->status = transfer_slot::state::idle;
            if (!list_empty(&_waiting)) {
                auto* node = _waiting.next;
                list_del(node);
                next = list_entry(node, bulk_transfer_awaiter, ws_node);
                next->on_dequeued_locked();
                slot->status = transfer_slot::state::reserved;
            } else {
                list_add_tail(&slot->link, &_free_slots);
                return;
            }
        }
        if (next)
            next->start_with_slot(slot);
    }

    bool remove_waiter(bulk_transfer_awaiter& waiter)
    {
        std::lock_guard<lock> guard(_lock);
        if (waiter.ws_node.next == &waiter.ws_node)
            return false;
        list_del(&waiter.ws_node);
        INIT_LIST_HEAD(&waiter.ws_node);
        return true;
    }

private:
    usb_device&                                 _dev;
    lock                                        _lock {};
    list_head                                   _free_slots;
    list_head                                   _waiting;
    std::vector<std::unique_ptr<transfer_slot>> _slots;
};

template <lockable lock> class usb_device<lock>::bulk_transfer_awaiter : public io_waiter_base {
public:
    bulk_transfer_awaiter(usb_device&    dev,
                          transfer_pool& pool,
                          unsigned char  endpoint,
                          unsigned char* buffer,
                          int            length,
                          unsigned int   timeout_ms,
                          bool           add_zero_packet)
        : _dev(dev)
        , _pool(pool)
        , _endpoint(endpoint)
        , _buffer(buffer)
        , _length(length)
        , _timeout(timeout_ms)
        , _add_zero_packet(add_zero_packet)
    {
        INIT_LIST_HEAD(&this->ws_node);
        if (_dev._cbq) {
            this->store_route_guard(_dev._cbq->retain_guard());
            this->route_ctx  = _dev._cbq->context();
            this->route_post = &callback_wq<lock>::post_adapter;
        }
    }

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h)
    {
        this->h    = h;
        auto* slot = _pool.acquire_or_enqueue(*this);
        if (slot)
            start_with_slot(slot);
        return true;
    }

    int await_resume() noexcept { return _result; }

    ~bulk_transfer_awaiter()
    {
        _cancelled.store(true, std::memory_order_release);
        this->h = std::coroutine_handle<>();
        if (_slot) {
            if (_submitted.load(std::memory_order_acquire))
                _slot->cancel();
        } else if (_waiting.load(std::memory_order_acquire)) {
            if (_pool.remove_waiter(*this)) {
                _waiting.store(false, std::memory_order_release);
            }
        }
    }

    void on_enqueued_locked() { _waiting.store(true, std::memory_order_release); }

    void on_dequeued_locked()
    {
        _waiting.store(false, std::memory_order_release);
        INIT_LIST_HEAD(&this->ws_node);
    }

    void start_with_slot(transfer_slot* slot)
    {
        if (_cancelled.load(std::memory_order_acquire)) {
            _pool.release_slot(slot);
            return;
        }
        _slot  = slot;
        int rc = slot->submit(*this);
        if (rc < 0) {
            _slot   = nullptr;
            _result = rc;
            post_completion();
            _pool.release_slot(slot);
        } else {
            _submitted.store(true, std::memory_order_release);
        }
    }

    void on_transfer_complete(int result, transfer_slot* slot)
    {
        _submitted.store(false, std::memory_order_release);
        _slot   = nullptr;
        _result = result;
        post_completion();
        _pool.release_slot(slot);
    }

    void post_completion()
    {
        if (_cancelled.load(std::memory_order_acquire))
            return;
        this->func = &io_waiter_base::resume_cb;
        post_via_route(_dev.exec(), *this);
    }

private:
    usb_device&      _dev;
    transfer_pool&   _pool;
    unsigned char    _endpoint;
    unsigned char*   _buffer;
    int              _length;
    unsigned int     _timeout;
    bool             _add_zero_packet;
    std::atomic_bool _cancelled { false };
    std::atomic_bool _waiting { false };
    std::atomic_bool _submitted { false };
    transfer_slot*   _slot { nullptr };
    int              _result { LIBUSB_ERROR_OTHER };

    friend struct transfer_slot;
    friend class transfer_pool;
};

} // namespace co_wq::net

#endif // USING_USB
