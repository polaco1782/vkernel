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
#include "fs.h"

namespace vk {

/* ============================================================
 * Ramfs — flat in-memory file table
 * ============================================================ */

static file_entry g_files[RAMFS_MAX_FILES];
static usize      g_file_count = 0;

/* Simple C-string helpers (no libc) */
static void str_copy(char* dst, const char* src, usize max) {
    usize i = 0;
    while (i + 1 < max && src[i]) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}

static bool str_equal(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return false;
        ++a; ++b;
    }
    return *a == *b;
}

auto ramfs::init() -> status_code {
    g_file_count = 0;
    memory::memory_set(g_files, 0, sizeof(g_files));
    return status_code::success;
}

auto ramfs::add_file(const char* name, const u8* data, usize size) -> status_code {
    if (g_file_count >= RAMFS_MAX_FILES) return status_code::no_memory;
    if (name == null || data == null) return status_code::invalid_param;

    auto& f = g_files[g_file_count];
    str_copy(f.name, name, sizeof(f.name));
    
    /* Allocate a copy in kernel heap */
    auto* buf = static_cast<u8*>(g_kernel_heap.allocate(size));
    if (buf == null) return status_code::no_memory;
    memory::memory_copy(buf, data, size);

    f.data  = buf;
    f.size  = size;
    f.valid = true;
    ++g_file_count;

#if VK_DEBUG_LEVEL >= 4
    console::puts("[DEBUG] ramfs: added '");
    console::puts(name);
    console::puts("' at heap=0x");
    console::put_hex(reinterpret_cast<u64>(buf));
    console::puts(" (");
    console::put_dec(size);
    console::puts(" bytes)\n");
#endif

    return status_code::success;
}

auto ramfs::find(const char* name) -> const file_entry* {
    for (usize i = 0; i < g_file_count; ++i) {
        if (g_files[i].valid && str_equal(g_files[i].name, name))
            return &g_files[i];
    }
    return null;
}

auto ramfs::file_count() -> usize { return g_file_count; }

auto ramfs::get_file(usize index) -> const file_entry* {
    if (index >= g_file_count) return null;
    return &g_files[index];
}

void ramfs::dump() {
    console::puts("Ramfs contents (");
    console::put_dec(g_file_count);
    console::puts(" files):\n");
    for (usize i = 0; i < g_file_count; ++i) {
        console::puts("  ");
        console::puts(g_files[i].name);
        console::puts("  ");
        console::put_dec(g_files[i].size);
        console::puts(" bytes\n");
    }
}

/* ============================================================
 * UEFI ESP Loader — Simple File System Protocol
 *
 * The UEFI Simple File System Protocol (GUID 0964e5b22...)
 * provides OpenVolume() which returns an EFI_FILE_PROTOCOL.
 * We use Open/Read/Close to load files from the ESP into memory
 * allocated via UEFI AllocatePool (still available before EBS).
 * ============================================================ */

namespace {

/* EFI_SIMPLE_FILE_SYSTEM_PROTOCOL GUID */
constexpr uefi::guid SFS_GUID = {
    0x0964e5b22, 0x6459, 0x11d2,
    { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b }
};

/* EFI_FILE_INFO GUID */
constexpr uefi::guid FILE_INFO_GUID = {
    0x09576e92, 0x6d3f, 0x11d2,
    { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b }
};

/* EFI_FILE_PROTOCOL — subset of function pointers we need */
struct efi_file_protocol;

using efi_file_open_fn  = [[gnu::ms_abi]] uefi::status(*)(
    efi_file_protocol* self, efi_file_protocol** new_handle,
    const char16_t* file_name, u64 open_mode, u64 attributes);

using efi_file_close_fn = [[gnu::ms_abi]] uefi::status(*)(efi_file_protocol* self);

using efi_file_read_fn  = [[gnu::ms_abi]] uefi::status(*)(
    efi_file_protocol* self, usize* buffer_size, void* buffer);

using efi_file_write_fn = [[gnu::ms_abi]] uefi::status(*)(
    efi_file_protocol* self, usize* buffer_size, const void* buffer);

using efi_file_get_info_fn = [[gnu::ms_abi]] uefi::status(*)(
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
    void*                set_position; /* offset 56 */
    efi_file_get_info_fn get_info;   /* offset 64 */
    /* ... more we don't need */
};

/* EFI_SIMPLE_FILE_SYSTEM_PROTOCOL */
struct efi_sfs_protocol;

using efi_sfs_open_volume_fn = [[gnu::ms_abi]] uefi::status(*)(
    efi_sfs_protocol* self, efi_file_protocol** root);

struct efi_sfs_protocol {
    u64                     revision;
    efi_sfs_open_volume_fn  open_volume;
};

/* File info structure (variable-length, but we only need size) */
struct efi_file_info {
    u64 size;         /* Size of this structure + filename */
    u64 file_size;
    u64 physical_size;
    /* ... timestamps, attributes, filename follow */
};

constexpr u64 EFI_FILE_MODE_READ = 0x0000000000000001ULL;

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

} // anonymous namespace

auto loader::load_file_from_esp(const char* path) -> loaded_file {
    if (uefi::g_system_table == null || uefi::g_system_table->boot_services == null)
        return { null, 0 };

    auto* bs = uefi::g_system_table->boot_services;

    /* Locate the Simple File System Protocol */
    void* sfs_iface = null;
    auto st = bs->locate_protocol(&SFS_GUID, null, &sfs_iface);
    if (st != uefi::status::success || sfs_iface == null) {
        console::puts("    SFS protocol not found\n");
        return { null, 0 };
    }
    auto* sfs = static_cast<efi_sfs_protocol*>(sfs_iface);
    log::debug("SFS protocol located");

    /* Open the root volume */
    efi_file_protocol* root = null;
    st = sfs->open_volume(sfs, &root);
    if (st != uefi::status::success || root == null) {
        console::puts("    Failed to open volume\n");
        return { null, 0 };
    }

    /* Open the requested file */
    efi_file_protocol* file = null;
    st = root->open(root, &file, to_ucs2(path), EFI_FILE_MODE_READ, 0);
    if (st != uefi::status::success || file == null) {
        console::puts("    File not found: ");
        console::puts(path);
        console::puts("\n");
        root->close(root);
        return { null, 0 };
    }

    /* Query file size via GetInfo */
    u8 info_buf[256];
    usize info_size = sizeof(info_buf);
    st = file->get_info(file, &FILE_INFO_GUID, &info_size, info_buf);
    if (st != uefi::status::success) {
        console::puts("    GetInfo failed\n");
        file->close(file);
        root->close(root);
        return { null, 0 };
    }
    auto* fi = reinterpret_cast<efi_file_info*>(info_buf);
    usize file_size = static_cast<usize>(fi->file_size);

#if VK_DEBUG_LEVEL >= 4
    console::puts("[DEBUG] '" );
    console::puts(path);
    console::puts("': ");
    console::put_dec(file_size);
    console::puts(" bytes\n");
#endif

    if (file_size == 0) {
        file->close(file);
        root->close(root);
        return { null, 0 };
    }

    /* Allocate buffer via UEFI AllocatePool (EfiLoaderData = type 2) */
    void* buf = null;
    st = bs->allocate_pool(2 /* EfiLoaderData */, file_size, &buf);
    if (st != uefi::status::success || buf == null) {
        console::puts("    AllocatePool failed\n");
        file->close(file);
        root->close(root);
        return { null, 0 };
    }

    /* Read the file */
    usize read_size = file_size;
    st = file->read(file, &read_size, buf);
    file->close(file);
    root->close(root);

    if (st != uefi::status::success || read_size != file_size) {
        console::puts("    Read failed\n");
        bs->free_pool(buf);
        return { null, 0 };
    }

#if VK_DEBUG_LEVEL >= 4
    console::puts("[DEBUG] read OK, buf=0x");
    console::put_hex(reinterpret_cast<u64>(buf));
    console::puts("\n");
#endif

    return { static_cast<u8*>(buf), file_size };
}

/* Load well-known files from the ESP into the ramfs */
auto loader::load_initrd() -> status_code {
    console::puts("Loading files from ESP...\n");

    ramfs::init();

    /* List of files to attempt loading from the ESP.
     * These live under \EFI\vkernel\ on the ESP. */
    static const char* const files[] = {
        "\\EFI\\vkernel\\shell.txt",
        "\\EFI\\vkernel\\hello.txt",
        "\\EFI\\vkernel\\motd.txt",
    };
    constexpr usize file_count = sizeof(files) / sizeof(files[0]);

    usize loaded = 0;
    for (usize i = 0; i < file_count; ++i) {
        auto result = load_file_from_esp(files[i]);
        if (result.data != null && result.size > 0) {
            /* Extract just the filename part for the ramfs name */
            const char* name = files[i];
            const char* p = name;
            while (*p) {
                if (*p == '\\' || *p == '/') name = p + 1;
                ++p;
            }
            if (ramfs::add_file(name, result.data, result.size) == status_code::success) {
                console::puts("  Loaded: ");
                console::puts(name);
                console::puts(" (");
                console::put_dec(result.size);
                console::puts(" bytes)\n");
                ++loaded;
            }
        }
    }

    console::puts("  ");
    console::put_dec(loaded);
    console::puts(" file(s) loaded from ESP\n");

    return status_code::success;
}

} // namespace vk
