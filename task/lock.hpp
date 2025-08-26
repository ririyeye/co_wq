#pragma once

#include <atomic>

namespace co_wq {

struct lockable {
    virtual void lock()   = 0;
    virtual void unlock() = 0;
};

struct SpinLock : public lockable {
    std::atomic_flag locked = ATOMIC_FLAG_INIT;

public:
    void lock() final
    {
        while (locked.test_and_set(std::memory_order_acquire)) {
            ;
        }
    }
    void unlock() final { locked.clear(std::memory_order_release); }
};

struct nolock : public lockable {

public:
    void lock() final { }
    void unlock() final { }
};
}
