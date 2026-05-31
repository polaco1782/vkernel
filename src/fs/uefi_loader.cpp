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

using efi_file_protocol = uefi::efi_file_protocol;
using efi_sfs_protocol = uefi::efi_sfs_protocol;
using efi_loaded_image_protocol = uefi::efi_loaded_image_protocol;
using efi_file_info = uefi::efi_file_info;

struct efi_file_handle_deleter {
    void operator()(efi_file_protocol* file) const noexcept {
        if (file != null && file->close != null) {
            file->close(file);
        }
    }
};

struct bootstrap_ramfs_entry {
    const char* esp_path;
    const char* ramfs_path;
};

static constexpr bootstrap_ramfs_entry k_bootstrap_ramfs_entries[] = {
    { "\\boot\\vkernel.elf.map", "/boot/vkernel.elf.map" },
    { "\\boot\\vkernel.elf.lines", "/boot/vkernel.elf.lines" },
};

constexpr u64 EFI_FILE_MODE_READ = 0x0000000000000001ULL;

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

static auto open_sfs_root(void* sfs_iface, const char* source_name) -> efi_file_protocol* {
    if (sfs_iface == null) {
        return null;
    }

    auto* sfs = static_cast<efi_sfs_protocol*>(sfs_iface);
    efi_file_protocol* root = null;
    const auto status = sfs->open_volume(sfs, &root);
    if (status != uefi::status::success || root == null) {
        log::warn() << "Failed to open " << source_name << " volume";
        return null;
    }

    return root;
}

static auto open_esp_root() -> efi_file_protocol* {
    if (uefi::g_system_table == null || uefi::g_system_table->boot_services == null) {
        return null;
    }

    auto* boot_services = uefi::g_system_table->boot_services;
    if (uefi::g_image_handle != null) {
        void* loaded_image_iface = null;
        auto status = boot_services->handle_protocol(uefi::g_image_handle, &uefi::LOADED_IMAGE_GUID, &loaded_image_iface);
        if (status == uefi::status::success && loaded_image_iface != null) {
            const auto* loaded_image = static_cast<efi_loaded_image_protocol*>(loaded_image_iface);
            if (loaded_image->device_handle != null) {
                void* image_sfs_iface = null;
                status = boot_services->handle_protocol(loaded_image->device_handle, &uefi::SFS_GUID, &image_sfs_iface);
                if (status == uefi::status::success && image_sfs_iface != null) {
                    auto* root = open_sfs_root(image_sfs_iface, "loaded image filesystem");
                    if (root != null) {
                        return root;
                    }
                }
            }
        }
    }

    void* sfs_iface = null;
    auto status = boot_services->locate_protocol(&uefi::SFS_GUID, null, &sfs_iface);
    if (status != uefi::status::success || sfs_iface == null) {
        log::warn() << "SFS protocol not found";
        return null;
    }

    return open_sfs_root(sfs_iface, "fallback Simple File System");
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

    usize loaded = 0;
    for (const auto& entry : k_bootstrap_ramfs_entries) {
        auto result = load_file_from_esp(entry.esp_path);
        if (result.data == null || result.size == 0) {
            log::warn() << "Failed to load bootstrap file: " << entry.esp_path;
            continue;
        }

        efi_pool_ptr file_data(result.data, efi_pool_deleter { .boot_services = uefi::g_system_table->boot_services });
        if (ramfs::add_file_nocopy(string_view(entry.ramfs_path), file_data.get(), result.size) != status_code::success) {
            log::warn() << "Failed to add bootstrap file to RAMFS: " << entry.ramfs_path;
            continue;
        }

        (void)file_data.release();
        ++loaded;
        log::info() << "Loaded: " << entry.ramfs_path << " ("
                    << static_cast<unsigned long long>(result.size) << " bytes)";
    }

    log::info() << static_cast<unsigned long long>(loaded) << " file(s) loaded from ESP";
    return status_code::success;
}

} // namespace vk
