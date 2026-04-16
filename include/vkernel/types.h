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

/* Alternate names (s = signed, u = unsigned) */
using s8  = i8;
using s16 = i16;
using s32 = i32;
using s64 = i64;

using usize  = unsigned long;
using isize  = long;

/* Physical and virtual address types */
using phys_addr = u64;
using virt_addr = u64;
using paddr     = u64;
using vaddr     = u64;
using size_phys = u64;

/* ============================================================
 * Freestanding std::nullptr_t equivalent
 * ============================================================ */

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
struct is_integral;

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

/* Simple is_class (uses sizeof trick) */
template<typename T>
struct is_class {
    static constexpr bool value = __is_class(T);
};

template<typename T>
inline constexpr bool is_class_v = is_class<T>::value;

/* ============================================================
 * Freestanding concepts (no <concepts>)
 * ============================================================ */

template<typename T>
concept Integral = is_integral_v<T>;

template<typename T>
concept SameAs = is_same_v<T, T>;

template<typename T>
concept Signed = is_integral_v<T> && (T)-1 < (T)0;

template<typename T>
concept Unsigned = is_integral_v<T> && !Signed<T>;

/* ============================================================
 * Status codes
 * ============================================================ */

enum class status_code : i32 {
    success         =  0,
    error           = -1,
    no_memory       = -2,
    invalid_param   = -3,
    not_implemented = -4,
    not_ready       = -5,
    busy            = -6,
};

/* ============================================================
 * Attributes and macros
 * ============================================================ */

#define VK_NORETURN    [[noreturn]]
#define VK_INLINE      inline
#define VK_FORCEINLINE [[gnu::always_inline]] inline

/* Compile-time assert */
#define VK_STATIC_ASSERT(cond) static_assert(cond, #cond)

/* Container of */
#define container_of(ptr, type, member) \
    (type*)((char*)(ptr) - __builtin_offsetof(type, member))

/* Min/Max - constexpr */
template<typename T>
[[nodiscard]] constexpr auto min(T a, T b) noexcept -> T {
    return a < b ? a : b;
}

template<typename T>
[[nodiscard]] constexpr auto max(T a, T b) noexcept -> T {
    return a > b ? a : b;
}

template<typename T, unsigned long N>
[[nodiscard]] consteval auto array_size(T (&)[N]) noexcept -> unsigned long {
    return N;
}

/* Round up/down */
template<typename T>
[[nodiscard]] constexpr auto align_up(T val, unsigned long align) noexcept -> T {
    return static_cast<T>((static_cast<unsigned long>(val) + align - 1) & ~(align - 1));
}

template<typename T>
[[nodiscard]] constexpr auto align_down(T val, unsigned long align) noexcept -> T {
    return static_cast<T>(static_cast<unsigned long>(val) & ~(align - 1));
}

template<typename T>
[[nodiscard]] constexpr auto is_aligned(T val, unsigned long align) noexcept -> bool {
    return (static_cast<unsigned long>(val) & (align - 1)) == 0;
}

/* Page size constants */
inline constexpr unsigned long PAGE_SIZE_4K  = 0x1000ULL;
inline constexpr unsigned long PAGE_SIZE_2MB = 0x200000ULL;
inline constexpr unsigned long PAGE_SIZE_1GB = 0x40000000ULL;

/* Assert */
#define VK_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            vk_panic(__FILE__, __LINE__, #cond); \
        } \
    } while(0)

/* Forward declarations */
VK_NORETURN void vk_panic(const char* file, unsigned int line, const char* condition);

#endif /* VKERNEL_TYPES_H */
