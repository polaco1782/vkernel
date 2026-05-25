/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * sound.cpp - Sound subsystem management
 */

#include "config.h"
#include "types.h"
#include "console.h"
#include "log.h"
#include "memory.h"
#include "scheduler.h"
#include "sound.h"
#include "spinlock.h"

namespace vk {
namespace sound {

namespace {

constexpr u32 MIX_OUTPUT_RATE   = 48000;
/* Max frames per hardware submission — sized to fit the AC'97 DMA buffer
 * (64 KB / 4 bytes-per-stereo-i16-frame = 16384 frames ≈ 341 ms). */
constexpr u32 MIX_WINDOW_FRAMES = 16384;
constexpr u32 MIX_TARGET_BUFFERED_FRAMES = MIX_OUTPUT_RATE / 4;
constexpr u64 SOUND_IDLE_SLEEP_TICKS = 1;
constexpr u32 MIX_QUEUE_CAPACITY = 4;

struct mix_segment_t {
    const u8*    data = null;
    const u8*    source_data = null;
    u32          src_total = 0;   /* total source samples or stereo frames */
    u32          src_pos = 0;     /* next source unit to mix */
    u32          sample_rate = 0;
    sound_format fmt = sound_format::unsigned_8;
    u8           vol_left = 0;    /* 0 = mute, 255 = full */
    u8           vol_right = 0;
};

struct mix_ch_t {
    mix_segment_t current {};
    mix_segment_t pending[MIX_QUEUE_CAPACITY] {};
    u8            pending_head = 0;
    u8            pending_tail = 0;
    u8            pending_count = 0;
    bool          active = false;
};

static const sound_driver_t* s_active = null;
static bool s_initialised = false;
static u8 s_volume_left = 255;
static u8 s_volume_right = 255;
static u32 s_requested_sample_rate = 0;
static bool s_background_worker_started = false;
static spinlock s_sound_lock;
static mix_ch_t s_mix_ch[MIX_CHANNELS];

/* Output buffer: stereo i16 = 2 samples/frame × 2 bytes = 4 bytes/frame */
static i16 s_mix_out[MIX_WINDOW_FRAMES * 2];
/* Accumulation buffer: i32 to avoid overflow while summing channels */
static i32 s_mix_acc[MIX_WINDOW_FRAMES * 2];

static auto bytes_per_unit(sound_format fmt) -> u32 {
    switch (fmt) {
        case sound_format::unsigned_8:
            return 1;
        case sound_format::signed_16:
            return 2;
        case sound_format::signed_16_stereo:
            return 4;
    }

    return 0;
}

static auto payload_bytes(sound_format fmt, u32 src_total) -> u32 {
    const u32 unit_bytes = bytes_per_unit(fmt);
    if (unit_bytes == 0 || src_total > (~0u / unit_bytes)) {
        return 0;
    }
    return src_total * unit_bytes;
}

static void reset_segment_locked(mix_segment_t& segment) {
    if (segment.data != null) {
        g_kernel_heap.free(const_cast<u8*>(segment.data));
    }
    segment = {};
}

static void reset_mix_channel_locked(mix_ch_t& ch) {
    reset_segment_locked(ch.current);
    for (u32 i = 0; i < MIX_QUEUE_CAPACITY; ++i) {
        reset_segment_locked(ch.pending[i]);
    }
    ch = {};
}

static void adopt_current_segment_locked(mix_ch_t& ch, mix_segment_t&& segment) {
    reset_segment_locked(ch.current);
    ch.current = segment;
    segment = {};
    ch.active = ch.current.data != null;
}

static bool enqueue_pending_segment_locked(mix_ch_t& ch, mix_segment_t&& segment) {
    if (ch.pending_count >= MIX_QUEUE_CAPACITY) {
        return false;
    }

    ch.pending[ch.pending_tail] = segment;
    segment = {};
    ch.pending_tail = static_cast<u8>((ch.pending_tail + 1u) % MIX_QUEUE_CAPACITY);
    ++ch.pending_count;
    return true;
}

static bool activate_next_segment_locked(mix_ch_t& ch) {
    if (ch.pending_count == 0) {
        ch.active = false;
        return false;
    }

    auto& next = ch.pending[ch.pending_head];
    ch.current = next;
    next = {};
    ch.pending_head = static_cast<u8>((ch.pending_head + 1u) % MIX_QUEUE_CAPACITY);
    --ch.pending_count;
    ch.active = true;
    return true;
}

static bool refresh_channel_locked(mix_ch_t& ch) {
    if (!ch.active) {
        return activate_next_segment_locked(ch);
    }

    while (ch.active) {
        if (ch.current.src_pos < ch.current.src_total) {
            return true;
        }

        reset_segment_locked(ch.current);
        if (!activate_next_segment_locked(ch)) {
            return false;
        }
    }

    return false;
}

static bool mix_is_playing_locked(int ch_idx) {
    if (ch_idx < 0 || static_cast<u32>(ch_idx) >= MIX_CHANNELS) return false;
    return refresh_channel_locked(s_mix_ch[ch_idx]);
}

static bool any_active_mix_channels_locked() {
    for (u32 i = 0; i < MIX_CHANNELS; ++i) {
        if (mix_is_playing_locked(static_cast<int>(i))) {
            return true;
        }
    }
    return false;
}

static bool driver_is_playing_locked() {
    return s_active != null
        && s_initialised
        && s_active->is_playing != null
        && s_active->is_playing();
}

static auto driver_buffered_frames_locked() -> u32 {
    if (s_active == null
            || !s_initialised
            || s_active->buffered_frames == null) {
        return 0;
    }
    return s_active->buffered_frames();
}

static void stop_driver_if_idle_locked() {
    if (s_active == null || !s_initialised || s_active->stop == null) {
        return;
    }
    if (any_active_mix_channels_locked()) {
        return;
    }
    if (driver_is_playing_locked()) {
        return;
    }
    s_active->stop();
}

static auto mix_do_submit_locked() -> bool {
    if (!s_active || !s_initialised) return false;

    /* Determine mix window length = max remaining frames across active channels */
    u32 out_frames = 0;
    for (u32 i = 0; i < MIX_CHANNELS; ++i) {
        auto& ch = s_mix_ch[i];
        if (!refresh_channel_locked(ch)) {
            continue;
        }
        const u32 remaining = ch.current.src_total - ch.current.src_pos;
        const u32 ch_frames = static_cast<u32>(
            (static_cast<u64>(remaining) * MIX_OUTPUT_RATE + ch.current.sample_rate - 1u)
            / ch.current.sample_rate);
        if (ch_frames > out_frames) out_frames = ch_frames;
    }

    if (out_frames == 0) {
        s_active->stop();
        return false;
    }
    if (out_frames > MIX_WINDOW_FRAMES) out_frames = MIX_WINDOW_FRAMES;

    memory::set(s_mix_acc, 0, out_frames * 2u * sizeof(i32));

    for (u32 i = 0; i < MIX_CHANNELS; ++i) {
        auto& ch = s_mix_ch[i];
        if (!refresh_channel_locked(ch)) continue;
        const u32 src_pos = ch.current.src_pos;
        const auto& segment = ch.current;

        for (u32 f = 0; f < out_frames; ++f) {
            const u32 src_idx = src_pos + static_cast<u32>(
                static_cast<u64>(f) * segment.sample_rate / MIX_OUTPUT_RATE);
            if (src_idx >= segment.src_total) break;

            i32 sample_l;
            i32 sample_r;
            if (segment.fmt == sound_format::unsigned_8) {
                sample_l = (static_cast<i32>(segment.data[src_idx]) - 128) << 8;
                sample_r = sample_l;
            } else if (segment.fmt == sound_format::signed_16) {
                sample_l = static_cast<i32>(
                    reinterpret_cast<const i16*>(segment.data)[src_idx]);
                sample_r = sample_l;
            } else {
                const auto* stereo = reinterpret_cast<const i16*>(segment.data);
                sample_l = static_cast<i32>(stereo[src_idx * 2u]);
                sample_r = static_cast<i32>(stereo[src_idx * 2u + 1u]);
            }

            s_mix_acc[f * 2    ] += (sample_l * static_cast<i32>(segment.vol_left))  >> 8;
            s_mix_acc[f * 2 + 1] += (sample_r * static_cast<i32>(segment.vol_right)) >> 8;
        }
    }

    for (u32 s = 0; s < out_frames * 2u; ++s) {
        i32 v = s_mix_acc[s];
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        s_mix_out[s] = static_cast<i16>(v);
    }

    s_requested_sample_rate = MIX_OUTPUT_RATE;
    s_active->set_sample_rate(MIX_OUTPUT_RATE);
    if (!s_active->play(reinterpret_cast<const u8*>(s_mix_out),
                        out_frames * 4u, sound_format::signed_16)) {
        return false;
    }

    for (u32 i = 0; i < MIX_CHANNELS; ++i) {
        auto& ch = s_mix_ch[i];
        if (!refresh_channel_locked(ch)) {
            continue;
        }

        const u32 remaining = ch.current.src_total - ch.current.src_pos;
        u32 advance = static_cast<u32>(
            static_cast<u64>(out_frames) * ch.current.sample_rate / MIX_OUTPUT_RATE);
        if (advance > remaining) {
            advance = remaining;
        }

        ch.current.src_pos += advance;
        (void)refresh_channel_locked(ch);
    }

    return true;
}

static void mix_update_locked() {
    if (!any_active_mix_channels_locked()) {
        return;
    }
    if (s_active != null
            && s_initialised
            && s_active->buffered_frames != null) {
        while (any_active_mix_channels_locked()) {
            const u32 buffered_before = driver_buffered_frames_locked();
            if (buffered_before >= MIX_TARGET_BUFFERED_FRAMES) {
                return;
            }
            if (!mix_do_submit_locked()) {
                return;
            }
            if (driver_buffered_frames_locked() <= buffered_before) {
                return;
            }
        }
        return;
    }

    if (driver_is_playing_locked()) return;
    (void)mix_do_submit_locked();
}

static void background_sound_task(void*) {
    while (true) {
        u64 sleep_ticks = SOUND_IDLE_SLEEP_TICKS;

        s_sound_lock.acquire();
        if (s_active != null && s_initialised) {
            if (any_active_mix_channels_locked()) {
                mix_update_locked();
            }

            if (driver_is_playing_locked() || any_active_mix_channels_locked()) {
                sleep_ticks = 1;
            }
        }
        s_sound_lock.release();

        sched::sleep(sleep_ticks);
    }
}

} // namespace

void register_driver(const sound_driver_t* drv) {
    s_sound_lock.acquire();
    if (s_active && s_initialised) {
        if (s_active->shutdown) s_active->shutdown();
        s_initialised = false;
    }
    s_active = drv;
    s_sound_lock.release();
}

auto active_driver() -> const sound_driver_t* {
    return s_active;
}

bool initialized() {
    return s_initialised;
}

bool init_active() {
    s_sound_lock.acquire();
    if (!s_active) {
        s_sound_lock.release();
        return false;
    }
    if (s_initialised) {
        s_sound_lock.release();
        return true;
    }
    if (s_active->init && s_active->init()) {
        s_initialised = true;
        s_sound_lock.release();
        return true;
    }
    s_sound_lock.release();
    return false;
}

void shutdown_active() {
    s_sound_lock.acquire();
    if (s_active && s_initialised && s_active->shutdown) {
        s_active->shutdown();
    }
    for (u32 i = 0; i < MIX_CHANNELS; ++i) {
        reset_mix_channel_locked(s_mix_ch[i]);
    }
    s_initialised = false;
    s_active = null;
    s_requested_sample_rate = 0;
    s_sound_lock.release();
}

bool start_background_worker() {
    s_sound_lock.acquire();
    const bool already_started = s_background_worker_started;
    s_sound_lock.release();
    if (already_started) {
        return true;
    }

    const i64 task_id = sched::create_task("sound", background_sound_task, null);
    if (task_id < 0) {
        return false;
    }

    s_sound_lock.acquire();
    s_background_worker_started = true;
    s_sound_lock.release();
    return true;
}

bool set_sample_rate(u32 rate_hz) {
    s_sound_lock.acquire();
    if (!s_active || !s_initialised || !s_active->set_sample_rate) {
        s_sound_lock.release();
        return false;
    }
    if (!s_active->set_sample_rate(rate_hz)) {
        s_sound_lock.release();
        return false;
    }
    s_requested_sample_rate = rate_hz;
    s_sound_lock.release();
    return true;
}

bool play(const u8* samples, u32 length, sound_format fmt) {
    s_sound_lock.acquire();
    if (!s_active || !s_initialised || !s_active->play) {
        s_sound_lock.release();
        return false;
    }
    const bool ok = s_active->play(samples, length, fmt);
    s_sound_lock.release();
    return ok;
}

void stop() {
    s_sound_lock.acquire();
    if (s_active && s_initialised && s_active->stop) {
        s_active->stop();
    }
    s_sound_lock.release();
}

bool is_playing() {
    s_sound_lock.acquire();
    const bool playing = driver_is_playing_locked();
    s_sound_lock.release();
    return playing;
}

void set_volume(u8 left, u8 right) {
    s_sound_lock.acquire();
    s_volume_left = left;
    s_volume_right = right;
    if (s_active && s_initialised && s_active->set_volume) {
        s_active->set_volume(left, right);
    }
    s_sound_lock.release();
}

auto sample_rate() -> u32 {
    return s_requested_sample_rate;
}

void volume(u8* out_left, u8* out_right) {
    if (out_left != null) {
        *out_left = s_volume_left;
    }
    if (out_right != null) {
        *out_right = s_volume_right;
    }
}

bool mix_play(int ch_idx, const u8* data, u32 src_samples, sound_format fmt,
              u32 sample_rate, u8 vol_left, u8 vol_right) {
    if (ch_idx < 0 || static_cast<u32>(ch_idx) >= MIX_CHANNELS) {
        return false;
    }
    if (!data || src_samples == 0 || sample_rate == 0) {
        return false;
    }

    const u32 total_bytes = payload_bytes(fmt, src_samples);
    if (total_bytes == 0) {
        return false;
    }

    auto* owned_copy = static_cast<u8*>(g_kernel_heap.allocate(total_bytes));
    if (owned_copy == null) {
        return false;
    }
    memory::copy(owned_copy, data, total_bytes);

    mix_segment_t segment {};
    segment.data = owned_copy;
    segment.source_data = data;
    segment.src_total = src_samples;
    segment.sample_rate = sample_rate;
    segment.fmt = fmt;
    segment.vol_left = vol_left;
    segment.vol_right = vol_right;

    s_sound_lock.acquire();
    auto& ch = s_mix_ch[ch_idx];
    reset_mix_channel_locked(ch);
    adopt_current_segment_locked(ch, static_cast<mix_segment_t&&>(segment));
    s_sound_lock.release();
    return true;
}

bool mix_queue_play(int ch_idx, const u8* data, u32 src_samples, sound_format fmt,
                    u32 sample_rate, u8 vol_left, u8 vol_right) {
    if (ch_idx < 0 || static_cast<u32>(ch_idx) >= MIX_CHANNELS) {
        return false;
    }
    if (!data || src_samples == 0 || sample_rate == 0) {
        return false;
    }

    const u32 total_bytes = payload_bytes(fmt, src_samples);
    if (total_bytes == 0) {
        return false;
    }

    auto* owned_copy = static_cast<u8*>(g_kernel_heap.allocate(total_bytes));
    if (owned_copy == null) {
        return false;
    }
    memory::copy(owned_copy, data, total_bytes);

    mix_segment_t segment {};
    segment.data = owned_copy;
    segment.source_data = data;
    segment.src_total = src_samples;
    segment.sample_rate = sample_rate;
    segment.fmt = fmt;
    segment.vol_left = vol_left;
    segment.vol_right = vol_right;

    s_sound_lock.acquire();
    auto& ch = s_mix_ch[ch_idx];
    if (!ch.active && ch.pending_count == 0) {
        adopt_current_segment_locked(ch, static_cast<mix_segment_t&&>(segment));
        s_sound_lock.release();
        return true;
    }
    if (!enqueue_pending_segment_locked(ch, static_cast<mix_segment_t&&>(segment))) {
        s_sound_lock.release();
        reset_segment_locked(segment);
        return false;
    }
    s_sound_lock.release();
    return true;
}

void mix_stop(int ch_idx) {
    s_sound_lock.acquire();
    if (ch_idx < 0 || static_cast<u32>(ch_idx) >= MIX_CHANNELS) {
        s_sound_lock.release();
        return;
    }
    reset_mix_channel_locked(s_mix_ch[ch_idx]);
    stop_driver_if_idle_locked();
    s_sound_lock.release();
}

bool mix_is_playing(int ch_idx) {
    s_sound_lock.acquire();
    const bool playing = mix_is_playing_locked(ch_idx);
    s_sound_lock.release();
    return playing;
}

auto active_mix_channels() -> u32 {
    s_sound_lock.acquire();
    u32 count = 0;
    for (u32 i = 0; i < MIX_CHANNELS; ++i) {
        if (mix_is_playing_locked(static_cast<int>(i))) {
            ++count;
        }
    }
    s_sound_lock.release();
    return count;
}

void mix_update() {
    s_sound_lock.acquire();
    mix_update_locked();
    s_sound_lock.release();
}

void mix_stop_range(const void* base, usize size) {
    s_sound_lock.acquire();
    if (base == null || size == 0) {
        s_sound_lock.release();
        return;
    }

    const usize start = reinterpret_cast<usize>(base);
    const usize end = start + size;
    if (end < start) {
        s_sound_lock.release();
        return;
    }

    for (u32 i = 0; i < MIX_CHANNELS; ++i) {
        auto& ch = s_mix_ch[i];
        bool should_reset = false;

        if (ch.current.source_data != null) {
            const usize ptr = reinterpret_cast<usize>(ch.current.source_data);
            should_reset = ptr >= start && ptr < end;
        }

        for (u32 pending = 0; !should_reset && pending < MIX_QUEUE_CAPACITY; ++pending) {
            if (ch.pending[pending].source_data == null) continue;
            const usize ptr = reinterpret_cast<usize>(ch.pending[pending].source_data);
            should_reset = ptr >= start && ptr < end;
        }

        if (should_reset) {
            reset_mix_channel_locked(ch);
        }
    }
    stop_driver_if_idle_locked();
    s_sound_lock.release();
}

void mix_shutdown() {
    s_sound_lock.acquire();
    for (u32 i = 0; i < MIX_CHANNELS; ++i) {
        reset_mix_channel_locked(s_mix_ch[i]);
    }
    s_sound_lock.release();
}

} // namespace sound
} // namespace vk
