/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * sound.cpp - Sound subsystem management
 */

#include "config.h"
#include "types.h"
#include "console.h"
#include "memory.h"
#include "scheduler.h"
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

/* ============================================================
 * Software mixer
 *
 * Output is always MIX_OUTPUT_RATE Hz, stereo, signed 16-bit.
 * Up to MIX_CHANNELS concurrent sources are mixed in software
 * before being submitted to the hardware driver as a single
 * signed-16 PCM buffer.
 *
 * mix_do_submit() is the internal workhorse — it reads the
 * current playback position of every active channel (derived
 * from wall-clock ticks), mixes them into s_mix_acc, clamps to
 * i16, and hands the result to the active driver's play().
 * ============================================================ */

namespace {

constexpr u32 MIX_OUTPUT_RATE   = 48000;
/* Max frames per hardware submission — sized to fit the AC'97 DMA buffer
 * (64 KB / 4 bytes-per-stereo-i16-frame = 16384 frames ≈ 341 ms). */
constexpr u32 MIX_WINDOW_FRAMES = 16384;
constexpr u32 SCHED_HZ          = 100; /* PIT / LAPIC timer rate */

struct mix_ch_t {
    const u8*    data;
    u32          src_total;    /* total source samples */
    u32          sample_rate;
    sound_format fmt;
    u8           vol_left;     /* 0 = mute, 255 = full */
    u8           vol_right;
    bool         active;
    u64          start_tick;   /* sched::tick_count() when mix_play() was called */
};

static mix_ch_t s_mix_ch[MIX_CHANNELS];

/* Output buffer: stereo i16 = 2 samples/frame × 2 bytes = 4 bytes/frame */
static i16 s_mix_out[MIX_WINDOW_FRAMES * 2];
/* Accumulation buffer: i32 to avoid overflow while summing channels */
static i32 s_mix_acc[MIX_WINDOW_FRAMES * 2];

/* Returns the number of source samples consumed for channel ch at the
 * current tick. */
static u32 ch_src_pos_now(const mix_ch_t& ch) {
    u64 elapsed  = sched::tick_count() - ch.start_tick;
    u64 consumed = elapsed * static_cast<u64>(ch.sample_rate) / SCHED_HZ;
    return (consumed >= ch.src_total) ? ch.src_total : static_cast<u32>(consumed);
}

static void mix_do_submit() {
    if (!s_active || !s_initialised) return;

    /* Determine mix window length = max remaining frames across active channels */
    u32 out_frames = 0;
    for (u32 i = 0; i < MIX_CHANNELS; ++i) {
        auto& ch = s_mix_ch[i];
        if (!ch.active) continue;
        u32 src_pos = ch_src_pos_now(ch);
        if (src_pos >= ch.src_total) {
            ch.active = false;
            continue;
        }
        u32 remaining = ch.src_total - src_pos;
        u32 ch_frames = static_cast<u32>(
            (static_cast<u64>(remaining) * MIX_OUTPUT_RATE + ch.sample_rate - 1u)
            / ch.sample_rate);
        if (ch_frames > out_frames) out_frames = ch_frames;
    }

    if (out_frames == 0) return;
    if (out_frames > MIX_WINDOW_FRAMES) out_frames = MIX_WINDOW_FRAMES;

    /* Zero the accumulation buffer for this window */
    memory::memory_set(s_mix_acc, 0, out_frames * 2u * sizeof(i32));

    /* Mix each active channel into the accumulation buffer */
    for (u32 i = 0; i < MIX_CHANNELS; ++i) {
        const auto& ch = s_mix_ch[i];
        if (!ch.active) continue;
        u32 src_pos = ch_src_pos_now(ch);
        if (src_pos >= ch.src_total) continue;

        for (u32 f = 0; f < out_frames; ++f) {
            /* Nearest-neighbour resample: map output frame f to source index */
            u32 src_idx = src_pos + static_cast<u32>(
                static_cast<u64>(f) * ch.sample_rate / MIX_OUTPUT_RATE);
            if (src_idx >= ch.src_total) break;

            i32 sample;
            if (ch.fmt == sound_format::unsigned_8) {
                /* 8-bit unsigned mono → signed 16-bit range */
                sample = (static_cast<i32>(ch.data[src_idx]) - 128) << 8;
            } else {
                /* 16-bit signed mono */
                sample = static_cast<i32>(
                    reinterpret_cast<const i16*>(ch.data)[src_idx]);
            }

            /* Apply per-channel volume (0-255 scale) */
            s_mix_acc[f * 2    ] += (sample * static_cast<i32>(ch.vol_left))  >> 8;
            s_mix_acc[f * 2 + 1] += (sample * static_cast<i32>(ch.vol_right)) >> 8;
        }
    }

    /* Clamp accumulated i32 values to i16 range and fill output buffer */
    for (u32 s = 0; s < out_frames * 2u; ++s) {
        i32 v = s_mix_acc[s];
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        s_mix_out[s] = static_cast<i16>(v);
    }

    /* Submit the mixed stereo buffer to hardware at the fixed output rate */
    s_active->set_sample_rate(MIX_OUTPUT_RATE);
    s_active->play(reinterpret_cast<const u8*>(s_mix_out),
                   out_frames * 4u, sound_format::signed_16);
}

} // anonymous namespace

/* ---- Public mixer API ---- */

bool mix_play(int ch_idx, const u8* data, u32 src_samples, sound_format fmt,
              u32 sample_rate, u8 vol_left, u8 vol_right) {
    if (ch_idx < 0 || static_cast<u32>(ch_idx) >= MIX_CHANNELS) return false;
    if (!data || src_samples == 0 || sample_rate == 0) return false;
    auto& ch       = s_mix_ch[ch_idx];
    ch.data        = data;
    ch.src_total   = src_samples;
    ch.sample_rate = sample_rate;
    ch.fmt         = fmt;
    ch.vol_left    = vol_left;
    ch.vol_right   = vol_right;
    ch.active      = true;
    ch.start_tick  = sched::tick_count();
    mix_do_submit();
    return true;
}

void mix_stop(int ch_idx) {
    if (ch_idx < 0 || static_cast<u32>(ch_idx) >= MIX_CHANNELS) return;
    s_mix_ch[ch_idx].active = false;
    mix_do_submit();
}

bool mix_is_playing(int ch_idx) {
    if (ch_idx < 0 || static_cast<u32>(ch_idx) >= MIX_CHANNELS) return false;
    const auto& ch = s_mix_ch[ch_idx];
    if (!ch.active) return false;
    u64 elapsed  = sched::tick_count() - ch.start_tick;
    u64 consumed = elapsed * static_cast<u64>(ch.sample_rate) / SCHED_HZ;
    if (consumed >= ch.src_total) {
        s_mix_ch[ch_idx].active = false;
        return false;
    }
    return true;
}

void mix_update() {
    /* Re-submit if any channel still has data but the hardware has finished
     * the previous window.  Called periodically by the application. */
    bool any_active = false;
    for (u32 i = 0; i < MIX_CHANNELS; ++i) {
        if (mix_is_playing(static_cast<int>(i))) {
            any_active = true;
            break;
        }
    }
    if (!any_active) return;
    if (is_playing()) return; /* hardware window not yet exhausted */
    mix_do_submit();
}

void mix_shutdown() {
    for (u32 i = 0; i < MIX_CHANNELS; ++i) {
        s_mix_ch[i] = {};
    }
}

} // namespace sound
} // namespace vk
