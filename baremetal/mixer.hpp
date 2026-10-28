#pragma once
// Doom's sound effects (and music, music.cpp), mixed in the timer interrupt
// (240 Hz) and handed to the sound card (audio.cpp). Samples are 8-bit unsigned PCM at any rate
// (the WAD's DMX lumps: 11025 Hz mostly), played mono. Integer only: it
// runs inside an interrupt that saves only the general-purpose registers.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
#define MIXER_CHANNELS 16
void mixer_init(int enabled);          // after audio_init(); 0: no card, stay silent
int mixer_enabled(void);
// Start (or restart) a channel. vol 0..127, sep 0 (left) .. 254 (right).
void mixer_start(int ch, const uint8_t* samples, uint32_t len, uint32_t rate, int vol, int sep);
void mixer_set(int ch, int vol, int sep);
void mixer_stop(int ch);
int mixer_playing(int ch);
void mixer_tick(void);                 // irq.cpp: every timer interrupt
#ifdef __cplusplus
}
#endif
