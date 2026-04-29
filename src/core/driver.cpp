/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * driver.cpp - Loadable driver framework implementation
 */

#include "config.h"
#include "types.h"
#include "console.h"
#include "log.h"
#include "memory.h"
#include "driver.h"
#include "sound.h"
#include "block.h"

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
static auto normalize_driver_name(string_view name) -> string_view {
    if (name.size() >= 4) {
        string_view suffix(name.data() + name.size() - 4, 4);
        if (suffix.equals(".vko")) {
            return string_view(name.data(), name.size() - 4);
        }
    }
    return name;
}

static bool name_match(string_view query, string_view driver_name) {
    return normalize_driver_name(query).equals(driver_name);
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

auto find(string_view name) -> const driver_descriptor* {
    for (usize i = 0; i < s_driver_count; ++i) {
        if (s_drivers[i].desc && name_match(name, s_drivers[i].desc->name)) {
            return s_drivers[i].desc;
        }
    }
    return null;
}

auto find(const char* name) -> const driver_descriptor* {
    return find(string_view(name));
}

auto load(string_view name) -> i32 {
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
                case driver_type::block:
                    if (s_drivers[i].desc->block) {
                        block::init();
                        if (!s_drivers[i].desc->block->init()) {
                            log::error("driver: block init failed for %s", s_drivers[i].desc->name);
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

    static_string<64> name_buf(name);
    log::warn("driver: not found: %s", name_buf.c_str());
    return -1;
}

auto load(const char* name) -> i32 {
    return load(string_view(name));
}

auto unload(string_view name) -> i32 {
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
                case driver_type::block:
                    if (s_drivers[i].desc->block && s_drivers[i].desc->block->shutdown) {
                        s_drivers[i].desc->block->shutdown();
                    }
                    break;
                default:
                    break;
            }

            s_drivers[i].loaded = false;
            log::info("driver: %s unloaded", s_drivers[i].desc->name);
            return 0;
        }
    }

    static_string<64> name_buf(name);
    log::warn("driver: not found: %s", name_buf.c_str());
    return -1;
}

auto unload(const char* name) -> i32 {
    return unload(string_view(name));
}

void list_loaded() {
    bool any = false;
    for (usize i = 0; i < s_driver_count; ++i) {
        if (s_drivers[i].loaded && s_drivers[i].desc) {
            log::info("driver: found loaded driver %s", s_drivers[i].desc->name);
            switch (s_drivers[i].desc->type) {
                case driver_type::sound: log::info(" (sound)"); break;
                case driver_type::block: log::info(" (block)"); break;
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
                case driver_type::block: log::info(" (block)"); break;
                default: log::info(" (unknown)"); break;
            }
        }
    }
}

} // namespace driver
} // namespace vk
