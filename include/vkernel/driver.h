/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * driver.h - Loadable driver framework
 *
 * Provides a simple registry for kernel drivers that can be loaded
 * at runtime via the shell 'drvload' command or programmatically.
 * Each driver has a name (e.g. "sb16") and a type (sound, …).
 *
 * The framework is intentionally minimal — no dynamic linking or
 * separate binaries.  Drivers are compiled into the kernel image
 * and activated on demand.
 */

#ifndef VKERNEL_DRIVER_H
#define VKERNEL_DRIVER_H

#include "types.h"
#include "sound.h"

namespace vk {

/* ============================================================
 * Driver types
 * ============================================================ */

enum class driver_type : u32 {
    none    = 0,
    sound   = 1,
    /* Future: network, block, display, … */
};

/* ============================================================
 * Driver descriptor — each built-in driver provides one
 * ============================================================ */

struct driver_descriptor {
    const char*            name;     /* e.g. "sb16" */
    driver_type            type;
    const sound_driver_t*  sound;    /* non-null for sound drivers */
    /* Future: const net_driver_t* net; etc. */
};

/* ============================================================
 * Driver registry
 * ============================================================ */

namespace driver {

inline constexpr usize MAX_DRIVERS = 16;

/* Initialise the driver framework (called once at boot). */
void init();

/* Register a built-in driver descriptor.
 * Called by each driver's init-time registration code. */
void register_driver(const driver_descriptor* desc);

/* Look up a driver by name (e.g. "sb16.vko" or just "sb16"). */
auto find(const char* name) -> const driver_descriptor*;

/* Load (activate) a driver by name.  Returns 0 on success, -1 on error. */
auto load(const char* name) -> i32;

/* Unload a driver by name.  Returns 0 on success, -1 on error. */
auto unload(const char* name) -> i32;

/* List loaded drivers to console. */
void list_loaded();

/* List all available (registered) drivers to console. */
void list_available();

} // namespace driver
} // namespace vk

#endif /* VKERNEL_DRIVER_H */
