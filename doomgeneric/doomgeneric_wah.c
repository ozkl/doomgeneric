// doomgeneric WAH (WebAssembly) platform layer
// Compiled to WASM, runs inside the WAH interpreter.
// All platform + I/O goes through host_* imports in the "env" module.

// Include headers BEFORE host_io.h to avoid macro conflicts.
// host_io.h redefines printf/fprintf/exit, so we must define
// our wrapper implementations before including it.

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "doomkeys.h"
#include "doomgeneric.h"

// ---- Host imports (declared directly, NOT via host_io.h) ----

extern void host_puts(const char *str, int len);
extern __attribute__((noreturn)) void host_exit(int code);

extern void host_init(void);
extern void host_draw_frame(uint32_t buffer_ptr, uint32_t width, uint32_t height);
extern void host_sleep_ms(uint32_t ms);
extern uint32_t host_get_ticks_ms(void);
extern int32_t host_get_key(void);
extern void host_set_window_title(uint32_t title_ptr, uint32_t len);


// ---- Console wrappers (called by doom code via macros in host_io.h) ----

int host_printf(const char *fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0)
        host_puts(buf, len);
    return len;
}

int host_fprintf(FILE *stream, const char *fmt, ...)
{
    (void)stream;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0)
        host_puts(buf, len);
    return len;
}

int host_vfprintf(FILE *stream, const char *fmt, va_list args)
{
    (void)stream;
    char buf[1024];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len > 0)
        host_puts(buf, len);
    return len;
}


// ---- DG_* platform interface ----

void DG_Init()
{
    host_init();
}

void DG_DrawFrame()
{
    host_draw_frame((uint32_t)(uintptr_t)DG_ScreenBuffer,
                    DOOMGENERIC_RESX, DOOMGENERIC_RESY);
}

void DG_SleepMs(uint32_t ms)
{
    host_sleep_ms(ms);
}

uint32_t DG_GetTicksMs()
{
    return host_get_ticks_ms();
}

int DG_GetKey(int* pressed, unsigned char* doomKey)
{
    int32_t result = host_get_key();
    if (result == 0)
        return 0;

    *pressed = (result >> 8) & 0xFF;
    *doomKey = result & 0xFF;
    return 1;
}

void DG_SetWindowTitle(const char* title)
{
    host_set_window_title((uint32_t)(uintptr_t)title, strlen(title));
}


// ---- Exported functions for host to call ----

__attribute__((export_name("doom_init")))
void doom_init(void)
{
    char *argv[] = {"doom", "-iwad", "doom1.wad"};
    doomgeneric_Create(3, argv);
}

__attribute__((export_name("doom_tick")))
void doom_tick(void)
{
    doomgeneric_Tick();
}
