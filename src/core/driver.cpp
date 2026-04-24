/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * driver.cpp - Loadable driver framework implementation
 */

#include "config.h"
#include "types.h"
#include "console.h"
#include "memory.h"
#include "driver.h"
#include "sound.h"

namespace vk {
namespace driver {

/* ============================================================
 * Internal state
 * ============================================================ */

struct driver_slot {
    const driver_descriptor* desc   = null;
    bool                     loaded = false;
};

static driver_slot s_drivers[MAX_DRIVERS];
static usize       s_driver_count = 0;
static bool        s_initialised  = false;

/* ============================================================
 * Public API
 * ============================================================ */

void init() {
    if (s_initialised) return;
    for (usize i = 0; i < MAX_DRIVERS; ++i) {
        s_drivers[i] = {};
    }
    s_driver_count = 0;
    s_initialised = true;
}

/* helper: compare driver names, ignoring a trailing ".vko" */
static bool name_match(const char* query, const char* driver_name) {
    /* Try exact match first */
    const char* a = query;
    const char* b = driver_name;
    while (*a && *b && *a == *b) { ++a; ++b; }
    if (*a == '\0' && *b == '\0') return true;

    /* Try stripping ".vko" from query */
    a = query;
    b = driver_name;
    while (*a && *b && *a == *b) { ++a; ++b; }
    if (*b == '\0') {
        /* remaining of a must be ".vko" */
        if (a[0] == '.' && a[1] == 'v' && a[2] == 'k' && a[3] == 'o' && a[4] == '\0')
            return true;
    }

    return false;
}

void register_driver(const driver_descriptor* desc) {
    if (!s_initialised) init();
    if (s_driver_count >= MAX_DRIVERS) {
        log::warn("driver: registry full, cannot register %s", desc->name);
        return;
    }
    s_drivers[s_driver_count].desc = desc;
    s_drivers[s_driver_count].loaded = false;
    ++s_driver_count;
}

auto find(const char* name) -> const driver_descriptor* {
    for (usize i = 0; i < s_driver_count; ++i) {
        if (s_drivers[i].desc && name_match(name, s_drivers[i].desc->name)) {
            return s_drivers[i].desc;
        }
    }
    return null;
}

auto load(const char* name) -> i32 {
    for (usize i = 0; i < s_driver_count; ++i) {
        if (s_drivers[i].desc && name_match(name, s_drivers[i].desc->name)) {
            if (s_drivers[i].loaded) {
                log::info("driver: %s already loaded", s_drivers[i].desc->name);
                return 0;
            }

            log::info("driver: loading %s...", s_drivers[i].desc->name);

            /* Activate based on type */
            switch (s_drivers[i].desc->type) {
                case driver_type::sound:
                    if (s_drivers[i].desc->sound) {
                        sound::register_driver(s_drivers[i].desc->sound);
                        if (!sound::init_active()) {
                            log::error("driver: sound init failed for %s", s_drivers[i].desc->name);
                            return -1;
                        }
                    }
                    break;
                default:
                    log::error("driver: unknown type for %s", s_drivers[i].desc->name);
                    return -1;
            }

            s_drivers[i].loaded = true;
            log::info("driver: %s loaded successfully", s_drivers[i].desc->name);
            return 0;
        }
    }

    log::warn("driver: not found: %s", name);
    return -1;
}

auto unload(const char* name) -> i32 {
    for (usize i = 0; i < s_driver_count; ++i) {
        if (s_drivers[i].desc && name_match(name, s_drivers[i].desc->name)) {
            if (!s_drivers[i].loaded) {
                log::warn("driver: %s not loaded", s_drivers[i].desc->name);
                return -1;
            }

            switch (s_drivers[i].desc->type) {
                case driver_type::sound:
                    sound::shutdown_active();
                    break;
                default:
                    break;
            }

            s_drivers[i].loaded = false;
            log::info("driver: %s unloaded", s_drivers[i].desc->name);
            return 0;
        }
    }

    log::warn("driver: not found: %s", name);
    return -1;
}

void list_loaded() {
    bool any = false;
    for (usize i = 0; i < s_driver_count; ++i) {
        if (s_drivers[i].loaded && s_drivers[i].desc) {
            log::info("driver: found loaded driver %s", s_drivers[i].desc->name);
            switch (s_drivers[i].desc->type) {
                case driver_type::sound: log::info(" (sound)"); break;
                default: log::info(" (unknown)"); break;
            }
            any = true;
        }
    }
    if (!any) {
        log::info("  (no drivers loaded)");
    }
}

void list_available() {
    if (s_driver_count == 0) {
        log::info("  (no drivers registered)");
        return;
    }
    for (usize i = 0; i < s_driver_count; ++i) {
        if (s_drivers[i].desc) {
            log::info("driver: found available driver %s", s_drivers[i].desc->name);
            switch (s_drivers[i].desc->type) {
                case driver_type::sound: log::info(" (sound)"); break;
                default: log::info(" (unknown)"); break;
            }
        }
    }
}

} // namespace driver
} // namespace vk
