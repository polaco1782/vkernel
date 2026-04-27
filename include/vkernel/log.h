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

/* Hex formatting helpers used by diagnostics and kernel stubs. */
auto hex(char* out, usize out_size, u64 value) -> usize;
auto hex_bytes(char* out, usize out_size, const u8* data, usize length) -> usize;

} // namespace log

} // namespace vk

#endif // VKERNEL_LOG_H