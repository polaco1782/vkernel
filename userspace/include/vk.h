/*
 * vkernel userspace - compatibility wrapper
 * Copyright (C) 2026 vkernel authors
 *
 * vk.h - libc-style convenience layer for freestanding userspace code
 *
 * Include this header when porting code that expects a small slice of
 * the C standard library. It builds on top of the canonical ABI header
 * in the repository include/vkernel/vk.h and provides lightweight
 * inline shims.
 */

#ifndef VK_USERSPACE_COMPAT_H
#define VK_USERSPACE_COMPAT_H

#include <stddef.h>
#include <stdarg.h>

#include "../../include/vkernel/vk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Memory
 * ============================================================ */

static inline void* malloc(size_t size) {
    return vk_malloc((vk_usize)size);
}

static inline void free(void* ptr) {
    vk_free(ptr);
}

static inline void* vk_calloc(vk_usize count, vk_usize size) {
    vk_usize total = count * size;
    if (count != 0 && total / count != size) return (void*)0;
    void* ptr = vk_malloc(total);
    if (ptr) vk_memset(ptr, 0, total);
    return ptr;
}

static inline void* calloc(size_t count, size_t size) {
    return vk_calloc((vk_usize)count, (vk_usize)size);
}

static inline void* realloc(void* ptr, size_t size) {
    return vk_realloc(ptr, (vk_usize)size);
}


static inline void* memcpy(void* dest, const void* src, size_t n) {
    return vk_memcpy(dest, src, (vk_usize)n);
}

static inline void* memmove(void* dest, const void* src, size_t n) {
    return vk_memmove(dest, src, (vk_usize)n);
}

static inline int memcmp(const void* lhs, const void* rhs, size_t n) {
    return vk_memcmp(lhs, rhs, (vk_usize)n);
}

static inline void* memchr(const void* ptr, int ch, size_t n) {
    return vk_memchr(ptr, ch, (vk_usize)n);
}

static inline void* memset(void* dest, int c, size_t n) {
    return vk_memset(dest, c, (vk_usize)n);
}

/* ============================================================
 * Strings
 * ============================================================ */

static inline vk_usize vk_strlen(const char* s) {
    vk_usize len = 0;
    while (s[len]) ++len;
    return len;
}

static inline vk_usize vk_strnlen(const char* s, vk_usize max_len) {
    vk_usize len = 0;
    while (len < max_len && s[len]) ++len;
    return len;
}

static inline int vk_strcmp(const char* lhs, const char* rhs) {
    while (*lhs && *lhs == *rhs) { ++lhs; ++rhs; }
    return (unsigned char)*lhs - (unsigned char)*rhs;
}

static inline int vk_strncmp(const char* lhs, const char* rhs, vk_usize n) {
    for (vk_usize i = 0; i < n; ++i) {
        if (lhs[i] != rhs[i] || lhs[i] == '\0')
            return (unsigned char)lhs[i] - (unsigned char)rhs[i];
    }
    return 0;
}

static inline char* vk_strcpy(char* dest, const char* src) {
    char* out = dest;
    while ((*dest++ = *src++) != '\0') {}
    return out;
}

static inline char* vk_strncpy(char* dest, const char* src, vk_usize n) {
    vk_usize i = 0;
    for (; i < n && src[i] != '\0'; ++i) dest[i] = src[i];
    for (; i < n; ++i) dest[i] = '\0';
    return dest;
}

static inline char* vk_strcat(char* dest, const char* src) {
    char* out = dest;
    while (*dest) ++dest;
    while ((*dest++ = *src++) != '\0') {}
    return out;
}

static inline char* vk_strncat(char* dest, const char* src, vk_usize n) {
    char* out = dest;
    while (*dest) ++dest;
    vk_usize i = 0;
    while (i < n && src[i]) { dest[i] = src[i]; ++i; }
    dest[i] = '\0';
    return out;
}

static inline char* vk_strchr(const char* s, int ch) {
    char v = (char)ch;
    while (*s) { if (*s == v) return (char*)s; ++s; }
    return v == '\0' ? (char*)s : (char*)0;
}

static inline char* vk_strrchr(const char* s, int ch) {
    char v = (char)ch;
    const char* last = (const char*)0;
    for (;;) {
        if (*s == v) last = s;
        if (*s == '\0') break;
        ++s;
    }
    return (char*)last;
}

static inline char* vk_strstr(const char* haystack, const char* needle) {
    if (*needle == '\0') return (char*)haystack;
    for (const char* h = haystack; *h; ++h) {
        const char* a = h;
        const char* b = needle;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (*b == '\0') return (char*)h;
    }
    return (char*)0;
}

static inline size_t strlen(const char* s) {
    return (size_t)vk_strlen(s);
}

static inline size_t strnlen(const char* s, size_t max_len) {
    return (size_t)vk_strnlen(s, (vk_usize)max_len);
}

static inline int strcmp(const char* lhs, const char* rhs) {
    return vk_strcmp(lhs, rhs);
}

static inline int strncmp(const char* lhs, const char* rhs, size_t n) {
    return vk_strncmp(lhs, rhs, (vk_usize)n);
}

static inline char* strcpy(char* dest, const char* src) {
    return vk_strcpy(dest, src);
}

static inline char* strncpy(char* dest, const char* src, size_t n) {
    return vk_strncpy(dest, src, (vk_usize)n);
}

static inline char* strcat(char* dest, const char* src) {
    return vk_strcat(dest, src);
}

static inline char* strncat(char* dest, const char* src, size_t n) {
    return vk_strncat(dest, src, (vk_usize)n);
}

static inline char* strchr(const char* s, int ch) {
    return vk_strchr(s, ch);
}

static inline char* strrchr(const char* s, int ch) {
    return vk_strrchr(s, ch);
}

static inline char* strstr(const char* haystack, const char* needle) {
    return vk_strstr(haystack, needle);
}

/* ============================================================
 * Console / stdio-style calls
 * ============================================================ */

static inline int putchar(int ch) {
    vk_putc((char)ch);
    return (unsigned char)ch;
}

/* ============================================================
 * Process control
 * ============================================================ */

static inline void exit(int code) {
    vk_exit(code);
}

static inline void abort(void) {
    vk_exit(134);
}

/* ============================================================
 * FILE / stdio compatibility layer
 * ============================================================ */

#ifndef EOF
#define EOF (-1)
#endif

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

typedef enum vk_stdio_file_kind {
    VK_STDIO_STDIN = 0,
    VK_STDIO_STDOUT = 1,
    VK_STDIO_STDERR = 2,
    VK_STDIO_KERNEL = 3,
} vk_stdio_file_kind_t;

typedef struct vk_file {
    vk_stdio_file_kind_t kind;
    vk_file_handle_t     handle;
    int                  eof;
    int                  error;
    int                  owns_handle;
} FILE;

static inline FILE* vk__stdin_stream(void) {
    static FILE s = { VK_STDIO_STDIN, 0, 0, 0, 0 };
    return &s;
}

static inline FILE* vk__stdout_stream(void) {
    static FILE s = { VK_STDIO_STDOUT, 0, 0, 0, 0 };
    return &s;
}

static inline FILE* vk__stderr_stream(void) {
    static FILE s = { VK_STDIO_STDERR, 0, 0, 0, 0 };
    return &s;
}

#define stdin  (vk__stdin_stream())
#define stdout (vk__stdout_stream())
#define stderr (vk__stderr_stream())

static inline int vk__file_putc(FILE* stream, int ch) {
    if (stream == NULL) return EOF;

    unsigned char value = (unsigned char)ch;
    switch (stream->kind) {
        case VK_STDIO_STDOUT:
        case VK_STDIO_STDERR:
            vk_putc((char)value);
            return (int)value;
        case VK_STDIO_KERNEL:
            if (vk_get_api()->vk_file_write_handle(stream->handle, &value, 1) != 1) {
                stream->error = 1;
                return EOF;
            }
            return (int)value;
        default:
            stream->error = 1;
            return EOF;
    }
}

static inline int vk__file_getc(FILE* stream) {
    if (stream == NULL) return EOF;

    switch (stream->kind) {
        case VK_STDIO_STDIN:
            return (unsigned char)vk_getc();
        case VK_STDIO_KERNEL: {
            unsigned char value = 0;
            if (vk_get_api()->vk_file_read_handle(stream->handle, &value, 1) != 1) {
                stream->eof = vk_get_api()->vk_file_eof(stream->handle) ? 1 : 0;
                if (!stream->eof) {
                    stream->error = 1;
                }
                return EOF;
            }
            return (int)value;
        }
        default:
            stream->error = 1;
            return EOF;
    }
}

static inline size_t vk__file_write(FILE* stream, const void* ptr, size_t n) {
    if (stream == NULL || ptr == NULL || n == 0) return 0;

    const unsigned char* bytes = (const unsigned char*)ptr;
    size_t written = 0;

    switch (stream->kind) {
        case VK_STDIO_STDOUT:
        case VK_STDIO_STDERR:
            for (; written < n; ++written) {
                vk_putc((char)bytes[written]);
            }
            return written;
        case VK_STDIO_KERNEL:
            written = (size_t)vk_get_api()->vk_file_write_handle(stream->handle, ptr, (vk_usize)n);
            if (written != n) {
                stream->error = 1;
            }
            return written;
        default:
            stream->error = 1;
            return 0;
    }
}

static inline size_t vk__file_read(FILE* stream, void* ptr, size_t n) {
    if (stream == NULL || ptr == NULL || n == 0) return 0;

    unsigned char* bytes = (unsigned char*)ptr;
    size_t read = 0;

    switch (stream->kind) {
        case VK_STDIO_STDIN:
            for (; read < n; ++read) {
                int ch = vk_getc();
                if (ch == EOF) {
                    break;
                }
                bytes[read] = (unsigned char)ch;
            }
            return read;
        case VK_STDIO_KERNEL:
            read = (size_t)vk_get_api()->vk_file_read_handle(stream->handle, ptr, (vk_usize)n);
            if (read == 0 && n != 0) {
                stream->eof = vk_get_api()->vk_file_eof(stream->handle) ? 1 : 0;
                if (!stream->eof) {
                    stream->error = 1;
                }
            }
            return read;
        default:
            stream->error = 1;
            return 0;
    }
}

static inline FILE* fopen(const char* path, const char* mode) {
    if (path == NULL || mode == NULL) return NULL;

    vk_file_handle_t handle = vk_get_api()->vk_file_open(path, mode);
    if (handle == 0) return NULL;

    FILE* file = (FILE*)malloc(sizeof(FILE));
    if (file == NULL) {
        vk_get_api()->vk_file_close(handle);
        return NULL;
    }

    file->kind = VK_STDIO_KERNEL;
    file->handle = handle;
    file->eof = 0;
    file->error = 0;
    file->owns_handle = 1;
    return file;
}

static inline int fclose(FILE* stream) {
    if (stream == NULL) return EOF;

    if (stream->kind == VK_STDIO_KERNEL && stream->owns_handle) {
        int result = vk_get_api()->vk_file_close(stream->handle);
        free(stream);
        return result;
    }

    if (stream != stdin && stream != stdout && stream != stderr) {
        free(stream);
    }
    return 0;
}

static inline size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (ptr == NULL || stream == NULL || size == 0 || nmemb == 0) return 0;

    size_t total = size * nmemb;
    size_t read = vk__file_read(stream, ptr, total);
    return read / size;
}

static inline size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (ptr == NULL || stream == NULL || size == 0 || nmemb == 0) return 0;

    size_t total = size * nmemb;
    size_t written = vk__file_write(stream, ptr, total);
    return written / size;
}

static inline int fseek(FILE* stream, long offset, int whence) {
    if (stream == NULL) return -1;
    if (stream == stdin || stream == stdout || stream == stderr) return -1;

    int result = vk_get_api()->vk_file_seek(stream->handle, (vk_i64)offset, whence);
    if (result == 0) {
        stream->eof = 0;
    } else {
        stream->error = 1;
    }
    return result;
}

static inline void clearerr(FILE* stream);

static inline long ftell(FILE* stream) {
    if (stream == NULL) return -1L;
    if (stream == stdin || stream == stdout || stream == stderr) return -1L;

    vk_i64 position = vk_get_api()->vk_file_tell(stream->handle);
    return position < 0 ? -1L : (long)position;
}

static inline void rewind(FILE* stream) {
    if (stream != NULL) {
        (void)fseek(stream, 0, SEEK_SET);
        clearerr(stream);
    }
}

static inline int feof(FILE* stream) {
    if (stream == NULL) return 1;
    if (stream == stdin || stream == stdout || stream == stderr) return stream->eof;
    return vk_get_api()->vk_file_eof(stream->handle);
}

static inline int ferror(FILE* stream) {
    if (stream == NULL) return 1;
    if (stream == stdin || stream == stdout || stream == stderr) return stream->error;
    return vk_get_api()->vk_file_error(stream->handle);
}

static inline void clearerr(FILE* stream) {
    if (stream == NULL) return;
    stream->eof = 0;
    stream->error = 0;
}

static inline int fflush(FILE* stream) {
    if (stream == NULL) return 0;
    if (stream == stdin || stream == stdout || stream == stderr) return 0;
    return vk_get_api()->vk_file_flush(stream->handle);
}

static inline int remove(const char* path) {
    return vk_get_api()->vk_file_remove(path);
}

static inline int rename(const char* old_path, const char* new_path) {
    return vk_get_api()->vk_file_rename(old_path, new_path);
}

typedef enum vk_format_length {
    VK_FMT_LEN_DEFAULT = 0,
    VK_FMT_LEN_LONG = 1,
    VK_FMT_LEN_LLONG = 2,
    VK_FMT_LEN_SIZE = 3,
} vk_format_length_t;

typedef struct vk_format_sink {
    FILE*  stream;
    char*  buffer;
    size_t capacity;
    size_t produced;
} vk_format_sink_t;

static inline void vk__sink_emit(vk_format_sink_t* sink, char ch) {
    if (sink->buffer != NULL && sink->capacity > 0 && sink->produced + 1 < sink->capacity) {
        sink->buffer[sink->produced] = ch;
    }
    if (sink->stream != NULL) {
        (void)vk__file_putc(sink->stream, ch);
    }
    ++sink->produced;
}

static inline void vk__sink_emit_cstr(vk_format_sink_t* sink, const char* s) {
    if (s == NULL) s = "(null)";
    while (*s != '\0') {
        vk__sink_emit(sink, *s++);
    }
}

static inline size_t vk__u64_to_text(unsigned long long value, unsigned int base, int uppercase, char* out) {
    char temp[32];
    size_t length = 0;
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    do {
        temp[length++] = digits[value % base];
        value /= base;
    } while (value != 0 && length < sizeof(temp));

    for (size_t i = 0; i < length; ++i) {
        out[i] = temp[length - 1 - i];
    }
    out[length] = '\0';
    return length;
}

static inline void vk__sink_emit_unsigned(vk_format_sink_t* sink, unsigned long long value, unsigned int base, int uppercase) {
    char text[32];
    size_t length = vk__u64_to_text(value, base, uppercase, text);
    for (size_t i = 0; i < length; ++i) {
        vk__sink_emit(sink, text[i]);
    }
}

static inline void vk__sink_emit_signed(vk_format_sink_t* sink, long long value) {
    if (value < 0) {
        vk__sink_emit(sink, '-');
        unsigned long long magnitude = (unsigned long long)(-(value + 1)) + 1ULL;
        vk__sink_emit_unsigned(sink, magnitude, 10, 0);
    } else {
        vk__sink_emit_unsigned(sink, (unsigned long long)value, 10, 0);
    }
}

/* Emit an integer with optional width, precision, zero-pad, and left-align. */
static inline void vk__sink_emit_int_padded(vk_format_sink_t* sink,
        unsigned long long value, unsigned int base, int uppercase,
        int is_neg, int flag_zero, int flag_left, int width, int precision) {
    char digits[32];
    size_t ndigits = vk__u64_to_text(value, base, uppercase, digits);
    /* precision 0 with value 0 produces no digits */
    if (precision == 0 && value == 0) { ndigits = 0; digits[0] = '\0'; }
    /* minimum digits from precision */
    int min_digits = (precision > 0) ? precision : 0;
    int zero_fill = (min_digits > (int)ndigits) ? (min_digits - (int)ndigits) : 0;
    int content = (is_neg ? 1 : 0) + zero_fill + (int)ndigits;
    int pad = (width > content) ? (width - content) : 0;
    /* right-align with spaces */
    if (!flag_left && !(flag_zero && precision < 0))
        for (int i = 0; i < pad; i++) vk__sink_emit(sink, ' ');
    if (is_neg) vk__sink_emit(sink, '-');
    /* right-align with zeros (flag_zero applies only when no precision) */
    if (!flag_left && flag_zero && precision < 0)
        for (int i = 0; i < pad; i++) vk__sink_emit(sink, '0');
    /* precision zero-fill */
    for (int i = 0; i < zero_fill; i++) vk__sink_emit(sink, '0');
    for (size_t i = 0; i < ndigits; i++) vk__sink_emit(sink, digits[i]);
    /* left-align padding */
    if (flag_left)
        for (int i = 0; i < pad; i++) vk__sink_emit(sink, ' ');
}

/* Emit a string with optional width and precision (max chars). */
static inline void vk__sink_emit_str_padded(vk_format_sink_t* sink,
        const char* s, int flag_left, int width, int precision) {
    if (s == NULL) s = "(null)";
    size_t slen = 0;
    while (s[slen] != '\0') slen++;
    size_t outlen = (precision >= 0 && (size_t)precision < slen) ? (size_t)precision : slen;
    int pad = (width > (int)outlen) ? (width - (int)outlen) : 0;
    if (!flag_left) for (int i = 0; i < pad; i++) vk__sink_emit(sink, ' ');
    for (size_t i = 0; i < outlen; i++) vk__sink_emit(sink, s[i]);
    if (flag_left)  for (int i = 0; i < pad; i++) vk__sink_emit(sink, ' ');
}

static inline void vk__format_to_sink(vk_format_sink_t* sink, const char* fmt, va_list args) {
    while (*fmt != '\0') {
        if (*fmt != '%') {
            vk__sink_emit(sink, *fmt++);
            continue;
        }

        ++fmt;
        if (*fmt == '%') {
            vk__sink_emit(sink, '%');
            ++fmt;
            continue;
        }

        /* Parse flags */
        int flag_zero = 0, flag_left = 0;
        for (;;) {
            if      (*fmt == '0') { flag_zero = 1; ++fmt; }
            else if (*fmt == '-') { flag_left = 1; ++fmt; }
            else if (*fmt == ' ' || *fmt == '+' || *fmt == '#') { ++fmt; }
            else break;
        }

        /* Parse width */
        int width = 0;
        while (*fmt >= '1' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            ++fmt;
        }

        /* Parse precision */
        int precision = -1;
        if (*fmt == '.') {
            ++fmt;
            precision = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                precision = precision * 10 + (*fmt - '0');
                ++fmt;
            }
        }

        /* Parse length modifier */
        vk_format_length_t length = VK_FMT_LEN_DEFAULT;
        if (*fmt == 'l') {
            ++fmt;
            if (*fmt == 'l') {
                length = VK_FMT_LEN_LLONG;
                ++fmt;
            } else {
                length = VK_FMT_LEN_LONG;
            }
        } else if (*fmt == 'z') {
            length = VK_FMT_LEN_SIZE;
            ++fmt;
        }

        char spec = *fmt++;
        switch (spec) {
            case 'c': {
                int value = va_arg(args, int);
                int pad = (width > 1) ? (width - 1) : 0;
                if (!flag_left) for (int i = 0; i < pad; i++) vk__sink_emit(sink, ' ');
                vk__sink_emit(sink, (char)value);
                if (flag_left)  for (int i = 0; i < pad; i++) vk__sink_emit(sink, ' ');
                break;
            }
            case 's': {
                const char* value = va_arg(args, const char*);
                vk__sink_emit_str_padded(sink, value, flag_left, width, precision);
                break;
            }
            case 'd':
            case 'i': {
                long long sv;
                if      (length == VK_FMT_LEN_LLONG) sv = va_arg(args, long long);
                else if (length == VK_FMT_LEN_LONG)  sv = va_arg(args, long);
                else if (length == VK_FMT_LEN_SIZE)  sv = (long long)va_arg(args, size_t);
                else                                  sv = va_arg(args, int);
                int is_neg = (sv < 0);
                unsigned long long uv = is_neg ? (unsigned long long)(-(sv + 1)) + 1ULL : (unsigned long long)sv;
                vk__sink_emit_int_padded(sink, uv, 10, 0, is_neg, flag_zero, flag_left, width, precision);
                break;
            }
            case 'u': {
                unsigned long long uv;
                if      (length == VK_FMT_LEN_LLONG) uv = va_arg(args, unsigned long long);
                else if (length == VK_FMT_LEN_LONG)  uv = va_arg(args, unsigned long);
                else if (length == VK_FMT_LEN_SIZE)  uv = (unsigned long long)va_arg(args, size_t);
                else                                  uv = va_arg(args, unsigned int);
                vk__sink_emit_int_padded(sink, uv, 10, 0, 0, flag_zero, flag_left, width, precision);
                break;
            }
            case 'x':
            case 'X': {
                int uppercase = (spec == 'X');
                unsigned long long uv;
                if      (length == VK_FMT_LEN_LLONG) uv = va_arg(args, unsigned long long);
                else if (length == VK_FMT_LEN_LONG)  uv = va_arg(args, unsigned long);
                else if (length == VK_FMT_LEN_SIZE)  uv = (unsigned long long)va_arg(args, size_t);
                else                                  uv = va_arg(args, unsigned int);
                vk__sink_emit_int_padded(sink, uv, 16, uppercase, 0, flag_zero, flag_left, width, precision);
                break;
            }
            case 'p': {
                vk__sink_emit(sink, '0');
                vk__sink_emit(sink, 'x');
                vk__sink_emit_unsigned(sink, (unsigned long long)va_arg(args, void*), 16, 0);
                break;
            }
            default: {
                vk__sink_emit(sink, '%');
                if (length == VK_FMT_LEN_LONG) {
                    vk__sink_emit(sink, 'l');
                } else if (length == VK_FMT_LEN_LLONG) {
                    vk__sink_emit(sink, 'l');
                    vk__sink_emit(sink, 'l');
                } else if (length == VK_FMT_LEN_SIZE) {
                    vk__sink_emit(sink, 'z');
                }
                vk__sink_emit(sink, spec);
                break;
            }
        }
    }

    if (sink->buffer != NULL && sink->capacity > 0) {
        size_t terminator = sink->produced < sink->capacity ? sink->produced : sink->capacity - 1;
        sink->buffer[terminator] = '\0';
    }
}

static inline int vfprintf(FILE* stream, const char* fmt, va_list args) {
    vk_format_sink_t sink = { stream, NULL, 0, 0 };
    vk__format_to_sink(&sink, fmt, args);
    return (int)sink.produced;
}

static inline int fprintf(FILE* stream, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int result = vfprintf(stream, fmt, args);
    va_end(args);
    return result;
}

static inline int vprintf(const char* fmt, va_list args) {
    return vfprintf(stdout, fmt, args);
}

static inline int printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int result = vprintf(fmt, args);
    va_end(args);
    return result;
}

static inline int vsnprintf(char* buffer, size_t capacity, const char* fmt, va_list args) {
    vk_format_sink_t sink = { NULL, buffer, capacity, 0 };
    vk__format_to_sink(&sink, fmt, args);
    return (int)sink.produced;
}

static inline int snprintf(char* buffer, size_t capacity, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int result = vsnprintf(buffer, capacity, fmt, args);
    va_end(args);
    return result;
}

static inline int vsprintf(char* buffer, const char* fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);

    if (buffer != NULL) {
        va_copy(copy, args);
        (void)vsnprintf(buffer, (size_t)length + 1, fmt, copy);
        va_end(copy);
    }

    return length;
}

static inline int sprintf(char* buffer, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int result = vsprintf(buffer, fmt, args);
    va_end(args);
    return result;
}

static inline int fputc(int ch, FILE* stream) {
    return vk__file_putc(stream, ch);
}

static inline int putc(int ch, FILE* stream) {
    return fputc(ch, stream);
}

static inline int fgetc(FILE* stream) {
    return vk__file_getc(stream);
}

static inline int getc(FILE* stream) {
    return fgetc(stream);
}

static inline int getchar(void) {
    return fgetc(stdin);
}

static inline int puts(const char* s) {
    int result = fprintf(stdout, "%s\n", s ? s : "(null)");
    return result;
}

static inline int fputs(const char* s, FILE* stream) {
    return fprintf(stream, "%s", s ? s : "(null)");
}

static inline void perror(const char* s) {
    if (s != NULL) {
        (void)fprintf(stderr, "%s: error\n", s);
    } else {
        (void)fprintf(stderr, "error\n");
    }
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VK_USERSPACE_COMPAT_H */