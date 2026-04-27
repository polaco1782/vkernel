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

using vk_va_list = __builtin_va_list;

namespace vk {

namespace log {

enum class log_level : u8 {
    printk = 0,
    error = 1,
    warn = 2,
    info = 3,
    debug = 4,
    verbose = 5,
    crash = 6,
};

struct format_state {
    char last_char = '\0';
};

enum class length_modifier : u8 {
    none,
    l,
    ll,
    z,
};

static void format_putc(format_state& state, char c) {
    console::putc(c);
    state.last_char = c;
}

static void format_puts(format_state& state, const char* str) {
    if (str == null) {
        str = "(null)";
    }
    while (*str != '\0') {
        format_putc(state, *str++);
    }
}

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

static void format_unsigned(format_state& state, u64 value, u32 base,
                            bool uppercase, bool prefix, usize min_digits = 1) {
    static constexpr char lower_digits[] = "0123456789abcdef";
    static constexpr char upper_digits[] = "0123456789ABCDEF";
    const char* digits = uppercase ? upper_digits : lower_digits;

    char buffer[32];
    constexpr usize buffer_capacity = sizeof(buffer) / sizeof(buffer[0]);
    usize count = 0;

    do {
        buffer[count++] = digits[value % base];
        value /= base;
    } while (value != 0 && count < buffer_capacity);

    while (count < min_digits && count < buffer_capacity) {
        buffer[count++] = '0';
    }

    if (prefix) {
        format_puts(state, uppercase ? "0X" : "0x");
    }

    while (count > 0) {
        format_putc(state, buffer[--count]);
    }
}

static void format_signed(format_state& state, i64 value) {
    u64 magnitude = static_cast<u64>(value);
    if (value < 0) {
        format_putc(state, '-');
        magnitude = static_cast<u64>(-(value + 1)) + 1;
    }
    format_unsigned(state, magnitude, 10, false, false);
}

static auto read_unsigned_arg(vk_va_list args, length_modifier length) -> u64 {
    switch (length) {
        case length_modifier::ll:
            return __builtin_va_arg(args, unsigned long long);
        case length_modifier::l:
            return __builtin_va_arg(args, unsigned long);
        case length_modifier::z:
            return __builtin_va_arg(args, usize);
        case length_modifier::none:
        default:
            return __builtin_va_arg(args, unsigned int);
    }
}

static auto read_signed_arg(vk_va_list args, length_modifier length) -> i64 {
    switch (length) {
        case length_modifier::ll:
            return __builtin_va_arg(args, long long);
        case length_modifier::l:
            return __builtin_va_arg(args, long);
        case length_modifier::z:
            return __builtin_va_arg(args, isize);
        case length_modifier::none:
        default:
            return __builtin_va_arg(args, int);
    }
}

static void vformat_to_console(format_state& state, const char* format, vk_va_list args) {
    if (format == null) {
        return;
    }

    while (*format != '\0') {
        if (*format != '%') {
            format_putc(state, *format++);
            continue;
        }

        ++format;
        if (*format == '%') {
            format_putc(state, *format++);
            continue;
        }

        bool alternate = false;
        while (*format == '-' || *format == '+' || *format == ' ' || *format == '#' || *format == '0') {
            if (*format == '#') {
                alternate = true;
            }
            ++format;
        }

        while (*format >= '0' && *format <= '9') {
            ++format;
        }

        if (*format == '.') {
            ++format;
            while (*format >= '0' && *format <= '9') {
                ++format;
            }
        }

        length_modifier length = length_modifier::none;
        if (*format == 'l') {
            ++format;
            if (*format == 'l') {
                ++format;
                length = length_modifier::ll;
            } else {
                length = length_modifier::l;
            }
        } else if (*format == 'z') {
            ++format;
            length = length_modifier::z;
        }

        char spec = *format;
        if (spec == '\0') {
            break;
        }
        ++format;

        switch (spec) {
            case 'c':
                format_putc(state, static_cast<char>(__builtin_va_arg(args, int)));
                break;
            case 's':
                format_puts(state, __builtin_va_arg(args, const char*));
                break;
            case 'd':
            case 'i':
                format_signed(state, read_signed_arg(args, length));
                break;
            case 'u':
                format_unsigned(state, read_unsigned_arg(args, length), 10, false, false);
                break;
            case 'x':
                format_unsigned(state, read_unsigned_arg(args, length), 16, false, alternate);
                break;
            case 'X':
                format_unsigned(state, read_unsigned_arg(args, length), 16, true, alternate);
                break;
            case 'p': {
                auto value = reinterpret_cast<usize>(__builtin_va_arg(args, const void*));
                format_unsigned(state, value, 16, false, true, sizeof(void*) * 2);
                break;
            }
            default:
                format_putc(state, '%');
                format_putc(state, spec);
                break;
        }
    }
}

static auto level_enabled(log_level level) -> bool {
    switch (level) {
        case log_level::printk:  return true;
        case log_level::crash:   return true;
        case log_level::error:   return error_enabled();
        case log_level::warn:    return warn_enabled();
        case log_level::info:    return info_enabled();
        case log_level::debug:   return debug_enabled();
        case log_level::verbose: return verbose_enabled();
        default:                 return false;
    }
}

static auto level_prefix(log_level level) -> const char* {
    switch (level) {
        case log_level::crash:   return null;
        case log_level::error:   return "[ERROR] ";
        case log_level::warn:    return "[WARN] ";
        case log_level::info:    return "[INFO] ";
        case log_level::debug:   return "[DEBUG] ";
        case log_level::verbose: return "[VERBOSE] ";
        case log_level::printk:
        default:
            return null;
    }
}

static auto level_color(log_level level) -> console_color {
    switch (level) {
        case log_level::crash:   return console_color::white;
        case log_level::error:   return console_color::light_red;
        case log_level::warn:    return console_color::yellow;
        case log_level::info:    return console_color::light_green;
        case log_level::debug:   return console_color::blue;
        case log_level::verbose: return console_color::gray;
        case log_level::printk:
        default:
            return console_color::white;
    }
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

static void vlog(log_level level, bool append_newline, const char* format, vk_va_list args) {
    if (!level_enabled(level)) {
        return;
    }

    bool we_locked = false;
    if (level == log_level::crash) {
        if (!s_log_lock.held_by_self()) {
            /* Try briefly; if another CPU is mid-log just print on top */
            for (int i = 0; i < 1000; ++i) {
                if (s_log_lock.try_acquire()) { we_locked = true; break; }
                arch::cpu_pause();
            }
        }
    } else {
        s_log_lock.acquire();
        we_locked = true;
    }

    format_state state{};

    const char* prefix = level_prefix(level);
    if (level == log_level::crash) {
        /* Keep existing behaviour: entire crash message highlighted */
        console::set_color(console_color::white, console_color::red);
    } else if (prefix != null) {
        /* Print only the coloured prefix, then reset to white for the message */
        console::set_color(level_color(level), console_color::black);
        format_puts(state, prefix);
        console::set_color(console_color::white, console_color::black);
    }

    vformat_to_console(state, format, args);

    if (append_newline && state.last_char != '\n') {
        format_putc(state, '\n');
    }

    if (level != log_level::printk) {
        console::set_color(console_color::white, console_color::black);
    }

    if (we_locked) s_log_lock.release();
}

auto hex(char* out, usize out_size, u64 value) -> usize {
    return format_hex_value_to_buffer(out, out_size, value);
}

auto hex_bytes(char* out, usize out_size, const u8* data, usize length) -> usize {
    return format_hex_bytes_to_buffer(out, out_size, data, length);
}

void printk(const char* format, ...) {
    vk_va_list args;
    __builtin_va_start(args, format);
    vlog(log_level::printk, false, format, args);
    __builtin_va_end(args);
}

void crash(const char* format, ...) {
    vk_va_list args;
    __builtin_va_start(args, format);
    vlog(log_level::crash, true, format, args);
    __builtin_va_end(args);
}

void error(const char* format, ...) {
    vk_va_list args;
    __builtin_va_start(args, format);
    vlog(log_level::error, true, format, args);
    __builtin_va_end(args);
}

void warn(const char* format, ...) {
    vk_va_list args;
    __builtin_va_start(args, format);
    vlog(log_level::warn, true, format, args);
    __builtin_va_end(args);
}

void info(const char* format, ...) {
    vk_va_list args;
    __builtin_va_start(args, format);
    vlog(log_level::info, true, format, args);
    __builtin_va_end(args);
}

void debug(const char* format, ...) {
    vk_va_list args;
    __builtin_va_start(args, format);
    vlog(log_level::debug, true, format, args);
    __builtin_va_end(args);
}

void verbose(const char* format, ...) {
    vk_va_list args;
    __builtin_va_start(args, format);
    vlog(log_level::verbose, true, format, args);
    __builtin_va_end(args);
}

} // namespace log

} // namespace vk