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
#include "smp.h"

namespace vk {

/* ============================================================
 * Internal state
 * ============================================================ */

static task   g_tasks[MAX_TASKS];
static usize  g_task_count       = 0;
static bool   g_scheduler_active = false;
static u64    g_tick_count       = 0;

/*
 * Per-CPU current-task index.
 * Indexed by APIC ID (0-based, matches smp::MAX_CPUS).
 * The BSP is always APIC ID 0 on a standard PC; on unusual hardware
 * the BSP's APIC ID might differ — we map via smp::current_cpu_apic_id().
 */
static constexpr u32 MAX_APIC_IDS = 256;
static usize g_per_cpu_task[MAX_APIC_IDS];   /* current task index per CPU */

/* Global scheduler spinlock — held only during the brief task-pick window */
static spinlock g_sched_lock;

/* APIC ID of the Bootstrap Processor — only the BSP drives g_tick_count
 * via the PIT (IRQ0).  APs use their LAPIC timer for preemption only;
 * they must not increment the global tick or sleep timers will run N× fast. */
static u8 g_bsp_apic_id = 0;

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

/* Current-task index for this CPU */
static inline usize cpu_current_task() {
    u8 apic_id = smp::current_cpu_apic_id();
    return g_per_cpu_task[apic_id];
}

static inline void cpu_set_current_task(usize idx) {
    u8 apic_id = smp::current_cpu_apic_id();
    g_per_cpu_task[apic_id] = idx;
}

[[maybe_unused]] static void task_trampoline(void* user_data) {
    auto& t = g_tasks[cpu_current_task()];
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

    char bytes_buf[count * 3 + 1];
    log::hex_bytes(bytes_buf, sizeof(bytes_buf), bytes, count);
    log::debug("Dumping %zu bytes at entry point %#llx: %s", count, static_cast<unsigned long long>(entry_va), bytes_buf);
}

/* ============================================================
 * Scheduler API
 * ============================================================ */

auto sched::init() -> status_code {
    memory::memory_set(g_tasks, 0, sizeof(g_tasks));
    /* Initialise every CPU's "current task" slot to SCHED_NO_TASK.
     * Zero-filling would leave them pointing at task 0, which is wrong:
     * an AP that fires its LAPIC timer before calling start_ap() would
     * corrupt g_tasks[0].rsp if the slot were 0. */
    for (usize i = 0; i < MAX_APIC_IDS; ++i)
        g_per_cpu_task[i] = SCHED_NO_TASK;
    g_task_count       = 0;
    g_scheduler_active = false;
    g_tick_count       = 0;
    g_sched_lock.locked = 0;
    g_bsp_apic_id      = smp::current_cpu_apic_id();

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
    /* Only count real PIT hardware ticks, not software yields.
     * Note: on APs the LAPIC timer fires on vector 32 as well;  the
     * PIT only fires on the BSP (IRQ0 is wired to the BSP by the PIC). */
    if (g_yield_in_progress) {
        g_yield_in_progress = false;
    } else if (smp::current_cpu_apic_id() == g_bsp_apic_id) {
        /* Only the BSP's PIT (IRQ0) advances the global clock.
         * APs fire vector 32 from their LAPIC timers for scheduling only;
         * if they also incremented g_tick_count, ticks would run N× fast. */
        ++g_tick_count;
    }

    /* Send EOI.
     * - The BSP receives IRQ0 via the 8259 PIC (pic_eoi() writes port 0x20).
     * - APs receive vector 32 from their local LAPIC timer.
     *   The LAPIC requires a write to its EOI register (offset 0xB0) to
     *   acknowledge the interrupt; the 8259 EOI is a no-op for LAPIC sources.
     * Writing both is safe: the PIC ignores spurious EOIs and the LAPIC
     * ignores the PIC port write.
     */
    pic_eoi();
    {
        constexpr u32 MSR_IA32_APIC_BASE  = 0x1B;
        constexpr u64 APIC_BASE_PHYS_MASK = 0x0000'0000'FFFF'F000ULL;
        constexpr u32 LAPIC_EOI = 0x0B0;
        const u64 lapic_phys = arch::rdmsr(MSR_IA32_APIC_BASE) & APIC_BASE_PHYS_MASK;
        auto* lapic = reinterpret_cast<volatile u32*>(static_cast<usize>(lapic_phys));
        lapic[LAPIC_EOI / 4] = 0;
    }

    wake_sleeping_tasks();

    if (!g_scheduler_active || g_task_count < 2) {
        return regs;
    }

    g_sched_lock.acquire();

    usize cur = cpu_current_task();

    /* Save current task only if this CPU is actually running one.
     * cur == SCHED_NO_TASK on an AP that has never been assigned a task yet
     * (set in start_ap() before the first LAPIC-timer preemption).  Saving
     * into g_tasks[SCHED_NO_TASK] would be an out-of-bounds write and would
     * also corrupt another task's saved RSP if two CPUs happen to both have
     * SCHED_NO_TASK simultaneously. */
    if (cur < g_task_count) {
        g_tasks[cur].rsp = reinterpret_cast<u64>(regs);
        if (g_tasks[cur].state == task_state::running)
            g_tasks[cur].state = task_state::ready;
    }

    /* Round-robin: find next runnable task.
     * When cur is SCHED_NO_TASK, start scanning from the last slot so that
     * the very first increment wraps to slot 0, giving a fair start point. */
    usize next = (cur < g_task_count) ? cur : (g_task_count - 1);
    bool found = false;
    for (usize i = 0; i < g_task_count; ++i) {
        next = (next + 1) % g_task_count;
        if (g_tasks[next].state == task_state::ready) {
            found = true;
            break;
        }
    }

    if (!found) {
        g_sched_lock.release();
        return regs;
    }

    cpu_set_current_task(next);
    g_tasks[next].state = task_state::running;

    g_sched_lock.release();

    return reinterpret_cast<arch::register_state*>(g_tasks[next].rsp);
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
    cpu_set_current_task(0);
    g_tasks[0].state = task_state::running;

    /* Unmask IRQ0 and enable interrupts (BSP only) */
    pic_unmask_irq0();

    log::info("Scheduler starting...");

    /* Jump into the first task — naked asm, no compiler interference */
    sched_switch_to(g_tasks[cpu_current_task()].rsp);
}

/* ============================================================
 * AP entry point — called from ap_init_secondary() after
 * arch::ap_activate() and smp::lapic_init_local().
 *
 * Each AP:
 *   1. Programs its local LAPIC timer to fire at SCHED_HZ.
 *   2. Sets its per-CPU current task to the idle task (index 0).
 *   3. Enables interrupts and idles — the LAPIC timer will start
 *      preempting and dispatching tasks immediately.
 * ============================================================ */
[[noreturn]] void sched::start_ap() {
    /*
     * Programme the local APIC timer on this AP.
     * We use a one-shot → periodic sequence:
     *   - Divide-by-16 (DCR register = 0x3)
     *   - LVT Timer register: vector 32 (same as PIT), periodic mode
     *   - Initial count = calibrated from BSP's PIT frequency.
     *
     * Simple calibration: the BSP ticks at SCHED_HZ on the PIT;
     * the LAPIC timer bus frequency is typically in the hundreds of MHz
     * range.  We use a fixed count that is a reasonable default for
     * QEMU (bus ~100 MHz → 100 MHz / 16 / 100 Hz = 62500).
     * Real hardware should calibrate via TSC or PIT, but this is
     * sufficient for correctness at this stage.
     */
    static constexpr u32 LAPIC_TIMER_DCR     = 0x3E0; /* Divide Config  */
    static constexpr u32 LAPIC_TIMER_INITIAL = 0x380; /* Initial Count  */
    static constexpr u32 LAPIC_TIMER_LVT     = 0x320; /* LVT Timer      */
    static constexpr u32 LAPIC_TIMER_CURRENT = 0x390; /* Current Count  */
    static constexpr u32 LAPIC_DCR_DIV16     = 0x3;   /* divide by 16   */
    static constexpr u32 LAPIC_LVT_PERIODIC  = (1u << 17); /* periodic mode */
    static constexpr u32 LAPIC_TIMER_VECTOR  = 32;    /* same vector as PIT */
    static constexpr u32 LAPIC_TIMER_COUNT   = 62500; /* ~100 Hz at 100 MHz / 16 */

    /* LAPIC base address — read from MSR (same as BSP) */
    constexpr u32 MSR_IA32_APIC_BASE  = 0x1B;
    constexpr u64 APIC_BASE_PHYS_MASK = 0x0000'0000'FFFF'F000ULL;
    const u64 lapic_phys = arch::rdmsr(MSR_IA32_APIC_BASE) & APIC_BASE_PHYS_MASK;
    auto* lapic = reinterpret_cast<volatile u32*>(static_cast<usize>(lapic_phys));

    /* Divide by 16 */
    lapic[LAPIC_TIMER_DCR / 4] = LAPIC_DCR_DIV16;

    /* LVT: vector 32, periodic, not masked */
    lapic[LAPIC_TIMER_LVT / 4] = LAPIC_LVT_PERIODIC | LAPIC_TIMER_VECTOR;

    /* Arm the timer */
    lapic[LAPIC_TIMER_INITIAL / 4] = LAPIC_TIMER_COUNT;
    (void)lapic[LAPIC_TIMER_CURRENT / 4]; /* serialise */

    log::debug("AP APIC %u: LAPIC timer armed, entering scheduler",
               smp::current_cpu_apic_id());

    /* Signal that this AP has no current task yet.
     * Using SCHED_NO_TASK prevents the first LAPIC-timer preemption from
     * overwriting g_tasks[0].rsp (task 0 may still be running on the BSP
     * or may not yet have been started).  preempt() will pick up the first
     * ready task on its own without touching any existing task's saved state. */
    g_sched_lock.acquire();
    cpu_set_current_task(SCHED_NO_TASK);
    g_sched_lock.release();

    /* Enable interrupts — LAPIC timer will fire immediately */
    arch::enable_interrupts();

    /* Idle spin — preemption will switch us to a real task */
    while (true) {
        arch::cpu_halt();
    }
}

auto sched::current_task_id() -> u64 {
    usize t = cpu_current_task();
    if (t >= g_task_count) return 0;
    return g_tasks[t].id;
}

auto sched::current_task_name() -> const char* {
    usize t = cpu_current_task();
    if (t >= g_task_count) return "<idle>";
    return g_tasks[t].name;
}

auto sched::current_task_user_data() -> void* {
    usize t = cpu_current_task();
    if (t >= g_task_count) return null;
    return g_tasks[t].user_data;
}

auto sched::tick_count() -> u64 {
    return g_tick_count;
}

void sched::sleep(u64 ticks) {
    if (ticks == 0) {
        yield();
        return;
    }

    auto& t = g_tasks[cpu_current_task()];
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
    usize cur = cpu_current_task();
    g_tasks[cur].state = task_state::terminated;
    log::debug("Task terminated: %s", g_tasks[cur].name);
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