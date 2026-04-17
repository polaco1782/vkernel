/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * vk.h - Kernel API table passed to loaded ELF programs
 *
 * This header is the single source of truth for the kernel/userspace
 * ABI. It is copied into the userspace include tree at build time so
 * freestanding binaries and the kernel always see the same layout.
 *
 * The kernel fills in a vk_api struct and passes a pointer to it
 * as the first argument of every ELF entry point:
 *
 *   int _start(const vk_api_t* api);
 */

#ifndef VKERNEL_VK_H
#define VKERNEL_VK_H

#ifndef VK_UNREACHABLE
    #if defined(_MSC_VER)
        #define VK_UNREACHABLE() __assume(0)
    #else
        #define VK_UNREACHABLE() __builtin_unreachable()
    #endif
#endif

#ifndef VK_NORETURN
    #if defined(_MSC_VER)
        #define VK_NORETURN __declspec(noreturn)
    #else
        #define VK_NORETURN __attribute__((noreturn))
    #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long long vk_u64;
typedef unsigned int       vk_u32;
#if defined(_MSC_VER)
typedef unsigned long long vk_usize;
#else
typedef unsigned long      vk_usize;
#endif

typedef enum vk_pixel_format {
    VK_PIXEL_FORMAT_RGBX_8BPP = 0,
    VK_PIXEL_FORMAT_BGRX_8BPP = 1,
    VK_PIXEL_FORMAT_BITMASK   = 2,
    VK_PIXEL_FORMAT_BLT_ONLY  = 3,
} vk_pixel_format_t;

typedef struct vk_framebuffer_info {
    vk_u64            base;
    vk_u32            width;
    vk_u32            height;
    vk_u32            stride;
    vk_pixel_format_t format;
    vk_u32            valid;
} vk_framebuffer_info_t;

/* ============================================================
 * vk_api_t — version 3
 *
 * Add new fields only at the END to preserve ABI compatibility.
 * Bump VK_API_VERSION when the layout changes in a breaking way.
 * ============================================================ */

typedef struct vk_api {
    /* ---- header (offset 0) ---- */
    vk_u64 api_version;

    /* ---- console output (offset 8) ---- */
    void (*puts)(const char* s);
    void (*putc)(char c);
    void (*put_hex)(vk_u64 v);
    void (*put_dec)(vk_u64 v);
    void (*clear)(void);

    /* ---- console input (offset 48) ---- */
    char (*getc)(void);
    char (*try_getc)(void);

    /* ---- memory (offset 64) ---- */
    void* (*malloc)(vk_usize size);
    void  (*free)(void* ptr);
    void* (*memset)(void* dest, int c, vk_usize n);
    void* (*memcpy)(void* dest, const void* src, vk_usize n);

    /* ---- filesystem / ramfs (offset 96) ---- */
    int         (*file_exists)(const char* name);
    vk_usize    (*file_size)(const char* name);
    vk_usize    (*file_read)(const char* name, void* buf, vk_usize buf_size);

    /* ---- process (offset 120) ---- */
    void (*exit)(int code);
    void (*yield)(void);
    void (*sleep)(vk_u64 ticks);

    /* ---- framebuffer (offset 144) ---- */
    void (*framebuffer_info)(vk_framebuffer_info_t* out);

} vk_api_t;

/* Current API version */
#define VK_API_VERSION 3ULL

/* ============================================================
 * Userspace runtime helpers
 * ============================================================ */

static inline const vk_api_t** vk_api_slot(void) {
    static const vk_api_t* api;
    return &api;
}

static inline void vk_init(const vk_api_t* api) {
    *vk_api_slot() = api;
}

static inline const vk_api_t* vk_get_api(void) {
    return *vk_api_slot();
}

static inline void vk_puts(const char* s) {
    vk_get_api()->puts(s);
}

static inline void vk_putc(char c) {
    vk_get_api()->putc(c);
}

static inline void vk_put_hex(vk_u64 v) {
    vk_get_api()->put_hex(v);
}

static inline void vk_put_dec(vk_u64 v) {
    vk_get_api()->put_dec(v);
}

static inline void vk_clear(void) {
    vk_get_api()->clear();
}

static inline char vk_getc(void) {
    return vk_get_api()->getc();
}

static inline char vk_try_getc(void) {
    return vk_get_api()->try_getc();
}

static inline void* vk_malloc(vk_usize size) {
    return vk_get_api()->malloc(size);
}

static inline void vk_free(void* ptr) {
    vk_get_api()->free(ptr);
}

static inline void* vk_memset(void* dest, int c, vk_usize n) {
    return vk_get_api()->memset(dest, c, n);
}

static inline void* vk_memcpy(void* dest, const void* src, vk_usize n) {
    return vk_get_api()->memcpy(dest, src, n);
}

static inline void vk_get_framebuffer_info(vk_framebuffer_info_t* out) {
    vk_get_api()->framebuffer_info(out);
}

static inline int vk_file_exists(const char* name) {
    return vk_get_api()->file_exists(name);
}

static inline vk_usize vk_file_size(const char* name) {
    return vk_get_api()->file_size(name);
}

static inline vk_usize vk_file_read(const char* name, void* buf, vk_usize buf_size) {
    return vk_get_api()->file_read(name, buf, buf_size);
}

static inline void vk_exit(int code) {
    vk_get_api()->exit(code);
    VK_UNREACHABLE();
}

static inline void vk_yield(void) {
    vk_get_api()->yield();
}

static inline void vk_sleep(vk_u64 ticks) {
    vk_get_api()->sleep(ticks);
}

static inline void vk_print_int(int n) {
    if (n < 0) {
        vk_putc('-');
        n = -n;
    }
    vk_put_dec((vk_u64)(unsigned int)n);
}

static inline vk_usize vk_strlen(const char* s) {
    vk_usize len = 0;
    while (s[len]) ++len;
    return len;
}

static inline vk_usize vk_getline(char* buf, vk_usize max) {
    vk_usize pos = 0;
    while (pos < max - 1) {
        char c = vk_getc();
        if (c == '\r' || c == '\n') {
            vk_putc('\n');
            break;
        }
        if ((c == 0x7F || c == '\b') && pos > 0) {
            --pos;
            vk_puts("\b \b");
            continue;
        }
        if (c >= ' ' && c < 0x7F) {
            buf[pos++] = c;
            vk_putc(c);
        }
    }
    buf[pos] = '\0';
    return pos;
}

#ifdef __cplusplus
} /* extern "C" */

namespace vk {
namespace kernel_api {

void init();
auto get_api() -> const vk_api_t*;

} // namespace kernel_api
} // namespace vk
#endif

#endif /* VKERNEL_VK_H */
