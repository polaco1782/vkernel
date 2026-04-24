/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * console.h - Console interface (freestanding C++26)
 */

#ifndef VKERNEL_CONSOLE_H
#define VKERNEL_CONSOLE_H

#include "config.h"
#include "types.h"
#include "uefi.h"

namespace vk {

/* Console color codes */
enum class console_color : u8 {
    black = 0,
    blue,
    green,
    cyan,
    red,
    magenta,
    brown,
    light_gray,
    gray,
    light_blue,
    light_green,
    light_cyan,
    light_red,
    light_magenta,
    yellow,
    white,
    count
};

/* Console state */
struct console_state {
    u32 column;
    u32 row;
    u32 max_columns;
    u32 max_rows;
    console_color foreground;
    console_color background;
    bool cursor_visible;

    [[nodiscard]] constexpr auto is_at_end() const -> bool {
        return column >= max_columns || row >= max_rows;
    }
};

/* ============================================================
 * Console namespace
 * ============================================================ */

namespace console {

auto init() -> status_code;

/*
 * After ExitBootServices the UEFI ConOut protocol is gone.
 * Call switch_to_serial() to redirect all console output to the
 * COM1 serial port (I/O port 0x3F8) so post-EBS output is visible.
 */
void switch_to_serial();

/*
 * Call init_framebuffer() BEFORE ExitBootServices (needs GOP query).
 * Then call switch_to_framebuffer() AFTER ExitBootServices.
 * Output will go to both framebuffer and serial simultaneously.
 */
void init_framebuffer(const uefi::framebuffer_info& fb);
void switch_to_framebuffer();
auto framebuffer() -> uefi::framebuffer_info;

void putc(char c);
void puts(const char* str);
void putw(const char16_t* str);

void clear();

void set_color(console_color foreground, console_color background);

auto get_position() -> console_state;
void set_position(u32 column, u32 row);

void write(const char* str);

/* Print a 64-bit value as 0x-prefixed hex */
void put_hex(u64 value);

/* Print a 64-bit value as unsigned decimal */
void put_dec(u64 value);

} // namespace console

/* ============================================================
 * Log namespace
 * ============================================================ */

namespace log {

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

void printk(const char* format, ...);
void crash(const char* format, ...);
void error(const char* format, ...);
void warn(const char* format, ...);
void info(const char* format, ...);
void debug(const char* format, ...);
void verbose(const char* format, ...);

/* Scoped logger with RAII */
class scoped_logger {
public:
    explicit scoped_logger(const char* context)
        : context_(context) {
        info("Entering");
    }

    ~scoped_logger() {
        info("Exiting");
    }

    scoped_logger(const scoped_logger&) = delete;
    auto operator=(const scoped_logger&) -> scoped_logger& = delete;

    scoped_logger(scoped_logger&& other) noexcept
        : context_(exchange_ptr(other.context_, null)) {}

    auto operator=(scoped_logger&& other) noexcept -> scoped_logger& {
        if (this != &other) {
            context_ = exchange_ptr(other.context_, null);
        }
        return *this;
    }

private:
    const char* context_;

    /* Freestanding exchange helper */
    static auto exchange_ptr(const char*& obj, const char* new_val) -> const char* {
        const char* old = obj;
        obj = new_val;
        return old;
    }
};

} // namespace log

} // namespace vk

#endif /* VKERNEL_CONSOLE_H */
