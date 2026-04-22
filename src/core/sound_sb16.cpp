/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * sound_sb16.cpp - Sound Blaster 16 ISA sound driver
 *
 * Drives the SB16 via direct ISA DMA (channel 1 for 8-bit,
 * channel 5 for 16-bit) and DSP I/O ports at base 0x220.
 *
 * QEMU's -device sb16 emulates this hardware faithfully.
 */

#include "config.h"
#include "types.h"
#include "console.h"
#include "memory.h"
#include "sound.h"
#include "driver.h"
#include "scheduler.h"
#include "arch/x86_64/arch.h"

namespace vk {
namespace {

/* ============================================================
 * SB16 I/O port layout (base = 0x220)
 * ============================================================ */

constexpr u16 SB_BASE       = 0x220;
constexpr u16 SB_MIXER_ADDR = SB_BASE + 0x04;
constexpr u16 SB_MIXER_DATA = SB_BASE + 0x05;
constexpr u16 SB_DSP_RESET  = SB_BASE + 0x06;
constexpr u16 SB_DSP_READ   = SB_BASE + 0x0A;
constexpr u16 SB_DSP_WRITE  = SB_BASE + 0x0C;
constexpr u16 SB_DSP_STATUS = SB_BASE + 0x0E;
constexpr u16 SB_DSP_ACK16  = SB_BASE + 0x0F;

/* DSP commands */
constexpr u8 DSP_CMD_SET_RATE      = 0x41;
constexpr u8 DSP_CMD_PLAY_8BIT     = 0xC0;   /* 8-bit single-cycle DMA */
constexpr u8 DSP_CMD_PLAY_16BIT    = 0xB0;   /* 16-bit single-cycle DMA */
constexpr u8 DSP_CMD_STOP_8BIT     = 0xD0;
constexpr u8 DSP_CMD_STOP_16BIT    = 0xD5;
constexpr u8 DSP_CMD_RESUME_8BIT   = 0xD4;
constexpr u8 DSP_CMD_SPEAKER_ON    = 0xD1;
constexpr u8 DSP_CMD_SPEAKER_OFF   = 0xD3;
constexpr u8 DSP_CMD_GET_VERSION   = 0xE1;

/* DMA mode bits for the SB16 transfer command */
constexpr u8 DSP_MODE_UNSIGNED = 0x00;
constexpr u8 DSP_MODE_SIGNED   = 0x10;
constexpr u8 DSP_MODE_MONO     = 0x00;
constexpr u8 DSP_MODE_STEREO   = 0x20;

/* ISA DMA controller ports */
/* 8-bit DMA channel 1 */
constexpr u16 DMA1_MASK_REG    = 0x0A;
constexpr u16 DMA1_MODE_REG    = 0x0B;
constexpr u16 DMA1_CLEAR_FF   = 0x0C;
constexpr u16 DMA1_ADDR_CH1   = 0x02;
constexpr u16 DMA1_COUNT_CH1  = 0x03;
constexpr u16 DMA1_PAGE_CH1   = 0x83;

/* 16-bit DMA channel 5 */
/* Master PIC (8259A) ports — used to poll IRQ5 (SB16) without a handler */
constexpr u16 PIC1_CMD           = 0x20;
constexpr u8  PIC_OCW3_READ_IRR  = 0x0A;  /* select IRR for next read from PIC1_CMD */

constexpr u16 DMA5_MASK_REG    = 0xD4;
constexpr u16 DMA5_MODE_REG    = 0xD6;
constexpr u16 DMA5_CLEAR_FF   = 0xD8;
constexpr u16 DMA5_ADDR_CH5   = 0xC4;
constexpr u16 DMA5_COUNT_CH5  = 0xC6;
constexpr u16 DMA5_PAGE_CH5   = 0x8B;

/* ============================================================
 * DMA buffer — must reside in the lower 16 MB of physical
 * memory and not cross a 64 KB boundary (ISA DMA constraint).
 * We allocate a 64 KB aligned buffer at init time.
 * ============================================================ */

constexpr u32 DMA_BUFFER_SIZE = 65536;
static u8*    s_dma_buffer    = null;
static u32    s_dma_phys_addr = 0;
static u32    s_sample_rate   = 11025;
static u32    s_programmed_rate = 0;
static bool   s_playing       = false;
static bool   s_is_16bit      = false;
static u64    s_play_end_tick = 0;  /* tick at which the current transfer finishes */

/* ============================================================
 * DSP helpers
 * ============================================================ */

static bool dsp_write(u8 value) {
    /* Keep this bounded to avoid stalling the whole frame when the DSP
     * is temporarily busy.  Caller can retry on a later tick. */
    for (int i = 0; i < 64; ++i) {
        if ((arch::inb(SB_DSP_WRITE) & 0x80) == 0) {
            arch::outb(SB_DSP_WRITE, value);
            return true;
        }
    }
    return false;
}

static u8 dsp_read() {
    for (int i = 0; i < 1000; ++i) {
        if (arch::inb(SB_DSP_STATUS) & 0x80) {
            return arch::inb(SB_DSP_READ);
        }
    }
    return 0;
}

static bool dsp_reset() {
    arch::outb(SB_DSP_RESET, 1);
    /* Wait ~3 μs — on modern hardware a few port reads suffice */
    for (int i = 0; i < 100; ++i) {
        (void)arch::inb(SB_DSP_STATUS);
    }
    arch::outb(SB_DSP_RESET, 0);

    /* Wait for DSP to signal ready (0xAA) */
    for (int i = 0; i < 1000; ++i) {
        if (arch::inb(SB_DSP_STATUS) & 0x80) {
            if (arch::inb(SB_DSP_READ) == 0xAA) {
                return true;
            }
        }
    }
    return false;
}

/* ============================================================
 * DMA programming
 * ============================================================ */

static void setup_dma_8bit(u32 phys_addr, u32 length) {
    u32 count = length - 1;

    /* Mask DMA channel 1 */
    arch::outb(DMA1_MASK_REG, 0x04 | 1);  /* bit 2 = mask, channel 1 */

    /* Clear flip-flop */
    arch::outb(DMA1_CLEAR_FF, 0);

    /* Set DMA mode: single transfer, read (from memory), channel 1 */
    arch::outb(DMA1_MODE_REG, 0x48 | 1);  /* 0x48 = single, read, auto-init off */

    /* Address low/high bytes */
    arch::outb(DMA1_ADDR_CH1, static_cast<u8>(phys_addr & 0xFF));
    arch::outb(DMA1_ADDR_CH1, static_cast<u8>((phys_addr >> 8) & 0xFF));

    /* Page register (bits 16-23) */
    arch::outb(DMA1_PAGE_CH1, static_cast<u8>((phys_addr >> 16) & 0xFF));

    /* Count low/high bytes */
    arch::outb(DMA1_COUNT_CH1, static_cast<u8>(count & 0xFF));
    arch::outb(DMA1_COUNT_CH1, static_cast<u8>((count >> 8) & 0xFF));

    /* Unmask DMA channel 1 */
    arch::outb(DMA1_MASK_REG, 1);  /* channel 1, unmask */
}

static void setup_dma_16bit(u32 phys_addr, u32 length_bytes) {
    /* 16-bit DMA counts in 16-bit words; address is word-aligned */
    u32 word_addr  = phys_addr / 2;
    u32 word_count = (length_bytes / 2) - 1;

    /* Mask DMA channel 5 */
    arch::outb(DMA5_MASK_REG, 0x04 | 1);  /* bit 2 = mask, channel 5 = bits 0-1 = 01 */

    /* Clear flip-flop */
    arch::outb(DMA5_CLEAR_FF, 0);

    /* Mode: single transfer, read, channel 5 (bits 0-1 = 01) */
    arch::outb(DMA5_MODE_REG, 0x48 | 1);

    /* Address (word offset within 128 KB page) */
    arch::outb(DMA5_ADDR_CH5, static_cast<u8>(word_addr & 0xFF));
    arch::outb(DMA5_ADDR_CH5, static_cast<u8>((word_addr >> 8) & 0xFF));

    /* Page register (bits 16-23 of byte address) */
    arch::outb(DMA5_PAGE_CH5, static_cast<u8>((phys_addr >> 16) & 0xFF));

    /* Count (in words) */
    arch::outb(DMA5_COUNT_CH5, static_cast<u8>(word_count & 0xFF));
    arch::outb(DMA5_COUNT_CH5, static_cast<u8>((word_count >> 8) & 0xFF));

    /* Unmask channel 5 */
    arch::outb(DMA5_MASK_REG, 1);
}

/* ============================================================
 * Mixer helpers
 * ============================================================ */

static void mixer_write(u8 reg, u8 value) {
    arch::outb(SB_MIXER_ADDR, reg);
    arch::outb(SB_MIXER_DATA, value);
}

/* ============================================================
 * Driver interface implementation
 * ============================================================ */

static bool sb16_init() {
    if (!dsp_reset()) {
        console::puts("sb16: DSP reset failed — hardware not found\n");
        return false;
    }

    /* Get version */
    if (!dsp_write(DSP_CMD_GET_VERSION)) {
        console::puts("sb16: failed to query DSP version\n");
        return false;
    }
    u8 major = dsp_read();
    u8 minor = dsp_read();
    console::puts("sb16: DSP version ");
    console::put_dec(major);
    console::puts(".");
    console::put_dec(minor);
    console::puts("\n");

    if (major < 4) {
        console::puts("sb16: DSP version < 4 — not a true SB16\n");
        /* Still usable for 8-bit, proceed anyway */
    }

    /* Allocate the ISA DMA buffer: must be 64 KB-aligned and entirely
     * below the 16 MB ISA DMA ceiling.  Request exactly 16 pages (64 KB)
     * with 0x10000 alignment and a 16 MB upper bound. */
    constexpr u32 ISA_DMA_CEILING = 0x1000000u; /* 16 MB */
    auto dma_phys = g_phys_alloc.allocate_pages(
        DMA_BUFFER_SIZE / PAGE_SIZE_4K,   /* 16 pages = 64 KB */
        0x10000u,                          /* 64 KB alignment  */
        ISA_DMA_CEILING                    /* must be < 16 MB  */
    );
    if (dma_phys == 0) {
        console::puts("sb16: failed to allocate DMA buffer below 16 MB\n");
        return false;
    }
    s_dma_phys_addr = static_cast<u32>(dma_phys);
    s_dma_buffer    = reinterpret_cast<u8*>(dma_phys); /* identity-mapped */

    console::puts("sb16: DMA buffer at physical 0x");
    console::put_hex(s_dma_phys_addr);
    console::puts(" (");
    console::put_dec(DMA_BUFFER_SIZE);
    console::puts(" bytes)\n");

    /* Turn speaker on */
    (void)dsp_write(DSP_CMD_SPEAKER_ON);

    /* Set default master volume (max) */
    mixer_write(0x22, 0xFF);  /* Master volume L+R */
    mixer_write(0x04, 0xFF);  /* Voice volume (DAC) */

    s_sample_rate = 11025;
    s_programmed_rate = 0;
    s_playing = false;

    console::puts("sb16: initialised\n");
    return true;
}

static void sb16_shutdown() {
    (void)dsp_write(DSP_CMD_STOP_8BIT);
    (void)dsp_write(DSP_CMD_STOP_16BIT);
    (void)dsp_write(DSP_CMD_SPEAKER_OFF);
    s_playing = false;
    console::puts("sb16: shutdown\n");
}

static bool sb16_set_sample_rate(u32 rate_hz) {
    if (rate_hz < 5000 || rate_hz > 44100) return false;
    s_sample_rate = rate_hz;
    return true;
}

static bool sb16_play(const u8* samples, u32 length, sound_format fmt) {
    if (!s_dma_buffer || length == 0) return false;

    /* Clamp to DMA buffer size */
    u32 transfer = length;
    if (transfer > DMA_BUFFER_SIZE) {
        transfer = DMA_BUFFER_SIZE;
    }

    /* Acknowledge any pending SB16 interrupt from a previous transfer.
     * Without this the DSP can refuse new commands on some emulators. */
    (void)arch::inb(SB_DSP_STATUS);   /* ack 8-bit IRQ  */
    (void)arch::inb(SB_DSP_ACK16);    /* ack 16-bit IRQ */

    /* Copy samples into the DMA buffer */
    memory::memory_copy(s_dma_buffer, samples, transfer);

    /* Program sample rate only when changed to reduce command overhead. */
    if (s_programmed_rate != s_sample_rate) {
        if (!dsp_write(DSP_CMD_SET_RATE)
            || !dsp_write(static_cast<u8>((s_sample_rate >> 8) & 0xFF))
            || !dsp_write(static_cast<u8>(s_sample_rate & 0xFF))) {
            return false;
        }
        s_programmed_rate = s_sample_rate;
    }

    if (fmt == sound_format::signed_16) {
        /* 16-bit stereo playback via DMA channel 5.
         * transfer bytes / 2 bytes-per-word = word count for the DSP.
         * The stereo frame rate equals sample_rate, so duration =
         * (transfer / 4) frames / sample_rate. */
        s_is_16bit = true;
        setup_dma_16bit(s_dma_phys_addr, transfer);

        u32 word_count   = (transfer / 2) - 1;
        u32 frame_count  = transfer / 4;  /* stereo: 2 × 2 bytes per frame */
        u64 dur_ticks    = ((u64)frame_count * 100u + s_sample_rate - 1u)
                           / s_sample_rate;  /* ceil, SCHED_HZ=100 */
        s_play_end_tick  = sched::tick_count() + (dur_ticks < 1u ? 1u : dur_ticks);

        /* DSP command: 16-bit single-cycle stereo output */
        if (!dsp_write(DSP_CMD_PLAY_16BIT | 0x00)
            || !dsp_write(DSP_MODE_SIGNED | DSP_MODE_STEREO)
            || !dsp_write(static_cast<u8>(word_count & 0xFF))
            || !dsp_write(static_cast<u8>((word_count >> 8) & 0xFF))) {
            return false;
        }
    } else {
        /* 8-bit mono single-cycle playback via DMA channel 1 */
        s_is_16bit = false;
        setup_dma_8bit(s_dma_phys_addr, transfer);

        u32 sample_count = transfer - 1;
        u64 dur_ticks    = ((u64)transfer * 100u + s_sample_rate - 1u)
                           / s_sample_rate;
        s_play_end_tick  = sched::tick_count() + (dur_ticks < 1u ? 1u : dur_ticks);

        /* DSP command: 8-bit single-cycle output */
        if (!dsp_write(DSP_CMD_PLAY_8BIT | 0x00)
            || !dsp_write(DSP_MODE_UNSIGNED | DSP_MODE_MONO)
            || !dsp_write(static_cast<u8>(sample_count & 0xFF))
            || !dsp_write(static_cast<u8>((sample_count >> 8) & 0xFF))) {
            return false;
        }
    }

    s_playing = true;
    return true;
}

static void sb16_stop() {
    if (s_is_16bit) {
        (void)dsp_write(DSP_CMD_STOP_16BIT);
    } else {
        (void)dsp_write(DSP_CMD_STOP_8BIT);
    }
    /* Ack any pending IRQ so the DSP is clean for next play */
    (void)arch::inb(SB_DSP_STATUS);
    (void)arch::inb(SB_DSP_ACK16);
    s_playing      = false;
    s_play_end_tick = 0;
}

static bool sb16_is_playing() {
    if (!s_playing) return false;

    /* Primary: poll PIC1 IRR for IRQ5 (SB16 DMA-complete interrupt).
     * When the DMA transfer ends the SB16 raises its INT line and the
     * PIC1 IRR bit 5 is set — even if IRQ5 is masked (no handler
     * installed) the request is still recorded in the IRR.  Reading
     * the SB16 acknowledge port de-asserts INT, which clears IRR bit 5.
     * This gives sub-microsecond completion detection with no tick-
     * granularity gap, which is the root cause of buffer-underrun crackle.
     * QEMU raises IRQ5 on DMA completion exactly as real hardware does. */
    arch::outb(PIC1_CMD, PIC_OCW3_READ_IRR);
    u8 irr = arch::inb(PIC1_CMD);
    if (irr & (1u << 5)) {           /* IRQ5 = SB16 */
        /* De-assert SB16 INT so the PIC clears IRR bit 5 */
        if (s_is_16bit) {
            (void)arch::inb(SB_DSP_ACK16);
        } else {
            (void)arch::inb(SB_DSP_STATUS);
        }
        s_playing = false;
        return false;
    }

    /* Fallback: tick-based deadline — should rarely trigger now */
    if (sched::tick_count() >= s_play_end_tick) {
        (void)arch::inb(SB_DSP_STATUS);
        (void)arch::inb(SB_DSP_ACK16);
        s_playing = false;
    }
    return s_playing;
}

static void sb16_set_volume(u8 left, u8 right) {
    /* SB16 mixer register 0x22: master volume
     * High nibble = left, Low nibble = right
     * Scale 0-255 → 0-15 */
    u8 l = static_cast<u8>(left >> 4);
    u8 r = static_cast<u8>(right >> 4);
    mixer_write(0x22, static_cast<u8>((l << 4) | r));

    /* Also set DAC/voice volume (register 0x04) */
    mixer_write(0x04, static_cast<u8>((l << 4) | r));
}

/* ============================================================
 * Driver descriptor
 * ============================================================ */

static const sound_driver_t sb16_sound_driver = {
    .name           = "sb16",
    .init           = sb16_init,
    .shutdown       = sb16_shutdown,
    .set_sample_rate = sb16_set_sample_rate,
    .play           = sb16_play,
    .stop           = sb16_stop,
    .is_playing     = sb16_is_playing,
    .set_volume     = sb16_set_volume,
};

static const driver_descriptor sb16_descriptor = {
    .name  = "sb16",
    .type  = driver_type::sound,
    .sound = &sb16_sound_driver,
};

} // anonymous namespace

/* ============================================================
 * Auto-registration — called from driver::init() or manually
 * ============================================================ */

namespace sb16_driver {

void register_builtin() {
    driver::register_driver(&sb16_descriptor);
}

} // namespace sb16_driver
} // namespace vk
