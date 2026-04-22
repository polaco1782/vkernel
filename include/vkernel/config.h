/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * config.h - Freestanding build configuration for C++26
 */

#ifndef VKERNEL_CONFIG_H
#define VKERNEL_CONFIG_H

#include "types.h"

namespace vk {
namespace config {

/* ============================================================
 * Compiler detection (freestanding, no <version>)
 * ============================================================ */

#if defined(_MSC_VER)
    inline constexpr bool is_msvc  = true;
    inline constexpr bool is_gcc   = false;
    inline constexpr bool is_clang = false;
    inline constexpr auto compiler_name    = "MSVC";
    inline constexpr int  compiler_version = _MSC_VER;
#elif defined(__GNUC__)
    inline constexpr bool is_msvc  = false;
    inline constexpr bool is_gcc   = true;
    inline constexpr bool is_clang = false;
    inline constexpr auto compiler_name    = "GCC";
    inline constexpr int  compiler_version =
        __GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__;
#elif defined(__clang__)
    inline constexpr bool is_msvc  = false;
    inline constexpr bool is_gcc   = false;
    inline constexpr bool is_clang = true;
    inline constexpr auto compiler_name    = "Clang";
    inline constexpr int  compiler_version =
        __clang_major__ * 10000 + __clang_minor__ * 100 + __clang_patchlevel__;
#else
    static_assert(false, "Unsupported compiler");
#endif

/* ============================================================
 * Architecture detection
 * ============================================================ */

#if defined(__x86_64__) || defined(_M_X64)
    inline constexpr bool is_x86_64  = true;
    inline constexpr bool is_aarch64 = false;
    inline constexpr bool is_riscv   = false;
    inline constexpr auto arch_name = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    inline constexpr bool is_x86_64  = false;
    inline constexpr bool is_aarch64 = true;
    inline constexpr bool is_riscv   = false;
    inline constexpr auto arch_name = "aarch64";
#elif defined(__riscv)
    inline constexpr bool is_x86_64  = false;
    inline constexpr bool is_aarch64 = false;
    inline constexpr bool is_riscv   = true;
    inline constexpr auto arch_name = "riscv";
#else
    static_assert(false, "Unsupported architecture");
#endif

/* ============================================================
 * Platform detection
 * ============================================================ */

#if defined(_WIN32) || defined(_WIN64)
    inline constexpr bool is_windows = true;
    inline constexpr bool is_linux   = false;
    inline constexpr bool is_macos   = false;
    inline constexpr auto platform_name = "Windows";
#elif defined(__linux__)
    inline constexpr bool is_windows = false;
    inline constexpr bool is_linux   = true;
    inline constexpr bool is_macos   = false;
    inline constexpr auto platform_name = "Linux";
#elif defined(__APPLE__)
    inline constexpr bool is_windows = false;
    inline constexpr bool is_linux   = false;
    inline constexpr bool is_macos   = true;
    inline constexpr auto platform_name = "macOS";
#else
    inline constexpr bool is_windows = false;
    inline constexpr bool is_linux   = false;
    inline constexpr bool is_macos   = false;
    inline constexpr auto platform_name = "Unknown";
#endif

/* ============================================================
 * Build type
 * ============================================================ */

#ifdef NDEBUG
    inline constexpr auto build_type = "Release";
    inline constexpr bool is_debug   = false;
#else
    inline constexpr auto build_type = "Debug";
    inline constexpr bool is_debug   = true;
#endif

/* ============================================================
 * Kernel version
 * ============================================================ */

inline constexpr u32 version_major = 0;
inline constexpr u32 version_minor = 1;
inline constexpr u32 version_patch = 0;
inline constexpr auto version_string = "0.1.0";

inline constexpr auto kernel_name = "vkernel";

/* ============================================================
 * Debug level
 *  0 - No debug
 *  1 - Errors only
 *  2 - Warnings + errors
 *  3 - Info + warnings + errors
 *  4 - Debug + info + warnings + errors
 *  5 - Verbose + debug + info + warnings + errors
 * ============================================================ */

#ifdef DEBUG
    inline constexpr u32 debug_level = 5;
    #define VK_DEBUG_LEVEL 5
#else
    inline constexpr u32 debug_level = 0;
    #define VK_DEBUG_LEVEL 0
#endif

/* ============================================================
 * Feature flags
 * ============================================================ */

inline constexpr bool feature_efi_console      = true;
inline constexpr bool feature_efi_framebuffer  = true;
inline constexpr bool feature_pci              = true;
inline constexpr bool feature_scheduler        = true;
inline constexpr bool feature_ipc              = true;
inline constexpr bool feature_virtual_memory   = true;

/* ============================================================
 * Maximum limits
 * ============================================================ */

inline constexpr u32 max_cpus              = 64;
inline constexpr u32 max_memory_map_entries = 256;
inline constexpr u32 max_tasks             = 256;
inline constexpr u32 max_ipc_endpoints     = 1024;
inline constexpr u32 max_devices           = 128;

/* ============================================================
 * Kernel address space layout (x86_64 higher-half)
 * ============================================================ */

#if defined(__x86_64__) || defined(_M_X64)
    inline constexpr u64 kernel_base        = 0xFFFF800000000000ULL;
    inline constexpr u64 kernel_heap_base   = 0xFFFF800000000000ULL;
    inline constexpr u64 kernel_heap_size   = 0x0000800000000000ULL;  /* 128 TB */
    inline constexpr u64 user_space_limit   = 0x0000800000000000ULL;
#endif

VK_STATIC_ASSERT(max_cpus <= 256);

} // namespace config
} // namespace vk

#endif /* VKERNEL_CONFIG_H */
