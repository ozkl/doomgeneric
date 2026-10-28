// doomgeneric's platform functions for bare metal: the kernel (kernel.cpp)
// draws the frame, reads the keyboard and mouse and keeps the time.
#include "doomgeneric.h"
#include "doomkeys.h"
#include "d_event.h"
#include "i_video.h"
#include "bm.h"

#ifndef CMAP256
#error "build with -DCMAP256 (8-bit frames)"
#endif

void DG_Init(void) {}

void DG_DrawFrame(void)
{
    // colors[] is 0xAARRGGBB in memory (struct color: b, g, r, a).
    if (palette_changed) {
        bm_set_palette((const uint32_t*)colors);
        palette_changed = false;
    }
    bm_draw((const uint8_t*)DG_ScreenBuffer, SCREENWIDTH, SCREENHEIGHT);
}

void DG_SleepMs(uint32_t ms) { bm_sleep_ms(ms); }

uint32_t DG_GetTicksMs(void) { return bm_ms(); }

// Keys from the queue; when it's empty, the mouse's motion since last time
// goes to Doom as one event.
int DG_GetKey(int* pressed, unsigned char* key)
{
    if (bm_key(pressed, key)) return 1;
    int buttons, dx, dy;
    if (bm_mouse(&buttons, &dx, &dy)) {
        event_t ev;
        ev.type = ev_mouse;
        ev.data1 = buttons;          // 1 left (fire), 2 right (strafe), 4 middle (forward)
        ev.data2 = dx * 2;
        ev.data3 = -dy * 2;          // away from you: forward
        ev.data4 = 0;
        D_PostEvent(&ev);
    }
    return 0;
}

void DG_SetWindowTitle(const char* title) { (void)title; }
