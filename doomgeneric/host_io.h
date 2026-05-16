// host_io.h — Host I/O functions for WAH WASM runtime
//
// Force-included via `-include host_io.h` in the build script.
// This header includes standard headers first, then redefines all I/O
// functions to go through host imports.

#ifndef HOST_IO_H
#define HOST_IO_H

// Pull in standard declarations FIRST, before any macros touch them.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

// ---- Host imports: File I/O ----
// Signatures match standard C, but FILE* is really just fd (i32).

extern FILE  *host_fopen(const char *path, const char *mode);
extern int    host_fclose(FILE *f);
extern size_t host_fread(void *buf, size_t size, size_t count, FILE *f);
extern size_t host_fwrite(const void *buf, size_t size, size_t count, FILE *f);
extern int    host_fseek(FILE *f, long offset, int whence);
extern long   host_ftell(FILE *f);
extern int    host_feof_stub(FILE *f);
extern int    host_remove(const char *path);
extern int    host_rename(const char *oldpath, const char *newpath);

// ---- Host imports: Console output ----

extern void host_puts(const char *str, int len);

// ---- Host imports: Process control ----

extern __attribute__((noreturn)) void host_exit(int code);

// ---- Console wrappers (implemented in doomgeneric_wah.c) ----

int host_printf(const char *fmt, ...);
int host_fprintf(FILE *stream, const char *fmt, ...);
int host_vfprintf(FILE *stream, const char *fmt, va_list args);

// ============================================================================
// Redirect standard names
// ============================================================================

#undef printf
#define printf    host_printf
#undef fprintf
#define fprintf   host_fprintf
#undef vfprintf
#define vfprintf  host_vfprintf
#undef puts
#define puts(s)   (host_puts((s), strlen((s))), host_puts("\n", 1))
#undef putchar
#define putchar(c) do { char _c = (c); host_puts(&_c, 1); } while(0)
#undef fflush
#define fflush(f) ((void)0)
#undef exit
#define exit      host_exit

#undef stdout
#define stdout    ((FILE*)1)
#undef stderr
#define stderr    ((FILE*)2)

#undef fopen
#define fopen     host_fopen
#undef fclose
#define fclose    host_fclose
#undef fread
#define fread     host_fread
#undef fwrite
#define fwrite    host_fwrite
#undef fseek
#define fseek     host_fseek
#undef ftell
#define ftell     host_ftell
#undef feof
#define feof      host_feof_stub
#undef remove
#define remove    host_remove
#undef rename
#define rename    host_rename
#undef system
#define system(s) (-1)

#endif // HOST_IO_H
