#ifndef GMSYNTH_H
#define GMSYNTH_H
#include <stdint.h>

/* General MIDI on an OPL3: 18 two-operator voices with the felixterminal
   instrument bank. Fed raw MIDI bytes by the MPU-401 emulation. */

#define GM_BEND_STEPS 64            /* pitch bend resolution per 2 semitones */

extern uint16_t gm_note_fnum[128];
extern uint8_t  gm_note_block[128];
extern uint8_t  gm_atten[128];
extern uint32_t gm_bend_mul[GM_BEND_STEPS * 2 + 1];

void gm_tables_init(int opl_rate_x100);     /* FPU; load time only */
void gm_reset(void);
void gm_midi_byte(uint8_t b);

#endif
