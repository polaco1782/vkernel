/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * fat32.cpp - FAT32 block filesystem driver
 *
 * The layout and cluster math are based on SerenityOS FATFS, adapted down to
 * the freestanding primitives available in vkernel.
 */

#include "fs/fat32.h"

#include "arch/x86_64/arch.h"
#include "block.h"
#include "log.h"
#include "memory.h"

namespace vk {
namespace fat32 {
namespace {

static constexpr u32 FAT32_MIN_CLUSTER_COUNT = 65525;
static constexpr u32 FAT32_END_OF_CHAIN = 0x0FFFFFF8u;
static constexpr u32 FAT32_FSINFO_SIGNATURE_1 = 0x41615252u;
static constexpr u32 FAT32_FSINFO_SIGNATURE_2 = 0x61417272u;
static constexpr u32 FAT32_FSINFO_SIGNATURE_3 = 0xAA550000u;
static constexpr u32 FAT32_FSINFO_UNKNOWN = 0xFFFFFFFFu;
static constexpr u32 FAT_CACHE_SECTOR_COUNT = 8;
static constexpr usize MAX_LFN_ENTRIES = 20;
static constexpr usize MAX_DIRECTORY_SLOTS = MAX_LFN_ENTRIES + 1;

static constexpr u8 ATTR_READ_ONLY = 0x01;
static constexpr u8 ATTR_HIDDEN = 0x02;
static constexpr u8 ATTR_SYSTEM = 0x04;
static constexpr u8 ATTR_VOLUME_ID = 0x08;
static constexpr u8 ATTR_DIRECTORY = 0x10;
static constexpr u8 ATTR_ARCHIVE = 0x20;
static constexpr u8 ATTR_LONG_FILE_NAME = 0x0F;
static constexpr u8 CASE_INFO_BASE_LOWER = 0x08;
static constexpr u8 CASE_INFO_EXT_LOWER = 0x10;

static constexpr u8 DIR_ENTRY_END = 0x00;
static constexpr u8 DIR_ENTRY_UNUSED = 0xE5;
static constexpr u8 LFN_LAST_ENTRY_MASK = 0x40;

static u8 s_empty_file_marker = 0;

#pragma pack(push, 1)

struct DOS3BIOSParameterBlock {
    u8 boot_jump[3];
    char oem_identifier[8];
    u16 bytes_per_sector;
    u8 sectors_per_cluster;
    u16 reserved_sector_count;
    u8 fat_count;
    u16 root_directory_entry_count;
    u16 sector_count_16bit;
    u8 media_descriptor_type;
    u16 sectors_per_fat_16bit;
    u16 sectors_per_track;
    u16 head_count;
    u32 hidden_sector_count;
    u32 sector_count_32bit;
};

struct DOS7BIOSParameterBlock {
    u32 sectors_per_fat_32bit;
    u16 flags;
    u16 fat_version;
    u32 root_directory_cluster;
    u16 fs_info_sector;
    u16 backup_boot_sector;
    u8 unused3[12];
    u8 drive_number;
    u8 unused4;
    u8 signature;
    u32 volume_id;
    char volume_label_string[11];
    char file_system_type[8];
};

struct FAT32FSInfo {
    u32 lead_signature;
    u8 unused1[480];
    u32 struct_signature;
    u32 last_known_free_cluster_count;
    u32 next_free_cluster_hint;
    u8 unused2[12];
    u32 trailing_signature;
};

struct FATEntry {
    char filename[8];
    char extension[3];
    u8 attributes;
    u8 case_info;
    u8 creation_time_tenths;
    u16 creation_time;
    u16 creation_date;
    u16 last_accessed_date;
    u16 first_cluster_high;
    u16 modification_time;
    u16 modification_date;
    u16 first_cluster_low;
    u32 file_size;
};

struct FATLongFileNameEntry {
    u8 order;
    u16 name1[5];
    u8 attributes;
    u8 entry_type;
    u8 checksum;
    u16 name2[6];
    u16 first_cluster_low;
    u16 name3[2];
};

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

static_assert(sizeof(DOS3BIOSParameterBlock) == 36);
static_assert(sizeof(DOS7BIOSParameterBlock) == 54);
static_assert(sizeof(FAT32FSInfo) == 512);
static_assert(sizeof(FATEntry) == 32);
static_assert(sizeof(FATLongFileNameEntry) == 32);
static_assert(sizeof(MBRPartitionEntry) == 16);

struct entry_location {
    u64 lba = 0;
    u16 offset = 0;
};

struct directory_entry_ref {
    FATEntry entry {};
    entry_location location {};
    entry_location lfn_locations[MAX_LFN_ENTRIES] {};
    u8 lfn_count = 0;
    static_string<256> name;
};

struct pending_lfn_state {
    FATLongFileNameEntry entries[MAX_LFN_ENTRIES] {};
    entry_location locations[MAX_LFN_ENTRIES] {};
    u8 count = 0;

    void clear() {
        count = 0;
    }

    void push(const FATLongFileNameEntry& entry, entry_location location) {
        if (count >= MAX_LFN_ENTRIES) {
            clear();
            return;
        }
        entries[count] = entry;
        locations[count] = location;
        ++count;
    }
};

struct mount_state {
    bool initialised = false;
    bool mounted = false;
    bool fs_info_valid = false;
    block_device* device = null;
    mount_info info {};
    u32 volume_root_cluster = 0;
    u32 total_clusters = 0;
    u32 free_cluster_count = FAT32_FSINFO_UNKNOWN;
    u32 next_free_cluster_hint = FAT32_FSINFO_UNKNOWN;
    mutable bool fat_cache_valid = false;
    mutable u32 fat_cache_first_sector = 0;
    mutable u32 fat_cache_sector_count = 0;
    mutable u8 fat_cache[FAT_CACHE_SECTOR_COUNT * 512] {};
};

static mount_state s_state;

struct atomic_lock {
    volatile u64 locked = 0;

    void acquire() {
        while (!arch::atomic_cmpxchg(&locked, 0, 1)) {
            arch::cpu_pause();
        }
        arch::memory_barrier();
    }

    void release() {
        arch::memory_barrier();
        locked = 0;
    }
};

struct atomic_lock_guard {
    atomic_lock& lock;

    explicit atomic_lock_guard(atomic_lock& lock_in)
        : lock(lock_in) {
        lock.acquire();
    }

    ~atomic_lock_guard() {
        lock.release();
    }
};

static atomic_lock s_fat_cache_lock;

static void reset_state(mount_state& state) {
    state = {};
    state.initialised = true;
    (void)state.info.filesystem_name.assign("fat32");
    (void)state.info.logical_root_path.assign("/");
}

static auto is_separator(char ch) -> bool {
    return ch == '/' || ch == '\\';
}

static auto ascii_upper(char ch) -> char {
    if (ch >= 'a' && ch <= 'z') {
        return static_cast<char>(ch - 'a' + 'A');
    }
    return ch;
}

static auto ascii_lower(char ch) -> char {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<char>(ch - 'A' + 'a');
    }
    return ch;
}

static auto is_ascii_alnum(char ch) -> bool {
    const char upper = ascii_upper(ch);
    return (upper >= 'A' && upper <= 'Z') || (ch >= '0' && ch <= '9');
}

static auto normalize_path(string_view path) -> string_view {
    while (path.size() >= 2 && path[0] == '.' && is_separator(path[1])) {
        path.remove_prefix(2);
    }
    return path;
}

static auto trim_trailing_separators(string_view path) -> string_view {
    usize end = path.size();
    while (end > 1 && is_separator(path[end - 1])) {
        --end;
    }
    return string_view(path.data(), end);
}

static auto is_absolute_path(string_view path) -> bool {
    return path.size() > 0 && is_separator(path[0]);
}

static auto read_sectors(const mount_state& state, u64 lba, u32 count, void* buffer) -> bool {
    return state.device != null && buffer != null && block::read_blocks(state.device, lba, count, buffer);
}

static auto write_sectors(const mount_state& state, u64 lba, u32 count, const void* buffer) -> bool {
    return state.device != null && buffer != null && block::write_blocks(state.device, lba, count, buffer);
}

static auto cluster_first_lba(const mount_state& state, u32 cluster) -> u64 {
    return state.info.partition_start_lba + state.info.first_data_sector
        + (static_cast<u64>(cluster - 2) * state.info.sectors_per_cluster);
}

static auto cluster_size_bytes(const mount_state& state) -> usize {
    return static_cast<usize>(state.info.bytes_per_sector) * state.info.sectors_per_cluster;
}

static auto first_cluster(const FATEntry& entry) -> u32 {
    return (static_cast<u32>(entry.first_cluster_high) << 16) | entry.first_cluster_low;
}

static auto is_directory(const FATEntry& entry) -> bool {
    return (entry.attributes & ATTR_DIRECTORY) != 0;
}

static auto set_lfn_character(FATLongFileNameEntry& entry, usize index, u16 value) -> void {
    if (index < 5) {
        entry.name1[index] = value;
    } else if (index < 11) {
        entry.name2[index - 5] = value;
    } else {
        entry.name3[index - 11] = value;
    }
}

static auto append_lfn_character(char* out, usize cap, usize& pos, u16 value) -> bool {
    if (value == 0x0000) {
        return true;
    }
    if (value == 0xFFFF) {
        return false;
    }
    if (pos + 1 >= cap) {
        return true;
    }
    out[pos++] = value < 0x80 ? static_cast<char>(value) : '?';
    return false;
}

static auto lfn_checksum(const FATEntry& entry) -> u8 {
    u8 checksum = 0;
    for (usize i = 0; i < 8; ++i) {
        checksum = static_cast<u8>((checksum << 7) + (checksum >> 1) + static_cast<u8>(entry.filename[i]));
    }
    for (usize i = 0; i < 3; ++i) {
        checksum = static_cast<u8>((checksum << 7) + (checksum >> 1) + static_cast<u8>(entry.extension[i]));
    }
    return checksum;
}

static auto decode_short_name(const FATEntry& entry, static_string<256>& out) -> bool {
    char name[256];
    usize pos = 0;
    const bool lower_base = (entry.case_info & CASE_INFO_BASE_LOWER) != 0;
    const bool lower_ext = (entry.case_info & CASE_INFO_EXT_LOWER) != 0;

    for (usize i = 0; i < 8; ++i) {
        if (entry.filename[i] == ' ') {
            break;
        }
        name[pos++] = lower_base ? ascii_lower(entry.filename[i]) : entry.filename[i];
    }

    if (entry.extension[0] != ' ') {
        name[pos++] = '.';
        for (usize i = 0; i < 3; ++i) {
            if (entry.extension[i] == ' ') {
                break;
            }
            name[pos++] = lower_ext ? ascii_lower(entry.extension[i]) : entry.extension[i];
        }
    }

    name[pos] = '\0';
    return out.assign(string_view(name, pos));
}

static auto decode_lfn_name(const FATEntry& entry, const pending_lfn_state& pending, static_string<256>& out) -> bool {
    if (pending.count == 0) {
        return false;
    }

    const u8 expected_checksum = lfn_checksum(entry);
    for (u8 i = 0; i < pending.count; ++i) {
        if (pending.entries[i].checksum != expected_checksum) {
            return false;
        }
    }

    char name[256];
    usize pos = 0;
    bool done = false;

    for (u8 idx = pending.count; idx > 0 && !done; --idx) {
        const auto& current = pending.entries[idx - 1];
        for (usize i = 0; i < 5 && !done; ++i) {
            done = append_lfn_character(name, sizeof(name), pos, current.name1[i]);
        }
        for (usize i = 0; i < 6 && !done; ++i) {
            done = append_lfn_character(name, sizeof(name), pos, current.name2[i]);
        }
        for (usize i = 0; i < 2 && !done; ++i) {
            done = append_lfn_character(name, sizeof(name), pos, current.name3[i]);
        }
    }

    name[pos] = '\0';
    return out.assign(string_view(name, pos));
}

static auto next_path_component(string_view path, usize& offset, string_view& component) -> bool {
    while (offset < path.size() && is_separator(path[offset])) {
        ++offset;
    }
    if (offset >= path.size()) {
        return false;
    }

    const usize start = offset;
    while (offset < path.size() && !is_separator(path[offset])) {
        ++offset;
    }
    component = string_view(path.data() + start, offset - start);
    return true;
}

static auto write_entry_bytes(const mount_state& state, entry_location location, const void* data, usize size) -> bool {
    if (size == 0 || location.offset + size > state.info.bytes_per_sector) {
        return false;
    }

    u8 sector[512];
    if (!read_sectors(state, location.lba, 1, sector)) {
        return false;
    }

    memory::copy(sector + location.offset, data, size);
    return write_sectors(state, location.lba, 1, sector);
}

static auto mark_entry_unused(const mount_state& state, entry_location location) -> bool {
    u8 sector[512];
    if (!read_sectors(state, location.lba, 1, sector)) {
        return false;
    }
    sector[location.offset] = DIR_ENTRY_UNUSED;
    return write_sectors(state, location.lba, 1, sector);
}

static auto read_cached_fat_sector(const mount_state& state, u32 sector_index, const u8*& sector_out) -> bool {
    if (sector_index >= state.info.sectors_per_fat) {
        return false;
    }

    const bool cache_hit = state.fat_cache_valid
        && sector_index >= state.fat_cache_first_sector
        && sector_index < state.fat_cache_first_sector + state.fat_cache_sector_count;

    if (!cache_hit) {
        u32 first_sector = sector_index & ~(FAT_CACHE_SECTOR_COUNT - 1);
        u32 sector_count = state.info.sectors_per_fat - first_sector;
        if (sector_count > FAT_CACHE_SECTOR_COUNT) {
            sector_count = FAT_CACHE_SECTOR_COUNT;
        }

        log::debug() << "fat32: FAT cache fill sector=["
                     << static_cast<unsigned long long>(first_sector) << ".."
                     << static_cast<unsigned long long>(first_sector + sector_count - 1) << "]";

        if (!read_sectors(state, state.info.partition_start_lba + state.info.reserved_sector_count + first_sector, sector_count, state.fat_cache)) {
            return false;
        }

        state.fat_cache_first_sector = first_sector;
        state.fat_cache_sector_count = sector_count;
        state.fat_cache_valid = true;
    }

    sector_out = state.fat_cache + ((sector_index - state.fat_cache_first_sector) * state.info.bytes_per_sector);
    return true;
}

static auto read_fat_value(const mount_state& state, u32 cluster, u32& value_out) -> bool {
    const u32 fat_offset = cluster * 4;
    const u32 fat_sector = fat_offset / state.info.bytes_per_sector;
    const u32 entry_offset = fat_offset % state.info.bytes_per_sector;

    atomic_lock_guard cache_guard(s_fat_cache_lock);

    const u8* sector = null;
    if (!read_cached_fat_sector(state, fat_sector, sector)) {
        return false;
    }

    const auto* value = reinterpret_cast<const u32*>(sector + entry_offset);
    value_out = *value & 0x0FFFFFFFu;
    return true;
}

static auto write_fat_value(const mount_state& state, u32 cluster, u32 value) -> bool {
    const u32 fat_offset = cluster * 4;
    const u32 fat_sector = fat_offset / state.info.bytes_per_sector;
    const u32 entry_offset = fat_offset % state.info.bytes_per_sector;

    for (u32 copy_index = 0; copy_index < state.info.fat_count; ++copy_index) {
        const u64 lba = state.info.partition_start_lba
            + state.info.reserved_sector_count
            + (copy_index * state.info.sectors_per_fat)
            + fat_sector;

        u8 sector[512];
        if (!read_sectors(state, lba, 1, sector)) {
            return false;
        }

        auto* current_value = reinterpret_cast<u32*>(sector + entry_offset);
        *current_value = (*current_value & 0xF0000000u) | (value & 0x0FFFFFFFu);

        if (!write_sectors(state, lba, 1, sector)) {
            return false;
        }

        if (copy_index == 0) {
            atomic_lock_guard cache_guard(s_fat_cache_lock);
            const bool cache_hit = state.fat_cache_valid
                && fat_sector >= state.fat_cache_first_sector
                && fat_sector < state.fat_cache_first_sector + state.fat_cache_sector_count;
            if (cache_hit) {
                memory::copy(state.fat_cache
                                 + ((fat_sector - state.fat_cache_first_sector) * state.info.bytes_per_sector),
                             sector,
                             sizeof(sector));
            }
        }
    }

    return true;
}

static auto update_fsinfo(mount_state& state, u32 free_cluster_count, u32 next_free_hint) -> bool {
    if (!state.fs_info_valid || state.info.fs_info_sector == 0) {
        state.free_cluster_count = free_cluster_count;
        state.next_free_cluster_hint = next_free_hint;
        return true;
    }

    FAT32FSInfo fs_info {};
    fs_info.lead_signature = FAT32_FSINFO_SIGNATURE_1;
    fs_info.struct_signature = FAT32_FSINFO_SIGNATURE_2;
    fs_info.last_known_free_cluster_count = free_cluster_count;
    fs_info.next_free_cluster_hint = next_free_hint;
    fs_info.trailing_signature = FAT32_FSINFO_SIGNATURE_3;

    if (!write_sectors(state, state.info.partition_start_lba + state.info.fs_info_sector, 1, &fs_info)) {
        return false;
    }

    state.free_cluster_count = free_cluster_count;
    state.next_free_cluster_hint = next_free_hint;
    return true;
}

static auto allocate_cluster(mount_state& state, u32& cluster_out) -> bool {
    const u32 max_cluster = state.total_clusters + 1;
    u32 start_cluster = state.next_free_cluster_hint;
    if (start_cluster < 2 || start_cluster > max_cluster) {
        start_cluster = 2;
    }

    for (u32 pass = 0; pass < 2; ++pass) {
        for (u32 cluster = start_cluster; cluster <= max_cluster; ++cluster) {
            u32 value = 0;
            if (!read_fat_value(state, cluster, value)) {
                return false;
            }
            if (value != 0) {
                continue;
            }

            if (!write_fat_value(state, cluster, FAT32_END_OF_CHAIN)) {
                return false;
            }

            const u32 free_cluster_count = state.free_cluster_count == FAT32_FSINFO_UNKNOWN
                ? FAT32_FSINFO_UNKNOWN
                : state.free_cluster_count - 1;
            u32 next_hint = cluster + 1;
            if (next_hint < 2 || next_hint > max_cluster) {
                next_hint = 2;
            }
            if (!update_fsinfo(state, free_cluster_count, next_hint)) {
                return false;
            }

            cluster_out = cluster;
            return true;
        }
        start_cluster = 2;
    }

    return false;
}

static auto free_cluster_chain(mount_state& state, u32 first_cluster_value) -> bool {
    if (first_cluster_value < 2 || first_cluster_value >= FAT32_END_OF_CHAIN) {
        return true;
    }

    u32 current = first_cluster_value;
    u32 freed = 0;
    while (current >= 2 && current < FAT32_END_OF_CHAIN) {
        u32 next = 0;
        if (!read_fat_value(state, current, next)) {
            return false;
        }
        if (!write_fat_value(state, current, 0)) {
            return false;
        }
        ++freed;
        if (next < 2 || next >= FAT32_END_OF_CHAIN) {
            break;
        }
        current = next;
    }

    const u32 free_cluster_count = state.free_cluster_count == FAT32_FSINFO_UNKNOWN
        ? FAT32_FSINFO_UNKNOWN
        : state.free_cluster_count + freed;
    const u32 next_hint = state.next_free_cluster_hint == FAT32_FSINFO_UNKNOWN
        ? first_cluster_value
        : (first_cluster_value < state.next_free_cluster_hint ? first_cluster_value : state.next_free_cluster_hint);
    return update_fsinfo(state, free_cluster_count, next_hint);
}

static auto zero_cluster(const mount_state& state, u32 cluster) -> bool {
    u8 sector[512];
    memory::set(sector, 0, sizeof(sector));
    for (u32 sector_index = 0; sector_index < state.info.sectors_per_cluster; ++sector_index) {
        if (!write_sectors(state, cluster_first_lba(state, cluster) + sector_index, 1, sector)) {
            return false;
        }
    }
    return true;
}

static auto append_directory_cluster(mount_state& state, u32 last_cluster, u32& new_cluster_out) -> bool {
    u32 new_cluster = 0;
    if (!allocate_cluster(state, new_cluster)) {
        return false;
    }
    if (!zero_cluster(state, new_cluster)) {
        (void)free_cluster_chain(state, new_cluster);
        return false;
    }
    if (!write_fat_value(state, last_cluster, new_cluster)) {
        (void)free_cluster_chain(state, new_cluster);
        return false;
    }
    new_cluster_out = new_cluster;
    return true;
}

template <typename Callback>
static auto scan_directory(const mount_state& state, u32 directory_cluster, Callback&& callback) -> bool {
    if (directory_cluster < 2) {
        return false;
    }

    pending_lfn_state pending {};
    u32 cluster = directory_cluster;

    while (cluster >= 2 && cluster < FAT32_END_OF_CHAIN) {
        for (u32 sector_index = 0; sector_index < state.info.sectors_per_cluster; ++sector_index) {
            const u64 lba = cluster_first_lba(state, cluster) + sector_index;
            u8 sector[512];
            if (!read_sectors(state, lba, 1, sector)) {
                return false;
            }

            const u32 entries_per_sector = state.info.bytes_per_sector / sizeof(FATEntry);
            for (u32 entry_index = 0; entry_index < entries_per_sector; ++entry_index) {
                const auto location = entry_location { lba, static_cast<u16>(entry_index * sizeof(FATEntry)) };
                auto* raw_entry = reinterpret_cast<FATEntry*>(sector + location.offset);
                const u8 first_byte = static_cast<u8>(raw_entry->filename[0]);

                if (first_byte == DIR_ENTRY_END) {
                    return true;
                }
                if (first_byte == DIR_ENTRY_UNUSED) {
                    pending.clear();
                    continue;
                }

                if (raw_entry->attributes == ATTR_LONG_FILE_NAME) {
                    pending.push(*reinterpret_cast<FATLongFileNameEntry*>(raw_entry), location);
                    continue;
                }

                if ((raw_entry->attributes & ATTR_VOLUME_ID) != 0 && (raw_entry->attributes & ATTR_DIRECTORY) == 0) {
                    pending.clear();
                    continue;
                }

                directory_entry_ref entry_ref {};
                entry_ref.entry = *raw_entry;
                entry_ref.location = location;
                entry_ref.lfn_count = pending.count;
                for (u8 i = 0; i < pending.count; ++i) {
                    entry_ref.lfn_locations[i] = pending.locations[i];
                }

                if (!decode_lfn_name(entry_ref.entry, pending, entry_ref.name)) {
                    if (!decode_short_name(entry_ref.entry, entry_ref.name)) {
                        pending.clear();
                        continue;
                    }
                }

                pending.clear();
                if (callback(entry_ref)) {
                    return true;
                }
            }
        }

        u32 next_cluster = 0;
        if (!read_fat_value(state, cluster, next_cluster)) {
            return false;
        }
        if (next_cluster < 2 || next_cluster >= FAT32_END_OF_CHAIN) {
            return true;
        }
        cluster = next_cluster;
    }

    return true;
}

static auto find_in_directory(const mount_state& state, u32 directory_cluster, string_view name, directory_entry_ref& entry_out) -> bool {
    bool found = false;
    const bool ok = scan_directory(state, directory_cluster, [&](const directory_entry_ref& entry) {
        if (name.compare(entry.name.view())) {
            entry_out = entry;
            found = true;
            return true;
        }
        return false;
    });
    return ok && found;
}

static auto resolve_path_from_cluster(const mount_state& state, u32 start_cluster, string_view raw_path, directory_entry_ref& entry_out) -> bool {
    string_view path = trim_trailing_separators(normalize_path(raw_path));
    usize offset = 0;
    string_view component;
    u32 current_cluster = start_cluster;
    bool saw_component = false;

    log::debug() << "fat32: resolving path '" << path << "' from cluster "
                 << static_cast<unsigned long long>(start_cluster);

    while (next_path_component(path, offset, component)) {
        saw_component = true;
        if (component.compare(".")) {
            continue;
        }

        directory_entry_ref current_entry {};
        if (!find_in_directory(state, current_cluster, component, current_entry)) {
            log::debug() << "fat32: resolve miss component='" << component << "' in cluster "
                         << static_cast<unsigned long long>(current_cluster);
            return false;
        }

        log::debug() << "fat32: resolve component='" << component << "' -> name='"
                     << current_entry.name.view() << "' cluster="
                     << static_cast<unsigned long long>(first_cluster(current_entry.entry))
                     << " dir=" << (is_directory(current_entry.entry) ? "yes" : "no")
                     << " size=" << static_cast<unsigned long long>(current_entry.entry.file_size);

        usize lookahead = offset;
        string_view ignored;
        if (!next_path_component(path, lookahead, ignored)) {
            entry_out = current_entry;
            return true;
        }

        if (!is_directory(current_entry.entry)) {
            return false;
        }
        current_cluster = first_cluster(current_entry.entry);
        if (current_cluster < 2) {
            return false;
        }
    }

    return saw_component && false;
}

static auto resolve_path(const mount_state& state, string_view raw_path, directory_entry_ref& entry_out) -> bool {
    string_view path = trim_trailing_separators(normalize_path(raw_path));
    if (path.empty()) {
        return false;
    }

    if (is_absolute_path(path)) {
        while (!path.empty() && is_separator(path[0])) {
            path.remove_prefix(1);
        }
        if (path.empty()) {
            return false;
        }
        return resolve_path_from_cluster(state, state.info.logical_root_cluster, path, entry_out);
    }

    return resolve_path_from_cluster(state, state.info.logical_root_cluster, path, entry_out);
}

static auto resolve_directory_cluster(const mount_state& state, string_view raw_path, u32& cluster_out) -> bool {
    string_view path = trim_trailing_separators(normalize_path(raw_path));

    if (path.empty() || path.compare(".")) {
        cluster_out = state.info.logical_root_cluster;
        return cluster_out >= 2;
    }

    if (is_absolute_path(path)) {
        while (!path.empty() && is_separator(path[0])) {
            path.remove_prefix(1);
        }
        if (path.empty()) {
            cluster_out = state.info.logical_root_cluster;
            return cluster_out >= 2;
        }
    }

    directory_entry_ref entry {};
    if (!resolve_path(state, raw_path, entry) || !is_directory(entry.entry)) {
        return false;
    }

    cluster_out = first_cluster(entry.entry);
    return cluster_out >= 2;
}

static auto resolve_parent_directory(const mount_state& state, string_view raw_path, u32& directory_cluster_out, static_string<256>& leaf_name_out) -> bool {
    string_view path = trim_trailing_separators(normalize_path(raw_path));
    if (path.empty()) {
        return false;
    }

    usize last_separator = path.size();
    for (usize i = 0; i < path.size(); ++i) {
        if (is_separator(path[i])) {
            last_separator = i;
        }
    }

    string_view leaf;
    if (last_separator >= path.size()) {
        leaf = path;
        directory_cluster_out = state.info.logical_root_cluster;
    } else {
        leaf = string_view(path.data() + last_separator + 1, path.size() - last_separator - 1);
        if (leaf.empty()) {
            return false;
        }

        if (last_separator == 0) {
            directory_cluster_out = state.info.logical_root_cluster;
        } else {
            directory_entry_ref parent_directory {};
            const string_view parent_path(path.data(), last_separator);
            if (!resolve_path(state, parent_path, parent_directory) || !is_directory(parent_directory.entry)) {
                return false;
            }
            directory_cluster_out = first_cluster(parent_directory.entry);
        }
    }

    return leaf_name_out.assign(leaf);
}

static auto is_valid_sfn_char(char ch) -> bool {
    const char upper = ascii_upper(ch);
    if (is_ascii_alnum(upper)) {
        return true;
    }
    switch (upper) {
        case '$':
        case '%':
        case '\'':
        case '-':
        case '_':
        case '@':
        case '~':
        case '!':
        case '(': 
        case ')':
        case '^':
        case '#':
        case '&':
            return true;
        default:
            return false;
    }
}

static auto split_name_for_alias(string_view name, string_view& base_out, string_view& extension_out) -> void {
    usize dot_index = name.size();
    for (usize i = 0; i < name.size(); ++i) {
        if (name[i] == '.') {
            dot_index = i;
        }
    }

    if (dot_index < name.size()) {
        base_out = string_view(name.data(), dot_index);
        extension_out = string_view(name.data() + dot_index + 1, name.size() - dot_index - 1);
    } else {
        base_out = name;
        extension_out = {};
    }
}

static auto is_valid_sfn_name(string_view name) -> bool {
    if (name.empty()) {
        return false;
    }

    usize dot_count = 0;
    for (usize i = 0; i < name.size(); ++i) {
        if (is_separator(name[i]) || name[i] == ' ') {
            return false;
        }
        if (name[i] == '.') {
            ++dot_count;
        }
    }
    if (dot_count > 1) {
        return false;
    }

    string_view base;
    string_view extension;
    split_name_for_alias(name, base, extension);
    if (base.empty() || base.size() > 8 || extension.size() > 3) {
        return false;
    }

    for (usize i = 0; i < base.size(); ++i) {
        if (!is_valid_sfn_char(base[i])) {
            return false;
        }
    }
    for (usize i = 0; i < extension.size(); ++i) {
        if (!is_valid_sfn_char(extension[i])) {
            return false;
        }
    }

    return true;
}

static auto encode_sfn(FATEntry& entry, string_view name) -> void {
    memory::set(entry.filename, ' ', sizeof(entry.filename));
    memory::set(entry.extension, ' ', sizeof(entry.extension));

    string_view base;
    string_view extension;
    split_name_for_alias(name, base, extension);

    for (usize i = 0; i < base.size() && i < sizeof(entry.filename); ++i) {
        entry.filename[i] = ascii_upper(base[i]);
    }
    for (usize i = 0; i < extension.size() && i < sizeof(entry.extension); ++i) {
        entry.extension[i] = ascii_upper(extension[i]);
    }
}

static auto component_has_lowercase(string_view value) -> bool {
    for (usize i = 0; i < value.size(); ++i) {
        if (value[i] >= 'a' && value[i] <= 'z') {
            return true;
        }
    }
    return false;
}

static auto encode_case_info(string_view name) -> u8 {
    string_view base;
    string_view extension;
    split_name_for_alias(name, base, extension);

    u8 case_info = 0;
    if (!base.empty() && component_has_lowercase(base)) {
        case_info |= CASE_INFO_BASE_LOWER;
    }
    if (!extension.empty() && component_has_lowercase(extension)) {
        case_info |= CASE_INFO_EXT_LOWER;
    }
    return case_info;
}

static auto sanitize_component(string_view input, char* output, usize output_cap) -> usize {
    usize out = 0;
    for (usize i = 0; i < input.size() && out < output_cap; ++i) {
        const char upper = ascii_upper(input[i]);
        if (is_ascii_alnum(upper) || upper == '_' || upper == '$' || upper == '-' || upper == '~') {
            output[out++] = upper;
        } else if (input[i] == ' ' || input[i] == '.') {
            continue;
        } else {
            output[out++] = '_';
        }
    }
    return out;
}

static auto render_decimal(u32 value, char* out, usize out_cap) -> usize {
    char tmp[16];
    usize count = 0;
    if (value == 0) {
        if (out_cap > 0) {
            out[0] = '0';
        }
        return 1;
    }
    while (value > 0 && count < sizeof(tmp)) {
        tmp[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    usize out_count = 0;
    while (count > 0 && out_count < out_cap) {
        out[out_count++] = tmp[--count];
    }
    return out_count;
}

static auto sfn_exists(const mount_state& state, u32 directory_cluster, const char alias[11]) -> bool {
    bool found = false;
    (void)scan_directory(state, directory_cluster, [&](const directory_entry_ref& entry) {
        char current_alias[11];
        memory::copy(current_alias, entry.entry.filename, 8);
        memory::copy(current_alias + 8, entry.entry.extension, 3);
        if (memory::compare(current_alias, alias, sizeof(current_alias)) == 0) {
            found = true;
            return true;
        }
        return false;
    });
    return found;
}

static auto generate_unique_sfn(const mount_state& state, u32 directory_cluster, string_view name, FATEntry& entry_out) -> bool {
    string_view base_name;
    string_view extension_name;
    split_name_for_alias(name, base_name, extension_name);

    char base[64];
    char extension[16];
    usize base_len = sanitize_component(base_name, base, sizeof(base));
    const usize ext_len = sanitize_component(extension_name, extension, sizeof(extension));
    if (base_len == 0) {
        base[0] = 'F';
        base[1] = 'I';
        base[2] = 'L';
        base[3] = 'E';
        base_len = 4;
    }

    for (u32 unique = 1; unique < 1000; ++unique) {
        char alias[11];
        memory::set(alias, ' ', sizeof(alias));

        char unique_digits[4];
        const usize unique_len = render_decimal(unique, unique_digits, sizeof(unique_digits));
        usize prefix_len = 8 - 1 - unique_len;
        if (prefix_len > base_len) {
            prefix_len = base_len;
        }
        if (prefix_len == 0) {
            prefix_len = 1;
        }

        memory::copy(alias, base, prefix_len);
        alias[prefix_len] = '~';
        memory::copy(alias + prefix_len + 1, unique_digits, unique_len);
        memory::copy(alias + 8, extension, ext_len > 3 ? 3 : ext_len);

        if (sfn_exists(state, directory_cluster, alias)) {
            continue;
        }

        memory::copy(entry_out.filename, alias, 8);
        memory::copy(entry_out.extension, alias + 8, 3);
        return true;
    }

    return false;
}

static auto create_lfn_entries(string_view name, u8 checksum, FATLongFileNameEntry* entries, u8& entry_count_out) -> bool {
    const usize entry_count = (name.size() + 12) / 13;
    if (entry_count == 0 || entry_count > MAX_LFN_ENTRIES) {
        return false;
    }

    entry_count_out = static_cast<u8>(entry_count);
    for (usize index = 0; index < entry_count; ++index) {
        FATLongFileNameEntry entry {};
        entry.order = static_cast<u8>(index + 1);
        if (index + 1 == entry_count) {
            entry.order = static_cast<u8>(entry.order | LFN_LAST_ENTRY_MASK);
        }
        entry.attributes = ATTR_LONG_FILE_NAME;
        entry.entry_type = 0;
        entry.checksum = checksum;
        entry.first_cluster_low = 0;

        for (usize char_index = 0; char_index < 13; ++char_index) {
            set_lfn_character(entry, char_index, 0xFFFF);
        }

        const usize start = index * 13;
        usize length = name.size() - start;
        if (length > 13) {
            length = 13;
        }
        for (usize char_index = 0; char_index < length; ++char_index) {
            set_lfn_character(entry, char_index, static_cast<u8>(name[start + char_index]));
        }
        if (length < 13) {
            set_lfn_character(entry, length, 0x0000);
        }

        entries[index] = entry;
    }

    return true;
}

static auto build_file_entry(const mount_state& state,
                             u32 directory_cluster,
                             string_view name,
                             FATEntry& entry_out,
                             FATLongFileNameEntry* lfn_entries,
                             u8& lfn_count_out) -> bool {
    entry_out = {};
    entry_out.attributes = ATTR_ARCHIVE;
    lfn_count_out = 0;

    if (is_valid_sfn_name(name)) {
        encode_sfn(entry_out, name);
        entry_out.case_info = encode_case_info(name);
        return true;
    }

    if (!generate_unique_sfn(state, directory_cluster, name, entry_out)) {
        return false;
    }

    return create_lfn_entries(name, lfn_checksum(entry_out), lfn_entries, lfn_count_out);
}

static auto allocate_directory_slots(mount_state& state, u32 directory_cluster, u32 needed, entry_location* locations_out) -> bool {
    if (directory_cluster < 2 || needed == 0 || needed > MAX_DIRECTORY_SLOTS) {
        return false;
    }

    const u32 entries_per_sector = state.info.bytes_per_sector / sizeof(FATEntry);
    u32 run_count = 0;
    bool end_zone = false;
    u32 cluster = directory_cluster;

    while (cluster >= 2 && cluster < FAT32_END_OF_CHAIN) {
        for (u32 sector_index = 0; sector_index < state.info.sectors_per_cluster; ++sector_index) {
            const u64 lba = cluster_first_lba(state, cluster) + sector_index;
            u8 sector[512];
            if (!read_sectors(state, lba, 1, sector)) {
                return false;
            }

            for (u32 entry_index = 0; entry_index < entries_per_sector; ++entry_index) {
                const auto location = entry_location { lba, static_cast<u16>(entry_index * sizeof(FATEntry)) };
                bool free_slot = end_zone;
                const u8 first_byte = sector[location.offset];

                if (!free_slot) {
                    if (first_byte == DIR_ENTRY_END) {
                        end_zone = true;
                        free_slot = true;
                    } else if (first_byte == DIR_ENTRY_UNUSED) {
                        free_slot = true;
                    }
                }

                if (free_slot) {
                    locations_out[run_count++] = location;
                    if (run_count == needed) {
                        return true;
                    }
                } else {
                    run_count = 0;
                }
            }
        }

        u32 next_cluster = 0;
        if (!read_fat_value(state, cluster, next_cluster)) {
            return false;
        }
        if (next_cluster < 2 || next_cluster >= FAT32_END_OF_CHAIN) {
            u32 new_cluster = 0;
            if (!append_directory_cluster(state, cluster, new_cluster)) {
                return false;
            }
            cluster = new_cluster;
            end_zone = true;
            continue;
        }

        cluster = next_cluster;
    }

    return false;
}

static auto read_file_data(const mount_state& state,
                           const directory_entry_ref& entry,
                           kernel_heap_ptr<u8>& owned_buffer,
                           usize& size_out,
                           const u8*& data_out) -> bool {
    size_out = static_cast<usize>(entry.entry.file_size);
    if (size_out == 0) {
        owned_buffer.reset();
        data_out = &s_empty_file_marker;
        return true;
    }

    log::debug() << "fat32: reading file '" << entry.name.view() << "' size="
                 << static_cast<unsigned long long>(size_out) << " bytes";

    const u32 first = first_cluster(entry.entry);
    if (first < 2 || first >= FAT32_END_OF_CHAIN) {
        return false;
    }

    log::debug() << "fat32: file '" << entry.name.view() << "' first_cluster="
                 << static_cast<unsigned long long>(first) << " cluster_bytes="
                 << static_cast<unsigned long long>(cluster_size_bytes(state));

    auto* raw_buffer = static_cast<u8*>(g_kernel_heap.allocate(size_out));
    if (raw_buffer == null) {
        return false;
    }
    owned_buffer = kernel_heap_ptr<u8>(raw_buffer);

    const usize cluster_bytes = cluster_size_bytes(state);
    usize copied = 0;
    u32 cluster = first;
    while (cluster >= 2 && cluster < FAT32_END_OF_CHAIN && copied < size_out) {
        const usize remaining = size_out - copied;
        if (remaining >= cluster_bytes) {
            usize full_clusters_remaining = remaining / cluster_bytes;
            u32 run_cluster_count = 1;
            u32 run_last_cluster = cluster;
            u32 next_cluster = FAT32_END_OF_CHAIN;

            while (run_cluster_count < full_clusters_remaining) {
                u32 candidate = 0;
                if (!read_fat_value(state, run_last_cluster, candidate)) {
                    owned_buffer.reset();
                    return false;
                }
                if (candidate != run_last_cluster + 1) {
                    next_cluster = candidate;
                    break;
                }
                run_last_cluster = candidate;
                ++run_cluster_count;
            }

            const u64 run_start_lba = cluster_first_lba(state, cluster);
            const u32 run_sector_count = run_cluster_count * state.info.sectors_per_cluster;

            const u64 run_end_lba = run_start_lba + run_sector_count - 1;
            log::debug() << "fat32: read run clusters=["
                            << static_cast<unsigned long long>(cluster) << ".."
                            << static_cast<unsigned long long>(run_last_cluster) << "] lba=["
                            << static_cast<unsigned long long>(run_start_lba) << ".."
                            << static_cast<unsigned long long>(run_end_lba) << "] sectors="
                            << static_cast<unsigned long long>(run_sector_count) << " bytes="
                            << static_cast<unsigned long long>(static_cast<u64>(run_cluster_count) * cluster_bytes);

            if (!read_sectors(state,
                              run_start_lba,
                              run_sector_count,
                              owned_buffer.get() + copied)) {
                owned_buffer.reset();
                return false;
            }

            copied += run_cluster_count * cluster_bytes;
            if (copied >= size_out) {
                break;
            }

            if (next_cluster >= FAT32_END_OF_CHAIN) {
                if (!read_fat_value(state, run_last_cluster, next_cluster)) {
                    owned_buffer.reset();
                    return false;
                }
            }

            if (next_cluster >= 2 && next_cluster < FAT32_END_OF_CHAIN) {
                log::debug() << "fat32: cluster chain discontinuity after "
                             << static_cast<unsigned long long>(run_last_cluster) << " -> "
                             << static_cast<unsigned long long>(next_cluster);
            }

            if (next_cluster < 2 || next_cluster >= FAT32_END_OF_CHAIN) {
                owned_buffer.reset();
                return false;
            }

            cluster = next_cluster;
            continue;
        }

        const u64 cluster_lba = cluster_first_lba(state, cluster);
        const usize whole_sectors = remaining / state.info.bytes_per_sector;
        const usize tail_bytes = remaining % state.info.bytes_per_sector;

        log::debug() << "fat32: reading tail cluster=" << static_cast<unsigned long long>(cluster)
                     << " lba_start=" << static_cast<unsigned long long>(cluster_lba)
                     << " whole_sectors=" << static_cast<unsigned long long>(whole_sectors)
                     << " tail_bytes=" << static_cast<unsigned long long>(tail_bytes);

        if (whole_sectors > 0) {
            if (!read_sectors(state, cluster_lba, static_cast<u32>(whole_sectors), owned_buffer.get() + copied)) {
                owned_buffer.reset();
                return false;
            }
            copied += whole_sectors * state.info.bytes_per_sector;
        }

        if (tail_bytes > 0) {
            u8 sector[512];
            if (!read_sectors(state, cluster_lba + whole_sectors, 1, sector)) {
                owned_buffer.reset();
                return false;
            }
            memory::copy(owned_buffer.get() + copied, sector, tail_bytes);
            copied += tail_bytes;
        }

        break;
    }

    if (copied != size_out) {
        owned_buffer.reset();
        return false;
    }

    data_out = owned_buffer.get();
    return true;
}

static auto build_file_descriptor(const directory_entry_ref& entry, file_descriptor& file_out) -> bool {
    if (is_directory(entry.entry)) {
        return false;
    }

    file_out = {};
    file_out.valid = true;
    file_out.first_cluster = first_cluster(entry.entry);
    file_out.size = static_cast<usize>(entry.entry.file_size);
    if (file_out.size > 0 && (file_out.first_cluster < 2 || file_out.first_cluster >= FAT32_END_OF_CHAIN)) {
        file_out = {};
        return false;
    }

    return true;
}

static auto cluster_for_file_offset(const mount_state& state,
                                    file_descriptor& file,
                                    usize cluster_index,
                                    u32& cluster_out) -> bool {
    if (!file.valid || file.size == 0) {
        return false;
    }

    u32 cluster = file.first_cluster;
    usize current_index = 0;

    if (file.cluster_cache_valid && file.cluster_cache_index <= cluster_index) {
        cluster = file.cluster_cache_value;
        current_index = file.cluster_cache_index;
    }

    while (current_index < cluster_index) {
        u32 next_cluster = 0;
        if (!read_fat_value(state, cluster, next_cluster)) {
            return false;
        }
        if (next_cluster < 2 || next_cluster >= FAT32_END_OF_CHAIN) {
            return false;
        }
        cluster = next_cluster;
        ++current_index;
    }

    file.cluster_cache_valid = true;
    file.cluster_cache_index = current_index;
    file.cluster_cache_value = cluster;
    cluster_out = cluster;
    return true;
}

static auto read_file_range(const mount_state& state,
                            file_descriptor& file,
                            usize offset,
                            void* buffer,
                            usize size,
                            usize& size_out) -> bool {
    size_out = 0;
    if (!file.valid || (size > 0 && buffer == null)) {
        return false;
    }
    if (size == 0 || offset >= file.size) {
        return true;
    }

    auto* out = static_cast<u8*>(buffer);
    usize remaining = file.size - offset;
    if (remaining > size) {
        remaining = size;
    }

    const usize cluster_bytes = cluster_size_bytes(state);
    const usize sector_bytes = state.info.bytes_per_sector;

    while (size_out < remaining) {
        const usize absolute_offset = offset + size_out;
        const usize cluster_index = absolute_offset / cluster_bytes;
        const usize cluster_offset = absolute_offset % cluster_bytes;

        u32 cluster = 0;
        if (!cluster_for_file_offset(state, file, cluster_index, cluster)) {
            return false;
        }

        const usize bytes_left = remaining - size_out;
        if (cluster_offset == 0 && bytes_left >= cluster_bytes) {
            const usize max_run_clusters = bytes_left / cluster_bytes;
            u32 run_cluster_count = 1;
            u32 run_last_cluster = cluster;
            u32 next_cluster = FAT32_END_OF_CHAIN;

            while (run_cluster_count < max_run_clusters) {
                u32 candidate = 0;
                if (!read_fat_value(state, run_last_cluster, candidate)) {
                    return false;
                }
                if (candidate != run_last_cluster + 1) {
                    next_cluster = candidate;
                    break;
                }
                run_last_cluster = candidate;
                ++run_cluster_count;
            }

            const u32 run_sector_count = run_cluster_count * state.info.sectors_per_cluster;
            if (!read_sectors(state, cluster_first_lba(state, cluster), run_sector_count, out + size_out)) {
                return false;
            }

            size_out += static_cast<usize>(run_cluster_count) * cluster_bytes;
            file.cluster_cache_valid = true;
            if (next_cluster >= 2 && next_cluster < FAT32_END_OF_CHAIN && size_out < remaining) {
                file.cluster_cache_index = cluster_index + run_cluster_count;
                file.cluster_cache_value = next_cluster;
            } else {
                file.cluster_cache_index = cluster_index + run_cluster_count - 1;
                file.cluster_cache_value = run_last_cluster;
            }
            continue;
        }

        usize chunk = cluster_bytes - cluster_offset;
        if (chunk > bytes_left) {
            chunk = bytes_left;
        }

        usize chunk_done = 0;
        u64 lba = cluster_first_lba(state, cluster) + (cluster_offset / sector_bytes);
        usize sector_offset = cluster_offset % sector_bytes;

        if (sector_offset != 0) {
            u8 sector[512];
            if (!read_sectors(state, lba, 1, sector)) {
                return false;
            }
            usize first_chunk = sector_bytes - sector_offset;
            if (first_chunk > chunk) {
                first_chunk = chunk;
            }
            memory::copy(out + size_out, sector + sector_offset, first_chunk);
            size_out += first_chunk;
            chunk_done += first_chunk;
            ++lba;
        }

        const usize whole_sectors = (chunk - chunk_done) / sector_bytes;
        if (whole_sectors > 0) {
            if (!read_sectors(state, lba, static_cast<u32>(whole_sectors), out + size_out)) {
                return false;
            }
            const usize whole_bytes = whole_sectors * sector_bytes;
            size_out += whole_bytes;
            chunk_done += whole_bytes;
            lba += whole_sectors;
        }

        const usize tail_bytes = chunk - chunk_done;
        if (tail_bytes > 0) {
            u8 sector[512];
            if (!read_sectors(state, lba, 1, sector)) {
                return false;
            }
            memory::copy(out + size_out, sector, tail_bytes);
            size_out += tail_bytes;
        }
    }

    return true;
}

static auto write_file_data(mount_state& state, const u8* data, usize size, u32& first_cluster_out) -> bool {
    first_cluster_out = 0;
    if (size == 0) {
        return true;
    }

    log::debug() << "fat32: write_file_data size=" << static_cast<unsigned long long>(size);

    u32 first_cluster = 0;
    u32 previous_cluster = 0;
    usize written = 0;

    while (written < size) {
        u32 cluster = 0;
        if (!allocate_cluster(state, cluster)) {
            if (first_cluster != 0) {
                (void)free_cluster_chain(state, first_cluster);
            }
            return false;
        }

        log::debug() << "fat32: allocated cluster " << static_cast<unsigned long long>(cluster)
                     << " for write offset=" << static_cast<unsigned long long>(written);

        if (first_cluster == 0) {
            first_cluster = cluster;
        }

        if (previous_cluster != 0 && !write_fat_value(state, previous_cluster, cluster)) {
            (void)free_cluster_chain(state, first_cluster);
            return false;
        }
        previous_cluster = cluster;

        for (u32 sector_index = 0; sector_index < state.info.sectors_per_cluster; ++sector_index) {
            u8 sector[512];
            memory::set(sector, 0, sizeof(sector));

            if (written < size) {
                usize chunk = size - written;
                if (chunk > state.info.bytes_per_sector) {
                    chunk = state.info.bytes_per_sector;
                }
                memory::copy(sector, data + written, chunk);
                written += chunk;
            }

            if (!write_sectors(state, cluster_first_lba(state, cluster) + sector_index, 1, sector)) {
                (void)free_cluster_chain(state, first_cluster);
                return false;
            }
        }
    }

    first_cluster_out = first_cluster;
    return true;
}

static auto all_zero_guid(const u8 guid[16]) -> bool {
    for (usize i = 0; i < 16; ++i) {
        if (guid[i] != 0) {
            return false;
        }
    }
    return true;
}

static auto probe_volume(block_device* device, u64 start_lba, mount_state& state_out) -> bool {
    log::debug() << "fat32: probing device='" << (device != null ? device->name.c_str() : "<null>")
                 << "' start_lba=" << static_cast<unsigned long long>(start_lba);
    reset_state(state_out);
    state_out.device = device;
    state_out.mounted = true;
    state_out.info.mounted = true;
    state_out.info.writable = device->ops != null && device->ops->write_blocks != null;
    (void)state_out.info.block_device.assign(device->name.view());
    state_out.info.partition_start_lba = start_lba;

    u8 boot_sector[512];
    if (!read_sectors(state_out, start_lba, 1, boot_sector)) {
        log::debug() << "fat32: probe failed at start_lba="
                     << static_cast<unsigned long long>(start_lba)
                     << " because boot sector read failed";
        return false;
    }
    if (boot_sector[510] != 0x55 || boot_sector[511] != 0xAA) {
        log::debug() << "fat32: probe failed at start_lba="
                     << static_cast<unsigned long long>(start_lba)
                     << " because boot signature is missing";
        return false;
    }

    const auto* common = reinterpret_cast<const DOS3BIOSParameterBlock*>(boot_sector);
    const auto* dos7 = reinterpret_cast<const DOS7BIOSParameterBlock*>(boot_sector + 0x24);

    if (common->bytes_per_sector != device->block_size || common->bytes_per_sector != 512) {
        log::debug() << "fat32: probe failed at start_lba="
                     << static_cast<unsigned long long>(start_lba)
                     << " bytes_per_sector=" << static_cast<unsigned long long>(common->bytes_per_sector)
                     << " device_block_size=" << static_cast<unsigned long long>(device->block_size);
        return false;
    }
    if (common->sectors_per_cluster == 0 || (common->sectors_per_cluster & (common->sectors_per_cluster - 1)) != 0) {
        log::debug() << "fat32: probe failed at start_lba="
                     << static_cast<unsigned long long>(start_lba)
                     << " invalid sectors_per_cluster="
                     << static_cast<unsigned long long>(common->sectors_per_cluster);
        return false;
    }
    if (common->reserved_sector_count == 0 || common->fat_count == 0) {
        log::debug() << "fat32: probe failed at start_lba="
                     << static_cast<unsigned long long>(start_lba)
                     << " reserved=" << static_cast<unsigned long long>(common->reserved_sector_count)
                     << " fats=" << static_cast<unsigned long long>(common->fat_count);
        return false;
    }

    const u32 sectors_per_fat = common->sectors_per_fat_16bit != 0
        ? common->sectors_per_fat_16bit
        : dos7->sectors_per_fat_32bit;
    if (sectors_per_fat == 0) {
        return false;
    }

    const u32 total_sectors = common->sector_count_16bit != 0
        ? common->sector_count_16bit
        : common->sector_count_32bit;
    if (total_sectors == 0) {
        return false;
    }

    const u32 root_directory_sectors = ((static_cast<u32>(common->root_directory_entry_count) * sizeof(FATEntry))
        + (common->bytes_per_sector - 1))
        / common->bytes_per_sector;
    const u32 non_data_sectors = common->reserved_sector_count
        + (static_cast<u32>(common->fat_count) * sectors_per_fat)
        + root_directory_sectors;
    if (total_sectors <= non_data_sectors) {
        return false;
    }

    const u32 data_sectors = total_sectors - non_data_sectors;
    const u32 total_clusters = data_sectors / common->sectors_per_cluster;
    if (total_clusters < FAT32_MIN_CLUSTER_COUNT) {
        log::debug() << "fat32: probe failed at start_lba="
                     << static_cast<unsigned long long>(start_lba)
                     << " cluster_count=" << static_cast<unsigned long long>(total_clusters)
                     << " minimum=" << static_cast<unsigned long long>(FAT32_MIN_CLUSTER_COUNT)
                     << " total_sectors=" << static_cast<unsigned long long>(total_sectors)
                     << " sectors_per_cluster="
                     << static_cast<unsigned long long>(common->sectors_per_cluster);
        return false;
    }
    if (dos7->signature != 0x28 && dos7->signature != 0x29) {
        return false;
    }
    if (dos7->root_directory_cluster < 2) {
        return false;
    }

    state_out.info.total_sectors = total_sectors;
    state_out.info.bytes_per_sector = common->bytes_per_sector;
    state_out.info.sectors_per_cluster = common->sectors_per_cluster;
    state_out.info.fat_count = common->fat_count;
    state_out.info.reserved_sector_count = common->reserved_sector_count;
    state_out.info.sectors_per_fat = sectors_per_fat;
    state_out.info.first_data_sector = non_data_sectors;
    state_out.info.root_cluster = dos7->root_directory_cluster;
    state_out.info.logical_root_cluster = dos7->root_directory_cluster;
    state_out.info.fs_info_sector = dos7->fs_info_sector;
    state_out.volume_root_cluster = dos7->root_directory_cluster;
    state_out.total_clusters = total_clusters;
    state_out.free_cluster_count = FAT32_FSINFO_UNKNOWN;
    state_out.next_free_cluster_hint = FAT32_FSINFO_UNKNOWN;

    if (dos7->fs_info_sector != 0) {
        FAT32FSInfo fs_info {};
        if (read_sectors(state_out, start_lba + dos7->fs_info_sector, 1, &fs_info)
            && fs_info.lead_signature == FAT32_FSINFO_SIGNATURE_1
            && fs_info.struct_signature == FAT32_FSINFO_SIGNATURE_2
            && fs_info.trailing_signature == FAT32_FSINFO_SIGNATURE_3) {
            state_out.fs_info_valid = true;
            state_out.free_cluster_count = fs_info.last_known_free_cluster_count;
            state_out.next_free_cluster_hint = fs_info.next_free_cluster_hint;
        }
    }

    log::debug() << "fat32: probe success device='" << device->name.c_str() << "' start_lba="
                 << static_cast<unsigned long long>(start_lba) << " total_sectors="
                 << static_cast<unsigned long long>(total_sectors) << " sectors_per_cluster="
                 << static_cast<unsigned long long>(common->sectors_per_cluster) << " sectors_per_fat="
                 << static_cast<unsigned long long>(sectors_per_fat) << " root_cluster="
                 << static_cast<unsigned long long>(state_out.info.root_cluster) << " logical_root='"
                 << state_out.info.logical_root_path.c_str() << "' logical_cluster="
                 << static_cast<unsigned long long>(state_out.info.logical_root_cluster);

    return true;
}

} // namespace

void init() {
    if (s_state.initialised) {
        return;
    }
    reset_state(s_state);
}

auto mount_first_available() -> status_code {
    if (!s_state.initialised) {
        init();
    }

    mount_state candidate {};
    const usize device_count = block::device_count();

    for (usize device_index = 0; device_index < device_count; ++device_index) {
        auto* device = block::get_device(device_index);
        if (device == null || device->block_size != 512) {
            continue;
        }

        log::debug() << "fat32: trying raw probe on device='" << device->name.c_str() << "'";

        if (probe_volume(device, 0, candidate)) {
            log::debug() << "fat32: mounted raw volume on device='" << device->name.c_str() << "'";
            s_state = candidate;
            return status_code::success;
        }

        u8 sector0[512];
        if (!block::read_blocks(device, 0, 1, sector0) || sector0[510] != 0x55 || sector0[511] != 0xAA) {
            continue;
        }

        u8 sector1[512];
        if (block::read_blocks(device, 1, 1, sector1)) {
            const auto* gpt = reinterpret_cast<const GPTHeader*>(sector1);
            if (gpt->signature[0] == 'E' && gpt->signature[1] == 'F' && gpt->signature[2] == 'I'
                && gpt->signature[3] == ' ' && gpt->signature[4] == 'P' && gpt->signature[5] == 'A'
                && gpt->signature[6] == 'R' && gpt->signature[7] == 'T'
                && gpt->partition_entry_size >= sizeof(GPTPartitionEntry)
                && gpt->partition_entry_count > 0) {
                log::debug() << "fat32: GPT detected on device='" << device->name.c_str()
                             << "' entries=" << static_cast<unsigned long long>(gpt->partition_entry_count)
                             << " entry_lba=" << static_cast<unsigned long long>(gpt->partition_entry_lba);
                const u32 entries_per_sector = 512 / gpt->partition_entry_size;
                u8 entry_sector[512];
                u32 cached_sector_index = static_cast<u32>(-1);

                for (u32 entry_index = 0; entry_index < gpt->partition_entry_count; ++entry_index) {
                    const u32 sector_index = entry_index / entries_per_sector;
                    if (sector_index != cached_sector_index) {
                        if (!block::read_blocks(device, gpt->partition_entry_lba + sector_index, 1, entry_sector)) {
                            break;
                        }
                        cached_sector_index = sector_index;
                    }

                    const u32 entry_offset = (entry_index % entries_per_sector) * gpt->partition_entry_size;
                    const auto* partition = reinterpret_cast<const GPTPartitionEntry*>(entry_sector + entry_offset);
                    if (all_zero_guid(partition->type_guid) || partition->first_lba == 0 || partition->last_lba < partition->first_lba) {
                        continue;
                    }

                    log::debug() << "fat32: probing GPT partition index="
                                 << static_cast<unsigned long long>(entry_index) << " start_lba="
                                 << static_cast<unsigned long long>(partition->first_lba) << " end_lba="
                                 << static_cast<unsigned long long>(partition->last_lba);

                    if (probe_volume(device, partition->first_lba, candidate)) {
                        log::debug() << "fat32: mounted GPT partition index="
                                     << static_cast<unsigned long long>(entry_index) << " on device='"
                                     << device->name.c_str() << "'";
                        s_state = candidate;
                        return status_code::success;
                    }
                }
            }
        }

        const auto* partitions = reinterpret_cast<const MBRPartitionEntry*>(sector0 + 446);
        for (usize partition_index = 0; partition_index < 4; ++partition_index) {
            if (partitions[partition_index].first_lba == 0 || partitions[partition_index].sector_count == 0) {
                continue;
            }

            log::debug() << "fat32: probing MBR partition index="
                         << static_cast<unsigned long long>(partition_index) << " start_lba="
                         << static_cast<unsigned long long>(partitions[partition_index].first_lba)
                         << " sectors="
                         << static_cast<unsigned long long>(partitions[partition_index].sector_count);
            if (probe_volume(device, partitions[partition_index].first_lba, candidate)) {
                log::debug() << "fat32: mounted MBR partition index="
                             << static_cast<unsigned long long>(partition_index) << " on device='"
                             << device->name.c_str() << "'";
                s_state = candidate;
                return status_code::success;
            }
        }
    }

    reset_state(s_state);
    return status_code::error;
}

auto is_mounted() -> bool {
    return s_state.mounted;
}

auto info() -> mount_info {
    if (!s_state.initialised) {
        init();
    }
    return s_state.info;
}

auto file_exists(string_view path) -> bool {
    if (!s_state.mounted) {
        return false;
    }

    directory_entry_ref entry {};
    return resolve_path(s_state, path, entry) && !is_directory(entry.entry);
}

auto file_size(string_view path) -> usize {
    if (!s_state.mounted) {
        return 0;
    }

    directory_entry_ref entry {};
    if (!resolve_path(s_state, path, entry) || is_directory(entry.entry)) {
        return 0;
    }
    return static_cast<usize>(entry.entry.file_size);
}

auto directory_exists(string_view path) -> bool {
    if (!s_state.mounted) {
        return false;
    }

    u32 directory_cluster = 0;
    return resolve_directory_cluster(s_state, path, directory_cluster);
}

auto list_directory(string_view path, directory_visit_callback callback, void* context) -> bool {
    if (!s_state.mounted || callback == null) {
        return false;
    }

    u32 directory_cluster = 0;
    if (!resolve_directory_cluster(s_state, path, directory_cluster)) {
        log::debug() << "fat32: list_directory resolve failed path='" << path << "'";
        return false;
    }

    log::debug() << "fat32: list_directory path='" << path << "' cluster=" << directory_cluster;
    const bool ok = scan_directory(s_state, directory_cluster, [&](const directory_entry_ref& entry) {
        const auto name = entry.name.view();
        if (name.compare(".") || name.compare("..")) {
            return false;
        }

        directory_entry_info info {};
        if (!info.name.assign(name)) {
            return false;
        }
        info.is_directory = is_directory(entry.entry);
        info.size = static_cast<usize>(entry.entry.file_size);
        info.first_cluster = first_cluster(entry.entry);
        return callback(info, context);
    });

    if (!ok) {
        log::debug() << "fat32: list_directory scan failed path='" << path << "'";
    }
    return ok;
}

auto open_file(string_view path, file_descriptor& file_out) -> bool {
    file_out = {};
    if (!s_state.mounted) {
        return false;
    }

    directory_entry_ref entry {};
    if (!resolve_path(s_state, path, entry)) {
        return false;
    }

    return build_file_descriptor(entry, file_out);
}

auto read_file(file_descriptor& file, usize offset, void* buffer, usize size, usize& size_out) -> bool {
    if (!s_state.mounted) {
        size_out = 0;
        return false;
    }
    return read_file_range(s_state, file, offset, buffer, size, size_out);
}

auto read_file(string_view path, kernel_heap_ptr<u8>& owned_buffer, usize& size_out) -> const u8* {
    owned_buffer.reset();
    size_out = 0;

    if (!s_state.mounted) {
        log::debug() << "FAT32: Attempted to read file while no volume is mounted";
        return null;
    }

    directory_entry_ref entry {};
    if (!resolve_path(s_state, path, entry) || is_directory(entry.entry)) {
        log::debug() << "FAT32: Failed to resolve path or path is a directory: " << path;
        return null;
    }

    log::debug() << "fat32: read_file path='" << path << "' resolved_name='" << entry.name.view()
                 << "' first_cluster=" << static_cast<unsigned long long>(first_cluster(entry.entry))
                 << " size=" << static_cast<unsigned long long>(entry.entry.file_size);

    const u8* data = null;
    if (!read_file_data(s_state, entry, owned_buffer, size_out, data)) {
        owned_buffer.reset();
        size_out = 0;
        return null;
    }
    return data;
}

auto write_file(string_view path, const u8* data, usize size) -> bool {
    if (!s_state.mounted || !s_state.info.writable) {
        return false;
    }
    if (size > 0 && data == null) {
        return false;
    }

    const string_view normalized = trim_trailing_separators(normalize_path(path));
    if (normalized.empty()) {
        return false;
    }

    log::debug() << "fat32: write_file path='" << normalized << "' size="
                 << static_cast<unsigned long long>(size);

    directory_entry_ref existing_entry {};
    const bool exists = resolve_path(s_state, normalized, existing_entry);
    if (exists && is_directory(existing_entry.entry)) {
        return false;
    }

    u32 parent_directory_cluster = 0;
    static_string<256> leaf_name;
    if (!resolve_parent_directory(s_state, normalized, parent_directory_cluster, leaf_name)) {
        return false;
    }

    FATEntry entry {};
    FATLongFileNameEntry lfn_entries[MAX_LFN_ENTRIES] {};
    u8 lfn_count = 0;
    entry_location new_locations[MAX_DIRECTORY_SLOTS] {};

    if (exists) {
        entry = existing_entry.entry;
    } else {
        if (!build_file_entry(s_state, parent_directory_cluster, leaf_name.view(), entry, lfn_entries, lfn_count)) {
            return false;
        }
        if (!allocate_directory_slots(s_state, parent_directory_cluster, static_cast<u32>(lfn_count + 1), new_locations)) {
            return false;
        }
    }

    const u32 old_cluster = first_cluster(entry);
    if (exists && old_cluster >= 2 && old_cluster < FAT32_END_OF_CHAIN) {
        log::debug() << "fat32: replacing existing file path='" << normalized << "' old_cluster="
                     << static_cast<unsigned long long>(old_cluster);
        if (!free_cluster_chain(s_state, old_cluster)) {
            return false;
        }
    }

    u32 new_cluster = 0;
    if (!write_file_data(s_state, data, size, new_cluster)) {
        if (exists) {
            entry.first_cluster_low = 0;
            entry.first_cluster_high = 0;
            entry.file_size = 0;
            (void)write_entry_bytes(s_state, existing_entry.location, &entry, sizeof(entry));
        }
        return false;
    }

    entry.first_cluster_low = static_cast<u16>(new_cluster & 0xFFFFu);
    entry.first_cluster_high = static_cast<u16>(new_cluster >> 16);
    entry.file_size = static_cast<u32>(size);
    entry.attributes = static_cast<u8>((entry.attributes & ~(ATTR_DIRECTORY | ATTR_VOLUME_ID)) | ATTR_ARCHIVE);
    if (lfn_count == 0) {
        entry.case_info = encode_case_info(leaf_name.view());
    } else {
        entry.case_info = 0;
    }
    entry.creation_time_tenths = 0;

    if (exists) {
        log::debug() << "fat32: updated file path='" << normalized << "' new_cluster="
                     << static_cast<unsigned long long>(new_cluster) << " size="
                     << static_cast<unsigned long long>(size);
        return write_entry_bytes(s_state, existing_entry.location, &entry, sizeof(entry));
    }

    if (!write_entry_bytes(s_state, new_locations[lfn_count], &entry, sizeof(entry))) {
        return false;
    }
    for (u8 i = 0; i < lfn_count; ++i) {
        const auto& lfn_entry = lfn_entries[lfn_count - i - 1];
        if (!write_entry_bytes(s_state, new_locations[i], &lfn_entry, sizeof(lfn_entry))) {
            return false;
        }
    }

    log::debug() << "fat32: created file path='" << normalized << "' first_cluster="
                 << static_cast<unsigned long long>(new_cluster) << " size="
                 << static_cast<unsigned long long>(size) << " lfn_count="
                 << static_cast<unsigned long long>(lfn_count);

    return true;
}

auto remove_file(string_view path) -> bool {
    if (!s_state.mounted || !s_state.info.writable) {
        return false;
    }

    directory_entry_ref entry {};
    if (!resolve_path(s_state, path, entry) || is_directory(entry.entry)) {
        return false;
    }

    log::debug() << "fat32: remove_file path='" << path << "' resolved_name='" << entry.name.view()
                 << "' first_cluster=" << static_cast<unsigned long long>(first_cluster(entry.entry))
                 << " lfn_count=" << static_cast<unsigned long long>(entry.lfn_count);

    if (!mark_entry_unused(s_state, entry.location)) {
        return false;
    }
    for (u8 i = 0; i < entry.lfn_count; ++i) {
        if (!mark_entry_unused(s_state, entry.lfn_locations[i])) {
            return false;
        }
    }

    return free_cluster_chain(s_state, first_cluster(entry.entry));
}

} // namespace fat32
} // namespace vk
