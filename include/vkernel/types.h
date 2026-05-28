/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * types.h - Freestanding type definitions for C++26
 */

#ifndef VKERNEL_TYPES_H
#define VKERNEL_TYPES_H

/* ============================================================
 * Freestanding fixed-width types (no <cstdint>)
 * ============================================================ */

using i8  = signed char;
using i16 = short;
using i32 = int;
using i64 = long long;

using u8  = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;

/* Short aliases. */
using s8  = i8;
using s16 = i16;
using s32 = i32;
using s64 = i64;

#if defined(_MSC_VER)
using usize  = unsigned long long;
using isize  = long long;
#else
using usize  = unsigned long;
using isize  = long;
#endif

/* Address aliases used across the kernel. */
using phys_addr = u64;
using virt_addr = u64;
using paddr     = u64;
using vaddr     = u64;
using size_phys = u64;

using nullptr_t = decltype(nullptr);
inline constexpr nullptr_t null = nullptr;

/* ============================================================
 * Freestanding type traits (no <type_traits>)
 * ============================================================ */

template<typename T>
struct remove_reference      { using type = T; };
template<typename T>
struct remove_reference<T&>  { using type = T; };
template<typename T>
struct remove_reference<T&&> { using type = T; };

template<typename T>
using remove_reference_t = typename remove_reference<T>::type;

template<typename T>
struct remove_const          { using type = T; };
template<typename T>
struct remove_const<const T> { using type = T; };

template<typename T>
using remove_const_t = typename remove_const<T>::type;

template<typename T>
struct is_integral { static constexpr bool value = false; };

template<typename T>
struct is_integral<const T> { static constexpr bool value = is_integral<T>::value; };

template<typename T>
struct is_integral<volatile T> { static constexpr bool value = is_integral<T>::value; };

template<typename T>
struct is_integral<const volatile T> { static constexpr bool value = is_integral<T>::value; };

template<> struct is_integral<bool>               { static constexpr bool value = true; };
template<> struct is_integral<char>               { static constexpr bool value = true; };
template<> struct is_integral<signed char>        { static constexpr bool value = true; };
template<> struct is_integral<unsigned char>      { static constexpr bool value = true; };
template<> struct is_integral<short>              { static constexpr bool value = true; };
template<> struct is_integral<unsigned short>     { static constexpr bool value = true; };
template<> struct is_integral<int>                { static constexpr bool value = true; };
template<> struct is_integral<unsigned int>       { static constexpr bool value = true; };
template<> struct is_integral<long>               { static constexpr bool value = true; };
template<> struct is_integral<unsigned long>      { static constexpr bool value = true; };
template<> struct is_integral<long long>          { static constexpr bool value = true; };
template<> struct is_integral<unsigned long long> { static constexpr bool value = true; };

template<typename T>
inline constexpr bool is_integral_v = is_integral<T>::value;

template<typename T, typename U>
struct is_same { static constexpr bool value = false; };

template<typename T>
struct is_same<T, T> { static constexpr bool value = true; };

template<typename T, typename U>
inline constexpr bool is_same_v = is_same<T, U>::value;

template<bool B, typename T = void>
struct enable_if {};

template<typename T>
struct enable_if<true, T> { using type = T; };

template<bool B, typename T = void>
using enable_if_t = typename enable_if<B, T>::type;

template<bool B, typename T, typename F>
struct conditional { using type = T; };

template<typename T, typename F>
struct conditional<false, T, F> { using type = F; };

template<bool B, typename T, typename F>
using conditional_t = typename conditional<B, T, F>::type;

/* Compiler intrinsic wrapper. */
template<typename T>
struct is_class {
    static constexpr bool value = __is_class(T);
};

template<typename T>
inline constexpr bool is_class_v = is_class<T>::value;

template<typename T>
concept Integral = is_integral_v<T>;

template<typename T>
concept SameAs = is_same_v<T, T>;

template<typename T>
concept Signed = is_integral_v<T> && (T)-1 < (T)0;

template<typename T>
concept Unsigned = is_integral_v<T> && !Signed<T>;

/* ============================================================
 * Freestanding string helpers (no <string> / <string_view>)
 * ============================================================ */

namespace vk {

[[nodiscard]] constexpr auto string_length(const char* str) noexcept -> usize {
    if (str == null) {
        return 0;
    }

    usize len = 0;
    while (str[len] != '\0') {
        ++len;
    }
    return len;
}

class string_view {
public:
    constexpr string_view() noexcept : data_(null), size_(0) {}

    constexpr string_view(const char* str) noexcept
        : data_(str), size_(string_length(str)) {}

    constexpr string_view(const char* data, usize size) noexcept
        : data_(data), size_(size) {}

    template<usize N>
    constexpr string_view(const char (&str)[N]) noexcept
        : data_(str), size_(N > 0 ? N - 1 : 0) {}

    [[nodiscard]] constexpr auto data() const noexcept -> const char* { return data_; }
    [[nodiscard]] constexpr auto size() const noexcept -> usize { return size_; }
    [[nodiscard]] constexpr auto empty() const noexcept -> bool { return size_ == 0; }

    [[nodiscard]] constexpr auto begin() const noexcept -> const char* { return data_; }
    [[nodiscard]] constexpr auto end() const noexcept -> const char* { return data_ + size_; }

    [[nodiscard]] constexpr auto operator[](usize index) const noexcept -> char {
        return data_[index];
    }

    constexpr void remove_prefix(usize count) noexcept {
        if (count > size_) {
            count = size_;
        }
        data_ += count;
        size_ -= count;
    }

    [[nodiscard]] constexpr auto starts_with(string_view prefix) const noexcept -> bool {
        if (prefix.size_ > size_) {
            return false;
        }
        for (usize i = 0; i < prefix.size_; ++i) {
            if (data_[i] != prefix.data_[i]) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] constexpr auto compare(string_view other) const noexcept -> bool {
        if (size_ != other.size_) {
            return false;
        }
        for (usize i = 0; i < size_; ++i) {
            if (data_[i] != other.data_[i]) {
                return false;
            }
        }
        return true;
    }

private:
    const char* data_;
    usize size_;
};

template<usize Capacity>
class static_string {
public:
    static_assert(Capacity > 0, "static_string requires non-zero capacity");

    constexpr static_string() noexcept : data_{'\0'}, size_(0) {}

    constexpr static_string(string_view str) noexcept : data_{'\0'}, size_(0) {
        (void)assign(str);
    }

    template<usize N>
    constexpr static_string(const char (&str)[N]) noexcept : data_{'\0'}, size_(0) {
        (void)assign(string_view(str));
    }

    [[nodiscard]] constexpr auto assign(string_view str) noexcept -> bool {
        if (str.size() >= Capacity) {
            clear();
            return false;
        }

        for (usize i = 0; i < str.size(); ++i) {
            data_[i] = str[i];
        }
        size_ = str.size();
        data_[size_] = '\0';
        return true;
    }

    constexpr void clear() noexcept {
        size_ = 0;
        data_[0] = '\0';
    }

    [[nodiscard]] constexpr auto c_str() const noexcept -> const char* { return data_; }
    [[nodiscard]] constexpr auto data() noexcept -> char* { return data_; }
    [[nodiscard]] constexpr auto data() const noexcept -> const char* { return data_; }
    [[nodiscard]] constexpr auto size() const noexcept -> usize { return size_; }
    [[nodiscard]] constexpr auto empty() const noexcept -> bool { return size_ == 0; }
    [[nodiscard]] static constexpr auto capacity() noexcept -> usize { return Capacity - 1; }
    [[nodiscard]] constexpr auto view() const noexcept -> string_view { return string_view(data_, size_); }

    [[nodiscard]] constexpr auto operator[](usize index) noexcept -> char& { return data_[index]; }
    [[nodiscard]] constexpr auto operator[](usize index) const noexcept -> const char& { return data_[index]; }

    [[nodiscard]] constexpr operator string_view() const noexcept {
        return view();
    }

private:
    char  data_[Capacity];
    usize size_;
};

} // namespace vk

enum class status_code : i32 {
    success         =  0,
    error           = -1,
    no_memory       = -2,
    invalid_param   = -3,
    not_implemented = -4,
    not_ready       = -5,
    busy            = -6,
};

#if defined(_MSC_VER)
#include <stddef.h>
#define container_of(ptr, type, member) \
    (type*)((char*)(ptr) - offsetof(type, member))
#else
#define container_of(ptr, type, member) \
    (type*)((char*)(ptr) - __builtin_offsetof(type, member))
#endif

template<typename T>
[[nodiscard]] constexpr auto min(T a, T b) noexcept -> T {
    return a < b ? a : b;
}

template<typename T>
[[nodiscard]] constexpr auto max(T a, T b) noexcept -> T {
    return a > b ? a : b;
}

template<typename T, usize N>
[[nodiscard]] consteval auto array_size(T (&)[N]) noexcept -> usize {
    return N;
}

template<typename T>
[[nodiscard]] constexpr auto align_up(T val, usize align) noexcept -> T {
    return static_cast<T>((static_cast<usize>(val) + align - 1) & ~(align - 1));
}

template<typename T>
[[nodiscard]] constexpr auto align_down(T val, usize align) noexcept -> T {
    return static_cast<T>(static_cast<usize>(val) & ~(align - 1));
}

template<typename T>
[[nodiscard]] constexpr auto is_aligned(T val, usize align) noexcept -> bool {
    return (static_cast<usize>(val) & (align - 1)) == 0;
}

inline constexpr usize PAGE_SIZE_4K  = 0x1000ULL;
inline constexpr usize PAGE_SIZE_2MB = 0x200000ULL;
inline constexpr usize PAGE_SIZE_1GB = 0x40000000ULL;

[[noreturn]] void vk_panic(const char* file, unsigned int line, const char* condition);

#if defined(_MSC_VER)
#define VK_UNREACHABLE() __assume(false)
#else
#define VK_UNREACHABLE() __builtin_unreachable()
#endif

#endif /* VKERNEL_TYPES_H */
