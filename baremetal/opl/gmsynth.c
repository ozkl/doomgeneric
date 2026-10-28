/* General MIDI synthesizer on two private OPL3s (36 two-operator voices).
 * Instrument bank from felixterminal. Integer-only: runs inside the port
 * trap handler, so no FPU and no library calls.
 */
#include <string.h>
#include "opl.h"
#include "gmsynth.h"
#include "gmbank.h"

#define VOICES 36                   /* 18 per chip */

/* Operator register offset of each channel's modulator; carrier is +3. */
static const uint8_t op_slot[9] = { 0x00, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x10, 0x11, 0x12 };

typedef struct {
    uint8_t active, sustained, chan, note, instr, vel;
    uint32_t age;
} Voice;

static Voice voice[VOICES];
static uint32_t clock;

static uint8_t prog[16], volume[16], expr[16], pan[16], sustain[16];
static uint16_t bend[16];

/* MIDI parser */
static uint8_t status, data[2], need, got, in_sysex;

/* ------------------------------------------------------------ registers */

/* Voice v lives on chip v / 18, register bank (v % 18) / 9, channel v % 9.
   The chip number rides in bit 12 of the register so callers stay simple. */
static uint32_t vbank(int v) { return (v >= 18 ? 0x1000 : 0) | ((v % 18) >= 9 ? 0x100 : 0); }
static void w(uint32_t reg, uint8_t val) { midi_opl_write(reg >> 12, reg & 0x1FF, val); }

static const uint8_t *instrument(int chan, int note)
{
    if (chan == 9) {
        int d = note - 35;
        if (d < 0 || d > 52) d = 0;
        return gm_bank[128 + d];
    }
    return gm_bank[prog[chan]];
}

static void key(int v, int on)
{
    Voice *vo = &voice[v];
    const uint8_t *ins = gm_bank[vo->instr];
    int note = vo->chan == 9 ? (ins[11] ? ins[11] : 60) : vo->note;
    uint32_t fnum = gm_note_fnum[note & 127];
    int block = gm_note_block[note & 127];

    if (vo->chan != 9) {
        int b = ((int)bend[vo->chan] - 8192) * GM_BEND_STEPS / 8192;
        if (b < -GM_BEND_STEPS) b = -GM_BEND_STEPS;
        if (b > GM_BEND_STEPS) b = GM_BEND_STEPS;
        fnum = (fnum * gm_bend_mul[b + GM_BEND_STEPS] + 0x8000) >> 16;
        while (fnum > 1023 && block < 7) { fnum = (fnum + 1) >> 1; block++; }
        if (fnum > 1023) fnum = 1023;
    }
    uint32_t c = vbank(v) + (v % 9);
    w(0xA0 + c, fnum & 0xFF);
    w(0xB0 + c, (on ? 0x20 : 0) | (block << 2) | ((fnum >> 8) & 3));
}

static void set_level(int v)
{
    Voice *vo = &voice[v];
    const uint8_t *ins = gm_bank[vo->instr];
    uint32_t base = vbank(v) + op_slot[v % 9];
    int att = gm_atten[vo->vel] + gm_atten[volume[vo->chan]] + gm_atten[expr[vo->chan]];

    int car = (ins[6] & 0x3F) + att;
    if (car > 63) car = 63;
    w(0x43 + base, (ins[6] & 0xC0) | car);

    int mod = ins[1] & 0x3F;
    if (ins[10] & 1) {                          /* additive: modulator is heard too */
        mod += att;
        if (mod > 63) mod = 63;
    }
    w(0x40 + base, (ins[1] & 0xC0) | mod);
}

static void set_pan(int v)
{
    Voice *vo = &voice[v];
    uint8_t p = pan[vo->chan] < 43 ? 0x10 : pan[vo->chan] > 85 ? 0x20 : 0x30;
    w(0xC0 + vbank(v) + (v % 9), (gm_bank[vo->instr][10] & 0x0F) | p);
}

static void load_instrument(int v)
{
    const uint8_t *ins = gm_bank[voice[v].instr];
    uint32_t base = vbank(v) + op_slot[v % 9];
    w(0x20 + base, ins[0]);
    w(0x60 + base, ins[2]);
    w(0x80 + base, ins[3]);
    w(0xE0 + base, ins[4]);
    w(0x23 + base, ins[5]);
    w(0x63 + base, ins[7]);
    w(0x83 + base, ins[8]);
    w(0xE3 + base, ins[9]);
}

/* ------------------------------------------------------------ notes */

static int alloc_voice(int chan, int note)
{
    int best = -1;
    for (int v = 0; v < VOICES; v++)                /* same note re-struck */
        if (voice[v].active && voice[v].chan == chan && voice[v].note == note) return v;
    for (int v = 0; v < VOICES; v++)
        if (!voice[v].active) return v;
    for (int v = 0; v < VOICES; v++)                /* steal: released, then oldest */
        if (voice[v].sustained && (best < 0 || voice[v].age < voice[best].age)) best = v;
    if (best >= 0) return best;
    for (int v = 0; v < VOICES; v++)
        if (best < 0 || voice[v].age < voice[best].age) best = v;
    return best;
}

static void note_off_voice(int v)
{
    if (!voice[v].active) return;
    key(v, 0);
    voice[v].active = 0;
    voice[v].sustained = 0;
}

static void note_on(int chan, int note, int vel)
{
    int v = alloc_voice(chan, note);
    if (voice[v].active) key(v, 0);
    const uint8_t *ins = instrument(chan, note);
    Voice *vo = &voice[v];
    vo->active = 1;
    vo->sustained = 0;
    vo->chan = chan;
    vo->note = note;
    vo->vel = vel;
    vo->instr = (uint8_t)((ins - gm_bank[0]) / 12);
    vo->age = ++clock;
    load_instrument(v);
    set_level(v);
    set_pan(v);
    key(v, 1);
}

static void note_off(int chan, int note)
{
    for (int v = 0; v < VOICES; v++) {
        if (!voice[v].active || voice[v].chan != chan || voice[v].note != note) continue;
        if (sustain[chan]) voice[v].sustained = 1;
        else note_off_voice(v);
    }
}

static void all_notes_off(int chan)
{
    for (int v = 0; v < VOICES; v++)
        if (voice[v].active && voice[v].chan == chan) note_off_voice(v);
}

static void reset_controllers(int c)
{
    volume[c] = 100;
    expr[c] = 127;
    pan[c] = 64;
    sustain[c] = 0;
    bend[c] = 8192;
}

static void controller(int chan, int num, int val)
{
    switch (num) {
    case 7:  volume[chan] = val; goto levels;
    case 11: expr[chan] = val; goto levels;
    case 10:
        pan[chan] = val;
        for (int v = 0; v < VOICES; v++)
            if (voice[v].active && voice[v].chan == chan) set_pan(v);
        return;
    case 64:
        sustain[chan] = val >= 64;
        if (!sustain[chan])
            for (int v = 0; v < VOICES; v++)
                if (voice[v].sustained && voice[v].chan == chan) note_off_voice(v);
        return;
    case 120: case 123:
        all_notes_off(chan);
        return;
    case 121:
        reset_controllers(chan);
        return;
    default:
        return;
    }
levels:
    for (int v = 0; v < VOICES; v++)
        if (voice[v].active && voice[v].chan == chan) set_level(v);
}

static void message(void)
{
    int chan = status & 0x0F;
    switch (status & 0xF0) {
    case 0x80: note_off(chan, data[0]); break;
    case 0x90:
        if (data[1]) note_on(chan, data[0], data[1]);
        else note_off(chan, data[0]);
        break;
    case 0xB0: controller(chan, data[0], data[1]); break;
    case 0xC0: prog[chan] = data[0]; break;
    case 0xE0:
        bend[chan] = data[0] | (data[1] << 7);
        for (int v = 0; v < VOICES; v++)
            if (voice[v].active && voice[v].chan == chan) key(v, 1);
        break;
    default: break;                             /* aftertouch: ignored */
    }
}

/* ------------------------------------------------------------ public */

void gm_reset(void)
{
    for (int v = 0; v < VOICES; v++) note_off_voice(v);
    memset(voice, 0, sizeof voice);
    for (int c = 0; c < 16; c++) { prog[c] = 0; reset_controllers(c); }
    status = need = got = in_sysex = 0;
    for (uint32_t c = 0; c < 0x2000; c += 0x1000) {
        w(c | 0x105, 0x01);                         /* OPL3 mode: stereo, 18 voices */
        w(c | 0x104, 0x00);                         /* no 4-operator pairs */
        w(c | 0x001, 0x20);                         /* waveform select */
        w(c | 0x0BD, 0x00);                         /* melodic mode */
    }
}

void gm_midi_byte(uint8_t b)
{
    if (b >= 0xF8) return;                          /* real-time: no effect on parsing */
    if (b & 0x80) {
        if (b == 0xF0) { in_sysex = 1; return; }
        if (b == 0xF7) { in_sysex = 0; return; }
        in_sysex = 0;
        if (b >= 0xF0) { status = 0; need = 0; return; }   /* system common: skip */
        status = b;
        got = 0;
        need = ((b & 0xF0) == 0xC0 || (b & 0xF0) == 0xD0) ? 1 : 2;
        return;
    }
    if (in_sysex || !status) return;
    data[got++] = b;
    if (got >= need) {
        message();
        got = 0;                                    /* running status */
    }
}
