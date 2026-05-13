/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * log.cpp - Logging implementation
 */

#include "log.h"
#include "types.h"
#include "console.h"
#include "spinlock.h"

namespace vk {

namespace log {

static constexpr char hex_digits[] = "0123456789ABCDEF";

static auto copy_to_buffer(char* out, usize out_size, const char* str) -> usize {
    if (out == null || out_size == 0) {
        return 0;
    }

    if (str == null) {
        str = "(null)";
    }

    usize pos = 0;
    while (*str != '\0' && pos + 1 < out_size) {
        out[pos++] = *str++;
    }
    out[pos] = '\0';
    return pos;
}

static auto format_hex_value_to_buffer(char* out, usize out_size, u64 value) -> usize {
    if (out == null || out_size == 0) {
        return 0;
    }

    char buffer[16];
    constexpr usize buffer_capacity = sizeof(buffer) / sizeof(buffer[0]);
    usize count = 0;

    do {
        buffer[count++] = hex_digits[value & 0xF];
        value >>= 4;
    } while (value != 0 && count < buffer_capacity);

    while (count < buffer_capacity) {
        buffer[count++] = '0';
    }

    usize pos = 0;
    if (out_size <= 1) {
        out[0] = '\0';
        return 0;
    }

    out[pos++] = '0';
    if (pos + 1 >= out_size) {
        out[0] = '\0';
        return 0;
    }
    out[pos++] = 'x';

    while (count > 0 && pos + 1 < out_size) {
        out[pos++] = buffer[--count];
    }

    out[pos] = '\0';
    return pos;
}

static auto format_hex_bytes_to_buffer(char* out, usize out_size, const u8* data, usize length) -> usize {
    if (out == null || out_size == 0) {
        return 0;
    }

    if (data == null) {
        return copy_to_buffer(out, out_size, "(null)");
    }

    usize pos = 0;
    for (usize i = 0; i < length; ++i) {
        if (pos + 3 >= out_size) {
            break;
        }

        u8 byte = data[i];
        out[pos++] = hex_digits[byte >> 4];
        out[pos++] = hex_digits[byte & 0xF];
        out[pos++] = ' ';
    }

    out[pos] = '\0';
    return pos;
}

static auto level_enabled(level lvl) -> bool {
    switch (lvl) {
        case level::printk:  return true;
        case level::crash:   return true;
        case level::error:   return error_enabled();
        case level::warn:    return warn_enabled();
        case level::info:    return info_enabled();
        case level::debug:   return debug_enabled();
        case level::verbose: return verbose_enabled();
        default:                 return false;
    }
}

static auto level_prefix(level lvl) -> const char* {
    switch (lvl) {
        case level::crash:   return null;
        case level::error:   return "[ERROR] ";
        case level::warn:    return "[WARN] ";
        case level::info:    return "[INFO] ";
        case level::debug:   return "[DEBUG] ";
        case level::verbose: return "[VERBOSE] ";
        case level::printk:
        default:
            return null;
    }
}

static auto level_color(level lvl) -> console_color {
    switch (lvl) {
        case level::crash:   return console_color::white;
        case level::error:   return console_color::light_red;
        case level::warn:    return console_color::yellow;
        case level::info:    return console_color::light_green;
        case level::debug:   return console_color::blue;
        case level::verbose: return console_color::gray;
        case level::printk:
        default:
            return console_color::white;
    }
}

/* Current log routing destination — modified via kobj at sys/log/route. */
static volatile u32 s_log_route = 1;  /* 0=default, 1=serial, 2=disabled */

auto get_route() -> route {
    return static_cast<route>(s_log_route);
}

void set_route(route r) {
    s_log_route = static_cast<u32>(r);
}

/* Spinlock for serialising log output across CPUs.
 *
 * Crash-level messages also take this lock, but use a recursive
 * try-acquire path so that:
 *   - if no other CPU holds it, we acquire normally;
 *   - if THIS CPU already holds it (we crashed mid-log), we just
 *     proceed without re-acquiring (which would self-deadlock);
 *   - if ANOTHER CPU holds it, we briefly spin then proceed anyway,
 *     accepting interleaved output rather than a hang. */
static spinlock s_log_lock;

void line::lock() {
    if (level_ == level::crash) {
        if (!s_log_lock.held_by_self()) {
            for (int i = 0; i < 1000; ++i) {
                if (s_log_lock.try_acquire()) {
                    locked_ = true;
                    break;
                }
                arch::cpu_pause();
            }
        }
    } else {
        s_log_lock.acquire();
        locked_ = true;
    }
}

void line::unlock() {
    if (locked_) {
        s_log_lock.release();
        locked_ = false;
    }
}

line::line(level lvl, bool append_newline)
    : level_(lvl),
      enabled_(level_enabled(lvl)),
      append_newline_(append_newline),
      locked_(false),
      last_char_('\0') {
    if (!enabled_) {
        return;
    }

    lock();

    const char* prefix = level_prefix(level_);
    const u32 route = s_log_route;
    if (level_ == level::crash) {
        if (route == 0) console::set_color(console_color::white, console_color::red);
    } else if (prefix != null) {
        if (route == 0) {
            console::set_color(level_color(level_), console_color::black);
            puts(prefix);
            console::set_color(console_color::white, console_color::black);
        } else if (route == 1) {
            console::puts_serial(prefix);
        }
    }
}

line::~line() {
    if (!enabled_) {
        return;
    }

    if (append_newline_ && last_char_ != '\n') {
        putc('\n');
    }

    if (level_ != level::printk && s_log_route == 0) {
        console::set_color(console_color::white, console_color::black);
    }

    unlock();
}

void line::putc(char c) {
    if (!enabled_) {
        return;
    }
    const u32 route = s_log_route;
    if (route == 1) {
        console::putc_serial(c);
    } else if (route == 0) {
        console::putc(c);
    }
    /* route == 2: disabled — suppress */
    last_char_ = c;
}

void line::puts(const char* str) {
    if (!enabled_) {
        return;
    }
    if (str == null) {
        str = "(null)";
    }
    while (*str != '\0') {
        putc(*str++);
    }
}

void line::append_unsigned(u64 value, u32 base, bool uppercase, bool prefix, usize min_digits) {
    if (!enabled_) {
        return;
    }
    static constexpr char lower_digits[] = "0123456789abcdef";
    static constexpr char upper_digits[] = "0123456789ABCDEF";
    const char* digits = uppercase ? upper_digits : lower_digits;

    char tmp[32];
    constexpr usize tmp_capacity = sizeof(tmp) / sizeof(tmp[0]);
    usize count = 0;

    do {
        tmp[count++] = digits[value % base];
        value /= base;
    } while (value != 0 && count < tmp_capacity);

    while (count < min_digits && count < tmp_capacity) {
        tmp[count++] = '0';
    }

    if (prefix) {
        putc('0');
        putc(uppercase ? 'X' : 'x');
    }

    while (count > 0) {
        putc(tmp[--count]);
    }
}

void line::append_signed(i64 value) {
    if (!enabled_) {
        return;
    }
    if (value < 0) {
        putc('-');
        append_unsigned(static_cast<u64>(-(value + 1)) + 1, 10, false, false, 1);
    } else {
        append_unsigned(static_cast<u64>(value), 10, false, false, 1);
    }
}

auto line::operator<<(const char* value) -> line& {
    puts(value);
    return *this;
}

auto line::operator<<(char value) -> line& {
    putc(value);
    return *this;
}

auto line::operator<<(bool value) -> line& {
    puts(value ? "true" : "false");
    return *this;
}

auto line::operator<<(const void* value) -> line& {
    append_unsigned(reinterpret_cast<usize>(value), 16, false, true, sizeof(void*) * 2);
    return *this;
}

auto line::operator<<(const volatile void* value) -> line& {
    append_unsigned(reinterpret_cast<usize>(value), 16, false, true, sizeof(void*) * 2);
    return *this;
}

auto line::operator<<(string_view value) -> line& {
    if (!enabled_) {
        return *this;
    }
    for (usize i = 0; i < value.size(); ++i) {
        putc(value.data()[i]);
    }
    return *this;
}

auto line::operator<<(hex_value value) -> line& {
    append_unsigned(value.value, 16, value.uppercase, value.prefix, value.min_digits);
    return *this;
}

auto hex(char* out, usize out_size, u64 value) -> usize {
    return format_hex_value_to_buffer(out, out_size, value);
}

auto hex_bytes(char* out, usize out_size, const u8* data, usize length) -> usize {
    return format_hex_bytes_to_buffer(out, out_size, data, length);
}

auto printk() -> line {
    return line(level::printk, false);
}

auto crash() -> line {
    return line(level::crash);
}

auto error() -> line {
    return line(level::error);
}

auto warn() -> line {
    return line(level::warn);
}

auto info() -> line {
    return line(level::info);
}

auto debug() -> line {
    return line(level::debug);
}

auto verbose() -> line {
    return line(level::verbose);
}

} // namespace log

} // namespace vk
