/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * sound.h - Sound subsystem interface
 *
 * Defines the sound driver interface used by both kernel-side drivers
 * and the userspace ABI.  A driver registers a sound_driver_t with
 * the driver framework; the kernel API forwards calls to the active
 * sound driver.
 */

#ifndef VKERNEL_SOUND_H
#define VKERNEL_SOUND_H

#include "types.h"

namespace vk {

/* ============================================================
 * Sound sample format passed from userspace
 * ============================================================ */

enum class sound_format : u32 {
    unsigned_8  = 0,   /* 8-bit unsigned PCM  */
    signed_16   = 1,   /* 16-bit signed PCM   */
};

/* ============================================================
 * sound_driver_t — vtable for a sound hardware driver
 *
 * A driver module fills this in and hands it to driver::register_sound_driver().
 * The kernel's sound API stubs delegate to whichever driver is active.
 * ============================================================ */

struct sound_driver_t {
    const char* name;           /* e.g. "sb16" */

    /* Lifecycle */
    bool  (*init)();            /* Probe + initialise hardware.  Return true on success. */
    void  (*shutdown)();        /* Release hardware resources. */

    /* Playback */
    bool  (*set_sample_rate)(u32 rate_hz);
    bool  (*play)(const u8* samples, u32 length, sound_format fmt);
    void  (*stop)();

    /* Status */
    bool  (*is_playing)();

    /* Volume: 0..255 */
    void  (*set_volume)(u8 left, u8 right);
};

/* ============================================================
 * Sound subsystem management
 * ============================================================ */

/* Maximum number of simultaneous software-mixer channels */
constexpr u32 MIX_CHANNELS = 8;

namespace sound {

/* Register a sound driver.  Only one active driver at a time. */
void register_driver(const sound_driver_t* drv);

/* Return the currently active sound driver (may be null). */
auto active_driver() -> const sound_driver_t*;

/* Convenience wrappers that go through the active driver (no-op if none). */
bool init_active();
void shutdown_active();
bool set_sample_rate(u32 rate_hz);
bool play(const u8* samples, u32 length, sound_format fmt);
void stop();
bool is_playing();
void set_volume(u8 left, u8 right);

/* ---- Software mixer ----
 *
 * Maintains up to MIX_CHANNELS independent audio channels.  Each call to
 * mix_play() mixes all active channels together and submits the result to
 * the hardware driver as a single stereo 16-bit PCM buffer at
 * MIX_OUTPUT_RATE Hz.  mix_update() should be called periodically (e.g.
 * once per game tick) so that long sounds are re-submitted after the
 * hardware has finished the previous window.
 */
bool mix_play(int ch, const u8* data, u32 src_samples, sound_format fmt,
              u32 sample_rate, u8 vol_left, u8 vol_right);
void mix_stop(int ch);
bool mix_is_playing(int ch);
void mix_update();
void mix_shutdown();

} // namespace sound
} // namespace vk

#endif /* VKERNEL_SOUND_H */
