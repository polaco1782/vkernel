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
        console::puts("driver: registry full, cannot register ");
        console::puts(desc->name);
        console::puts("\n");
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
                console::puts("driver: ");
                console::puts(s_drivers[i].desc->name);
                console::puts(" already loaded\n");
                return 0;
            }

            console::puts("driver: loading ");
            console::puts(s_drivers[i].desc->name);
            console::puts("...\n");

            /* Activate based on type */
            switch (s_drivers[i].desc->type) {
                case driver_type::sound:
                    if (s_drivers[i].desc->sound) {
                        sound::register_driver(s_drivers[i].desc->sound);
                        if (!sound::init_active()) {
                            console::puts("driver: sound init failed for ");
                            console::puts(s_drivers[i].desc->name);
                            console::puts("\n");
                            return -1;
                        }
                    }
                    break;
                default:
                    console::puts("driver: unknown type for ");
                    console::puts(s_drivers[i].desc->name);
                    console::puts("\n");
                    return -1;
            }

            s_drivers[i].loaded = true;
            console::puts("driver: ");
            console::puts(s_drivers[i].desc->name);
            console::puts(" loaded successfully\n");
            return 0;
        }
    }

    console::puts("driver: not found: ");
    console::puts(name);
    console::puts("\n");
    return -1;
}

auto unload(const char* name) -> i32 {
    for (usize i = 0; i < s_driver_count; ++i) {
        if (s_drivers[i].desc && name_match(name, s_drivers[i].desc->name)) {
            if (!s_drivers[i].loaded) {
                console::puts("driver: ");
                console::puts(s_drivers[i].desc->name);
                console::puts(" not loaded\n");
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
            console::puts("driver: ");
            console::puts(s_drivers[i].desc->name);
            console::puts(" unloaded\n");
            return 0;
        }
    }

    console::puts("driver: not found: ");
    console::puts(name);
    console::puts("\n");
    return -1;
}

void list_loaded() {
    bool any = false;
    for (usize i = 0; i < s_driver_count; ++i) {
        if (s_drivers[i].loaded && s_drivers[i].desc) {
            console::puts("  [loaded] ");
            console::puts(s_drivers[i].desc->name);
            switch (s_drivers[i].desc->type) {
                case driver_type::sound: console::puts(" (sound)"); break;
                default: console::puts(" (unknown)"); break;
            }
            console::puts("\n");
            any = true;
        }
    }
    if (!any) {
        console::puts("  (no drivers loaded)\n");
    }
}

void list_available() {
    if (s_driver_count == 0) {
        console::puts("  (no drivers registered)\n");
        return;
    }
    for (usize i = 0; i < s_driver_count; ++i) {
        if (s_drivers[i].desc) {
            console::puts("  ");
            if (s_drivers[i].loaded) {
                console::puts("[*] ");
            } else {
                console::puts("[ ] ");
            }
            console::puts(s_drivers[i].desc->name);
            console::puts(".vko");
            switch (s_drivers[i].desc->type) {
                case driver_type::sound: console::puts(" (sound)"); break;
                default: console::puts(" (unknown)"); break;
            }
            console::puts("\n");
        }
    }
}

} // namespace driver
} // namespace vk
