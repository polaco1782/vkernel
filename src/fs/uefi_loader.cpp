/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * uefi_loader.cpp - UEFI Simple File System fallback loader
 */

#include "config.h"
#include "console.h"
#include "fs.h"
#include "fs/ramfs.h"
#include "log.h"
#include "memory.h"
#include "resource_ptr.h"
#include "types.h"
#include "uefi.h"

namespace vk {
namespace {

struct efi_file_protocol;

using efi_file_open_fn = VK_MSABI uefi::status(*)(
    efi_file_protocol* self,
    efi_file_protocol** new_handle,
    const char16_t* file_name,
    u64 open_mode,
    u64 attributes);
using efi_file_close_fn = VK_MSABI uefi::status(*)(efi_file_protocol* self);
using efi_file_read_fn = VK_MSABI uefi::status(*)(efi_file_protocol* self, usize* buffer_size, void* buffer);
using efi_file_write_fn = VK_MSABI uefi::status(*)(efi_file_protocol* self, usize* buffer_size, const void* buffer);
using efi_file_set_position_fn = VK_MSABI uefi::status(*)(efi_file_protocol* self, u64 position);
using efi_file_get_info_fn = VK_MSABI uefi::status(*)(efi_file_protocol* self, const uefi::guid* info_type, usize* buffer_size, void* buffer);

struct efi_file_protocol {
    u64 revision;
    efi_file_open_fn open;
    efi_file_close_fn close;
    void* del;
    efi_file_read_fn read;
    efi_file_write_fn write;
    void* get_position;
    efi_file_set_position_fn set_position;
    efi_file_get_info_fn get_info;
};

struct efi_file_handle_deleter {
    void operator()(efi_file_protocol* file) const noexcept {
        if (file != null && file->close != null) {
            file->close(file);
        }
    }
};

struct efi_sfs_protocol;

using efi_sfs_open_volume_fn = VK_MSABI uefi::status(*)(efi_sfs_protocol* self, efi_file_protocol** root);

struct efi_sfs_protocol {
    u64 revision;
    efi_sfs_open_volume_fn open_volume;
};

struct efi_time {
    u16 year;
    u8 month;
    u8 day;
    u8 hour;
    u8 minute;
    u8 second;
    u8 pad1;
    u32 nanosecond;
    i16 time_zone;
    u8 daylight;
    u8 reserved;
};

struct efi_file_info {
    u64 size;
    u64 file_size;
    u64 physical_size;
    efi_time create_time;
    efi_time last_access_time;
    efi_time modification_time;
    u64 attribute;
    char16_t file_name[1];
};

static auto is_bootstrap_ramfs_name(const char* name) -> bool {
    return string_view(name).equals(string_view("shell.vbin"))
        || string_view(name).equals(string_view("shell.vbin.lines"))
        || string_view(name).equals(string_view("vkernel.elf.map"));
}

constexpr u64 EFI_FILE_MODE_READ = 0x0000000000000001ULL;
constexpr u64 EFI_FILE_DIRECTORY = 0x10;

static char16_t s_ucs2_buf[256];

static auto to_ucs2(const char* ascii) -> const char16_t* {
    usize i = 0;
    while (ascii[i] != '\0' && i < 255) {
        s_ucs2_buf[i] = ascii[i] == '/' ? u'\\' : static_cast<char16_t>(ascii[i]);
        ++i;
    }
    s_ucs2_buf[i] = 0;
    return s_ucs2_buf;
}

static void ucs2_to_ascii(const char16_t* src, char* dst, usize max) {
    usize i = 0;
    while (i + 1 < max && src[i] != 0) {
        const char16_t ch = src[i];
        dst[i] = (ch >= 32 && ch < 127) ? static_cast<char>(ch) : '?';
        ++i;
    }
    dst[i] = '\0';
}

static auto is_dot_entry(const char* name) -> bool {
    return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
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
    if (uefi::g_system_table == null || uefi::g_system_table->boot_services == null) {
        return null;
    }

    auto* boot_services = uefi::g_system_table->boot_services;
    void* sfs_iface = null;
    auto status = boot_services->locate_protocol(&uefi::SFS_GUID, null, &sfs_iface);
    if (status != uefi::status::success || sfs_iface == null) {
        log::warn() << "SFS protocol not found";
        return null;
    }

    auto* sfs = static_cast<efi_sfs_protocol*>(sfs_iface);
    efi_file_protocol* root = null;
    status = sfs->open_volume(sfs, &root);
    if (status != uefi::status::success || root == null) {
        log::warn() << "Failed to open ESP volume";
        return null;
    }

    return root;
}

template <typename Visitor>
static auto for_each_directory_entry(efi_file_protocol* directory, Visitor&& visit) -> status_code {
    if (directory == null) return status_code::invalid_param;

    if (directory->set_position != null) {
        auto status = directory->set_position(directory, 0);
        if (status != uefi::status::success) {
            log::error() << "Failed to rewind directory (status="
                         << static_cast<unsigned long long>(status) << ")";
            return status_code::error;
        }
    }

    u8 info_buf[1024];
    while (true) {
        usize info_size = sizeof(info_buf);
        auto status = directory->read(directory, &info_size, info_buf);
        if (status != uefi::status::success) {
            log::error() << "Failed reading ESP directory (status="
                         << static_cast<unsigned long long>(status) << ")";
            return status_code::error;
        }
        if (info_size == 0) {
            break;
        }

        auto* file_info = reinterpret_cast<efi_file_info*>(info_buf);
        char name[256];
        ucs2_to_ascii(file_info->file_name, name, sizeof(name));
        if (is_dot_entry(name)) {
            continue;
        }

        auto result = visit(*file_info, name);
        if (result != status_code::success) {
            return result;
        }
    }

    return status_code::success;
}

static auto build_esp_path(char* dst, usize max, const char* directory, const char* name) -> bool {
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

} // namespace

auto loader::load_file_from_esp(const char* path) -> loaded_file {
    efi_file_ptr root(open_esp_root());
    if (!root) {
        return { null, 0 };
    }

    auto* boot_services = uefi::g_system_table->boot_services;
    efi_file_protocol* file = null;
    auto status = root->open(root.get(), &file, to_ucs2(path), EFI_FILE_MODE_READ, 0);
    if (status != uefi::status::success || file == null) {
        log::warn() << "ESP file not found: " << path;
        return { null, 0 };
    }
    efi_file_ptr file_handle(file);

    u8 info_buf[256];
    usize info_size = sizeof(info_buf);
    status = file_handle->get_info(file_handle.get(), &uefi::FILE_INFO_GUID, &info_size, info_buf);
    if (status != uefi::status::success) {
        log::warn() << "GetInfo failed for " << path;
        return { null, 0 };
    }

    auto* info = reinterpret_cast<efi_file_info*>(info_buf);
    const usize file_size = static_cast<usize>(info->file_size);
    if (file_size == 0) {
        return { null, 0 };
    }

    void* raw_buffer = null;
    status = boot_services->allocate_pool(2, file_size, &raw_buffer);
    if (status != uefi::status::success || raw_buffer == null) {
        log::warn() << "AllocatePool failed for " << path;
        return { null, 0 };
    }

    efi_pool_ptr buffer(static_cast<u8*>(raw_buffer), efi_pool_deleter { .boot_services = boot_services });
    usize read_size = file_size;
    status = file_handle->read(file_handle.get(), &read_size, buffer.get());
    if (status != uefi::status::success || read_size != file_size) {
        log::warn() << "Read failed for " << path;
        return { null, 0 };
    }

    log::debug() << "ESP read OK: " << path << " -> "
                 << reinterpret_cast<const void*>(buffer.get());
    return { buffer.release(), file_size };
}

auto loader::load_initrd() -> status_code {
    log::info() << "Loading files from ESP...";

    ramfs::init();

    efi_file_ptr root(open_esp_root());
    if (!root) {
        return status_code::error;
    }

    constexpr const char* initrd_dir = "\\EFI\\vkernel";
    efi_file_protocol* directory = null;
    auto status = root->open(root.get(), &directory, to_ucs2(initrd_dir), EFI_FILE_MODE_READ, 0);
    if (status != uefi::status::success || directory == null) {
        log::error() << "Failed to open ESP directory: " << initrd_dir;
        return status_code::error;
    }
    efi_file_ptr directory_handle(directory);

    usize loaded = 0;
    auto rc = for_each_directory_entry(directory_handle.get(), [&](const efi_file_info& info, const char* name) {
        if ((info.attribute & EFI_FILE_DIRECTORY) != 0) {
            log::debug() << "Skipping directory: " << name;
            return status_code::success;
        }
        if (!is_bootstrap_ramfs_name(name)) {
            return status_code::success;
        }

        char path[256];
        if (!build_esp_path(path, sizeof(path), initrd_dir, name)) {
            log::warn() << "Skipping overlong ESP path: " << name;
            return status_code::success;
        }

        auto result = load_file_from_esp(path);
        if (result.data == null || result.size == 0) {
            log::warn() << "Failed to load: " << path;
            return status_code::success;
        }

        efi_pool_ptr file_data(result.data, efi_pool_deleter { .boot_services = uefi::g_system_table->boot_services });
        if (ramfs::add_file_nocopy(name, file_data.get(), result.size) != status_code::success) {
            log::warn() << "Failed to add to RAMFS: " << name;
            return status_code::success;
        }

        (void)file_data.release();
        ++loaded;
        log::info() << "Loaded: " << name << " ("
                    << static_cast<unsigned long long>(result.size) << " bytes)";
        return status_code::success;
    });

    log::info() << static_cast<unsigned long long>(loaded) << " file(s) loaded from ESP";
    return rc;
}

} // namespace vk
