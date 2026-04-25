/*
 * vgui/cxxabi_stubs.cpp
 * Minimal freestanding C++ ABI support for vkernel.
 *
 * Provides the symbols normally supplied by libstdc++ / libsupc++ that
 * Dear ImGui requires when compiled in a bare-metal environment:
 *
 *   operator new / delete      — heap allocation via newlib malloc/free
 *   __cxa_guard_*              — thread-safe static-local initialisation
 *                                (trivial single-threaded version)
 *   __cxa_atexit/__cxa_finalize — destructor registration (no-op; the
 *                                vkernel process model does not need it)
 *   __dso_handle               — DSO identifier for atexit (null)
 *   __cxa_pure_virtual         — trap if a pure-virtual call ever happens
 *   std::terminate             — last-resort abort
 *
 * Compile with the same flags as the rest of the project.
 * Must appear in the link BEFORE any static library that would otherwise
 * supply conflicting definitions (e.g. the host's libstdc++.a).
 */

#include <stddef.h>   /* size_t      */
#include <stdlib.h>   /* malloc, free, abort */

/* ================================================================
 * DSO handle (required by __cxa_atexit ABI)
 * ================================================================ */

void* __dso_handle __attribute__((weak, visibility("hidden"))) = nullptr;

/* ================================================================
 * atexit / finalize  — no-op in this single-process environment
 * ================================================================ */

extern "C" int __cxa_atexit(void (*/*func*/)(void*),
                              void* /*arg*/,
                              void* /*dso*/) noexcept
{
    return 0;   /* success, but we never actually run the destructor */
}

extern "C" void __cxa_finalize(void* /*dso*/) noexcept
{
    /* nothing */
}

/* ================================================================
 * Static-local initialisation guards
 *
 * Itanium ABI: the guard object is 64 bits.  Byte 0 of the object
 * (lowest address) is the initialisation flag:
 *   0  → not yet initialised
 *   1  → fully initialised
 * Byte 1 is the "in-progress" flag used for re-entrancy / threads;
 * we ignore thread-safety here (single-threaded environment).
 * ================================================================ */

extern "C" int __cxa_guard_acquire(long long* guard) noexcept
{
    /* If byte 0 is already 1 the object is initialised; skip. */
    if (*reinterpret_cast<unsigned char*>(guard) == 1u)
        return 0;
    /* Otherwise signal that we should proceed with initialisation. */
    return 1;
}

extern "C" void __cxa_guard_release(long long* guard) noexcept
{
    /* Mark the object as fully initialised. */
    *reinterpret_cast<unsigned char*>(guard) = 1u;
}

extern "C" void __cxa_guard_abort(long long* guard) noexcept
{
    /* Initialisation failed; reset so it can be retried. */
    *reinterpret_cast<unsigned char*>(guard) = 0u;
}

/* ================================================================
 * operator new / delete  — routed through newlib malloc/free
 * ================================================================ */

void* operator new(size_t sz)
{
    if (sz == 0) sz = 1;
    void* p = malloc(sz);
    if (!p) abort();
    return p;
}

void* operator new[](size_t sz)
{
    if (sz == 0) sz = 1;
    void* p = malloc(sz);
    if (!p) abort();
    return p;
}

/* nothrow variants */
void* operator new(size_t sz, const struct _nothrow_t&) noexcept
{
    if (sz == 0) sz = 1;
    return malloc(sz);
}

void* operator new[](size_t sz, const struct _nothrow_t&) noexcept
{
    if (sz == 0) sz = 1;
    return malloc(sz);
}

void operator delete(void* p) noexcept            { free(p); }
void operator delete[](void* p) noexcept          { free(p); }
void operator delete(void* p, size_t) noexcept    { free(p); }
void operator delete[](void* p, size_t) noexcept  { free(p); }

/* ================================================================
 * Pure-virtual call handler
 * ================================================================ */

extern "C" void __cxa_pure_virtual() noexcept
{
    abort();
}

/* ================================================================
 * std::terminate  — called by the C++ runtime on unhandled exceptions,
 *                   constraint violations, or explicit terminate() calls.
 *                   Since we compile with -fno-exceptions this should
 *                   never be reached, but must be defined.
 * ================================================================ */

namespace std {
    void terminate() noexcept __attribute__((noreturn));
    void terminate() noexcept { abort(); }
}
