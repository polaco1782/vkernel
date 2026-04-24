/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * scheduler.cpp - Round-robin preemptive scheduler
 *
 * Uses the PIT (8254) on IRQ0 (vector 32) for preemption.
 * Context switches save/restore RSP; the full register file is
 * pushed by the ISR stub in interrupts.S.
 */

#include "config.h"
#include "types.h"
#include "console.h"
#include "memory.h"
#include "scheduler.h"
#include "panic.h"
#include "arch/x86_64/arch.h"

namespace vk {

/* ============================================================
 * Internal state
 * ============================================================ */

static task  g_tasks[MAX_TASKS];
static usize g_task_count    = 0;
static usize g_current_task  = 0;
static bool  g_scheduler_active = false;
static u64   g_tick_count = 0;

/* ============================================================
 * PIT (Programmable Interval Timer) helpers — 8254 chip
 * Channel 0 → IRQ 0 → IDT vector 32
 * ============================================================ */

static constexpr u16 PIT_CH0_DATA = 0x40;
static constexpr u16 PIT_CMD      = 0x43;
static constexpr u32 PIT_FREQ     = 1193182;  /* Hz */
static constexpr u32 SCHED_HZ     = 100;      /* 100 Hz → 10 ms tick */

/* PIC (8259A) ports */
static constexpr u16 PIC1_CMD  = 0x20;
static constexpr u16 PIC1_DATA = 0x21;
static constexpr u16 PIC2_CMD  = 0xA0;
static constexpr u16 PIC2_DATA = 0xA1;

static void pic_remap() {
    /* Save masks */
    u8 mask1 = arch::inb(PIC1_DATA);
    u8 mask2 = arch::inb(PIC2_DATA);

    log::debug("PIC: remapping — IRQ0→vec32, IRQ8→vec40");

    /* ICW1: init + cascade + ICW4 needed */
    arch::outb(PIC1_CMD, 0x11);
    arch::outb(PIC2_CMD, 0x11);

    /* ICW2: vector offsets — IRQ0 → 32, IRQ8 → 40 */
    arch::outb(PIC1_DATA, 32);
    arch::outb(PIC2_DATA, 40);

    /* ICW3: master has slave on IRQ2, slave ID 2 */
    arch::outb(PIC1_DATA, 4);
    arch::outb(PIC2_DATA, 2);

    /* ICW4: 8086 mode */
    arch::outb(PIC1_DATA, 0x01);
    arch::outb(PIC2_DATA, 0x01);

    /* Restore masks — mask everything initially */
    arch::outb(PIC1_DATA, 0xFF);
    arch::outb(PIC2_DATA, 0xFF);
}

static void pit_init() {
    u16 divisor = static_cast<u16>(PIT_FREQ / SCHED_HZ);

    log::debug("PIT: divisor=%#x (target %u Hz)", divisor, SCHED_HZ);

    /* Channel 0, lobyte/hibyte, rate generator */
    arch::outb(PIT_CMD, 0x36);
    arch::outb(PIT_CH0_DATA, static_cast<u8>(divisor & 0xFF));
    arch::outb(PIT_CH0_DATA, static_cast<u8>((divisor >> 8) & 0xFF));
}

static void pic_unmask_irq0() {
    u8 mask = arch::inb(PIC1_DATA);
    mask &= ~(1u << 0);  /* Clear bit 0 → unmask IRQ0 */
    arch::outb(PIC1_DATA, mask);
}

static void pic_eoi() {
    arch::outb(PIC1_CMD, 0x20);
}

/* ============================================================
 * C-string helper
 * ============================================================ */

static void str_copy(char* dst, const char* src, usize max) {
    usize i = 0;
    while (i + 1 < max && src[i]) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}

/* ============================================================
 * Scheduler internals
 * ============================================================ */
[[maybe_unused]] static void task_trampoline(void* user_data) {
    auto& t = g_tasks[g_current_task];
    if (t.entry) t.entry(t.user_data);
    sched::exit_task();
}

static void wake_sleeping_tasks() {
    for (usize i = 0; i < g_task_count; ++i) {
        if (g_tasks[i].state == task_state::blocked && g_tasks[i].wake_tick <= g_tick_count) {
            g_tasks[i].state = task_state::ready;
        }
    }
}

/* ============================================================
 * Entry point validation helper (freestanding-safe)
 * ============================================================ */

[[maybe_unused]] static bool validate_entry_point(u64 entry_va) {
    const u8* bytes = reinterpret_cast<const u8*>(entry_va);
    
    /* Check for obvious fill patterns (0x00, 0xFF, 0xCC) */
    bool has_real_code = false;
    for (int i = 0; i < 16; ++i) {
        u8 b = bytes[i];
        if (b != 0x00 && b != 0xFF && b != 0xCC && b != 0x90) {
            has_real_code = true;
            break;
        }
    }
    return has_real_code;
}

[[maybe_unused]] static void dump_entry_bytes(u64 entry_va, usize count = 32) {
    const u8* bytes = reinterpret_cast<const u8*>(entry_va);

    log::debug("Dumping %zu bytes at entry point %#llx:", count, static_cast<unsigned long long>(entry_va));

    for (usize i = 0; i < count; ++i) {
        log::debug("  [%02zu] %#02x", i, bytes[i]);

        if ((i + 1) % 16 == 0 && i + 1 < count)
            log::debug("");
    }
}

/* ============================================================
 * Scheduler API
 * ============================================================ */

auto sched::init() -> status_code {
    memory::memory_set(g_tasks, 0, sizeof(g_tasks));
    g_task_count   = 0;
    g_current_task = 0;
    g_scheduler_active = false;
    g_tick_count = 0;

    /* Remap PIC so IRQ0 -> vector 32 */
    pic_remap();

    /* Init PIT for periodic ticks */
    pit_init();

    log::info("Scheduler initialized (PIT @ 100 Hz)");
    return status_code::success;
}

auto sched::create_task(const char* name, task_entry_fn entry, void* user_data) -> i64 {
    if (g_task_count >= MAX_TASKS) return -1;

    auto& t = g_tasks[g_task_count];
    t.id    = g_task_count;
    t.state = task_state::ready;
    t.wake_tick = 0;
    t.entry = entry;
    t.user_data = user_data;
    str_copy(t.name, name, sizeof(t.name));

    /*
     * Set up the initial stack frame so the first context switch
     * "returns" into task_trampoline.
     *
     * The stack layout matches what our ISR stubs push:
     *   [top]  SS, RSP, RFLAGS, CS, RIP  (iretq frame)
     *          error_code, int_no
     *          FS, GS
     *          R15..R8, RBP, RDI, RSI, RDX, RCX, RBX, RAX
     *
     * Total: 5 + 2 + 2 + 15 = 24 u64s
     */
    auto* stack_top = reinterpret_cast<u64*>(
        reinterpret_cast<usize>(&t.stack[TASK_STACK_SIZE]) & ~0xFull);

    /* iretq frame */
    stack_top[-1] = arch::SEG_KERNEL_DATA; /* SS */
    stack_top[-2] = reinterpret_cast<u64>(stack_top - 1); /* RSP — 16-byte aligned entry stack */
    stack_top[-3] = 0x202;  /* RFLAGS: IF=1 */
    stack_top[-4] = arch::SEG_KERNEL_CODE; /* CS */
    stack_top[-5] = reinterpret_cast<u64>(&task_trampoline); /* RIP */

    /* error_code + int_no */
    stack_top[-6] = 0; /* error_code */
    stack_top[-7] = 0; /* int_no */

    /* FS, GS — FS at lower address (pushed last by ISR), GS at higher */
    stack_top[-8] = 0; /* GS (higher addr) */
    stack_top[-9] = 0; /* FS (lower addr) */

    /* 15 GPRs: R15..R8, RBP, RDI, RSI, RDX, RCX, RBX, RAX */
    for (int i = 10; i <= 24; ++i)
        stack_top[-i] = 0;

    t.rsp = reinterpret_cast<u64>(stack_top - 24);

    if constexpr (log::debug_enabled()) {
        log::debug("task '%s': entry=%#llx user_data=%p rsp=%#llx",
                   name,
                   static_cast<unsigned long long>(reinterpret_cast<u64>(entry)),
                   user_data,
                   static_cast<unsigned long long>(t.rsp));

        if (!validate_entry_point(reinterpret_cast<u64>(entry))) {
            log::warn("Task '%s' entry point may contain fill pattern", name);
            dump_entry_bytes(reinterpret_cast<u64>(entry), 32);
        } else {
            log::debug("Task '%s' entry point looks like valid code", name);
            dump_entry_bytes(reinterpret_cast<u64>(entry), 16);
        }

        auto* frame = reinterpret_cast<u64*>(t.rsp);
        log::debug("Initial frame @%#llx: RIP=%#llx CS=%#llx RFLAGS=%#llx RSP=%#llx SS=%#llx RCX=%#llx",
                   static_cast<unsigned long long>(t.rsp),
                   static_cast<unsigned long long>(frame[19]),
                   static_cast<unsigned long long>(frame[20]),
                   static_cast<unsigned long long>(frame[21]),
                   static_cast<unsigned long long>(frame[22]),
                   static_cast<unsigned long long>(frame[23]),
                   static_cast<unsigned long long>(frame[16]));
    }

    i64 id = static_cast<i64>(g_task_count);
    ++g_task_count;

    log::info("Task created: %s (id=%llu)",
              name, static_cast<unsigned long long>(id));

    return id;
}

static volatile bool g_yield_in_progress = false;

void sched::yield() {
    if (!g_scheduler_active || g_task_count < 2) return;
    g_yield_in_progress = true;
    asm_int_timer();
}

auto sched::preempt(arch::register_state* regs) -> arch::register_state* {
    /* Only count real PIT hardware ticks, not software yields */
    if (g_yield_in_progress) {
        g_yield_in_progress = false;
    } else {
        ++g_tick_count;
    }
    wake_sleeping_tasks();

    if (!g_scheduler_active || g_task_count < 2) {
        pic_eoi();
        return regs;
    }

    /* Save current RSP (points to the register_state on the stack) */
    g_tasks[g_current_task].rsp = reinterpret_cast<u64>(regs);
    if (g_tasks[g_current_task].state == task_state::running)
        g_tasks[g_current_task].state = task_state::ready;

    /* Round-robin: find next runnable task */
    usize next = g_current_task;
    bool found = false;
    for (usize i = 0; i < g_task_count; ++i) {
        next = (next + 1) % g_task_count;
        if (g_tasks[next].state == task_state::ready) {
            found = true;
            break;
        }
    }

    if (!found) {
        pic_eoi();
        return regs;
    }

    g_current_task = next;
    g_tasks[g_current_task].state = task_state::running;

    pic_eoi();

    /* Return the new task's saved context — the ISR assembly will
     * set RSP to this value before popping registers + iretq. */
    return reinterpret_cast<arch::register_state*>(g_tasks[g_current_task].rsp);
}

/*
 * sched_switch_to — load a full register_state frame and iretq.
 * On MSVC this is implemented in msvc_asm.asm as asm_sched_switch_to.
 * On GCC/Clang we use a naked function with inline asm.
 */
[[noreturn]] static inline void sched_switch_to(u64 rsp) {
    asm_sched_switch_to(rsp);
}

[[noreturn]] void sched::start() {
    if (g_task_count == 0) {
        vk_panic(__FILE__, __LINE__, "No tasks to schedule");
    }

    g_scheduler_active = true;
    g_current_task = 0;
    g_tasks[0].state = task_state::running;

    /* Unmask IRQ0 and enable interrupts */
    pic_unmask_irq0();

    log::info("Scheduler starting...");

    if constexpr (log::debug_enabled()) {
        auto* s = reinterpret_cast<u64*>(g_tasks[0].rsp);
        log::debug("=== FIRST TASK INITIAL FRAME ===");
        log::debug("Task: %s", g_tasks[0].name);
        log::debug("Frame base (RSP on iretq): %#llx", static_cast<unsigned long long>(g_tasks[0].rsp));
        log::debug("RIP=%#llx CS=%#llx RFLAGS=%#llx RSP=%#llx SS=%#llx RCX=%#llx RDX=%#llx R8=%#llx R9=%#llx",
                   static_cast<unsigned long long>(s[19]),
                   static_cast<unsigned long long>(s[20]),
                   static_cast<unsigned long long>(s[21]),
                   static_cast<unsigned long long>(s[22]),
                   static_cast<unsigned long long>(s[23]),
                   static_cast<unsigned long long>(s[16]),
                   static_cast<unsigned long long>(s[15]),
                   static_cast<unsigned long long>(s[12]),
                   static_cast<unsigned long long>(s[11]));

        u64 entry = s[19];
        if (!validate_entry_point(entry)) {
            log::error("Entry point contains fill pattern - aborting");
            dump_entry_bytes(entry, 64);
            __builtin_trap();
        }

        log::debug("Entry point first 32 bytes:");
        dump_entry_bytes(entry, 32);
        log::debug("=====================================");
    }

    /* Jump into the first task — naked asm, no compiler interference */
    sched_switch_to(g_tasks[0].rsp);
}

auto sched::current_task_id() -> u64 {
    return g_tasks[g_current_task].id;
}

auto sched::current_task_name() -> const char* {
    return g_tasks[g_current_task].name;
}

auto sched::current_task_user_data() -> void* {
    return g_tasks[g_current_task].user_data;
}

auto sched::tick_count() -> u64 {
    return g_tick_count;
}

void sched::sleep(u64 ticks) {
    if (ticks == 0) {
        yield();
        return;
    }

    auto& t = g_tasks[g_current_task];
    t.wake_tick = g_tick_count + ticks;
    t.state = task_state::blocked;

    while (t.state == task_state::blocked) {
        yield();
        if (t.state == task_state::blocked) {
            arch::cpu_halt();
        }
    }
}

void sched::wait_for_task(u64 task_id) {
    while (true) {
        bool found = false;
        for (usize i = 0; i < g_task_count; ++i) {
            if (g_tasks[i].id == task_id) {
                found = true;
                if (g_tasks[i].state == task_state::terminated)
                    return;
                break;
            }
        }
        if (!found) return; /* task never existed */
        yield();
    }
}

[[noreturn]] void sched::exit_task() {
    arch::disable_interrupts();
    g_tasks[g_current_task].state = task_state::terminated;
    log::debug("Task terminated: %s", g_tasks[g_current_task].name);
    arch::enable_interrupts();
    /* Yield to let scheduler pick another task */
    while (true) { sched::yield(); arch::cpu_halt(); }
    VK_UNREACHABLE();
}

void sched::dump_tasks() {
    log::info("Task list:");
    for (usize i = 0; i < g_task_count; ++i) {
        log::info("  [%llu] %s - %s",
                  static_cast<unsigned long long>(g_tasks[i].id),
                  g_tasks[i].name,
                  (g_tasks[i].state == task_state::ready) ? "ready" :
                  (g_tasks[i].state == task_state::running) ? "running" :
                  (g_tasks[i].state == task_state::blocked) ? "blocked" :
                  (g_tasks[i].state == task_state::terminated) ? "terminated" : "unknown");
    }
}

} // namespace vk