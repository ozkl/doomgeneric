/* Doom's music: MIDI (Doom's MUS lumps converted by mus2mid) played by
   SBPRO's General MIDI synth on two emulated OPL3s, rendered in the timer
   interrupt alongside the sound effects (mixer.cpp). */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void  music_init(void);                                  /* boot: tables, chips */
void* music_load(const uint8_t* midi, uint32_t len);     /* a parsed song, or 0 */
void  music_free(void* song);
void  music_play(void* song, int loop);
void  music_stop(void);
void  music_pause(int paused);
void  music_volume(int vol);                             /* 0..127 */
int   music_playing(void);
void  music_render(int32_t* mono, int frames);           /* irq: adds to the mix at 48 kHz */
#ifdef __cplusplus
}
#endif
