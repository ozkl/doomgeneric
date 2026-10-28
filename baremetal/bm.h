/* The kernel's side of the Doom port (kernel.cpp), for
   doomgeneric_baremetal.c and i_sound_baremetal.c. */
#pragma once
#include <stdint.h>

/* Doom's frame: 8-bit palette indexes, w x h. 320x200 by default; up to
   1278x600 (i_video.h: -hires, -widescreen). Its pixels are 1.2 times
   taller than wide, as on the monitors Doom was drawn for. */

#ifdef __cplusplus
extern "C" {
#endif
void bm_set_palette(const uint32_t* rgb);   /* 256 entries, 0x00RRGGBB */
void bm_draw(const uint8_t* pixels, int w, int h);
void bm_poll(void);
int bm_key(int* pressed, unsigned char* key);   /* doomkeys.h codes; 0: none */
int bm_mouse(int* buttons, int* dx, int* dy);   /* dy: down; 0: no change */
uint32_t bm_ms(void);
void bm_sleep_ms(uint32_t ms);
#ifdef BM_HOSTED
/* In a window (Linux): fullscreen or not, for the Options menu. */
int bm_fullscreen(void);
void bm_set_fullscreen(int on);
#endif
#ifdef __cplusplus
}
#endif
