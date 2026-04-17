/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * efi_main.cpp - UEFI entry point with C++26
 */

#include "config.h"
#include "types.h"
#include "uefi.h"
#include "console.h"
#include "memory.h"
#include "fs.h"
#include "scheduler.h"
#include "input.h"
#include "panic.h"
#include "process.h"
#include "arch/x86_64/arch.h"

namespace vk {

/* ============================================================
 * Self-relocator for the GOT
 *
 * Our PE is loaded at an arbitrary base by UEFI but has NO
 * base relocations (the .reloc section is an empty stub).
 * Code is fully RIP-relative (-fpic), but the Global Offset
 * Table (.got) contains absolute link-time addresses that the
 * linker baked in.  We patch them here by adding the load delta.
 * ============================================================ */

extern "C" {
    extern char ImageBase[];   /* linker script: ImageBase = 0 */
}

static void self_relocate() {
#if defined(_MSC_VER)
    /*
     * On MSVC/PE the linker generates proper base relocations when
     * /FIXED:NO is used (the default for EFI Application subsystem).
     * The UEFI firmware applies them at load time, so there is nothing
     * for us to do here.  The function is kept as a no-op so the call
     * site in efi_main() compiles unchanged.
     */
    (void)ImageBase;
#else
    /* Runtime address of ImageBase via RIP-relative LEA */
    u64 runtime_base;
    asm volatile("lea ImageBase(%%rip), %0" : "=r"(runtime_base));

    /* Link-time base is 0 (from linker script: ImageBase = 0) */
    const u64 delta = runtime_base;

    if (delta == 0) return;  /* Loaded at link-time base — nothing to do */

    /* Scan ALL of .data for pointer-like values.
     * A value is a link-time pointer if it falls within the
     * image range [0x1000, _edata).  We add delta to relocate. */
    u64* data_start;
    u64* data_end;
    u64  end_val;
    asm volatile("lea _data(%%rip), %0"  : "=r"(data_start));
    asm volatile("lea _edata(%%rip), %0" : "=r"(data_end));
    /* _end includes BSS — use it as upper bound for pointer detection.
     * Compute link-time value: runtime__end - delta */
    u64* end_ptr;
    asm volatile("lea _end(%%rip), %0" : "=r"(end_ptr));
    end_val = reinterpret_cast<u64>(end_ptr) - delta;

    for (u64* p = data_start; p < data_end; ++p) {
        /* Link-time pointers fall in [0x1000, link-time _end) */
        if (*p >= 0x1000 && *p < end_val) {
            *p += delta;
        }
    }
#endif
}

/*
 * UEFI Entry Point
 * 
 * This is the entry point for the UEFI application.
 * It is called by the UEFI firmware when the image is loaded.
 */
auto efi_main(
    uefi::handle image_handle,
    uefi::system_table* system_table
) -> uefi::status {
    
    /* Self-relocate: patch GOT entries before using any cross-TU pointers */
    self_relocate();

    /* Store the system table pointer */
    uefi::g_system_table = system_table;
    
    /* Initialize the console */
    if (auto status = console::init(); status != status_code::success) {
        return uefi::status::device_error;
    }
    
    /* Print welcome message */
    console::puts("vkernel ");
    console::puts(config::version_string);
    console::puts(" - UEFI Microkernel\n");
    console::puts("Booting on ");
    console::puts(config::arch_name);
    console::puts(" using ");
    console::puts(config::compiler_name);
    console::puts("\n\n");
    
    /* Prepare architecture tables (GDT/IDT in memory, not yet loaded) */
    arch::init();
    log::debug("arch tables prepared (GDT/IDT built, not yet loaded)");

    /* ============================================================
     * Phase 1 — while UEFI boot services are still available
     * ============================================================ */

    /* Query the UEFI memory map */
    console::write("Querying UEFI memory map...");
    auto raw = uefi::query_memory_map();
    if (raw.count == 0) {
        console::write("ERROR: Failed to query UEFI memory map");
        return uefi::status::load_error;
    }

    /* Convert raw UEFI descriptors → kernel memory_map_entry */
    static memory_map_entry s_map[config::max_memory_map_entries];
    u32 map_count = 0;

    for (usize i = 0; i < raw.count && map_count < config::max_memory_map_entries; ++i) {
        const auto* d = reinterpret_cast<const uefi::memory_descriptor*>(
            reinterpret_cast<const u8*>(raw.entries) + i * raw.descriptor_size
        );
        const auto mtype = (d->type < static_cast<u32>(memory_type::count))
            ? static_cast<memory_type>(d->type)
            : memory_type::reserved;

        auto& entry = s_map[map_count++];
        entry.physical_start = d->physical_start;
        entry.virtual_start = d->virtual_start;
        entry.number_of_pages = d->number_of_pages;
        entry.type = mtype;
        entry.attribute = d->attribute;
    }

    console::puts("  Found ");
    console::put_dec(map_count);
    console::puts(" memory map entries\n");

    /* Print summary before we lose console access */
    u64 total_conventional_pages = 0;
    for (u32 i = 0; i < map_count; ++i) {
        if (s_map[i].type == memory_type::conventional) {
            total_conventional_pages += s_map[i].number_of_pages;
        }
    }
    console::puts("  Conventional memory: ");
    console::put_dec((total_conventional_pages * 0x1000ULL) / (1024 * 1024));
    console::puts(" MB\n");

#if VK_DEBUG_LEVEL >= 4
    console::puts("[DEBUG] memory map: ");
    console::put_dec(map_count);
    console::puts(" entries, ");
    console::put_dec(total_conventional_pages);
    console::puts(" conventional pages\n");
#endif

    /* Query the GOP framebuffer (must happen before ExitBootServices) */
    console::puts("Querying framebuffer...\n");
    auto fb_info = uefi::query_gop();
    if (fb_info.valid) {
        console::puts("  Framebuffer: ");
        console::put_dec(fb_info.width);
        console::puts("x");
        console::put_dec(fb_info.height);
        console::puts(" @ 0x");
        console::put_hex(fb_info.base);
        console::puts("\n");
        console::init_framebuffer(fb_info);
    } else {
        console::puts("  No framebuffer available.\n");
    }

    /* Load files from ESP into ramfs (must happen before ExitBootServices) */
    loader::load_initrd();

    /* ============================================================
     * Phase 2 — Exit Boot Services
     * ============================================================ */

    console::write("Exiting UEFI boot services...");

    /* Disable interrupts: prevents UEFI timer callbacks from modifying
     * the memory map between GetMemoryMap and ExitBootServices, which
     * would invalidate the map key and cause EBS to fail. */
    arch::disable_interrupts();

    {
        /* Critical section: GetMemoryMap → ExitBootServices.
         * Absolutely no other UEFI calls between these two. */
        auto fresh = uefi::query_memory_map();
        auto ebs_status = uefi::do_exit_boot_services(image_handle, fresh.map_key);

        if (ebs_status != uefi::status::success) {
            /* UEFI spec: one retry is permitted after re-querying the map */
            fresh = uefi::query_memory_map();
            ebs_status = uefi::do_exit_boot_services(image_handle, fresh.map_key);

            if (ebs_status != uefi::status::success) {
                /* Boot services still active here — safe to print via ConOut */
                arch::enable_interrupts();
                console::write("FATAL: ExitBootServices failed after 2 attempts");
                while (true) { arch::disable_interrupts(); arch::cpu_halt(); }
            }
        }
    }

    /* Boot services are now terminated. Switch to serial console — ConOut
     * is a boot service and must NOT be called after ExitBootServices. */
    console::switch_to_serial();
    if (fb_info.valid) {
        console::switch_to_framebuffer();
    }
    console::write("Boot services exited. Serial + framebuffer console active.");

    /* ============================================================
     * Phase 3 — we own the machine
     * ============================================================ */

    /* Load our GDT, IDT, TSS and harden paging */
    arch::activate();

    /* Initialize keyboard and serial input */
    input::init();

    /* Initialize the kernel memory subsystem with the saved map */
    if (auto status = memory::init(span(s_map, map_count)); status != status_code::success) {
        vk_panic(__FILE__, __LINE__, "Memory subsystem initialization failed");
    }

    console::write("Kernel initialization complete.");

    /* ============================================================
    * Phase 4 — Scheduler + Userspace Shell
     * ============================================================ */

    /* Initialize the scheduler (sets up PIC + PIT) */
    if (auto status = sched::init(); status != status_code::success) {
        vk_panic(__FILE__, __LINE__, "Scheduler initialization failed");
    }

    /* Create the idle task (task 0) — just halts when nothing else runs */
    sched::create_task("idle", [](void*) {
        while (true) { arch::cpu_halt(); }
    });

    /* Launch the userspace shell binary. */
    console::write("Launching userspace shell...");
#if defined(_MSC_VER)
    if (process::run("shell.exe") < 0) {
#else
    if (process::run("shell.elf") < 0) {
#endif
        vk_panic(__FILE__, __LINE__, "Failed to launch userspace shell!");
    }

    /* Start the scheduler — does not return */
    log::debug("Transferring control to scheduler...");
    sched::start();

    return uefi::status::success;
}

} // namespace vk

/* Export the entry point with C linkage for UEFI */
extern "C" {
    using vk::uefi::handle;
    using vk::uefi::system_table;
    using vk::uefi::status;

#if defined(_MSC_VER)
    status efi_main(handle image_handle, system_table* system_table) {
#else
    [[gnu::ms_abi]] status efi_main(handle image_handle, system_table* system_table) {
#endif
        return vk::efi_main(image_handle, system_table);
    }
}
