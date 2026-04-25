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
#include "driver.h"
#include "pci.h"
#include "acpi.h"
#include "smp.h"

namespace vk {

/* Forward declarations for built-in driver registration */
namespace sb16_driver { void register_builtin(); }
namespace ac97_driver { void register_builtin(); }

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
    u64 runtime_base = asm_get_image_base();

    /* Link-time base is 0 (from linker script: ImageBase = 0) */
    const u64 delta = runtime_base;

    if (delta == 0) return;  /* Loaded at link-time base — nothing to do */

    /* Scan ALL of .data for pointer-like values.
     * A value is a link-time pointer if it falls within the
     * image range [0x1000, _edata).  We add delta to relocate. */
    u64* data_start = reinterpret_cast<u64*>(asm_get_data_start());
    u64* data_end   = reinterpret_cast<u64*>(asm_get_data_end());
    /* _end includes BSS — use it as upper bound for pointer detection.
     * Compute link-time value: runtime__end - delta */
    u64* end_ptr = reinterpret_cast<u64*>(asm_get_end());
    u64  end_val = reinterpret_cast<u64>(end_ptr) - delta;

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
    log::printk("vkernel %s - UEFI Microkernel\n", config::version_string);
    log::printk("Booting on %s using %s\n\n",
                config::arch_name, config::compiler_name);

    log::debug("UEFI entry point reached: image_handle=%p, system_table=%p",
               image_handle, system_table);
    log::debug(".text start=%p", 0x1000+asm_get_image_base());
    log::debug(".data start=%p, end=%p", asm_get_data_start(), asm_get_data_end());
    log::debug("_end=%p", asm_get_end());

    /* Prepare architecture tables (GDT/IDT in memory, not yet loaded) */
    arch::init();
    log::debug("arch tables prepared (GDT/IDT built, not yet loaded)");

    /* ============================================================
     * Phase 1 — while UEFI boot services are still available
     * ============================================================ */

    /* Query the UEFI memory map */
    log::info("Querying UEFI memory map...");
    auto raw = uefi::query_memory_map();
    if (raw.count == 0) {
        log::error("Failed to query UEFI memory map");
        vk_panic(__FILE__, __LINE__, "Failed to query UEFI memory map");
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

    log::info("Found %u memory map entries", map_count);

    /* Print summary before we lose console access */
    u64 total_conventional_pages = 0;
    for (u32 i = 0; i < map_count; ++i) {
        if (s_map[i].type == memory_type::conventional) {
            total_conventional_pages += s_map[i].number_of_pages;
        }
    }
    log::info("Conventional memory: %llu MB",
              (total_conventional_pages * 0x1000ULL) / (1024 * 1024));

    log::debug("memory map: %u entries, %llu conventional pages",
               map_count, total_conventional_pages);

    /* Query the GOP framebuffer (must happen before ExitBootServices) */
    log::info("Querying framebuffer...");
    auto fb_info = uefi::query_gop();
    if (fb_info.valid) {
        log::info("Framebuffer: %ux%u @ %#llx",
                  fb_info.width, fb_info.height,
                  static_cast<unsigned long long>(fb_info.base));
        console::init_framebuffer(fb_info);
    } else {
        log::warn("No framebuffer available");
    }

    /* Load files from ESP into ramfs (must happen before ExitBootServices) */
    loader::load_initrd();

    /* Locate ACPI tables via UEFI configuration table while boot services
     * are still active.  The RSDP and all referenced SDTs reside in
     * ACPI-reclaimable memory and remain valid after ExitBootServices.  */
    log::info("Initializing ACPI...");
    acpi::init(uefi::g_system_table);

    /* ============================================================
     * Phase 2 — Exit Boot Services
     * ============================================================ */

    log::info("Exiting UEFI boot services...");

    /* Disable interrupts: prevents UEFI timer callbacks from modifying
     * the memory map between GetMemoryMap and ExitBootServices, which
     * would invalidate the map key and cause EBS to fail. */
    arch::disable_interrupts();

    // Critical section: GetMemoryMap → ExitBootServices.
    // Absolutely no other UEFI calls between these two.
    {
        auto fresh = uefi::query_memory_map();
        auto ebs_status = uefi::do_exit_boot_services(image_handle, fresh.map_key);

        if (ebs_status != uefi::status::success) {
            /* UEFI spec: one retry is permitted after re-querying the map */
            fresh = uefi::query_memory_map();
            ebs_status = uefi::do_exit_boot_services(image_handle, fresh.map_key);

            if (ebs_status != uefi::status::success) {
                /* Boot services still active here — safe to print via ConOut */
                arch::enable_interrupts();
                log::error("ExitBootServices failed after 2 attempts");

                vk_panic(__FILE__, __LINE__, "Failed to exit UEFI boot services");
            }
        }
    }

    /* Boot services are now terminated. Switch to serial console — ConOut
     * is a boot service and must NOT be called after ExitBootServices. */
    console::switch_to_serial();

    if (fb_info.valid) {
        console::switch_to_framebuffer();
		//console::clear();
    }
    log::printk("Boot services exited. Serial + framebuffer console active.\n");

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

    log::info("Kernel initialization complete");

    /* ============================================================
     * Phase 4 — Driver framework + Scheduler + Userspace Shell
     * ============================================================ */

    /* Initialize PCI bus */
    pci::init();
    pci::list_devices();

    /* Initialize the driver framework and register built-in drivers */
    driver::init();
    sb16_driver::register_builtin();
    ac97_driver::register_builtin();
    log::info("Driver framework initialised (2 built-in drivers registered)");

    /* Bring up Application Processors */
    //log::info("Initializing SMP...");
    //smp::init();
    //smp::dump_cpus();

    /* Initialize the scheduler (sets up PIC + PIT) */
    if (auto status = sched::init(); status != status_code::success) {
        vk_panic(__FILE__, __LINE__, "Scheduler initialization failed");
    }

    /* Create the idle task (task 0) — just halts when nothing else runs */
    (void)sched::create_task("idle", [](void*) {
        while (true) { arch::cpu_halt(); }
    });

    /* Launch the userspace shell binary. */
    if (fb_info.valid) {
        log::info("Launching graphical shell...");
        if (process::run("shell.vbin", process::console_interface::graphical) < 0) {
            vk_panic(__FILE__, __LINE__, "Failed to launch graphical shell!");
        }
    } else {
        log::warn("Framebuffer unavailable; graphical shell not launched");
    }

    log::info("Launching serial shell...");
    if (process::run("shell.vbin", process::console_interface::serial) < 0) {
        vk_panic(__FILE__, __LINE__, "Failed to launch serial shell!");
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
