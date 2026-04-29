/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * log.h - Logging interface (freestanding C++26)
 */

#ifndef VKERNEL_LOG_H
#define VKERNEL_LOG_H

#include "config.h"
#include "types.h"
#include "uefi.h"

/* ============================================================
 * Log namespace
 * ============================================================ */

namespace vk {

namespace log {

enum class level : u8 {
    printk = 0,
    error = 1,
    warn = 2,
    info = 3,
    debug = 4,
    verbose = 5,
    crash = 6,
};

struct hex_value {
    u64 value;
    usize min_digits;
    bool prefix;
    bool uppercase;
};

class line {
public:
    explicit line(level lvl, bool append_newline = true);
    line(const line&) = delete;
    auto operator=(const line&) -> line& = delete;
    ~line();

    auto operator<<(const char* value) -> line&;
    auto operator<<(char* value) -> line& { return *this << static_cast<const char*>(value); }
    auto operator<<(char value) -> line&;
    auto operator<<(bool value) -> line&;
    auto operator<<(const void* value) -> line&;
    auto operator<<(const volatile void* value) -> line&;
    auto operator<<(void* value) -> line& { return *this << static_cast<const void*>(value); }
    auto operator<<(volatile void* value) -> line& { return *this << static_cast<const volatile void*>(value); }
    auto operator<<(string_view value) -> line&;
    auto operator<<(hex_value value) -> line&;

    template<typename T>
    auto operator<<(const T* value) -> line& {
        return *this << static_cast<const void*>(value);
    }

    template<typename T>
    auto operator<<(const volatile T* value) -> line& {
        return *this << static_cast<const volatile void*>(value);
    }

    template<Integral T>
    auto operator<<(T value) -> line& {
        if constexpr (Signed<T>) {
            append_signed(static_cast<i64>(value));
        } else {
            append_unsigned(static_cast<u64>(value), 10, false, false, 1);
        }
        return *this;
    }

private:
    void putc(char c);
    void puts(const char* str);
    void append_unsigned(u64 value, u32 base, bool uppercase, bool prefix, usize min_digits);
    void append_signed(i64 value);
    void lock();
    void unlock();

    level level_;
    bool enabled_;
    bool append_newline_;
    bool locked_;
    char last_char_;
};

[[nodiscard]] constexpr auto error_enabled() -> bool {
    return config::debug_level >= 1;
}

[[nodiscard]] constexpr auto warn_enabled() -> bool {
    return config::debug_level >= 2;
}

[[nodiscard]] constexpr auto info_enabled() -> bool {
    return config::debug_level >= 3;
}

[[nodiscard]] constexpr auto debug_enabled() -> bool {
    return config::debug_level >= 4;
}

[[nodiscard]] constexpr auto verbose_enabled() -> bool {
    return config::debug_level >= 5;
}

[[nodiscard]] auto printk() -> line;
[[nodiscard]] auto crash() -> line;
[[nodiscard]] auto error() -> line;
[[nodiscard]] auto warn() -> line;
[[nodiscard]] auto info() -> line;
[[nodiscard]] auto debug() -> line;
[[nodiscard]] auto verbose() -> line;

[[nodiscard]] constexpr auto hex(u64 value, usize min_digits = 1,
                                 bool prefix = true, bool uppercase = false) -> hex_value {
    return { value, min_digits, prefix, uppercase };
}

/* Hex formatting helpers used by diagnostics and kernel stubs. */
auto hex(char* out, usize out_size, u64 value) -> usize;
auto hex_bytes(char* out, usize out_size, const u8* data, usize length) -> usize;

} // namespace log

} // namespace vk

#endif // VKERNEL_LOG_H
