/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * spinlock.h - Simple test-and-set spinlock
 *
 * Suitable for short critical sections.  Used by the scheduler,
 * memory allocators, console, and any other subsystem that needs
 * SMP-safe mutual exclusion.
 */

#ifndef VKERNEL_SPINLOCK_H
#define VKERNEL_SPINLOCK_H

#include "types.h"
#include "arch/x86_64/arch.h"

namespace vk {

/* Forward decl from smp.h to avoid circular include */
namespace smp { u8 current_cpu_apic_id(); }

/*
 * Interrupt-safe test-and-set spinlock with re-entry detection.
 *
 * Both acquire() and release() save/restore the caller's RFLAGS.IF
 * so that a critical section is atomic with respect to local
 * interrupts as well as other CPUs.
 *
 * try_acquire() returns false instead of spinning if the lock is
 * already held by another CPU; this lets crash/panic paths print
 * concurrently without deadlocking when one CPU is mid-log and an
 * exception fires on a different CPU that wants to log too.
 *
 * Without disabling interrupts, a single-CPU system can self-deadlock:
 *   1. Task acquires the lock (IF=1)
 *   2. Timer ISR fires
 *   3. ISR tries to acquire the same lock → spins forever, since
 *      the only CPU that could release it is preempted.
 */
struct spinlock {
    /* locked: 0 = free, 1+ = APIC id of owner + 1 (so 0 means free) */
    volatile u64 locked = 0;
    u64 saved_flags = 0;

    void acquire() {
        u64 flags = arch::read_rflags();
        arch::disable_interrupts();
        u64 self = static_cast<u64>(smp::current_cpu_apic_id()) + 1;
        while (!arch::atomic_cmpxchg(&locked, 0, self)) {
            arch::cpu_pause();
        }
        arch::memory_barrier();
        saved_flags = flags;
    }

    /* Try to acquire without spinning.  Returns true if we got it. */
    bool try_acquire() {
        u64 flags = arch::read_rflags();
        arch::disable_interrupts();
        u64 self = static_cast<u64>(smp::current_cpu_apic_id()) + 1;
        if (!arch::atomic_cmpxchg(&locked, 0, self)) {
            /* Restore IF if we never owned the lock */
            if (flags & arch::FLAGS_INTERRUPT) arch::enable_interrupts();
            return false;
        }
        arch::memory_barrier();
        saved_flags = flags;
        return true;
    }

    /* True if THIS cpu currently holds the lock. */
    bool held_by_self() const {
        u64 self = static_cast<u64>(smp::current_cpu_apic_id()) + 1;
        return locked == self;
    }

    void release() {
        u64 flags = saved_flags;
        arch::memory_barrier();
        locked = 0;
        if (flags & arch::FLAGS_INTERRUPT) {
            arch::enable_interrupts();
        }
    }
};

} // namespace vk

#endif /* VKERNEL_SPINLOCK_H */
