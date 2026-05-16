// host_io.h — Host I/O functions for WAH WASM runtime
//
// Force-included via `-include host_io.h` in the build script.
// This header includes <stdio.h> first, then redefines all I/O
// functions to go through host imports.
//
// IMPORTANT: This must be the very first include in every translation unit.

#ifndef HOST_IO_H
#define HOST_IO_H

// Pull in standard declarations FIRST, before any macros touch them.
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

// ---- File I/O (WASM imports from "env" module) ----

extern int host_fopen(const char *path, const char *mode);
extern int host_fclose(int fd);
extern int host_fread(int fd, void *buf, int size);
extern int host_fwrite(int fd, const void *buf, int size);
extern int host_fseek(int fd, int offset, int whence);
extern int host_ftell(int fd);

// ---- Console output (WASM import) ----

extern void host_puts(const char *str, int len);

// ---- Process control (WASM import) ----

extern __attribute__((noreturn)) void host_exit(int code);

// ---- Console wrappers (implemented in doomgeneric_wah.c) ----

int host_printf(const char *fmt, ...);
int host_fprintf(FILE *stream, const char *fmt, ...);
int host_vfprintf(FILE *stream, const char *fmt, va_list args);

// ============================================================================
// FILE* ↔ fd conversion using pointer arithmetic (no casts)
// ============================================================================

// FILE is an opaque/incomplete type — we can't make an array of it.
// Instead, use a char array as address space: each slot gives a unique
// pointer that we hand out as a fake FILE*.

#define HOST_MAX_FDS 32
static char g_fake_file_slots[HOST_MAX_FDS];

static inline FILE *fd_to_file(int fd) {
    return (fd >= 0) ? (FILE *)&g_fake_file_slots[fd] : NULL;
}

static inline int file_to_fd(FILE *f) {
    return (int)((char *)f - g_fake_file_slots);
}

// ============================================================================
// fopen/fclose/fread/fwrite/fseek/ftell wrappers
// ============================================================================

static inline FILE *host_fopen_wrap(const char *path, const char *mode) {
    int fd = host_fopen(path, mode);
    return fd_to_file(fd);
}

static inline int host_fclose_wrap(FILE *f) {
    return host_fclose(file_to_fd(f));
}

static inline size_t host_fread_wrap(void *buf, size_t size, size_t count, FILE *f) {
    return (size_t)host_fread(file_to_fd(f), buf, (int)(size * count));
}

static inline size_t host_fwrite_wrap(const void *buf, size_t size, size_t count, FILE *f) {
    return (size_t)host_fwrite(file_to_fd(f), buf, (int)(size * count));
}

static inline int host_fseek_wrap(FILE *f, long offset, int whence) {
    return host_fseek(file_to_fd(f), (int)offset, whence);
}

static inline long host_ftell_wrap(FILE *f) {
    return (long)host_ftell(file_to_fd(f));
}

static inline int host_feof_wrap(FILE *f) {
    (void)f;
    return 0;  // stub — not used in active doom code paths
}

// ============================================================================
// Redirect standard names
// ============================================================================

#undef printf
#undef fprintf
#undef vfprintf
#undef puts
#undef putchar
#undef fflush
#undef exit
#undef fopen
#undef fclose
#undef fread
#undef fwrite
#undef fseek
#undef ftell
#undef feof

#define printf    host_printf
#define fprintf   host_fprintf
#define vfprintf  host_vfprintf
#define puts(s)   (host_puts((s), strlen((s))), host_puts("\n", 1))
#define putchar(c) do { char _c = (c); host_puts(&_c, 1); } while(0)
#define fflush(f) ((void)0)
#define exit      host_exit

#define fopen     host_fopen_wrap
#define fclose    host_fclose_wrap
#define fread     host_fread_wrap
#define fwrite    host_fwrite_wrap
#define fseek     host_fseek_wrap
#define ftell     host_ftell_wrap
#define feof      host_feof_wrap

#endif // HOST_IO_H
