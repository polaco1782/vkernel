/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * ata_pio.cpp - Legacy ATA PIO block driver
 *
 * Synchronous ATA PIO driver for early filesystem bring-up.
 */

#include "config.h"
#include "types.h"
#include "console.h"
#include "log.h"
#include "memory.h"
#include "block.h"
#include "driver.h"
#include "pci.h"
#include "arch/x86_64/arch.h"

namespace vk {
namespace {

/* ATA task-file registers, relative to channel I/O base. */
constexpr u16 ATA_REG_DATA       = 0x00;
constexpr u16 ATA_REG_ERROR      = 0x01;
constexpr u16 ATA_REG_FEATURES   = 0x01;
constexpr u16 ATA_REG_SECCOUNT0  = 0x02;
constexpr u16 ATA_REG_LBA0       = 0x03;
constexpr u16 ATA_REG_LBA1       = 0x04;
constexpr u16 ATA_REG_LBA2       = 0x05;
constexpr u16 ATA_REG_HDDEVSEL   = 0x06;
constexpr u16 ATA_REG_COMMAND    = 0x07;
constexpr u16 ATA_REG_STATUS     = 0x07;
constexpr u16 ATA_REG_ALTSTATUS  = 0x00;
constexpr u16 ATA_REG_CONTROL    = 0x00;

constexpr u8 ATA_CMD_READ_PIO    = 0x20;
constexpr u8 ATA_CMD_WRITE_PIO   = 0x30;
constexpr u8 ATA_CMD_CACHE_FLUSH = 0xE7;
constexpr u8 ATA_CMD_IDENTIFY    = 0xEC;

constexpr u8 ATA_SR_ERR          = 0x01;
constexpr u8 ATA_SR_DRQ          = 0x08;
constexpr u8 ATA_SR_DF           = 0x20;
constexpr u8 ATA_SR_DRDY         = 0x40;
constexpr u8 ATA_SR_BSY          = 0x80;

constexpr u32 ATA_SECTOR_SIZE    = 512;
constexpr u32 ATA_TIMEOUT_SPINS  = 1000000;
constexpr u64 ATA_LBA28_SECTORS  = 0x10000000ULL;

struct ata_channel {
    u16 io_base;
    u16 ctrl_base;
    const char* name;
};

struct ata_drive {
    ata_channel channel;
    u8          drive;
    bool        present;
    u64         sectors;
    static_string<41> model;
};

static ata_drive s_drives[4];
static usize     s_drive_count = 0;

static inline u8 ata_read8(const ata_channel& ch, u16 reg) {
    return arch::inb(static_cast<u16>(ch.io_base + reg));
}

static inline void ata_write8(const ata_channel& ch, u16 reg, u8 value) {
    arch::outb(static_cast<u16>(ch.io_base + reg), value);
}

static inline u8 ata_ctrl_read8(const ata_channel& ch, u16 reg) {
    return arch::inb(static_cast<u16>(ch.ctrl_base + reg));
}

static inline void ata_ctrl_write8(const ata_channel& ch, u16 reg, u8 value) {
    arch::outb(static_cast<u16>(ch.ctrl_base + reg), value);
}

static void ata_io_wait(const ata_channel& ch) {
    (void)ata_ctrl_read8(ch, ATA_REG_ALTSTATUS);
    (void)ata_ctrl_read8(ch, ATA_REG_ALTSTATUS);
    (void)ata_ctrl_read8(ch, ATA_REG_ALTSTATUS);
    (void)ata_ctrl_read8(ch, ATA_REG_ALTSTATUS);
}

static bool wait_not_busy(const ata_channel& ch) {
    for (u32 i = 0; i < ATA_TIMEOUT_SPINS; ++i) {
        if ((ata_read8(ch, ATA_REG_STATUS) & ATA_SR_BSY) == 0) {
            return true;
        }
    }
    return false;
}

static bool wait_drq(const ata_channel& ch) {
    for (u32 i = 0; i < ATA_TIMEOUT_SPINS; ++i) {
        u8 st = ata_read8(ch, ATA_REG_STATUS);
        if ((st & ATA_SR_BSY) != 0) continue;
        if ((st & (ATA_SR_ERR | ATA_SR_DF)) != 0) return false;
        if ((st & ATA_SR_DRQ) != 0) return true;
    }
    return false;
}

static void read_data_words(const ata_channel& ch, u16* out, u32 words) {
    for (u32 i = 0; i < words; ++i) {
        out[i] = arch::inw(static_cast<u16>(ch.io_base + ATA_REG_DATA));
    }
}

static void read_data_sector_bytes(const ata_channel& ch, u8* out) {
    for (u32 i = 0; i < ATA_SECTOR_SIZE / 2; ++i) {
        u16 w = arch::inw(static_cast<u16>(ch.io_base + ATA_REG_DATA));
        out[i * 2] = static_cast<u8>(w & 0xFF);
        out[i * 2 + 1] = static_cast<u8>(w >> 8);
    }
}

static void write_data_sector_bytes(const ata_channel& ch, const u8* in) {
    for (u32 i = 0; i < ATA_SECTOR_SIZE / 2; ++i) {
        const u16 word = static_cast<u16>(in[i * 2])
            | static_cast<u16>(static_cast<u16>(in[i * 2 + 1]) << 8);
        arch::outw(static_cast<u16>(ch.io_base + ATA_REG_DATA), word);
    }
}

static u64 identify_lba_capacity(const u16* id) {
    const bool lba48 = (id[83] & (1u << 10)) != 0;
    if (lba48) {
        return static_cast<u64>(id[100]) |
               (static_cast<u64>(id[101]) << 16) |
               (static_cast<u64>(id[102]) << 32) |
               (static_cast<u64>(id[103]) << 48);
    }
    return static_cast<u64>(id[60]) | (static_cast<u64>(id[61]) << 16);
}

static void identify_model(const u16* id, static_string<41>& out) {
    char model[41];
    for (usize i = 0; i < 20; ++i) {
        u16 w = id[27 + i];
        model[i * 2] = static_cast<char>(w >> 8);
        model[i * 2 + 1] = static_cast<char>(w & 0xFF);
    }
    model[40] = '\0';

    usize end = 40;
    while (end > 0 && model[end - 1] == ' ') {
        --end;
    }
    model[end] = '\0';
    (void)out.assign(string_view(model, end));
}

static bool identify_drive(const ata_channel& ch, u8 drive, ata_drive& out) {
    ata_write8(ch, ATA_REG_HDDEVSEL, static_cast<u8>(0xA0 | (drive << 4)));
    ata_io_wait(ch);

    ata_write8(ch, ATA_REG_SECCOUNT0, 0);
    ata_write8(ch, ATA_REG_LBA0, 0);
    ata_write8(ch, ATA_REG_LBA1, 0);
    ata_write8(ch, ATA_REG_LBA2, 0);
    ata_write8(ch, ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_io_wait(ch);

    u8 st = ata_read8(ch, ATA_REG_STATUS);
    if (st == 0) return false;
    if (!wait_not_busy(ch)) return false;

    u8 lba1 = ata_read8(ch, ATA_REG_LBA1);
    u8 lba2 = ata_read8(ch, ATA_REG_LBA2);
    if (lba1 != 0 || lba2 != 0) {
        return false; /* ATAPI or non-ATA device. */
    }
    if (!wait_drq(ch)) return false;

    u16 id[256];
    read_data_words(ch, id, 256);

    u64 sectors = identify_lba_capacity(id);
    if (sectors == 0) return false;
    if (sectors > ATA_LBA28_SECTORS) {
        sectors = ATA_LBA28_SECTORS;
    }

    out.channel = ch;
    out.drive = drive;
    out.present = true;
    out.sectors = sectors;
    identify_model(id, out.model);
    return true;
}

static bool ata_read_one_lba28(ata_drive* drive, u64 lba, u8* out) {
    if (drive == null || out == null) return false;
    if (lba > 0x0FFFFFFFULL) return false;

    auto& ch = drive->channel;
    if (!wait_not_busy(ch)) return false;

    ata_write8(ch, ATA_REG_HDDEVSEL,
               static_cast<u8>(0xE0 | (drive->drive << 4) | ((lba >> 24) & 0x0F)));
    ata_io_wait(ch);
    ata_write8(ch, ATA_REG_FEATURES, 0);
    ata_write8(ch, ATA_REG_SECCOUNT0, 1);
    ata_write8(ch, ATA_REG_LBA0, static_cast<u8>(lba));
    ata_write8(ch, ATA_REG_LBA1, static_cast<u8>(lba >> 8));
    ata_write8(ch, ATA_REG_LBA2, static_cast<u8>(lba >> 16));
    ata_write8(ch, ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    if (!wait_drq(ch)) return false;
    read_data_sector_bytes(ch, out);
    ata_io_wait(ch);
    return true;
}

static bool ata_read_lba28(ata_drive* drive, u64 lba, u8 sector_count, u8* out) {
    if (drive == null || out == null || sector_count == 0) return false;
    if (lba > 0x0FFFFFFFULL || (lba + sector_count - 1) > 0x0FFFFFFFULL) return false;
    if (sector_count == 1) return ata_read_one_lba28(drive, lba, out);

    auto& ch = drive->channel;
    if (!wait_not_busy(ch)) return false;

    ata_write8(ch, ATA_REG_HDDEVSEL,
               static_cast<u8>(0xE0 | (drive->drive << 4) | ((lba >> 24) & 0x0F)));
    ata_io_wait(ch);
    ata_write8(ch, ATA_REG_FEATURES, 0);
    ata_write8(ch, ATA_REG_SECCOUNT0, sector_count);
    ata_write8(ch, ATA_REG_LBA0, static_cast<u8>(lba));
    ata_write8(ch, ATA_REG_LBA1, static_cast<u8>(lba >> 8));
    ata_write8(ch, ATA_REG_LBA2, static_cast<u8>(lba >> 16));
    ata_write8(ch, ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    for (u8 sector_index = 0; sector_index < sector_count; ++sector_index) {
        if (!wait_drq(ch)) return false;
        read_data_sector_bytes(ch, out + static_cast<u32>(sector_index) * ATA_SECTOR_SIZE);
        ata_io_wait(ch);
    }

    return true;
}

static bool ata_block_read(block_device* dev, u64 lba, u32 count, void* buffer) {
    if (dev == null || buffer == null || count == 0) return false;

    auto* drive = static_cast<ata_drive*>(dev->driver_data);
    auto* out = static_cast<u8*>(buffer);
    u64 current_lba = lba;
    u32 remaining = count;
    while (remaining > 0) {
        const u8 chunk = remaining > 255 ? 255 : static_cast<u8>(remaining);
        if (!ata_read_lba28(drive, current_lba, chunk, out)) {
            log::warn() << "ata_pio: read failed dev=" << dev->name.c_str() << " lba="
                        << static_cast<unsigned long long>(current_lba);
            return false;
        }
        current_lba += chunk;
        out += static_cast<u32>(chunk) * ATA_SECTOR_SIZE;
        remaining -= chunk;
    }
    return true;
}

static bool ata_write_one_lba28(ata_drive* drive, u64 lba, const u8* in) {
    if (drive == null || in == null) return false;
    if (lba > 0x0FFFFFFFULL) return false;

    auto& ch = drive->channel;
    if (!wait_not_busy(ch)) return false;

    ata_write8(ch, ATA_REG_HDDEVSEL,
               static_cast<u8>(0xE0 | (drive->drive << 4) | ((lba >> 24) & 0x0F)));
    ata_io_wait(ch);
    ata_write8(ch, ATA_REG_FEATURES, 0);
    ata_write8(ch, ATA_REG_SECCOUNT0, 1);
    ata_write8(ch, ATA_REG_LBA0, static_cast<u8>(lba));
    ata_write8(ch, ATA_REG_LBA1, static_cast<u8>(lba >> 8));
    ata_write8(ch, ATA_REG_LBA2, static_cast<u8>(lba >> 16));
    ata_write8(ch, ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    if (!wait_drq(ch)) return false;
    write_data_sector_bytes(ch, in);
    ata_io_wait(ch);

    ata_write8(ch, ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    return wait_not_busy(ch);
}

static bool ata_write_lba28(ata_drive* drive, u64 lba, u8 sector_count, const u8* in) {
    if (drive == null || in == null || sector_count == 0) return false;
    if (lba > 0x0FFFFFFFULL || (lba + sector_count - 1) > 0x0FFFFFFFULL) return false;
    if (sector_count == 1) return ata_write_one_lba28(drive, lba, in);

    auto& ch = drive->channel;
    if (!wait_not_busy(ch)) return false;

    ata_write8(ch, ATA_REG_HDDEVSEL,
               static_cast<u8>(0xE0 | (drive->drive << 4) | ((lba >> 24) & 0x0F)));
    ata_io_wait(ch);
    ata_write8(ch, ATA_REG_FEATURES, 0);
    ata_write8(ch, ATA_REG_SECCOUNT0, sector_count);
    ata_write8(ch, ATA_REG_LBA0, static_cast<u8>(lba));
    ata_write8(ch, ATA_REG_LBA1, static_cast<u8>(lba >> 8));
    ata_write8(ch, ATA_REG_LBA2, static_cast<u8>(lba >> 16));
    ata_write8(ch, ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    for (u8 sector_index = 0; sector_index < sector_count; ++sector_index) {
        if (!wait_drq(ch)) return false;
        write_data_sector_bytes(ch, in + static_cast<u32>(sector_index) * ATA_SECTOR_SIZE);
        ata_io_wait(ch);
    }

    ata_write8(ch, ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    return wait_not_busy(ch);
}

static bool ata_block_write(block_device* dev, u64 lba, u32 count, const void* buffer) {
    if (dev == null || buffer == null || count == 0) return false;

    auto* drive = static_cast<ata_drive*>(dev->driver_data);
    const auto* in = static_cast<const u8*>(buffer);
    u64 current_lba = lba;
    u32 remaining = count;
    while (remaining > 0) {
        const u8 chunk = remaining > 255 ? 255 : static_cast<u8>(remaining);
        if (!ata_write_lba28(drive, current_lba, chunk, in)) {
            log::warn() << "ata_pio: write failed dev=" << dev->name.c_str() << " lba="
                        << static_cast<unsigned long long>(current_lba);
            return false;
        }
        current_lba += chunk;
        in += static_cast<u32>(chunk) * ATA_SECTOR_SIZE;
        remaining -= chunk;
    }
    return true;
}

static const block_ops s_ata_block_ops = {
    .read_blocks = ata_block_read,
    .write_blocks = ata_block_write,
};

static bool register_ata_drive(ata_drive& drive) {
    block_device dev{};
    char name[16];
    name[0] = 'b';
    name[1] = 'l';
    name[2] = 'o';
    name[3] = 'c';
    name[4] = 'k';
    name[5] = static_cast<char>('0' + block::device_count());
    name[6] = '\0';

    (void)dev.name.assign(name);
    dev.block_count = drive.sectors;
    dev.block_size = ATA_SECTOR_SIZE;
    dev.removable = false;
    dev.driver_data = &drive;
    dev.ops = &s_ata_block_ops;

    i32 id = block::register_device(dev);
    if (id < 0) return false;

    log::info() << "ata_pio: " << dev.name.c_str() << " " << drive.channel.name << " "
                << (drive.drive == 0 ? "master" : "slave") << ", "
                << static_cast<unsigned long long>(drive.sectors) << " sectors";
    if (!drive.model.empty()) {
        log::info() << "ata_pio: model: " << drive.model.c_str();
    }
    return true;
}

static bool ata_pio_init() {
    block::init();
    s_drive_count = 0;
    memory::set(s_drives, 0, sizeof(s_drives));

    auto* ide = pci::find_by_class(pci_ids::CLASS_STORAGE, pci_ids::SUBCLASS_IDE);
    if (ide != null) {
        log::info() << "ata_pio: IDE controller at " << log::hex(static_cast<u64>(ide->addr.bus), 1, true, false) << ":" << log::hex(static_cast<u64>(ide->addr.device), 1, true, false) << "." << log::hex(static_cast<u64>(ide->addr.function), 1, true, false) << " vendor=" << log::hex(static_cast<u64>(ide->vendor_id), 1, true, false) << " device=" << log::hex(static_cast<u64>(ide->device_id), 1, true, false) << " prog_if=" << log::hex(static_cast<u64>(ide->prog_if), 1, true, false);
        pci::enable_bus_master(ide->addr);
    } else {
        log::warn() << "ata_pio: no PCI IDE controller found, probing legacy ports";
    }

    const ata_channel channels[2] = {
        { 0x1F0, 0x3F6, "primary" },
        { 0x170, 0x376, "secondary" },
    };

    for (usize c = 0; c < array_size(channels); ++c) {
        ata_ctrl_write8(channels[c], ATA_REG_CONTROL, 0x02); /* nIEN: disable ATA IRQs. */
        for (u8 drive = 0; drive < 2; ++drive) {
            if (s_drive_count >= array_size(s_drives)) break;
            ata_drive candidate{};
            if (identify_drive(channels[c], drive, candidate)) {
                s_drives[s_drive_count] = candidate;
                (void)register_ata_drive(s_drives[s_drive_count]);
                ++s_drive_count;
            }
        }
    }

    if (s_drive_count == 0) {
        log::warn() << "ata_pio: no ATA disks detected";
        return false;
    }

    block::list_devices();

    u8 sector[ATA_SECTOR_SIZE];
    if (block::device_count() > 0 &&
        block::read_blocks(block::get_device(0), 0, 1, sector)) {
        u16 sig = static_cast<u16>((static_cast<u16>(sector[511]) << 8) | sector[510]);
        log::info() << "ata_pio: block0 sector0 signature " << log::hex(sig);
    }

    return true;
}

static void ata_pio_shutdown() {
    s_drive_count = 0;
}

static const block_driver_t s_ata_pio_block_driver = {
    .name = "ata_pio",
    .init = ata_pio_init,
    .shutdown = ata_pio_shutdown,
};

static const driver_descriptor s_ata_pio_driver = {
    .name = "ata_pio",
    .type = driver_type::block,
    .sound = null,
    .block = &s_ata_pio_block_driver,
};

} // namespace

namespace ata_pio_driver {

void register_builtin() {
    driver::register_driver(&s_ata_pio_driver);
}

} // namespace ata_pio_driver
} // namespace vk
