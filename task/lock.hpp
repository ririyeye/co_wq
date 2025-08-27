#pragma once

#include <atomic>
#include <concepts>
namespace co_wq {

template <typename T>
concept lockable = requires(T a) {
    { a.lock() } -> std::same_as<void>;
    { a.unlock() } -> std::same_as<void>;
};

struct SpinLock {
    std::atomic_flag locked = ATOMIC_FLAG_INIT;

public:
    void lock()
    {
        while (locked.test_and_set(std::memory_order_acquire)) {
            ;
        }
    }
    void unlock() { locked.clear(std::memory_order_release); }
};

struct nolock {

public:
    void lock() { }
    void unlock() { }
};
}
