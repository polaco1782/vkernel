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
#include "log.h"
#include "memory.h"
#include "fs.h"
#include "scheduler.h"
#include "sound.h"
#include "input.h"
#include "panic.h"
#include "process.h"
#include "virtual_memory.h"
#include "arch/x86_64/arch.h"
#include "driver.h"
#include "pci.h"
#include "acpi.h"
#include "smp.h"
#include "ipv4.h"

namespace vk {

/* Forward declarations for built-in driver registration */
namespace ac97_driver { void register_builtin(); }
namespace fat32_driver { void register_builtin(); }
namespace virtio_blk_driver { void register_builtin(); }
namespace virtio_net_driver { void register_builtin(); }

static constexpr net::ipv4_address DEFAULT_KERNEL_IPV4 =
    net::make_ipv4(10, 0, 0, 2);

#if defined(KERNEL_GDB_WAIT)
/* Reuse unused trampoline bytes as a simple GDB handshake mailbox. */
static constexpr usize GDB_MAILBOX_IMAGE_BASE = 0x8160;
static constexpr usize GDB_MAILBOX_RELEASE    = 0x8168;
static constexpr u64   GDB_RELEASE_MAGIC      = 0x564B4442474FULL; /* "VKDBGO" */

static void wait_for_debugger_attach() {
    auto* image_base_slot =
        reinterpret_cast<volatile u64*>(GDB_MAILBOX_IMAGE_BASE);
    auto* release_slot =
        reinterpret_cast<volatile u64*>(GDB_MAILBOX_RELEASE);

    *release_slot = 0;
    *image_base_slot = asm_get_image_base();
    asm_memory_barrier();

    while (*release_slot != GDB_RELEASE_MAGIC) {
        asm_pause();
        asm_memory_barrier();
    }
}
#endif

/* Fix absolute GOT entries when the firmware loads us away from RVA 0. */

extern "C" {
    extern char ImageBase[];   /* linker script: ImageBase = 0 */
}

static void self_relocate() {
#if defined(_MSC_VER)
    /* MSVC emits normal PE relocations, so firmware already fixed this up. */
    (void)ImageBase;
#else
    u64 runtime_base = asm_get_image_base();
    const u64 delta = runtime_base;

    if (delta == 0) return;  /* Loaded at link-time base — nothing to do */

    /* Relocate pointer-like values still pointing at link-time image RVAs. */
    u64* data_start = reinterpret_cast<u64*>(asm_get_data_start());
    u64* data_end   = reinterpret_cast<u64*>(asm_get_data_end());
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

auto efi_main(
    uefi::handle image_handle,
    uefi::system_table* system_table
) -> uefi::status {
#if defined(KERNEL_GDB_WAIT)
    /* Pause early so GDB can attach before init runs. */
    wait_for_debugger_attach();
#endif

    /* Fix GOT entries before using cross-TU data. */
    self_relocate();

    /* Store the system table pointer */
    uefi::g_system_table = system_table;
    
    /* Initialize the console */
    if (auto status = console::init(); status != status_code::success) {
        return uefi::status::device_error;
    }
  
    /* Print welcome message */
    log::printk() << "vkernel " << config::version_string << " - UEFI Microkernel\n";
    log::printk() << "Booting on " << config::arch_name << " using "
                  << config::compiler_name << "\n\n";

    log::debug() << "UEFI entry point reached: image_handle=" << reinterpret_cast<const void*>(image_handle) << ", system_table=" << reinterpret_cast<const void*>(system_table);
    log::debug() << ".text start=" << reinterpret_cast<const void*>(0x1000+asm_get_image_base());
    log::debug() << ".data start=" << reinterpret_cast<const void*>(asm_get_data_start()) << ", end=" << reinterpret_cast<const void*>(asm_get_data_end());
    log::debug() << "_end=" << reinterpret_cast<const void*>(asm_get_end());

    /* Prepare architecture tables (GDT/IDT in memory, not yet loaded) */
    arch::init();
    log::debug() << "arch tables prepared (GDT/IDT built, not yet loaded)";

    /* ============================================================
     * Phase 1 — while UEFI boot services are still available
     * ============================================================ */

    /* Query the UEFI memory map */
    log::info() << "Querying UEFI memory map...";
    auto raw = uefi::query_memory_map();
    if (raw.count == 0) {
        log::error() << "Failed to query UEFI memory map";
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

    log::info() << "Found " << map_count << " memory map entries";

    /* Print summary before we lose console access */
    u64 total_conventional_pages = 0;
    for (u32 i = 0; i < map_count; ++i) {
        if (s_map[i].type == memory_type::conventional) {
            total_conventional_pages += s_map[i].number_of_pages;
        }
    }
    log::info() << "Conventional memory: " << (total_conventional_pages * 0x1000ULL) / (1024 * 1024) << " MB";

    log::debug() << "memory map: " << map_count << " entries, " << total_conventional_pages << " conventional pages";

    /* Query the GOP framebuffer (must happen before ExitBootServices) */
    log::info() << "Querying framebuffer...";
    auto fb_info = uefi::query_gop();
    if (fb_info.valid) {
        log::info() << "Framebuffer: " << fb_info.width << "x" << fb_info.height << " @ " << log::hex(static_cast<u64>(static_cast<unsigned long long>(fb_info.base)), 1, true, false);
        console::init_framebuffer(fb_info);
    } else {
        log::warn() << "No framebuffer available";
    }

    /* Load files from ESP into ramfs (must happen before ExitBootServices) */
    loader::load_initrd();

    /* ACPI discovery still needs the UEFI configuration table here. */
    log::info() << "Initializing ACPI...";
    acpi::init(uefi::g_system_table);

    /* ============================================================
     * Phase 2 — Exit Boot Services
     * ============================================================ */

    log::info() << "Exiting UEFI boot services...";

    /* Keep the memory map stable across the GetMemoryMap -> EBS window. */
    arch::disable_interrupts();

    /* No other UEFI calls may happen inside this window. */
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
                log::error() << "ExitBootServices failed after 2 attempts";

                vk_panic(__FILE__, __LINE__, "Failed to exit UEFI boot services");
            }
        }
    }

    /* ConOut is gone after EBS, so switch to kernel-owned backends. */
    console::switch_to_serial();

    if (fb_info.valid) {
        console::switch_to_framebuffer();
    }
    log::printk() << "Boot services exited. Serial + framebuffer console active.\n";

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
    vm::init();

    log::info() << "Kernel initialization complete";

    /* ============================================================
     * Phase 4 — Driver framework + Scheduler + Userspace Shell
     * ============================================================ */

    /* Initialize PCI bus */
    pci::init();
    pci::list_devices();

    /* Initialize the driver framework and register built-in drivers */
    driver::init();
    virtio_blk_driver::register_builtin();
    virtio_net_driver::register_builtin();
    ac97_driver::register_builtin();
    fat32_driver::register_builtin();
    log::info() << "Driver framework initialised (" << static_cast<unsigned long long>(driver::registered_count())
                << " built-in drivers registered)";

    /* Keep RAMFS alive while block-backed VFS comes up. */
    (void)driver::load("virtio_blk");
    (void)driver::load("fat32");
    (void)driver::load("virtio_net");
    (void)fs::mount_boot_filesystem();
    (void)driver::load("ac97");

    /* Initialize the scheduler (sets up PIC + PIT) */
    if (auto status = sched::init(); status != status_code::success) {
        vk_panic(__FILE__, __LINE__, "Scheduler initialization failed");
    }

    /* Bring up Application Processors */
    log::info() << "Initializing SMP...";
    smp::init();
    smp::dump_cpus();

    /* Create the idle task (task 0) — just halts when nothing else runs */
    (void)sched::create_task("idle", [](void*) {
        while (true) { arch::cpu_halt(); }
    });

    /* Start the network background worker */
    if(!net::start_background_worker()) {
        log::warn() << "net: failed to start background worker";
    }

    /* Start the sound background worker */
    if (!sound::start_background_worker()) {
        log::warn() << "sound: failed to start background worker";
    }

    /* Configure default IPv4 settings */
    (void)net::ipv4::configure_default(DEFAULT_KERNEL_IPV4);

    /* Launch the serial shell first so scheduler startup is visible on COM1. */
    log::info() << "Launching serial shell...";
    if (process::run("/bin/shell.vbin", process::console_interface::serial) < 0) {
        vk_panic(__FILE__, __LINE__, "Failed to launch serial shell!");
    }

    /* Prefer vkGUI for framebuffer sessions; fall back to the classic shell. */
    if (fb_info.valid) {
        log::info() << "Launching graphical shell...";
        if (process::run("/bin/vkgui.vbin", process::console_interface::graphical) < 0 &&
            process::run_command_line("/bin/shell.vbin --startup", process::console_interface::graphical) < 0) {
            vk_panic(__FILE__, __LINE__, "Failed to launch graphical shell!");
        }
    } else {
        log::warn() << "Framebuffer unavailable; graphical shell not launched";
    }

    /* Start the scheduler — does not return */
    log::debug() << "Transferring control to scheduler...";
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
