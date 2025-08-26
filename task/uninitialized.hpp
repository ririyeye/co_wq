#pragma once
#include "non_void_helper.hpp"

namespace co_wq {
template <class T> struct Uninitialized {
    union {
        T mValue;
    };
    Uninitialized() noexcept { }

    Uninitialized(Uninitialized&&) = delete;

    ~Uninitialized() { }

    T const& ref() const noexcept { return mValue; }

    T& ref() noexcept { return mValue; }

    void destroy() { mValue.~T(); }

    T move()
    {
        T ret(std::move(mValue));
        mValue.~T();
        return ret;
    }

    template <class... Ts>
        requires std::constructible_from<T, Ts...>
    void emplace(Ts&&... args)
    {
        std::construct_at(std::addressof(mValue), std::forward<Ts>(args)...);
    }
};

template <> struct Uninitialized<void> {
    void ref() const noexcept { }

    void destroy() { }

    Void move() { return Void(); }

    void emplace(Void) { }

    void emplace() { }
};

template <> struct Uninitialized<Void> : Uninitialized<void> { };

template <class T> struct Uninitialized<T const> : Uninitialized<T> { };

template <class T> struct Uninitialized<T&> : Uninitialized<std::reference_wrapper<T>> {
private:
    using Base = Uninitialized<std::reference_wrapper<T>>;

public:
    T const& ref() const noexcept { return Base::ref().get(); }

    T& ref() noexcept { return Base::ref().get(); }

    T& move() { return Base::move().get(); }
};

template <class T> struct Uninitialized<T&&> : Uninitialized<T&> {
private:
    using Base = Uninitialized<T&>;

public:
    T&& move() { return std::move(Base::move().get()); }
};
} // namespace co_wq
