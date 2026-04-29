/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * fs.cpp - Ramfs + UEFI ESP file loader implementation
 */

#include "config.h"
#include "types.h"
#include "uefi.h"
#include "memory.h"
#include "console.h"
#include "log.h"
#include "fs.h"
#include "resource_ptr.h"

namespace vk {

/* ============================================================
 * Ramfs — flat in-memory file table
 * ============================================================ */

static file_entry g_files[RAMFS_MAX_FILES];
static usize      g_file_count = 0;

auto ramfs::init() -> status_code {
    g_file_count = 0;
    memory::set(g_files, 0, sizeof(g_files));
    return status_code::success;
}

auto ramfs::add_file(string_view name, const u8* data, usize size) -> status_code {
    if (g_file_count >= RAMFS_MAX_FILES) return status_code::no_memory;
    if (name.data() == null || data == null) return status_code::invalid_param;

    auto& f = g_files[g_file_count];
    if (!f.name.assign(name)) return status_code::invalid_param;
    
    /* Allocate a copy in kernel heap */
    kernel_heap_ptr<u8> buf(static_cast<u8*>(g_kernel_heap.allocate(size)));
    if (!buf) return status_code::no_memory;
    memory::copy(buf.get(), data, size);

    f.data  = buf.release();
    f.size  = size;
    f.valid = true;
    ++g_file_count;

    log::debug() << "ramfs: added '" << f.name.c_str() << "' at heap=" << reinterpret_cast<const void*>(f.data) << " (" << size << " bytes)";

    return status_code::success;
}

auto ramfs::add_file(const char* name, const u8* data, usize size) -> status_code {
    return add_file(string_view(name), data, size);
}

auto ramfs::add_file_nocopy(string_view name, u8* data, usize size) -> status_code {
    if (g_file_count >= RAMFS_MAX_FILES) return status_code::no_memory;
    if (name.data() == null || data == null) return status_code::invalid_param;

    auto& f = g_files[g_file_count];
    if (!f.name.assign(name)) return status_code::invalid_param;
    f.data  = data;
    f.size  = size;
    f.valid = true;
    ++g_file_count;

    log::debug() << "ramfs: registered (nocopy) '" << f.name.c_str() << "' at " << reinterpret_cast<const void*>(data) << " (" << size << " bytes)";

    return status_code::success;
}

auto ramfs::add_file_nocopy(const char* name, u8* data, usize size) -> status_code {
    return add_file_nocopy(string_view(name), data, size);
}

auto ramfs::find(string_view name) -> const file_entry* {
    /* Normalize: strip leading "./" so "./doom2.wad" matches "doom2.wad" */
    if (name.starts_with("./")) {
        name.remove_prefix(2);
    }
    for (usize i = 0; i < g_file_count; ++i) {
        if (g_files[i].valid && g_files[i].name.view().equals(name))
            return &g_files[i];
    }
    return null;
}

auto ramfs::find(const char* name) -> const file_entry* {
    return find(string_view(name));
}

auto ramfs::file_count() -> usize { return g_file_count; }

auto ramfs::get_file(usize index) -> const file_entry* {
    if (index >= g_file_count) return null;
    return &g_files[index];
}

void ramfs::dump() {
    log::info() << "RAMFS: " << g_file_count << " file(s)";
    for (usize i = 0; i < g_file_count; ++i) {
        if (g_files[i].valid) {
            log::info() << "  [" << i << "] '" << g_files[i].name.c_str() << "' (" << g_files[i].size << " bytes)";
        }
    }
}

namespace {

/* EFI_FILE_PROTOCOL — subset of function pointers we need */
struct efi_file_protocol;

using efi_file_open_fn  = VK_MSABI uefi::status(*)(
    efi_file_protocol* self, efi_file_protocol** new_handle,
    const char16_t* file_name, u64 open_mode, u64 attributes);

using efi_file_close_fn = VK_MSABI uefi::status(*)(efi_file_protocol* self);

using efi_file_read_fn  = VK_MSABI uefi::status(*)(
    efi_file_protocol* self, usize* buffer_size, void* buffer);

using efi_file_write_fn = VK_MSABI uefi::status(*)(
    efi_file_protocol* self, usize* buffer_size, const void* buffer);

using efi_file_set_position_fn = VK_MSABI uefi::status(*)(
    efi_file_protocol* self, u64 position);

using efi_file_get_info_fn = VK_MSABI uefi::status(*)(
    efi_file_protocol* self, const uefi::guid* info_type,
    usize* buffer_size, void* buffer);

struct efi_file_protocol {
    u64                  revision;
    efi_file_open_fn     open;       /* offset  8 */
    efi_file_close_fn    close;      /* offset 16 */
    void*                del;        /* offset 24 */
    efi_file_read_fn     read;       /* offset 32 */
    efi_file_write_fn    write;      /* offset 40 */
    void*                get_position; /* offset 48 */
    efi_file_set_position_fn set_position; /* offset 56 */
    efi_file_get_info_fn get_info;   /* offset 64 */
    /* ... more we don't need */
};

struct efi_file_handle_deleter {
    void operator()(efi_file_protocol* file) const noexcept {
        if (file != null && file->close != null) {
            file->close(file);
        }
    }
};

/* EFI_SIMPLE_FILE_SYSTEM_PROTOCOL */
struct efi_sfs_protocol;

using efi_sfs_open_volume_fn = VK_MSABI uefi::status(*)(
    efi_sfs_protocol* self, efi_file_protocol** root);

struct efi_sfs_protocol {
    u64                     revision;
    efi_sfs_open_volume_fn  open_volume;
};

/* File info structure (variable-length, but we only need size) */
struct efi_time {
    u16 Year;
    u8  Month;
    u8  Day;
    u8  Hour;
    u8  Minute;
    u8  Second;
    u8  Pad1;
    u32 Nanosecond;
    i16 TimeZone;
    u8  Daylight;
    u8  Reserved;
};

struct efi_file_info {
    u64 size;         /* Size of this structure + filename */
    u64 file_size;
    u64 physical_size;
    efi_time create_time;
    efi_time last_access_time;
    efi_time modification_time;
    u64    attribute;
    char16_t FileName[1]; /* variable-length UCS-2 filename */
};

// Open modes
constexpr u64 EFI_FILE_MODE_READ = 0x0000000000000001ULL;
constexpr u64 EFI_FILE_MODE_WRITE = 0x0000000000000002ULL;
constexpr u64 EFI_FILE_MODE_CREATE = 0x8000000000000000ULL;

// File attributes
constexpr u64 EFI_FILE_READ_ONLY = 0x1;
constexpr u64 EFI_FILE_HIDDEN = 0x2;
constexpr u64 EFI_FILE_SYSTEM = 0x4;
constexpr u64 EFI_FILE_RESERVED = 0x8;
constexpr u64 EFI_FILE_DIRECTORY = 0x10;
constexpr u64 EFI_FILE_ARCHIVE = 0x20;

/* Convert ASCII path to UCS-2 in a static buffer */
static char16_t s_ucs2_buf[256];

static auto to_ucs2(const char* ascii) -> const char16_t* {
    usize i = 0;
    while (ascii[i] && i < 255) {
        /* Convert forward slashes to backslashes for UEFI */
        s_ucs2_buf[i] = (ascii[i] == '/') ? u'\\' : static_cast<char16_t>(ascii[i]);
        ++i;
    }
    s_ucs2_buf[i] = 0;
    return s_ucs2_buf;
}

static void ucs2_to_ascii(const char16_t* src, char* dst, usize max) {
    usize i = 0;
    while (i + 1 < max && src[i]) {
        char16_t c = src[i];
        if (c >= 32 && c < 127)
            dst[i] = static_cast<char>(c);
        else
            dst[i] = '?';
        ++i;
    }
    dst[i] = '\0';
}

static bool is_dot_entry(const char* name) {
    return name[0] == '.'
        && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

struct efi_pool_deleter {
    uefi::boot_services_table* boot_services = null;

    void operator()(u8* ptr) const noexcept {
        if (ptr != null && boot_services != null) {
            boot_services->free_pool(ptr);
        }
    }
};

using efi_file_ptr = unique_ptr<efi_file_protocol, efi_file_handle_deleter>;
using efi_pool_ptr = unique_ptr<u8, efi_pool_deleter>;

static auto open_esp_root() -> efi_file_protocol* {
    if (uefi::g_system_table == null || uefi::g_system_table->boot_services == null)
        return null;

    auto* bs = uefi::g_system_table->boot_services;

    void* sfs_iface = null;
    auto st = bs->locate_protocol(&uefi::SFS_GUID, null, &sfs_iface);
    if (st != uefi::status::success || sfs_iface == null) {
        log::warn() << "SFS protocol not found";
        return null;
    }

    auto* sfs = static_cast<efi_sfs_protocol*>(sfs_iface);
    efi_file_protocol* root = null;
    st = sfs->open_volume(sfs, &root);
    if (st != uefi::status::success || root == null) {
        log::warn() << "Failed to open ESP volume";
        return null;
    }

    return root;
}

template <typename Visitor>
static auto for_each_directory_entry(
    efi_file_protocol* directory,
    Visitor&& visit
) -> status_code {
    if (directory == null) return status_code::invalid_param;

    if (directory->set_position != null) {
        auto st = directory->set_position(directory, 0);
        if (st != uefi::status::success) {
            log::error() << "Failed to rewind directory (status=" << static_cast<unsigned long long>(st) << ")";
            return status_code::error;
        }
    }

    u8 info_buf[1024];
    while (true) {
        usize info_size = sizeof(info_buf);
        auto st = directory->read(directory, &info_size, info_buf);
        if (st != uefi::status::success) {
            log::error() << "Failed reading ESP directory (status=" << static_cast<unsigned long long>(st) << ")";
            return status_code::error;
        }

        if (info_size == 0) break;

        auto* fi = reinterpret_cast<efi_file_info*>(info_buf);
        char name[256];
        ucs2_to_ascii(fi->FileName, name, sizeof(name));

        if (is_dot_entry(name)) continue;

        auto rc = visit(*fi, name);
        if (rc != status_code::success) return rc;
    }

    return status_code::success;
}

static auto build_esp_path(
    char* dst,
    usize max,
    const char* directory,
    const char* name
) -> bool {
    if (dst == null || directory == null || name == null || max == 0) return false;

    usize out = 0;
    for (usize i = 0; directory[i] != '\0'; ++i) {
        if (out + 1 >= max) return false;
        dst[out++] = directory[i];
    }

    if (out == 0 || (dst[out - 1] != '\\' && dst[out - 1] != '/')) {
        if (out + 1 >= max) return false;
        dst[out++] = '\\';
    }

    for (usize i = 0; name[i] != '\0'; ++i) {
        if (out + 1 >= max) return false;
        dst[out++] = name[i];
    }

    dst[out] = '\0';
    return true;
}

} // anonymous namespace

auto loader::load_file_from_esp(const char* path) -> loaded_file {
    efi_file_ptr root(open_esp_root());
    if (!root)
        return { null, 0 };

    auto* bs = uefi::g_system_table->boot_services;

    /* Open the requested file */
    efi_file_protocol* file = null;
    auto st = root->open(root.get(), &file, to_ucs2(path), EFI_FILE_MODE_READ, 0);
    if (st != uefi::status::success || file == null) {
        log::warn() << "ESP file not found: " << path;
        return { null, 0 };
    }
    efi_file_ptr file_handle(file);

    /* Query file size via GetInfo */
    u8 info_buf[256];
    usize info_size = sizeof(info_buf);
    st = file_handle->get_info(file_handle.get(), &uefi::FILE_INFO_GUID, &info_size, info_buf);
    if (st != uefi::status::success) {
        log::warn() << "GetInfo failed for " << path;
        return { null, 0 };
    }
    auto* fi = reinterpret_cast<efi_file_info*>(info_buf);
    usize file_size = static_cast<usize>(fi->file_size);

    log::debug() << "ESP file '" << path << "': " << file_size << " bytes";

    if (file_size == 0) {
        return { null, 0 };
    }

    /* Allocate buffer via UEFI AllocatePool (EfiLoaderData = type 2) */
    void* buf = null;
    st = bs->allocate_pool(2 /* EfiLoaderData */, file_size, &buf);
    if (st != uefi::status::success || buf == null) {
        log::warn() << "AllocatePool failed for " << path;
        return { null, 0 };
    }
    efi_pool_ptr pool_buf(
        static_cast<u8*>(buf),
        efi_pool_deleter { .boot_services = bs });

    /* Read the file */
    usize read_size = file_size;
    st = file_handle->read(file_handle.get(), &read_size, pool_buf.get());

    if (st != uefi::status::success || read_size != file_size) {
        log::warn() << "Read failed for " << path;
        return { null, 0 };
    }

    log::debug() << "ESP read OK: " << path << " -> " << reinterpret_cast<const void*>(pool_buf.get());

    return { pool_buf.release(), file_size };
}

/* Load all regular files from the vkernel ESP directory into the ramfs. */
auto loader::load_initrd() -> status_code {
    log::info() << "Loading files from ESP...";

    ramfs::init();

    efi_file_ptr root(open_esp_root());
    if (!root) return status_code::error;

    constexpr const char* initrd_dir = "\\EFI\\vkernel";
    efi_file_protocol* dir = null;
    auto st = root->open(root.get(), &dir, to_ucs2(initrd_dir), EFI_FILE_MODE_READ, 0);
    if (st != uefi::status::success || dir == null) {
        log::error() << "Failed to open ESP directory: " << initrd_dir;
        return status_code::error;
    }
    efi_file_ptr dir_handle(dir);

    usize loaded = 0;
    auto rc = for_each_directory_entry(dir_handle.get(), [&](const efi_file_info& fi, const char* name) {
        if ((fi.attribute & EFI_FILE_DIRECTORY) != 0) {
            log::debug() << "Skipping directory: " << name;
            return status_code::success;
        }

        char path[256];
        if (!build_esp_path(path, sizeof(path), initrd_dir, name)) {
            log::warn() << "Skipping overlong ESP path: " << name;
            return status_code::success;
        }

        auto result = load_file_from_esp(path);
        if (result.data != null && result.size > 0) {
            efi_pool_ptr file_data(
                result.data,
                efi_pool_deleter { .boot_services = uefi::g_system_table->boot_services });
            if (ramfs::add_file_nocopy(name, file_data.get(), result.size) == status_code::success) {
                (void)file_data.release();
                log::info() << "Loaded: " << name << " (" << result.size << " bytes)";
                ++loaded;
            } else {
                log::warn() << "Failed to add to RAMFS: " << name;
            }
        } else {
            log::warn() << "Failed to load: " << path;
        }
        return status_code::success;
    });

    log::info() << loaded << " file(s) loaded from ESP";

    return rc;
}

} // namespace vk
