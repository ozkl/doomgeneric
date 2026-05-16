// doomgeneric WAH (WebAssembly) platform layer
// This file is compiled to WASM and runs inside the WAH interpreter.
// All platform functions call extern host functions imported from the host.

#include "doomkeys.h"
#include "doomgeneric.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

// ---- Host functions (WASM imports from "env" module) ----

// Called once during DG_Init
extern void host_init(void);

// Called every frame: host reads WASM memory at buffer_ptr to blit pixels
extern void host_draw_frame(uint32_t buffer_ptr, uint32_t width, uint32_t height);

// Sleep for ms milliseconds
extern void host_sleep_ms(uint32_t ms);

// Returns current time in milliseconds
extern uint32_t host_get_ticks_ms(void);

// Returns 0 if no key, or (pressed << 8 | doomkey)
extern int32_t host_get_key(void);

// Set window title: host reads string from WASM memory at title_ptr
extern void host_set_window_title(uint32_t title_ptr, uint32_t len);

// Host file I/O: opens a real file on the host side
extern int host_open(uint32_t path_ptr, int flags, int mode);

// Override emcc/musl's __syscall_openat so fopen() works via the host.
// Our .o definition takes precedence over musl's libc.a version.
// Returns fd on success, negative errno on failure.
long __syscall_openat(int dirfd, long path, int flags, int mode) {
    (void)dirfd;
    return host_open((uint32_t)path, flags, mode);
}


// ---- DG_* platform interface implementation ----

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
