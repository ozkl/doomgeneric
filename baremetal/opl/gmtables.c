/* Frequency tables for the General MIDI synth. Built once at load time with
   the FPU (called from opl_init, inside its FPU save/restore). */
#include <stdint.h>
#include "gmsynth.h"

double pow(double, double);
double log10(double);

uint16_t gm_note_fnum[128];
uint8_t  gm_atten[128];                         /* MIDI 0-127 -> OPL level steps */
uint8_t  gm_note_block[128];
uint32_t gm_bend_mul[GM_BEND_STEPS * 2 + 1];    /* 16.16 frequency multipliers */

void gm_tables_init(int opl_rate_x100)
{
    double rate = opl_rate_x100 / 100.0;
    for (int n = 0; n < 128; n++) {
        double freq = 440.0 * pow(2.0, (n - 69) / 12.0);
        int block = 0;
        double f = freq * (double)(1 << 20) / rate;
        while (f > 1023.0 && block < 7) { f /= 2.0; block++; }
        if (f > 1023.0) f = 1023.0;
        gm_note_fnum[n] = (uint16_t)(f + 0.5);
        gm_note_block[n] = (uint8_t)block;
    }
    /* 40 dB of range over MIDI's 0-127 (velocity/volume/expression curve),
       in the OPL's 0.75 dB total-level steps. */
    gm_atten[0] = 63;
    for (int v = 1; v < 128; v++) {
        double db = -40.0 * log10(v / 127.0);
        int steps = (int)(db / 0.75 + 0.5);
        gm_atten[v] = (uint8_t)(steps > 63 ? 63 : steps);
    }
    for (int i = -GM_BEND_STEPS; i <= GM_BEND_STEPS; i++) {
        double semis = 2.0 * i / GM_BEND_STEPS;          /* +-2 semitones */
        gm_bend_mul[i + GM_BEND_STEPS] = (uint32_t)(pow(2.0, semis / 12.0) * 65536.0 + 0.5);
    }
}
