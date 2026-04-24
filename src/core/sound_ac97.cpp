/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * sound_ac97.cpp - Intel AC'97 (ICH) PCI sound driver
 *
 * Drives the AC'97 codec via PCI I/O BARs.
 * BAR0 = Native Audio Mixer (NAM) — codec register access
 * BAR1 = Native Audio Bus Master (NABM) — DMA engine
 *
 * QEMU's default audio device on q35 is the ICH AC'97
 * (vendor 0x8086, device 0x2415).
 */

#include "config.h"
#include "types.h"
#include "console.h"
#include "memory.h"
#include "sound.h"
#include "driver.h"
#include "pci.h"
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

/* ============================================================
 * Buffer Descriptor List (BDL) entry — 8 bytes each
 * Hardware requires the BDL array to be 8-byte aligned.
 * ============================================================ */

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

inline constexpr u32 BDL_COUNT       = 32;   /* Max 32 entries in BDL */
inline constexpr u32 DMA_BUFFER_SIZE = 65536; /* 64 KB DMA buffer */

/* ============================================================
 * Driver state
 * ============================================================ */

static u16  s_nam_base  = 0;    /* BAR0 I/O base */
static u16  s_nabm_base = 0;    /* BAR1 I/O base */
static pci_address s_pci_addr = {};

/* BDL and DMA buffer — identity-mapped physical memory */
static ac97_bd* s_bdl       = null;
static u32      s_bdl_phys  = 0;
static u8*      s_dma_buf   = null;
static u32      s_dma_phys  = 0;

static u32  s_sample_rate      = 48000;
static bool s_playing          = false;
static u64  s_play_end_tick    = 0;
static u32  s_current_length   = 0;  /* bytes submitted */

/* ============================================================
 * NAM / NABM I/O helpers
 * ============================================================ */

static u16 nam_read16(u16 reg)             { return arch::inw(static_cast<u16>(s_nam_base + reg)); }
static void nam_write16(u16 reg, u16 val)  { arch::outw(static_cast<u16>(s_nam_base + reg), val); }

static u16 nabm_read16(u16 reg)            { return arch::inw(static_cast<u16>(s_nabm_base + reg)); }
static void nabm_write8(u16 reg, u8 val)   { arch::outb(static_cast<u16>(s_nabm_base + reg), val); }
static void nabm_write16(u16 reg, u16 val) { arch::outw(static_cast<u16>(s_nabm_base + reg), val); }
static void nabm_write32(u16 reg, u32 val) { arch::outl(static_cast<u16>(s_nabm_base + reg), val); }

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
        log::warn("ac97: no AC'97 PCI device found");
        return false;
    }

    s_pci_addr = dev->addr;

    log::info("ac97: found PCI %#x:%#x at %#x:%#x.%#x",
              dev->vendor_id, dev->device_id,
              dev->addr.bus, dev->addr.device, dev->addr.function);

    /* Extract I/O BAR addresses (bit 0 set = I/O space) */
    s_nam_base  = static_cast<u16>(dev->bar[0] & 0xFFFC);
    s_nabm_base = static_cast<u16>(dev->bar[1] & 0xFFFC);

    if (s_nam_base == 0 || s_nabm_base == 0) {
        log::error("ac97: invalid BAR addresses (NAM=%#x NABM=%#x)", s_nam_base, s_nabm_base);
        return false;
    }

    log::debug("ac97: NAM I/O base = %#x, NABM I/O base = %#x", s_nam_base, s_nabm_base);

    /* Enable I/O space access + bus mastering */
    pci::enable_bus_master(s_pci_addr);

    /* ---- Cold reset the AC'97 controller ---- */
    /* Set cold reset bit in Global Control */
    nabm_write32(NABM_GLOB_CNT, GC_COLD_RST);
    /* Wait for codec ready — spin for a while */
    for (int i = 0; i < 100000; ++i) {
        arch::cpu_nop();
    }

    /* Reset the PCM Out channel */
    nabm_write8(PO_CR, CR_RR);
    for (int i = 0; i < 10000; ++i) arch::cpu_nop();
    nabm_write8(PO_CR, 0);

    /* ---- Reset codec via NAM ---- */
    nam_write16(NAM_RESET, 0xFFFF);
    for (int i = 0; i < 100000; ++i) arch::cpu_nop();

    /* Set master volume to max (0 = max, 0x8000 = mute) */
    nam_write16(NAM_MASTER_VOL, 0x0000);
    nam_write16(NAM_AUX_OUT_VOL, 0x0000);
    nam_write16(NAM_MONO_VOL, 0x0000);
    nam_write16(NAM_PCM_OUT_VOL, 0x0808);  /* Low attenuation */

    /* ---- Enable variable-rate audio if supported ---- */
    u16 ext_id = nam_read16(NAM_EXT_AUDIO_ID);
    if (ext_id & 0x0001) {
        /* VRA supported — enable it */
        u16 ext_ctrl = nam_read16(NAM_EXT_AUDIO_CTRL);
        ext_ctrl |= 0x0001;  /* VRA bit */
        nam_write16(NAM_EXT_AUDIO_CTRL, ext_ctrl);
        log::info("ac97: variable-rate audio enabled");
    }

    /* Set default sample rate */
    nam_write16(NAM_PCM_FRONT_RATE, static_cast<u16>(s_sample_rate));

    /* ---- Allocate BDL and DMA buffer ---- */
    /* BDL: 32 entries × 8 bytes = 256 bytes, needs to be below 4 GB for 32-bit addresses */
    auto bdl_phys = g_phys_alloc.allocate_pages(1, PAGE_SIZE_4K, 0);
    if (bdl_phys == 0) {
        log::error("ac97: failed to allocate BDL page");
        return false;
    }
    s_bdl_phys = static_cast<u32>(bdl_phys);
    s_bdl      = reinterpret_cast<ac97_bd*>(bdl_phys);
    memory::memory_set(s_bdl, 0, PAGE_SIZE_4K);

    /* DMA buffer — 64 KB, 4K aligned */
    auto dma_phys = g_phys_alloc.allocate_pages(
        DMA_BUFFER_SIZE / PAGE_SIZE_4K, PAGE_SIZE_4K, 0);
    if (dma_phys == 0) {
        log::error("ac97: failed to allocate DMA buffer");
        return false;
    }
    s_dma_phys = static_cast<u32>(dma_phys);
    s_dma_buf  = reinterpret_cast<u8*>(dma_phys);
    memory::memory_set(s_dma_buf, 0, DMA_BUFFER_SIZE);

    log::debug("ac97: DMA buffer at %#x, BDL at %#x", s_dma_phys, s_bdl_phys);

    /* Point the hardware at our BDL */
    nabm_write32(PO_BDBAR, s_bdl_phys);

    s_playing = false;
    log::info("ac97: initialised");
    return true;
}

static void ac97_shutdown() {
    /* Stop DMA */
    nabm_write8(PO_CR, 0);
    /* Clear status */
    nabm_write16(PO_SR, SR_LVBCI | SR_BCIS | SR_FIFOE);
    s_playing = false;
    log::info("ac97: shutdown");
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
        log::info("ac97: rate adjusted to %u Hz", actual);
    }
    return true;
}

static bool ac97_play(const u8* samples, u32 length, sound_format fmt) {
    if (!s_dma_buf || !s_bdl || length == 0) return false;

    /* Clamp to DMA buffer size */
    u32 transfer = length;
    if (transfer > DMA_BUFFER_SIZE) {
        transfer = DMA_BUFFER_SIZE;
    }

    /* Stop current playback first */
    nabm_write8(PO_CR, CR_RR);
    for (int i = 0; i < 1000; ++i) arch::cpu_nop();
    nabm_write8(PO_CR, 0);

    /* Clear status bits */
    nabm_write16(PO_SR, SR_LVBCI | SR_BCIS | SR_FIFOE);

    /* Copy samples to DMA buffer */
    memory::memory_copy(s_dma_buf, samples, transfer);

    /* Set sample rate */
    nam_write16(NAM_PCM_FRONT_RATE, static_cast<u16>(s_sample_rate));

    /* AC'97 is a stereo 16-bit device.  The BDL 'length' field is the
     * total number of 16-bit words in the buffer (both channels combined),
     * i.e. frames × 2.  Duration = (length / 2) frames / sample_rate. */

    u32 sample_count;  /* Total 16-bit words handed to AC97 (frames × 2) */

    if (fmt == sound_format::unsigned_8) {
        /* Each 8-bit mono input sample → one stereo frame (2 × i16).
         * Clamp so the expanded buffer still fits in the DMA window. */
        u32 num_frames = transfer;
        if (num_frames * 4 > DMA_BUFFER_SIZE) {
            num_frames = DMA_BUFFER_SIZE / 4;
        }
        auto* dst = reinterpret_cast<i16*>(s_dma_buf);
        /* Expand backwards: each src byte → two i16 words (L = R = sample). */
        for (i32 i = static_cast<i32>(num_frames) - 1; i >= 0; --i) {
            i16 s = static_cast<i16>((static_cast<i16>(s_dma_buf[i]) - 128) << 8);
            dst[i * 2]     = s;   /* left  */
            dst[i * 2 + 1] = s;   /* right */
        }
        sample_count = num_frames * 2;   /* total 16-bit words = frames × 2 */
        transfer     = num_frames * 4;   /* total bytes = frames × 4 */
    } else {
        /* signed_16 stereo: transfer bytes / 2 = total 16-bit words */
        sample_count = transfer / 2;
    }

    /* Fill BDL — use a single entry pointing to the entire buffer */
    s_bdl[0].addr   = s_dma_phys;
    s_bdl[0].length = static_cast<u16>(sample_count & 0xFFFF);
    s_bdl[0].flags  = BD_IOC | BD_BUP;

    /* Clear remaining entries */
    for (u32 i = 1; i < BDL_COUNT; ++i) {
        s_bdl[i].addr   = 0;
        s_bdl[i].length = 0;
        s_bdl[i].flags  = 0;
    }

    /* Set BDL base address */
    nabm_write32(PO_BDBAR, s_bdl_phys);

    /* Set Last Valid Index = 0 (only one buffer entry) */
    nabm_write8(PO_LVI, 0);

    /* Calculate duration for tick-based fallback.
     * sample_count is always total 16-bit words; frames = sample_count / 2
     * because AC97 is always stereo (2 words per frame). */
    u32 frames = sample_count / 2;
    u64 dur_ticks = ((u64)frames * 100u + s_sample_rate - 1u) / s_sample_rate;
    s_play_end_tick = sched::tick_count() + (dur_ticks < 1u ? 1u : dur_ticks);
    s_current_length = transfer;

    /* Start DMA playback */
    nabm_write8(PO_CR, CR_RPBM | CR_LVBIE | CR_IOCE);

    s_playing = true;
    return true;
}

static void ac97_stop() {
    nabm_write8(PO_CR, 0);  /* Stop DMA */
    nabm_write16(PO_SR, SR_LVBCI | SR_BCIS | SR_FIFOE);  /* Clear status */
    s_playing = false;
    s_play_end_tick = 0;
}

static bool ac97_is_playing() {
    if (!s_playing) return false;

    /* Check hardware status: DCH (DMA Controller Halted) or CELV (current==last) */
    u16 sr = nabm_read16(PO_SR);
    if (sr & (SR_DCH | SR_CELV | SR_LVBCI)) {
        /* Clear status bits */
        nabm_write16(PO_SR, SR_LVBCI | SR_BCIS | SR_FIFOE);
        s_playing = false;
        return false;
    }

    /* Tick-based fallback */
    if (sched::tick_count() >= s_play_end_tick) {
        nabm_write8(PO_CR, 0);
        nabm_write16(PO_SR, SR_LVBCI | SR_BCIS | SR_FIFOE);
        s_playing = false;
        return false;
    }

    return true;
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
    .set_volume     = ac97_set_volume,
};

static const driver_descriptor ac97_descriptor = {
    .name  = "ac97",
    .type  = driver_type::sound,
    .sound = &ac97_sound_driver,
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
