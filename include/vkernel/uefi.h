/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * uefi.h - UEFI interface with C++26 features
 */

 // https://uefi.org/sites/default/files/resources/UEFI%20Spec%202.8B%20May%202020.pdf

#ifndef VKERNEL_UEFI_H
#define VKERNEL_UEFI_H

#include "types.h"

namespace vk {
namespace uefi {

/* UEFI Status codes */
enum class status : s64 {
    success = 0,
    load_error = static_cast<s64>(0x8000000000000001ULL),
    invalid_parameter = static_cast<s64>(0x8000000000000002ULL),
    unsupported = static_cast<s64>(0x8000000000000003ULL),
    bad_buffer_size = static_cast<s64>(0x8000000000000004ULL),
    buffer_too_small = static_cast<s64>(0x8000000000000005ULL),
    not_ready = static_cast<s64>(0x8000000000000006ULL),
    device_error = static_cast<s64>(0x8000000000000007ULL),
    write_protected = static_cast<s64>(0x8000000000000008ULL),
    out_of_resources = static_cast<s64>(0x8000000000000009ULL),
    not_found = static_cast<s64>(0x8000000000000014ULL),
    already_started = static_cast<s64>(0x8000000000000019ULL),
};

/* UEFI handle */
using handle = void*;

/* UEFI GUID */
struct guid {
    u32 data1;
    u16 data2;
    u16 data3;
    u8 data4[8];
    
    [[nodiscard]] constexpr auto operator==(const guid& other) const -> bool {
        return data1 == other.data1 &&
               data2 == other.data2 &&
               data3 == other.data3 &&
               data4[0] == other.data4[0] &&
               data4[1] == other.data4[1] &&
               data4[2] == other.data4[2] &&
               data4[3] == other.data4[3] &&
               data4[4] == other.data4[4] &&
               data4[5] == other.data4[5] &&
               data4[6] == other.data4[6] &&
               data4[7] == other.data4[7];
    }
};

/* UEFI table header */
struct table_header {
    u64 signature;
    u32 revision;
    u32 header_size;
    u32 crc32;
    u32 reserved;
};

/* Simple Text Output Protocol interface */
struct text_output_protocol;

/*
 * ALL UEFI protocol function pointers use the Microsoft x64 ABI.
 * On MSVC this is the default; on GCC/Clang we need [[gnu::ms_abi]].
 */
#if defined(_MSC_VER)
#define VK_MSABI
#else
#define VK_MSABI [[gnu::ms_abi]]
#endif

using text_output_reset_fn      = VK_MSABI status(*)(text_output_protocol* self, bool extended_verification);
using text_output_string_fn     = VK_MSABI status(*)(text_output_protocol* self, const char16_t* string);
using text_output_test_fn       = VK_MSABI status(*)(text_output_protocol* self, const char16_t* string);
using text_output_mode_fn       = VK_MSABI status(*)(text_output_protocol* self, usize mode_number, usize* columns, usize* rows);
using text_output_set_mode_fn   = VK_MSABI status(*)(text_output_protocol* self, usize mode_number);
using text_output_attr_fn       = VK_MSABI status(*)(text_output_protocol* self, usize attribute);
using text_output_clear_fn      = VK_MSABI status(*)(text_output_protocol* self);
using text_output_cursor_fn     = VK_MSABI status(*)(text_output_protocol* self, usize column, usize row);

/* Text output mode structure */
struct text_output_mode {
    s32 max_mode;
    s32 mode;
    s32 attribute;
    s32 cursor_column;
    s32 cursor_row;
    bool cursor_visible;
};

/* Simple Text Output Protocol
 * EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL — 9 function pointers then *Mode.
 * There is NO separate DisableCursor slot; EnableCursor takes a BOOLEAN. */
struct text_output_protocol {
    text_output_reset_fn reset;             // offset  0
    text_output_string_fn output_string;    // offset  8
    text_output_test_fn test_string;        // offset 16
    text_output_mode_fn query_mode;         // offset 24
    text_output_set_mode_fn set_mode;       // offset 32
    text_output_attr_fn set_attribute;      // offset 40
    text_output_clear_fn clear_screen;      // offset 48
    text_output_cursor_fn set_cursor_position; // offset 56
    void* enable_cursor;                    // offset 64
    text_output_mode* mode;                 // offset 72  ← was at 80 before (bug!)
};

/* ============================================================
 * Raw UEFI memory descriptor (EFI_MEMORY_DESCRIPTOR wire format)
 * ============================================================ */

struct memory_descriptor {
    u32       type;
    u32       pad;              /* alignment padding */
    phys_addr physical_start;
    virt_addr virtual_start;
    u64       number_of_pages;
    u64       attribute;
};

/* Boot Services function pointer types (all MS ABI) */
using get_memory_map_fn     = VK_MSABI status(*)(usize*, memory_descriptor*, usize*, usize*, u32*);
using allocate_pool_fn      = VK_MSABI status(*)(u32, usize, void**);
using free_pool_fn          = VK_MSABI status(*)(void*);
using exit_boot_services_fn = VK_MSABI status(*)(handle, usize);
using locate_protocol_fn       = VK_MSABI status(*)(const guid*, void*, void**);
using handle_protocol_fn       = VK_MSABI status(*)(handle, const guid*, void**);
using locate_handle_buffer_fn  = VK_MSABI status(*)(u32 search_type, const guid*, void* search_key, usize* no_handles, handle** buffer);
using connect_controller_fn    = VK_MSABI status(*)(handle controller, handle* driver_image_handle, void* remaining_device_path, bool recursive);

/* UEFI Boot Services Table (EFI_BOOT_SERVICES, partial)
 * Each void* placeholder preserves the correct field offset. */
struct boot_services_table {
    table_header          hdr;                          // offset   0 (24 bytes)
    void*                 raise_tpl;                    // offset  24
    void*                 restore_tpl;                  // offset  32
    void*                 allocate_pages;               // offset  40
    void*                 free_pages;                   // offset  48
    get_memory_map_fn     get_memory_map;               // offset  56
    allocate_pool_fn      allocate_pool;                // offset  64
    free_pool_fn          free_pool;                    // offset  72
    void*                 create_event;                 // offset  80
    void*                 set_timer;                    // offset  88
    void*                 wait_for_event;               // offset  96
    void*                 signal_event;                 // offset 104
    void*                 close_event;                  // offset 112
    void*                 check_event;                  // offset 120
    void*                 install_protocol_interface;   // offset 128
    void*                 reinstall_protocol_interface; // offset 136
    void*                 uninstall_protocol_interface; // offset 144
    handle_protocol_fn    handle_protocol;              // offset 152
    void*                 reserved;                     // offset 160
    void*                 register_protocol_notify;     // offset 168
    void*                 locate_handle;                // offset 176
    void*                 locate_device_path;           // offset 184
    void*                 install_configuration_table;  // offset 192
    void*                 load_image;                   // offset 200
    void*                 start_image;                  // offset 208
    void*                 exit_fn;                      // offset 216
    void*                 unload_image;                 // offset 224
    exit_boot_services_fn exit_boot_services;           // offset 232
    void*                 get_next_monotonic_count;     // offset 240
    void*                 stall;                        // offset 248
    void*                 set_watchdog_timer;           // offset 256
    connect_controller_fn connect_controller;           // offset 264
    void*                 disconnect_controller;        // offset 272
    void*                 open_protocol;                // offset 280
    void*                 close_protocol;               // offset 288
    void*                 open_protocol_information;    // offset 296
    void*                 protocols_per_handle;         // offset 304
    locate_handle_buffer_fn locate_handle_buffer;       // offset 312
    locate_protocol_fn    locate_protocol;               // offset 320
};

/* ============================================================
 * Higher-level C++26-friendly wrappers around UEFI
 * ============================================================ */

// info obtained from https://krinkinmu.github.io/2020/10/31/efi-file-access.html

 /* Graphics Output Protocol (GOP) GUID */
constexpr uefi::guid GOP_GUID = {
    0x9042A9DE, 0x23DC, 0x4A38,
    { 0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A }
};

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

/*
* UEFI Configuration Table GUIDs for ACPI:
*   ACPI 2.0 : {8868e871-e4f1-11d3-bc22-0080c73c8881}
*   ACPI 1.0 : {eb9d2d30-2d88-11d3-9a16-0090273fc14d}
*/
constexpr uefi::guid ACPI_20_GUID = {
    0x8868e871u, 0xe4f1u, 0x11d3u,
    { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 }
};

constexpr uefi::guid ACPI_10_GUID = {
    0xeb9d2d30u, 0x2d88u, 0x11d3u,
    { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d }
};

enum class pixel_format : u32 {
    rgbx_8bpp  = 0,   /* R[7:0] G[15:8] B[23:16] X[31:24] */
    bgrx_8bpp  = 1,   /* B[7:0] G[15:8] R[23:16] X[31:24] */
    bitmask    = 2,
    blt_only   = 3,
};

struct pixel_bitmask {
    u32 red;
    u32 green;
    u32 blue;
    u32 reserved;
};

struct gop_mode_info {
    u32           version;
    u32           horizontal_resolution;
    u32           vertical_resolution;
    pixel_format  fmt;
    pixel_bitmask pixel_info;
    u32           pixels_per_scan_line;
};

struct gop_mode {
    u32           max_mode;
    u32           mode;
    gop_mode_info* info;
    usize          size_of_info;
    phys_addr      frame_buffer_base;
    usize          frame_buffer_size;
};

struct gop_protocol {
    void*     query_mode;
    void*     set_mode;
    void*     blt;
    gop_mode* mode;
};

/* Framebuffer info captured before ExitBootServices */
struct framebuffer_info {
    phys_addr base;
    u32       width;
    u32       height;
    u32       stride;        /* pixels per scan line */
    pixel_format format;
    bool      valid;
};

/* Result returned by query_memory_map() */
struct memory_map_result {
    const memory_descriptor* entries = null;       /* pointer into internal raw buffer */
    usize                    count = 0;            /* number of valid entries */
    usize                    descriptor_size = 0;  /* per-entry byte stride (may be > sizeof(memory_descriptor)) */
    usize                    map_key = 0;          /* needed for ExitBootServices */
    status                   query_status = status::not_ready;
    usize                    required_size = 0;    /* firmware-reported byte size for the map */
};

/* ============================================================
 * EFI_CONFIGURATION_TABLE entry
 * Each entry is a (GUID, pointer) pair in the system table's
 * configuration table array.
 * ============================================================ */

struct configuration_table_entry {
    guid  vendor_guid;
    void* vendor_table;
};

/* UEFI System Table */
inline constexpr u64 SYSTEM_TABLE_SIGNATURE = 0x5453595320494249ULL;

struct system_table {
    table_header hdr;
    char16_t* firmware_vendor;
    u32 firmware_revision;
    handle console_in_handle;
    void* con_in;
    handle console_out_handle;
    text_output_protocol* con_out;
    handle standard_error_handle;
    text_output_protocol* std_err;
    void* runtime_services;
    boot_services_table* boot_services;
    usize number_of_table_entries;
    configuration_table_entry* configuration_table;
};

/* Global system table pointer */
extern system_table* g_system_table;

/* UEFI initialization */
auto init(handle image_handle, system_table* system_table) -> status;
void set_system_table(system_table* table);

/* Console access */
auto get_console() -> text_output_protocol*;

/* String utilities */
usize strlen(const char16_t* str);
void strcpy(char16_t* dest, const char16_t* src);
i32 strcmp(const char16_t* s1, const char16_t* s2);

/* Memory map */
auto query_memory_map() -> memory_map_result;
auto do_exit_boot_services(handle image_handle, usize map_key) -> status;

/* Configuration table search */
void* find_configuration_table(const guid& target_guid);

/* Graphics */
auto query_gop() -> framebuffer_info;

/* EFIAPI calling convention (Microsoft ABI for x86_64) */
#ifdef _MSC_VER
    #define EFIAPI
#elif defined(__GNUC__) || defined(__clang__)
    #define EFIAPI __attribute__((ms_abi))
#else
    #define EFIAPI
#endif

} // namespace uefi
} // namespace vk

#endif /* VKERNEL_UEFI_H */
