/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * sound.cpp - Sound subsystem management
 */

#include "config.h"
#include "types.h"
#include "console.h"
#include "sound.h"

namespace vk {
namespace sound {

static const sound_driver_t* s_active = null;
static bool s_initialised = false;

void register_driver(const sound_driver_t* drv) {
    if (s_active && s_initialised) {
        /* Shutdown previous driver */
        if (s_active->shutdown) s_active->shutdown();
        s_initialised = false;
    }
    s_active = drv;
}

auto active_driver() -> const sound_driver_t* {
    return s_active;
}

bool init_active() {
    if (!s_active) return false;
    if (s_initialised) return true;
    if (s_active->init && s_active->init()) {
        s_initialised = true;
        return true;
    }
    return false;
}

void shutdown_active() {
    if (s_active && s_initialised && s_active->shutdown) {
        s_active->shutdown();
    }
    s_initialised = false;
    s_active = null;
}

bool set_sample_rate(u32 rate_hz) {
    if (!s_active || !s_initialised || !s_active->set_sample_rate) return false;
    return s_active->set_sample_rate(rate_hz);
}

bool play(const u8* samples, u32 length, sound_format fmt) {
    if (!s_active || !s_initialised || !s_active->play) return false;
    return s_active->play(samples, length, fmt);
}

void stop() {
    if (s_active && s_initialised && s_active->stop) {
        s_active->stop();
    }
}

bool is_playing() {
    if (!s_active || !s_initialised || !s_active->is_playing) return false;
    return s_active->is_playing();
}

void set_volume(u8 left, u8 right) {
    if (s_active && s_initialised && s_active->set_volume) {
        s_active->set_volume(left, right);
    }
}

} // namespace sound
} // namespace vk
