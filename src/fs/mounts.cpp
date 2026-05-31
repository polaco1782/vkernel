/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * mounts.cpp - Block partition scanning for filesystem mounts
 */

#include "fs/mounts.h"

#include "block.h"
#include "log.h"

namespace vk {
namespace fs {
namespace mounts {
namespace {

#pragma pack(push, 1)

struct MBRPartitionEntry {
    u8 status;
    u8 chs_first[3];
    u8 type;
    u8 chs_last[3];
    u32 first_lba;
    u32 sector_count;
};

struct GPTHeader {
    char signature[8];
    u32 revision;
    u32 header_size;
    u32 header_crc32;
    u32 reserved;
    u64 current_lba;
    u64 backup_lba;
    u64 first_usable_lba;
    u64 last_usable_lba;
    u8 disk_guid[16];
    u64 partition_entry_lba;
    u32 partition_entry_count;
    u32 partition_entry_size;
    u32 partition_entry_array_crc32;
};

struct GPTPartitionEntry {
    u8 type_guid[16];
    u8 unique_guid[16];
    u64 first_lba;
    u64 last_lba;
    u64 attributes;
    u16 name[36];
};

#pragma pack(pop)

static_assert(sizeof(MBRPartitionEntry) == 16);

static auto all_zero_guid(const u8 guid[16]) -> bool {
    for (usize i = 0; i < 16; ++i) {
        if (guid[i] != 0) {
            return false;
        }
    }
    return true;
}

static auto is_valid_gpt_header(const GPTHeader& gpt) -> bool {
    return gpt.signature[0] == 'E' && gpt.signature[1] == 'F' && gpt.signature[2] == 'I'
        && gpt.signature[3] == ' ' && gpt.signature[4] == 'P' && gpt.signature[5] == 'A'
        && gpt.signature[6] == 'R' && gpt.signature[7] == 'T'
        && gpt.partition_entry_size >= sizeof(GPTPartitionEntry)
        && gpt.partition_entry_count > 0;
}

static auto read_gpt_header(block_device* device, GPTHeader& gpt_out) -> bool {
    if (device == null || device->block_size != 512) {
        return false;
    }

    u8 sector1[512];
    if (!block::read_blocks(device, 1, 1, sector1)) {
        return false;
    }

    const auto* gpt = reinterpret_cast<const GPTHeader*>(sector1);
    if (!is_valid_gpt_header(*gpt)) {
        return false;
    }

    gpt_out = *gpt;
    return true;
}

static auto read_gpt_partition_entry(block_device* device,
                                     const GPTHeader& gpt,
                                     u32 entry_index,
                                     GPTPartitionEntry& partition_out) -> bool {
    if (device == null || gpt.partition_entry_size < sizeof(GPTPartitionEntry)
        || gpt.partition_entry_count == 0 || entry_index >= gpt.partition_entry_count) {
        return false;
    }

    const u32 entries_per_sector = 512 / gpt.partition_entry_size;
    if (entries_per_sector == 0) {
        return false;
    }

    u8 entry_sector[512];
    const u32 sector_index = entry_index / entries_per_sector;
    if (!block::read_blocks(device, gpt.partition_entry_lba + sector_index, 1, entry_sector)) {
        return false;
    }

    const u32 entry_offset = (entry_index % entries_per_sector) * gpt.partition_entry_size;
    partition_out = *reinterpret_cast<const GPTPartitionEntry*>(entry_sector + entry_offset);
    return true;
}

static auto try_mount_partition(const partition_view& partition,
                                partition_mount_callback callback,
                                void* context) -> bool {
    return callback != null && callback(partition, context);
}

static auto try_mount_raw_device(block_device* device,
                                 partition_mount_callback callback,
                                 void* context) -> bool {
    log::debug() << "mounts: trying raw volume on device='" << device->name.c_str() << "'";

    const partition_view partition {
        .device = device,
        .start_lba = 0,
        .sector_count = device->block_count,
    };

    if (!try_mount_partition(partition, callback, context)) {
        return false;
    }

    log::debug() << "mounts: mounted raw volume on device='" << device->name.c_str() << "'";
    return true;
}

static auto try_mount_specific_gpt_partition(block_device* device,
                                             const GPTHeader& gpt,
                                             u32 partition_index,
                                             partition_mount_callback callback,
                                             void* context) -> bool {
    GPTPartitionEntry partition_entry {};
    if (!read_gpt_partition_entry(device, gpt, partition_index, partition_entry)) {
        return false;
    }
    if (all_zero_guid(partition_entry.type_guid) || partition_entry.first_lba == 0
        || partition_entry.last_lba < partition_entry.first_lba) {
        return false;
    }

    log::debug() << "mounts: trying GPT partition index="
                 << static_cast<unsigned long long>(partition_index) << " on device='"
                 << device->name.c_str() << "' start_lba="
                 << static_cast<unsigned long long>(partition_entry.first_lba);

    const partition_view partition {
        .device = device,
        .start_lba = partition_entry.first_lba,
        .sector_count = partition_entry.last_lba - partition_entry.first_lba + 1,
    };

    if (!try_mount_partition(partition, callback, context)) {
        return false;
    }

    log::debug() << "mounts: mounted GPT partition index="
                 << static_cast<unsigned long long>(partition_index) << " on device='"
                 << device->name.c_str() << "'";
    return true;
}

static auto try_mount_any_gpt_partition(block_device* device,
                                        const GPTHeader& gpt,
                                        partition_mount_callback callback,
                                        void* context) -> bool {
    log::debug() << "mounts: GPT detected on device='" << device->name.c_str()
                 << "' entries=" << static_cast<unsigned long long>(gpt.partition_entry_count)
                 << " entry_lba=" << static_cast<unsigned long long>(gpt.partition_entry_lba);

    for (u32 entry_index = 0; entry_index < gpt.partition_entry_count; ++entry_index) {
        if (try_mount_specific_gpt_partition(device, gpt, entry_index, callback, context)) {
            return true;
        }
    }

    return false;
}

static auto try_mount_mbr_partitions(block_device* device,
                                     const u8* sector0,
                                     partition_mount_callback callback,
                                     void* context) -> bool {
    const auto* partitions = reinterpret_cast<const MBRPartitionEntry*>(sector0 + 446);
    for (usize partition_index = 0; partition_index < 4; ++partition_index) {
        if (partitions[partition_index].first_lba == 0 || partitions[partition_index].sector_count == 0) {
            continue;
        }

        log::debug() << "mounts: trying MBR partition index="
                     << static_cast<unsigned long long>(partition_index) << " on device='"
                     << device->name.c_str() << "' start_lba="
                     << static_cast<unsigned long long>(partitions[partition_index].first_lba);

        const partition_view partition {
            .device = device,
            .start_lba = partitions[partition_index].first_lba,
            .sector_count = partitions[partition_index].sector_count,
        };

        if (!try_mount_partition(partition, callback, context)) {
            continue;
        }

        log::debug() << "mounts: mounted MBR partition index="
                     << static_cast<unsigned long long>(partition_index) << " on device='"
                     << device->name.c_str() << "'";
        return true;
    }

    return false;
}

} // namespace

auto mount_gpt_partition_index(u32 partition_index,
                               partition_mount_callback callback,
                               void* context) -> status_code {
    const usize device_count = block::device_count();

    for (usize device_index = 0; device_index < device_count; ++device_index) {
        auto* device = block::get_device(device_index);
        if (device == null || device->block_size != 512) {
            continue;
        }

        GPTHeader gpt {};
        if (!read_gpt_header(device, gpt)) {
            continue;
        }

        if (try_mount_specific_gpt_partition(device, gpt, partition_index, callback, context)) {
            return status_code::success;
        }
    }

    return status_code::error;
}

auto mount_first_available(partition_mount_callback callback,
                           void* context) -> status_code {
    const usize device_count = block::device_count();

    for (usize device_index = 0; device_index < device_count; ++device_index) {
        auto* device = block::get_device(device_index);
        if (device == null || device->block_size != 512) {
            continue;
        }

        if (try_mount_raw_device(device, callback, context)) {
            return status_code::success;
        }

        u8 sector0[512];
        if (!block::read_blocks(device, 0, 1, sector0) || sector0[510] != 0x55 || sector0[511] != 0xAA) {
            continue;
        }

        GPTHeader gpt {};
        if (read_gpt_header(device, gpt) && try_mount_any_gpt_partition(device, gpt, callback, context)) {
            return status_code::success;
        }

        if (try_mount_mbr_partitions(device, sector0, callback, context)) {
            return status_code::success;
        }
    }

    return status_code::error;
}

} // namespace mounts
} // namespace fs
} // namespace vk