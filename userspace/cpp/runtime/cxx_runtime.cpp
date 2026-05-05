#include <new>
#include <stddef.h>
#include <stdlib.h>

void* __dso_handle __attribute__((weak, visibility("hidden"))) = nullptr;

extern "C" int __cxa_atexit(void (*)(void*), void*, void*) noexcept
{
    return 0;
}

extern "C" void __cxa_finalize(void*) noexcept
{
}

extern "C" int __cxa_guard_acquire(long long* guard) noexcept
{
    return *reinterpret_cast<unsigned char*>(guard) == 1u ? 0 : 1;
}

extern "C" void __cxa_guard_release(long long* guard) noexcept
{
    *reinterpret_cast<unsigned char*>(guard) = 1u;
}

extern "C" void __cxa_guard_abort(long long* guard) noexcept
{
    *reinterpret_cast<unsigned char*>(guard) = 0u;
}

static void* allocate_or_abort(size_t size)
{
    if (size == 0) {
        size = 1;
    }
    void* ptr = malloc(size);
    if (ptr == nullptr) {
        abort();
    }
    return ptr;
}

void* operator new(std::size_t size)
{
    return allocate_or_abort(size);
}

void* operator new[](std::size_t size)
{
    return allocate_or_abort(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    if (size == 0) {
        size = 1;
    }
    return malloc(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    if (size == 0) {
        size = 1;
    }
    return malloc(size);
}

void operator delete(void* ptr) noexcept
{
    free(ptr);
}

void operator delete[](void* ptr) noexcept
{
    free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept
{
    free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept
{
    free(ptr);
}

extern "C" void __cxa_pure_virtual() noexcept
{
    abort();
}

namespace std {

const nothrow_t nothrow{};

void terminate() noexcept
{
    abort();
}

}  // namespace std