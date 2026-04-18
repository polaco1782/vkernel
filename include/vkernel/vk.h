/*
 * vkernel - UEFI Microkernel
 * Copyright (C) 2026 vkernel authors
 *
 * vk.h - Kernel API table passed to loaded ELF programs
 *
 * This header is the single source of truth for the kernel/userspace
 * ABI. Userspace code should include it directly, or via the thin
 * wrapper in userspace/include/vk.h, so the build always sees the
 * current layout.
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
typedef long long          vk_i64;
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

typedef vk_u64 vk_file_handle_t;

/* ============================================================
 * Freestanding math helpers
 * ============================================================ */

static inline float vk_absf(float v) {
    return v < 0.0f ? -v : v;
}

static inline float vk_fminf(float a, float b) {
    return a < b ? a : b;
}

static inline float vk_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float vk_sqrtf(float v) {
    if (v <= 0.0f) {
        return 0.0f;
    }

    float g = v > 1.0f ? v : 1.0f;
    for (int i = 0; i < 12; ++i) {
        g = 0.5f * (g + v / g);
    }
    return g;
}

/*
 * tan(x) via Taylor series. Accurate to < 0.01% for |x| < 0.9 rad (~52 deg),
 * which covers practical camera vfov half-angles and defocus angles.
 */
static inline float vk_tanf(float x) {
    float x2 = x * x;
    return x * (1.0f + x2 * (1.0f / 3.0f
                       + x2 * (2.0f / 15.0f
                       + x2 * (17.0f / 315.0f
                       + x2 * (62.0f / 2835.0f)))));
}

static inline float vk_degrees_to_radians(float deg) {
    return deg * 3.14159265358979323846f / 180.0f;
}

/* ============================================================
 * vk_api_t — version 8
 *
 * Add new fields only at the END to preserve ABI compatibility.
 * Bump VK_API_VERSION when the layout changes in a breaking way.
 * ============================================================ */

typedef struct vk_api {
    /* ---- header ---- */
    vk_u64 api_version;

    /* ---- console output ---- */
    void (*vk_puts)(const char* s);
    void (*vk_putc)(char c);
    void (*vk_put_hex)(vk_u64 v);
    void (*vk_put_dec)(vk_u64 v);
    void (*vk_clear)(void);

    /* ---- console input ---- */
    char (*vk_getc)(void);
    char (*vk_try_getc)(void);

    /* ---- memory ---- */
    void* (*vk_malloc)(vk_usize size);
    void  (*vk_free)(void* ptr);
    void* (*vk_memset)(void* dest, int c, vk_usize n);
    void* (*vk_memcpy)(void* dest, const void* src, vk_usize n);
    void* (*vk_memmove)(void* dest, const void* src, vk_usize n);
    void* (*vk_memchr)(const void* ptr, int ch, vk_usize n);
    int   (*vk_memcmp)(const void* lhs, const void* rhs, vk_usize n);
    void* (*vk_realloc)(void* ptr, vk_usize size);

    /* ---- filesystem / ramfs ---- */
    int         (*vk_file_exists)(const char* name);
    vk_usize    (*vk_file_size)(const char* name);
    vk_usize    (*vk_file_read)(const char* name, void* buf, vk_usize buf_size);

    /* ---- process ---- */
    void (*vk_exit)(int code);
    void (*vk_yield)(void);
    void (*vk_sleep)(vk_u64 ticks);

    /* ---- framebuffer ---- */
    void (*vk_framebuffer_info)(vk_framebuffer_info_t* out);

    /* ---- file streams and filesystem ops ---- */
    vk_file_handle_t (*vk_file_open)(const char* path, const char* mode);
    int              (*vk_file_close)(vk_file_handle_t handle);
    vk_usize         (*vk_file_read_handle)(vk_file_handle_t handle, void* buf, vk_usize buf_size);
    vk_usize         (*vk_file_write_handle)(vk_file_handle_t handle, const void* buf, vk_usize buf_size);
    int              (*vk_file_seek)(vk_file_handle_t handle, vk_i64 offset, int whence);
    vk_i64           (*vk_file_tell)(vk_file_handle_t handle);
    int              (*vk_file_eof)(vk_file_handle_t handle);
    int              (*vk_file_error)(vk_file_handle_t handle);
    int              (*vk_file_flush)(vk_file_handle_t handle);
    int              (*vk_file_remove)(const char* path);
    int              (*vk_file_rename)(const char* old_path, const char* new_path);

    /* ---- process utilities ---- */
    vk_i64 (*vk_run)(const char* path);
    vk_u64 (*vk_tick_count)(void);
    void (*vk_dump_memory)(void);
    void (*vk_dump_tasks)(void);
    void (*vk_dump_idt)(void);
    void (*vk_reboot)(void);

} vk_api_t;

/* Current API version */
#define VK_API_VERSION 8ULL

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
    vk_get_api()->vk_puts(s);
}

static inline void vk_putc(char c) {
    vk_get_api()->vk_putc(c);
}

static inline void vk_put_hex(vk_u64 v) {
    vk_get_api()->vk_put_hex(v);
}

static inline void vk_put_dec(vk_u64 v) {
    vk_get_api()->vk_put_dec(v);
}

static inline void vk_clear(void) {
    vk_get_api()->vk_clear();
}

static inline char vk_getc(void) {
    return vk_get_api()->vk_getc();
}

static inline char vk_try_getc(void) {
    return vk_get_api()->vk_try_getc();
}

static inline void* vk_malloc(vk_usize size) {
    return vk_get_api()->vk_malloc(size);
}

static inline void vk_free(void* ptr) {
    vk_get_api()->vk_free(ptr);
}

static inline void* vk_memset(void* dest, int c, vk_usize n) {
    return vk_get_api()->vk_memset(dest, c, n);
}

static inline void* vk_memcpy(void* dest, const void* src, vk_usize n) {
    return vk_get_api()->vk_memcpy(dest, src, n);
}

static inline void* vk_memmove(void* dest, const void* src, vk_usize n) {
    return vk_get_api()->vk_memmove(dest, src, n);
}

static inline void* vk_memchr(const void* ptr, int ch, vk_usize n) {
    return vk_get_api()->vk_memchr(ptr, ch, n);
}

static inline int vk_memcmp(const void* lhs, const void* rhs, vk_usize n) {
    return vk_get_api()->vk_memcmp(lhs, rhs, n);
}

static inline void* vk_realloc(void* ptr, vk_usize size) {
    return vk_get_api()->vk_realloc(ptr, size);
}

static inline void vk_get_framebuffer_info(vk_framebuffer_info_t* out) {
    vk_get_api()->vk_framebuffer_info(out);
}

static inline int vk_file_exists(const char* name) {
    return vk_get_api()->vk_file_exists(name);
}

static inline vk_usize vk_file_size(const char* name) {
    return vk_get_api()->vk_file_size(name);
}

static inline vk_usize vk_file_read(const char* name, void* buf, vk_usize buf_size) {
    return vk_get_api()->vk_file_read(name, buf, buf_size);
}

static inline void vk_exit(int code) {
    vk_get_api()->vk_exit(code);
    VK_UNREACHABLE();
}

static inline void vk_yield(void) {
    vk_get_api()->vk_yield();
}

static inline void vk_sleep(vk_u64 ticks) {
    vk_get_api()->vk_sleep(ticks);
}

static inline vk_i64 vk_run(const char* path) {
    return vk_get_api()->vk_run(path);
}

static inline vk_u64 vk_tick_count(void) {
    return vk_get_api()->vk_tick_count();
}

static inline void vk_dump_memory(void) {
    vk_get_api()->vk_dump_memory();
}

static inline void vk_dump_tasks(void) {
    vk_get_api()->vk_dump_tasks();
}

static inline void vk_dump_idt(void) {
    vk_get_api()->vk_dump_idt();
}

static inline void vk_reboot(void) {
    vk_get_api()->vk_reboot();
}

static inline void vk_print_int(int n) {
    if (n < 0) {
        vk_putc('-');
        n = -n;
    }
    vk_put_dec((vk_u64)(unsigned int)n);
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
