/*
 * vkernel userspace - newlib system call stubs
 * Copyright (C) 2026 vkernel authors
 *
 * syscalls.c - Implements the POSIX-like system calls that newlib's
 *              libc.a expects.  Each stub translates to the vkernel
 *              kernel API via vk_get_api().
 *
 * Compile with newlib headers (-isystem sysroot/include).
 */

/*
 * For the x86_64-elf target newlib maps the underscore syscall names
 * (e.g. _close) to their non-underscore equivalents (close) via the
 * MISSING_SYSCALL_NAMES macro in <_syslist.h>.  Define it here so that
 * our own function definitions are renamed in the same way and the
 * linker finds them.
 */
#define MISSING_SYSCALL_NAMES
#include <_syslist.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <reent.h>

#include "../../include/vkernel/vk.h"

/* ============================================================
 * File-descriptor table
 *
 * newlib's stdio operates on integer file descriptors.  The vkernel
 * kernel uses opaque vk_file_handle_t (u64) handles.  We maintain a
 * small mapping table.  Descriptors 0-2 are reserved for stdin,
 * stdout, stderr and are handled specially (console I/O).
 * ============================================================ */

#define VK_MAX_FDS   64
#define VK_FD_STDIN   0
#define VK_FD_STDOUT  1
#define VK_FD_STDERR  2

typedef struct {
    vk_file_handle_t handle;
    int              in_use;
} vk_fd_entry_t;

static vk_fd_entry_t _fd_table[VK_MAX_FDS] = {
    [VK_FD_STDIN]  = { 0, 1 },
    [VK_FD_STDOUT] = { 0, 1 },
    [VK_FD_STDERR] = { 0, 1 },
};

static int _fd_alloc(vk_file_handle_t h)
{
    for (int i = 3; i < VK_MAX_FDS; ++i) {
        if (!_fd_table[i].in_use) {
            _fd_table[i].handle = h;
            _fd_table[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

static void _fd_free(int fd)
{
    if (fd >= 3 && fd < VK_MAX_FDS) {
        _fd_table[fd].in_use = 0;
        _fd_table[fd].handle = 0;
    }
}

static int _fd_valid(int fd)
{
    return fd >= 0 && fd < VK_MAX_FDS && _fd_table[fd].in_use;
}

/* ============================================================
 * Heap — _sbrk
 *
 * We request one large arena from the kernel's malloc, and hand
 * it out linearly.  If the arena is exhausted we try to allocate
 * a second (larger) chunk.
 * ============================================================ */

#define VK_HEAP_INITIAL_SIZE  (4 * 1024 * 1024)   /* 4 MiB */
#define VK_HEAP_GROW_SIZE     (4 * 1024 * 1024)   /* 4 MiB increments */

static char* _heap_start = 0;
static char* _heap_ptr   = 0;
static char* _heap_end   = 0;

void* _sbrk(ptrdiff_t incr)
{
    /* First call — allocate the initial arena. */
    if (_heap_start == 0) {
        _heap_start = (char*)VK_CALL(malloc, VK_HEAP_INITIAL_SIZE);
        if (!_heap_start) {
            errno = ENOMEM;
            return (void*)-1;
        }
        _heap_ptr = _heap_start;
        _heap_end = _heap_start + VK_HEAP_INITIAL_SIZE;
    }

    /* Can we satisfy from the current arena? */
    if (_heap_ptr + incr <= _heap_end) {
        char* prev = _heap_ptr;
        _heap_ptr += incr;
        return prev;
    }

    /* Try to grow.  Allocate a new, larger chunk and hope it is
       contiguous (the kernel heap usually is for small requests). */
    vk_usize grow = (vk_usize)incr > VK_HEAP_GROW_SIZE
                        ? (vk_usize)incr
                        : VK_HEAP_GROW_SIZE;
    char* extra = (char*)VK_CALL(malloc, grow);
    if (!extra) {
        errno = ENOMEM;
        return (void*)-1;
    }

    /* If the new block happens to be contiguous, extend the arena. */
    if (extra == _heap_end) {
        _heap_end += grow;
        char* prev = _heap_ptr;
        _heap_ptr += incr;
        return prev;
    }

    /* Non-contiguous — switch to the new block (wastes the tail of
       the old one, but keeps things simple). */
    _heap_start = extra;
    _heap_ptr   = extra + incr;
    _heap_end   = extra + grow;
    return extra;
}

/* ============================================================
 * Process control
 * ============================================================ */

void _exit(int code)
{
    VK_CALL(exit, code);
    __builtin_unreachable();
}

int _kill(int pid, int sig)
{
    (void)pid;
    (void)sig;
    errno = ESRCH;
    return -1;
}

int _getpid(void)
{
    return 1;
}

/* ============================================================
 * File I/O
 * ============================================================ */

int _open(const char* path, int flags, int mode)
{
    (void)flags;
    (void)mode;

    if (!path) {
        errno = EFAULT;
        return -1;
    }

    /*
     * Translate POSIX flags to a simple mode string.
     * newlib's _open uses O_RDONLY=0, O_WRONLY=1, O_RDWR=2, O_CREAT etc.
     * We map conservatively; the kernel side is simple enough.
     */
    const char* vk_mode = "r";

    /* O_WRONLY or O_RDWR */
    int accmode = flags & 3;  /* O_ACCMODE */
    if (accmode == 1)      vk_mode = "w";
    else if (accmode == 2) vk_mode = "r+";

    /* O_APPEND (0x0400 on Linux, 0x0008 on newlib) — try both bits */
    if (flags & 0x0008 || flags & 0x0400) vk_mode = "a";

    vk_file_handle_t h = VK_CALL(file_open, path, vk_mode);
    if (h == 0) {
        errno = ENOENT;
        return -1;
    }

    int fd = _fd_alloc(h);
    if (fd < 0) {
        VK_CALL(file_close, h);
        errno = EMFILE;
        return -1;
    }

    return fd;
}

int _close(int fd)
{
    if (fd <= VK_FD_STDERR) return 0;   /* never close std streams */

    if (!_fd_valid(fd)) {
        errno = EBADF;
        return -1;
    }

    int rc = VK_CALL(file_close, _fd_table[fd].handle);
    _fd_free(fd);
    return rc;
}

int _read(int fd, char* buf, int len)
{
    if (len <= 0) return 0;

    /* stdin — character-at-a-time from console */
    if (fd == VK_FD_STDIN) {
        for (int i = 0; i < len; ++i) {
            char c = VK_CALL(getc);
            buf[i] = c;
            if (c == '\n' || c == '\r')
                return i + 1;
        }
        return len;
    }

    if (!_fd_valid(fd)) {
        errno = EBADF;
        return -1;
    }

    vk_usize n = VK_CALL(file_read_handle, _fd_table[fd].handle, buf, (vk_usize)len);
    return (int)n;
}

int _write(int fd, const char* buf, int len)
{
    if (len <= 0) return 0;

    /* stdout / stderr → console */
    if (fd == VK_FD_STDOUT || fd == VK_FD_STDERR) {
        for (int i = 0; i < len; ++i)
            VK_CALL(putc, buf[i]);
        return len;
    }

    if (!_fd_valid(fd)) {
        errno = EBADF;
        return -1;
    }

    vk_usize n = VK_CALL(file_write_handle, _fd_table[fd].handle, buf, (vk_usize)len);
    return (int)n;
}

int _lseek(int fd, int offset, int whence)
{
    if (fd <= VK_FD_STDERR) {
        errno = ESPIPE;
        return -1;
    }

    if (!_fd_valid(fd)) {
        errno = EBADF;
        return -1;
    }

    int rc = VK_CALL(file_seek, _fd_table[fd].handle, (vk_i64)offset, whence);
    if (rc != 0) {
        errno = EINVAL;
        return -1;
    }

    vk_i64 pos = VK_CALL(file_tell, _fd_table[fd].handle);
    return (int)pos;
}

int _fstat(int fd, struct stat* st)
{
    if (!st) {
        errno = EFAULT;
        return -1;
    }

    VK_CALL(memset, st, 0, sizeof(*st));

    if (fd <= VK_FD_STDERR) {
        st->st_mode = S_IFCHR;
        return 0;
    }

    if (!_fd_valid(fd)) {
        errno = EBADF;
        return -1;
    }

    /* We don't have real stat info — report a regular file. */
    st->st_mode = S_IFREG;
    return 0;
}

int _stat(const char* path, struct stat* st)
{
    if (!path || !st) {
        errno = EFAULT;
        return -1;
    }

    VK_CALL(memset, st, 0, sizeof(*st));

    if (VK_CALL(file_exists, path)) {
        st->st_mode = S_IFREG;
        st->st_size = (off_t)VK_CALL(file_size, path);
        return 0;
    }

    errno = ENOENT;
    return -1;
}

int _link(const char* old, const char* new_path)
{
    (void)old;
    (void)new_path;
    errno = EMLINK;
    return -1;
}

int _unlink(const char* path)
{
    if (!path) {
        errno = EFAULT;
        return -1;
    }

    return VK_CALL(file_remove, path);
}

int _isatty(int fd)
{
    return (fd <= VK_FD_STDERR) ? 1 : 0;
}

/* ============================================================
 * Time (stub — no RTC yet)
 * ============================================================ */

#include <sys/time.h>
#include <sys/times.h>

int _gettimeofday(struct timeval* tv, void* tz)
{
    (void)tz;
    if (tv) {
        vk_u64 ticks = VK_CALL(tick_count);
        vk_u32 tps   = VK_CALL(ticks_per_sec);
        tv->tv_sec  = (time_t)(ticks / tps);
        tv->tv_usec = (suseconds_t)((ticks % tps) * 1000000 / tps);
    }
    return 0;
}

clock_t _times(struct tms* buf)
{
    vk_u64 ticks = VK_CALL(tick_count);
    if (buf) {
        buf->tms_utime  = (clock_t)ticks;
        buf->tms_stime  = 0;
        buf->tms_cutime = 0;
        buf->tms_cstime = 0;
    }
    return (clock_t)ticks;
}

/* ============================================================
 * Misc stubs
 * ============================================================ */

int _fork(void)
{
    errno = ENOSYS;
    return -1;
}

int _wait(int* status)
{
    (void)status;
    errno = ECHILD;
    return -1;
}

int _execve(const char* name, char* const argv[], char* const env[])
{
    (void)name;
    (void)argv;
    (void)env;
    errno = ENOSYS;
    return -1;
}
