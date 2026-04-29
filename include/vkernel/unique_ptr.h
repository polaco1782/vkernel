#ifndef VKERNEL_UNIQUE_PTR_H
#define VKERNEL_UNIQUE_PTR_H

#include "types.h"

namespace vk {

template<typename T>
constexpr auto move(T&& value) noexcept -> remove_reference_t<T>&& {
    return static_cast<remove_reference_t<T>&&>(value);
}

template<typename T>
struct default_delete {
    constexpr default_delete() noexcept = default;

    void operator()(T* ptr) const noexcept {
        delete ptr;
    }
};

template<typename T, typename Deleter = default_delete<T>>
class unique_ptr {
public:
    constexpr unique_ptr() noexcept = default;
    constexpr unique_ptr(nullptr_t) noexcept {}

    explicit constexpr unique_ptr(T* ptr) noexcept
        : ptr_(ptr) {}

    constexpr unique_ptr(T* ptr, Deleter deleter) noexcept
        : ptr_(ptr), deleter_(move(deleter)) {}

    unique_ptr(const unique_ptr&) = delete;
    auto operator=(const unique_ptr&) -> unique_ptr& = delete;

    constexpr unique_ptr(unique_ptr&& other) noexcept
        : ptr_(other.release()), deleter_(move(other.deleter_)) {}

    auto operator=(unique_ptr&& other) noexcept -> unique_ptr& {
        if (this != &other) {
            reset(other.release());
            deleter_ = move(other.deleter_);
        }
        return *this;
    }

    ~unique_ptr() {
        reset();
    }

    [[nodiscard]] constexpr auto get() const noexcept -> T* {
        return ptr_;
    }

    [[nodiscard]] explicit constexpr operator bool() const noexcept {
        return ptr_ != null;
    }

    [[nodiscard]] constexpr auto operator*() const noexcept -> T& {
        return *ptr_;
    }

    [[nodiscard]] constexpr auto operator->() const noexcept -> T* {
        return ptr_;
    }

    [[nodiscard]] constexpr auto release() noexcept -> T* {
        T* ptr = ptr_;
        ptr_ = null;
        return ptr;
    }

    constexpr void reset(T* ptr = null) noexcept {
        if (ptr_ != null) {
            deleter_(ptr_);
        }
        ptr_ = ptr;
    }

private:
    T* ptr_ = null;
    Deleter deleter_ {};
};

} // namespace vk

#endif /* VKERNEL_UNIQUE_PTR_H */
