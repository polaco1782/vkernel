/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * sound_ac97.cpp - Intel AC'97 (ICH) PCI sound driver
 */

#include "config.h"
#include "types.h"
#include "console.h"
#include "log.h"
#include "memory.h"
#include "sound.h"
#include "driver.h"
#include "pci.h"
#include "resource_ptr.h"
#include "scheduler.h"
#include "arch/x86_64/arch.h"

namespace vk {
namespace {

/* ============================================================
 * AC'97 Native Audio Mixer (NAM) registers — offsets from BAR0
 * ============================================================ */

constexpr u16 NAM_RESET          = 0x00;
constexpr u16 NAM_MASTER_VOL     = 0x02;   /* Master volume (L/R mute + atten) */
constexpr u16 NAM_AUX_OUT_VOL    = 0x04;
constexpr u16 NAM_MONO_VOL       = 0x06;
constexpr u16 NAM_PCM_OUT_VOL    = 0x18;   /* PCM out volume */
constexpr u16 NAM_EXT_AUDIO_ID   = 0x28;
constexpr u16 NAM_EXT_AUDIO_CTRL = 0x2A;
constexpr u16 NAM_PCM_FRONT_RATE = 0x2C;   /* PCM front DAC rate */

/* ============================================================
 * AC'97 Native Audio Bus Master (NABM) registers — offsets from BAR1
 * ============================================================ */

/* PCM Out channel (channel index 0x10) */
constexpr u16 NABM_PCM_OUT       = 0x10;
constexpr u16 PO_BDBAR           = 0x10;   /* Buffer Descriptor Base Address */
constexpr u16 PO_CIV             = 0x14;   /* Current Index Value */
constexpr u16 PO_LVI             = 0x15;   /* Last Valid Index */
constexpr u16 PO_SR              = 0x16;   /* Status Register */
constexpr u16 PO_PICB            = 0x18;   /* Position in Current Buffer (samples) */
constexpr u16 PO_PIV             = 0x1A;   /* Prefetched Index Value */
constexpr u16 PO_CR              = 0x1B;   /* Control Register */

/* Global Control register */
constexpr u16 NABM_GLOB_CNT      = 0x2C;
constexpr u16 NABM_GLOB_STA      = 0x30;

/* Control register bits */
constexpr u8 CR_RPBM  = 0x01;   /* Run/Pause Bus Master */
constexpr u8 CR_RR    = 0x02;   /* Reset Registers */
constexpr u8 CR_LVBIE = 0x04;   /* Last Valid Buffer Interrupt Enable */
constexpr u8 CR_FEIE  = 0x08;   /* FIFO Error Interrupt Enable */
constexpr u8 CR_IOCE  = 0x10;   /* Interrupt On Completion Enable */

/* Status register bits */
constexpr u16 SR_DCH   = 0x01;  /* DMA Controller Halted */
constexpr u16 SR_CELV  = 0x02;  /* Current Equals Last Valid */
constexpr u16 SR_LVBCI = 0x04;  /* Last Valid Buffer Completion Interrupt */
constexpr u16 SR_BCIS  = 0x08;  /* Buffer Completion Interrupt Status */
constexpr u16 SR_FIFOE = 0x10;  /* FIFO Error */

/* Global control bits */
constexpr u32 GC_GIE     = (1u << 0);  /* GPI Interrupt Enable */
constexpr u32 GC_COLD_RST = (1u << 1); /* Cold reset */
constexpr u32 GC_WARM_RST = (1u << 2); /* Warm reset */

/* Hardware BDL entries are 8 bytes each and must stay 8-byte aligned. */

#pragma pack(push, 1)
struct ac97_bd {
    u32 addr;          /* Physical address of sample buffer */
    u16 length;        /* Number of samples (not bytes!) — 0 means 0 samples */
    u16 flags;         /* Bit 15 = BUP (buffer underrun policy)
                          Bit 14 = IOC (interrupt on completion) */
};
#pragma pack(pop)

static_assert(sizeof(ac97_bd) == 8, "BD entry must be 8 bytes");

inline constexpr u16 BD_IOC = (1u << 14);
inline constexpr u16 BD_BUP = (1u << 15);

inline constexpr u32 STREAM_BUFFER_COUNT = 2;
inline constexpr u32 BDL_COUNT       = 32;   /* Max 32 entries in BDL */
inline constexpr u32 DMA_BUFFER_SIZE = 65536; /* 64 KB DMA buffer */
inline constexpr u32 DMA_PAGE_COUNT  = DMA_BUFFER_SIZE / 0x1000u;
inline constexpr u64 PLAYBACK_WATCHDOG_SLACK_TICKS = 2; /* 20 ms at 100 Hz */
inline constexpr u8 INVALID_BUFFER_SLOT = 0xFF;

static_assert(STREAM_BUFFER_COUNT <= BDL_COUNT, "stream queue must fit in the AC97 BDL");

/* ============================================================
 * Driver state
 * ============================================================ */

static u16  s_nam_base  = 0;    /* BAR0 I/O base */
static u16  s_nabm_base = 0;    /* BAR1 I/O base */
static pci_address s_pci_addr = {};

/* BDL and DMA buffers live in identity-mapped physical memory. */
static ac97_bd* s_bdl       = null;
static u32      s_bdl_phys  = 0;
static u8*      s_dma_buf[STREAM_BUFFER_COUNT]  = {};
static u32      s_dma_phys[STREAM_BUFFER_COUNT] = {};
static bool     s_dma_in_use[STREAM_BUFFER_COUNT] = {};
static u8       s_desc_buffer[BDL_COUNT] = {};
static u32      s_desc_frames[BDL_COUNT] = {};

static u32  s_sample_rate      = 48000;
static bool s_playing          = false;
static u64  s_play_end_tick    = 0;
static u32  s_queued_frames    = 0;
static u8   s_queue_head_desc  = 0;
static u8   s_queue_tail_desc  = 0;
static u8   s_queue_count      = 0;

/* ============================================================
 * NAM / NABM I/O helpers
 * ============================================================ */

static u16 nam_read16(u16 reg)             { return arch::inw(static_cast<u16>(s_nam_base + reg)); }
static void nam_write16(u16 reg, u16 val)  { arch::outw(static_cast<u16>(s_nam_base + reg), val); }

static u8 nabm_read8(u16 reg)              { return arch::inb(static_cast<u16>(s_nabm_base + reg)); }
static u16 nabm_read16(u16 reg)            { return arch::inw(static_cast<u16>(s_nabm_base + reg)); }
static void nabm_write8(u16 reg, u8 val)   { arch::outb(static_cast<u16>(s_nabm_base + reg), val); }
static void nabm_write16(u16 reg, u16 val) { arch::outw(static_cast<u16>(s_nabm_base + reg), val); }
static void nabm_write32(u16 reg, u32 val) { arch::outl(static_cast<u16>(s_nabm_base + reg), val); }

static void ac97_clear_status() {
    nabm_write16(PO_SR, SR_LVBCI | SR_BCIS | SR_FIFOE);
}

static void ac97_reset_channel() {
    nabm_write8(PO_CR, CR_RR);
    for (int i = 0; i < 1000; ++i) {
        arch::cpu_nop();
    }
    nabm_write8(PO_CR, 0);
    ac97_clear_status();
}

static void ac97_reset_stream_state() {
    s_playing = false;
    s_play_end_tick = 0;
    s_queued_frames = 0;
    s_queue_head_desc = 0;
    s_queue_tail_desc = 0;
    s_queue_count = 0;

    if (s_bdl != null) {
        for (u32 i = 0; i < BDL_COUNT; ++i) {
            s_bdl[i].addr = 0;
            s_bdl[i].length = 0;
            s_bdl[i].flags = 0;
            s_desc_buffer[i] = INVALID_BUFFER_SLOT;
            s_desc_frames[i] = 0;
        }
    }

    for (u32 i = 0; i < STREAM_BUFFER_COUNT; ++i) {
        s_dma_in_use[i] = false;
    }
}

static auto ac97_acquire_buffer_slot() -> i32 {
    for (u32 slot = 0; slot < STREAM_BUFFER_COUNT; ++slot) {
        if (!s_dma_in_use[slot]) {
            s_dma_in_use[slot] = true;
            return static_cast<i32>(slot);
        }
    }

    return -1;
}

static void ac97_update_watchdog() {
    if (!s_playing || s_queued_frames == 0 || s_sample_rate == 0) {
        s_play_end_tick = 0;
        return;
    }

    u64 dur_ticks = ((u64)s_queued_frames * SCHED_TICK_HZ + s_sample_rate - 1u) / s_sample_rate;
    s_play_end_tick = sched::tick_count()
                    + (dur_ticks < 1u ? 1u : dur_ticks)
                    + PLAYBACK_WATCHDOG_SLACK_TICKS;
}

static void ac97_release_head_buffer() {
    if (s_queue_count == 0) {
        return;
    }

    const u8 desc = s_queue_head_desc;
    const u8 slot = s_desc_buffer[desc];
    if (slot < STREAM_BUFFER_COUNT) {
        s_dma_in_use[slot] = false;
    }

    if (s_desc_frames[desc] <= s_queued_frames) {
        s_queued_frames -= s_desc_frames[desc];
    } else {
        s_queued_frames = 0;
    }
    s_desc_frames[desc] = 0;
    s_desc_buffer[desc] = INVALID_BUFFER_SLOT;
    s_bdl[desc].addr = 0;
    s_bdl[desc].length = 0;
    s_bdl[desc].flags = 0;

    s_queue_head_desc = static_cast<u8>((s_queue_head_desc + 1u) % BDL_COUNT);
    --s_queue_count;
}

static void ac97_refresh_playback_state() {
    if (!s_playing) {
        return;
    }

    const u16 sr = nabm_read16(PO_SR);
    const u8 civ = nabm_read8(PO_CIV);

    while (s_queue_count > 1 && s_queue_head_desc != civ) {
        ac97_release_head_buffer();
    }

    if (sr & (SR_LVBCI | SR_BCIS | SR_FIFOE)) {
        ac97_clear_status();
    }

    const bool fifo_error = (sr & SR_FIFOE) != 0;
    const bool dma_halted = (sr & SR_DCH) != 0;

    if (fifo_error || dma_halted) {
        while (s_queue_count > 0) {
            ac97_release_head_buffer();
        }
        nabm_write8(PO_CR, 0);
        s_playing = false;
        s_play_end_tick = 0;
        return;
    }

    if (s_play_end_tick != 0 && sched::tick_count() >= s_play_end_tick) {
        ac97_reset_channel();
        ac97_reset_stream_state();
        return;
    }

    ac97_update_watchdog();
}

/* ============================================================
 * Driver interface implementation
 * ============================================================ */

static bool ac97_init() {
    /* Find the AC'97 PCI device */
    auto* dev = pci::find_device(pci_ids::VENDOR_INTEL, pci_ids::DEVICE_AC97);
    if (!dev) {
        /* Also try ICH4 variant */
        dev = pci::find_device(pci_ids::VENDOR_INTEL, pci_ids::DEVICE_ICH4);
    }
    if (!dev) {
        /* Try by class: multimedia audio */
        dev = pci::find_by_class(pci_ids::CLASS_MULTIMEDIA, pci_ids::SUBCLASS_AUDIO);
    }
    if (!dev) {
        log::warn() << "ac97: no AC'97 PCI device found";
        return false;
    }

    s_pci_addr = dev->addr;

    log::info() << "ac97: found PCI " << log::hex(static_cast<u64>(dev->vendor_id), 1, true, false) << ":" << log::hex(static_cast<u64>(dev->device_id), 1, true, false) << " at " << log::hex(static_cast<u64>(dev->addr.bus), 1, true, false) << ":" << log::hex(static_cast<u64>(dev->addr.device), 1, true, false) << "." << log::hex(static_cast<u64>(dev->addr.function), 1, true, false);

    /* Extract I/O BAR addresses (bit 0 set = I/O space) */
    s_nam_base  = static_cast<u16>(dev->bar[0] & 0xFFFC);
    s_nabm_base = static_cast<u16>(dev->bar[1] & 0xFFFC);

    if (s_nam_base == 0 || s_nabm_base == 0) {
        log::error() << "ac97: invalid BAR addresses (NAM=" << log::hex(static_cast<u64>(s_nam_base), 1, true, false) << " NABM=" << log::hex(static_cast<u64>(s_nabm_base), 1, true, false) << ")";
        return false;
    }

    log::debug() << "ac97: NAM I/O base = " << log::hex(static_cast<u64>(s_nam_base), 1, true, false) << ", NABM I/O base = " << log::hex(static_cast<u64>(s_nabm_base), 1, true, false);

    /* Enable I/O space access + bus mastering */
    pci::enable_bus_master(s_pci_addr);

    /* Cold-reset the controller, then reset the PCM-out channel. */
    nabm_write32(NABM_GLOB_CNT, GC_COLD_RST);
    for (int i = 0; i < 100000; ++i) {
        arch::cpu_nop();
    }

    nabm_write8(PO_CR, CR_RR);
    for (int i = 0; i < 10000; ++i) arch::cpu_nop();
    nabm_write8(PO_CR, 0);

    nam_write16(NAM_RESET, 0xFFFF);
    for (int i = 0; i < 100000; ++i) arch::cpu_nop();

    /* Set master volume to max (0 = max, 0x8000 = mute) */
    nam_write16(NAM_MASTER_VOL, 0x0000);
    nam_write16(NAM_AUX_OUT_VOL, 0x0000);
    nam_write16(NAM_MONO_VOL, 0x0000);
    nam_write16(NAM_PCM_OUT_VOL, 0x0808);  /* Low attenuation */

    u16 ext_id = nam_read16(NAM_EXT_AUDIO_ID);
    if (ext_id & 0x0001) {
        u16 ext_ctrl = nam_read16(NAM_EXT_AUDIO_CTRL);
        ext_ctrl |= 0x0001;  /* VRA bit */
        nam_write16(NAM_EXT_AUDIO_CTRL, ext_ctrl);
        log::info() << "ac97: variable-rate audio enabled";
    }

    /* Set default sample rate */
    nam_write16(NAM_PCM_FRONT_RATE, static_cast<u16>(s_sample_rate));

    /* BDL must stay below 4 GiB because the hardware stores 32-bit addresses. */
    physical_pages_ptr<ac97_bd> bdl(
        reinterpret_cast<ac97_bd*>(g_phys_alloc.allocate_pages(1, 0x1000u, 0)),
        physical_pages_deleter { .page_count = 1 });
    if (!bdl) {
        log::error() << "ac97: failed to allocate BDL page";
        return false;
    }
    s_bdl_phys = static_cast<u32>(reinterpret_cast<phys_addr>(bdl.get()));
    s_bdl      = bdl.get();
    memory::set(s_bdl, 0, 0x1000u);

    /* Double-buffer so the next block can queue before the current one drains. */
    for (u32 slot = 0; slot < STREAM_BUFFER_COUNT; ++slot) {
        physical_pages_ptr<u8> dma_buf(
            reinterpret_cast<u8*>(g_phys_alloc.allocate_pages(
                DMA_PAGE_COUNT, 0x1000u, 0)),
            physical_pages_deleter { .page_count = DMA_PAGE_COUNT });
        if (!dma_buf) {
            log::error() << "ac97: failed to allocate DMA buffer slot";
            for (u32 previous = 0; previous < slot; ++previous) {
                g_phys_alloc.free_pages(reinterpret_cast<phys_addr>(s_dma_buf[previous]), DMA_PAGE_COUNT);
                s_dma_buf[previous] = null;
                s_dma_phys[previous] = 0;
            }
            return false;
        }

        s_dma_phys[slot] = static_cast<u32>(reinterpret_cast<phys_addr>(dma_buf.get()));
        s_dma_buf[slot] = dma_buf.get();
        memory::set(s_dma_buf[slot], 0, DMA_BUFFER_SIZE);
        (void)dma_buf.release();
    }

    log::debug() << "ac97: DMA buffers at "
                 << log::hex(static_cast<u64>(s_dma_phys[0]), 1, true, false)
                 << " and "
                 << log::hex(static_cast<u64>(s_dma_phys[1]), 1, true, false)
                 << ", BDL at "
                 << log::hex(static_cast<u64>(s_bdl_phys), 1, true, false);

    /* Point the hardware at our BDL */
    nabm_write32(PO_BDBAR, s_bdl_phys);

    (void)bdl.release();

    ac97_reset_stream_state();
    log::info() << "ac97: initialised";
    return true;
}

static void ac97_shutdown() {
    /* Stop DMA */
    nabm_write8(PO_CR, 0);
    /* Clear status */
    ac97_clear_status();
    for (u32 slot = 0; slot < STREAM_BUFFER_COUNT; ++slot) {
        if (s_dma_buf[slot] != null) {
            g_phys_alloc.free_pages(reinterpret_cast<phys_addr>(s_dma_buf[slot]), DMA_PAGE_COUNT);
            s_dma_buf[slot] = null;
            s_dma_phys[slot] = 0;
            s_dma_in_use[slot] = false;
        }
    }
    if (s_bdl != null) {
        g_phys_alloc.free_pages(reinterpret_cast<phys_addr>(s_bdl), 1);
        s_bdl = null;
        s_bdl_phys = 0;
    }
    ac97_reset_stream_state();
    log::info() << "ac97: shutdown";
}

static bool ac97_set_sample_rate(u32 rate_hz) {
    if (rate_hz < 8000 || rate_hz > 48000) return false;
    s_sample_rate = rate_hz;
    nam_write16(NAM_PCM_FRONT_RATE, static_cast<u16>(rate_hz));
    /* Read back to verify */
    u16 actual = nam_read16(NAM_PCM_FRONT_RATE);
    if (actual != static_cast<u16>(rate_hz)) {
        /* Some codecs only support 48000; accept whatever it gives us */
        s_sample_rate = actual;
        log::info() << "ac97: rate adjusted to codec-supported value";
    }
    return true;
}

static bool ac97_play(const u8* samples, u32 length, sound_format fmt) {
    if (!s_bdl || length == 0) return false;

    ac97_refresh_playback_state();
    if (!s_playing) {
        ac97_reset_channel();
        ac97_reset_stream_state();
        nabm_write32(PO_BDBAR, s_bdl_phys);
    } else if (s_queue_count >= BDL_COUNT) {
        return false;
    }

    const i32 buffer_slot_index = ac97_acquire_buffer_slot();
    if (buffer_slot_index < 0) {
        return false;
    }

    const u8 slot = static_cast<u8>(buffer_slot_index);
    const u8 desc = s_queue_tail_desc;
    if (s_dma_buf[slot] == null) {
        s_dma_in_use[slot] = false;
        return false;
    }

    /* Clamp to DMA buffer size */
    u32 transfer = length;
    if (transfer > DMA_BUFFER_SIZE) {
        transfer = DMA_BUFFER_SIZE;
    }

    /* Set sample rate */
    nam_write16(NAM_PCM_FRONT_RATE, static_cast<u16>(s_sample_rate));

    /* BDL length is measured in 16-bit words, not bytes. */
    u32 sample_count;

    if (fmt == sound_format::unsigned_8) {
        /* Expand unsigned 8-bit mono into signed 16-bit stereo frames. */
        u32 num_frames = transfer;
        if (num_frames * 4 > DMA_BUFFER_SIZE) {
            num_frames = DMA_BUFFER_SIZE / 4;
        }
        auto* dst = reinterpret_cast<i16*>(s_dma_buf[slot]);
        for (i32 i = static_cast<i32>(num_frames) - 1; i >= 0; --i) {
            i16 s = static_cast<i16>((static_cast<i16>(samples[i]) - 128) << 8);
            dst[i * 2]     = s;
            dst[i * 2 + 1] = s;
        }
        sample_count = num_frames * 2;
        transfer     = num_frames * 4;
    } else {
        memory::copy(s_dma_buf[slot], samples, transfer);
        sample_count = transfer / 2;
    }

    s_bdl[desc].addr   = s_dma_phys[slot];
    s_bdl[desc].length = static_cast<u16>(sample_count & 0xFFFF);
    s_bdl[desc].flags  = BD_IOC | BD_BUP;

    s_desc_buffer[desc] = slot;
    s_desc_frames[desc] = sample_count / 2;
    s_queued_frames += s_desc_frames[desc];
    if (s_queue_count == 0) {
        s_queue_head_desc = desc;
    }
    s_queue_tail_desc = static_cast<u8>((desc + 1u) % BDL_COUNT);
    ++s_queue_count;

    nabm_write8(PO_LVI, desc);

    if (!s_playing) {
        nabm_write8(PO_CR, CR_RPBM | CR_LVBIE | CR_IOCE);
        s_playing = true;
    }

    ac97_update_watchdog();
    return true;
}

static void ac97_stop() {
    nabm_write8(PO_CR, 0);  /* Stop DMA */
    ac97_clear_status();
    ac97_reset_stream_state();
}

static bool ac97_is_playing() {
    ac97_refresh_playback_state();
    return s_playing;
}

static u32 ac97_buffered_frames() {
    ac97_refresh_playback_state();
    return s_queued_frames;
}

static void ac97_set_volume(u8 left, u8 right) {
    /* AC'97 volume: 6-bit attenuation, 0 = max, 0x3F = min.
     * Bit 15 = mute.  Register uses 5-bit fields for L and R. */
    u8 l_atten = static_cast<u8>(0x1F - (left >> 3));   /* 0-255 → 31-0 */
    u8 r_atten = static_cast<u8>(0x1F - (right >> 3));
    u16 vol = static_cast<u16>((l_atten << 8) | r_atten);
    nam_write16(NAM_MASTER_VOL, vol);
    nam_write16(NAM_PCM_OUT_VOL, vol);
}

/* ============================================================
 * Driver descriptor
 * ============================================================ */

static const sound_driver_t ac97_sound_driver = {
    .name           = "ac97",
    .init           = ac97_init,
    .shutdown       = ac97_shutdown,
    .set_sample_rate = ac97_set_sample_rate,
    .play           = ac97_play,
    .stop           = ac97_stop,
    .is_playing     = ac97_is_playing,
    .buffered_frames = ac97_buffered_frames,
    .set_volume     = ac97_set_volume,
};

static const driver_descriptor ac97_descriptor = {
    .name  = "ac97",
    .type  = driver_type::sound,
    .sound = &ac97_sound_driver,
    .block = null,
    .net   = null,
};

} // anonymous namespace

/* ============================================================
 * Auto-registration
 * ============================================================ */

namespace ac97_driver {

void register_builtin() {
    driver::register_driver(&ac97_descriptor);
}

} // namespace ac97_driver
} // namespace vk
