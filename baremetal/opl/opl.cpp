// The General MIDI synth's two OPL3 chips (dbopl). Adapted from SBPRO's
// opl.cpp, minus the game's own FM chip, which Doom doesn't have.
#include <stdint.h>
#include <stddef.h>
#include "dbopl.h"
#include "opl.h"

inline void* operator new(size_t, void* p) noexcept { return p; }

alignas(8) static unsigned char handler_mem[2][sizeof(DBOPL::Handler)];
static DBOPL::Handler* chips[2];
static Bit32s buf[2 * OPL_MAX_FRAMES];

extern "C" void gm_tables_init(int opl_rate_x100);

extern "C" void opl_init(int rate) {
    for (int i = 0; i < 2; i++) {
        chips[i] = new (handler_mem[i]) DBOPL::Handler();
        chips[i]->Init((Bitu)rate);
    }
    gm_tables_init(4971591);                /* 14318180 / 288 Hz, x100 */
}

extern "C" void midi_opl_write(int chip, uint32_t reg, uint8_t val) {
    if (chip >= 0 && chip < 2 && chips[chip]) chips[chip]->WriteReg(reg, val);
}

extern "C" void opl_mix(int32_t* out, int frames) {
    if (frames > OPL_MAX_FRAMES) frames = OPL_MAX_FRAMES;
    for (int c = 0; c < 2; c++) {
        DBOPL::Handler* h = chips[c];
        if (!h) continue;
        h->Generate(buf, (Bitu)frames);
        if (h->chip.opl3Active) for (int i = 0; i < frames * 2; i++) out[i] += buf[i];
        else for (int i = 0; i < frames; i++) { out[i * 2] += buf[i]; out[i * 2 + 1] += buf[i]; }
    }
}
