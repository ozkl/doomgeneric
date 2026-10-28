#ifndef OPL_H
#define OPL_H
#include <stdint.h>

/* The two OPL3s (DOSBox's dbopl) behind the General MIDI synth, from
   SBPRO (github.com/jasonbrianhall/sbpro2hda); gmsynth.c drives them. */

#define OPL_MAX_FRAMES 1024

#ifdef __cplusplus
extern "C" {
#endif
void opl_init(int rate);                                   /* builds tables (FPU) */
void midi_opl_write(int chip, uint32_t reg, uint8_t val);  /* chip 0/1, reg 0x000-0x1FF */
void opl_mix(int32_t *out, int frames);                    /* adds interleaved stereo */
#ifdef __cplusplus
}
#endif

#endif
