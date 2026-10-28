// Doom's music. See music.h. A Standard MIDI File is parsed once, on the
// main thread, into one time-ordered list of events stamped in 48 kHz
// output frames; the timer interrupt then walks that list, feeding each
// event to SBPRO's General MIDI synth (gmsynth.c) and rendering the two
// OPL3s between events, so timing is exact to the sample.
//
// The interrupt-time part (music_render) is integer-only; the timer's
// entry stub saves the FPU/SSE state anyway, since dbopl is ordinary code.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "music.h"
#include "opl/opl.h"

extern "C" {
void gm_reset(void);
void gm_midi_byte(uint8_t b);
}

#define OUT_RATE 48000
#ifdef __x86_64__
#define DIV 1                       // OPL3s at 48 kHz
#else
#define DIV 2                       // 24 kHz on a 386/486: half the work, each sample sent twice
#endif

struct Ev { uint32_t frame; uint8_t len, b[3]; };
struct Song { Ev* ev; uint32_t n, end; };

static Song* volatile cur;
static uint32_t idx, pos;           // next event, output frames since the start
static volatile int playing, looping, paused;
static volatile int gain = 100;     // Doom's music volume, 0..127
static bool ready;

#ifdef BM_HOSTED
// A hosted build (Linux): no interrupt; the mix is made on the main thread,
// so there is nothing to keep out (and cli would fault in user mode).
static inline uintptr_t irq_off() { return 0; }
static inline void irq_restore(uintptr_t) {}
#else
static inline uintptr_t irq_off() {
    uintptr_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uintptr_t f) { __asm__ volatile("push %0; popf" :: "r"(f) : "memory", "cc"); }
#endif

extern "C" void music_init(void) {
    opl_init(OUT_RATE / DIV);
    gm_reset();
    ready = true;
}

// ---------------------------------------------------------------- parsing
struct Raw { uint32_t tick, seq, tempo; uint8_t len, b[3]; };   // tempo != 0: a tempo change

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

static int raw_cmp(const void* a, const void* b) {
    const Raw* x = (const Raw*)a; const Raw* y = (const Raw*)b;
    if (x->tick != y->tick) return x->tick < y->tick ? -1 : 1;
    // Tempo changes first at the same tick, then file order.
    if ((x->tempo != 0) != (y->tempo != 0)) return x->tempo ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

extern "C" void* music_load(const uint8_t* d, uint32_t len) {
    if (!ready || len < 14 || memcmp(d, "MThd", 4) || be32(d + 4) < 6) return nullptr;
    uint32_t hlen = be32(d + 4);
    uint16_t ntrk = be16(d + 10), division = be16(d + 12);
    // Ticks to microseconds: PPQN with a tempo, or SMPTE frames x ticks per second.
    bool smpte = division & 0x8000;
    uint32_t tps = smpte ? (uint32_t)(-(int8_t)(division >> 8)) * (division & 0xFF) : 0;
    if (smpte ? !tps : !division) return nullptr;

    size_t cap = 1024, n = 0;
    Raw* raw = (Raw*)malloc(cap * sizeof(Raw));
    if (!raw) return nullptr;
    uint32_t end_tick = 0, seq = 0;
    const uint8_t* p = d + 8 + hlen;
    const uint8_t* e = d + len;
    for (int t = 0; t < ntrk && p + 8 <= e; t++) {
        uint32_t tl = be32(p + 4);
        bool is_track = !memcmp(p, "MTrk", 4);
        const uint8_t* q = p + 8;
        const uint8_t* qe = q + tl > e ? e : q + tl;
        p = q + tl;
        if (!is_track) { t--; continue; }               // unknown chunk: skip, doesn't count
        uint32_t tick = 0;
        uint8_t status = 0;
        while (q < qe) {
            uint32_t delta = 0;
            for (int k = 0; k < 4 && q < qe; k++) { uint8_t c = *q++; delta = delta << 7 | (c & 0x7F); if (!(c & 0x80)) break; }
            tick += delta;
            if (q >= qe) break;
            uint8_t c = *q;
            Raw r{};
            r.tick = tick;
            if (c == 0xFF) {                                 // meta
                if (q + 2 > qe) break;
                uint8_t type = q[1];
                q += 2;
                uint32_t ml = 0;
                for (int k = 0; k < 4 && q < qe; k++) { uint8_t x = *q++; ml = ml << 7 | (x & 0x7F); if (!(x & 0x80)) break; }
                if (type == 0x51 && ml >= 3 && q + 3 <= qe) r.tempo = (uint32_t)q[0] << 16 | q[1] << 8 | q[2];
                q += ml;
                if (type == 0x2F) break;                     // end of track
                if (!r.tempo) continue;
            } else if (c == 0xF0 || c == 0xF7) {             // sysex: skip
                q++;
                uint32_t sl = 0;
                for (int k = 0; k < 4 && q < qe; k++) { uint8_t x = *q++; sl = sl << 7 | (x & 0x7F); if (!(x & 0x80)) break; }
                q += sl;
                continue;
            } else {
                if (c & 0x80) { status = c; q++; }
                if (!status) break;                          // data without a status: corrupt
                int need = ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) ? 1 : 2;
                if (q + need > qe) break;
                r.len = (uint8_t)(1 + need);
                r.b[0] = status; r.b[1] = q[0]; r.b[2] = need > 1 ? q[1] : 0;
                q += need;
            }
            if (tick > end_tick) end_tick = tick;
            r.seq = seq++;
            if (n == cap) {
                Raw* nr = (Raw*)realloc(raw, cap * 2 * sizeof(Raw));
                if (!nr) { free(raw); return nullptr; }
                raw = nr; cap *= 2;
            }
            raw[n++] = r;
        }
        if (tick > end_tick) end_tick = tick;
    }
    qsort(raw, n, sizeof(Raw), raw_cmp);

    Song* s = (Song*)malloc(sizeof(Song));
    Ev* ev = (Ev*)malloc((n ? n : 1) * sizeof(Ev));
    if (!s || !ev) { free(s); free(ev); free(raw); return nullptr; }
    // Ticks -> microseconds -> output frames, following the tempo changes.
    uint64_t us = 0;
    uint32_t last = 0, tempo = 500000, m = 0;
    auto advance = [&](uint32_t tick) {
        uint64_t dt = tick - last;
        us += smpte ? dt * 1000000ull / tps : dt * tempo / division;
        last = tick;
    };
    for (size_t i = 0; i < n; i++) {
        advance(raw[i].tick);
        if (raw[i].tempo) { tempo = raw[i].tempo; continue; }
        ev[m].frame = (uint32_t)(us * OUT_RATE / 1000000u);
        ev[m].len = raw[i].len;
        memcpy(ev[m].b, raw[i].b, 3);
        m++;
    }
    advance(end_tick);
    free(raw);
    s->ev = ev; s->n = m;
    s->end = (uint32_t)(us * OUT_RATE / 1000000u);
    if (s->n && s->end < s->ev[s->n - 1].frame) s->end = s->ev[s->n - 1].frame;
    printf("Music: %u events, %u.%u s\n", m, s->end / OUT_RATE, s->end % OUT_RATE * 10 / OUT_RATE);
    return s;
}

static void silence() {
    for (int c = 0; c < 16; c++) { gm_midi_byte(0xB0 | c); gm_midi_byte(123); gm_midi_byte(0); }   // all notes off
}

extern "C" void music_play(void* song, int loop) {
    if (!ready || !song) return;
    uintptr_t f = irq_off();
    gm_reset();
    cur = (Song*)song;
    idx = pos = 0;
    looping = loop; paused = 0; playing = 1;
    irq_restore(f);
}
extern "C" void music_stop(void) {
    if (!ready) return;
    uintptr_t f = irq_off();
    playing = 0;
    gm_reset();
    irq_restore(f);
}
extern "C" void music_free(void* song) {
    if (!song) return;
    if (cur == song) { music_stop(); cur = nullptr; }
    free(((Song*)song)->ev);
    free(song);
}
extern "C" void music_pause(int p) { paused = p; }
extern "C" void music_volume(int v) { gain = v < 0 ? 0 : v > 127 ? 127 : v; }
extern "C" int music_playing(void) { return playing && !paused; }

// ---------------------------------------------------------------- rendering
// OPL frames at OUT_RATE / DIV, each sent DIV times.
static int32_t opl_buf[2 * OPL_MAX_FRAMES];
static int32_t held_l, held_r;
static int held;                    // DIV == 2: the second copy of a frame still to send

static void render_span(int32_t* mono, int frames) {
    int g = gain;
    while (frames > 0) {
        if (held) {
            *mono++ += (held_l + held_r) * g >> 5;
            held = 0; frames--;
            continue;
        }
        int need = (frames + DIV - 1) / DIV;
        if (need > OPL_MAX_FRAMES) need = OPL_MAX_FRAMES;
        memset(opl_buf, 0, (size_t)need * 2 * sizeof(int32_t));
        opl_mix(opl_buf, need);
        for (int i = 0; i < need; i++) {
            int32_t l = opl_buf[i * 2], r = opl_buf[i * 2 + 1];
            for (int k = 0; k < DIV; k++) {
                if (frames == 0) { held_l = l; held_r = r; held = 1; break; }
                *mono++ += (l + r) * g >> 5;
                frames--;
            }
        }
    }
}

extern "C" void music_render(int32_t* mono, int frames) {
    Song* s = cur;
    if (!ready || !playing || paused || !s) return;
    while (frames > 0) {
        while (idx < s->n && s->ev[idx].frame <= pos) {
            const Ev& e = s->ev[idx++];
            for (int k = 0; k < e.len; k++) gm_midi_byte(e.b[k]);
        }
        uint32_t until = idx < s->n ? s->ev[idx].frame : s->end;
        if (pos >= until) {                                  // the song's end
            if (!looping || s->end == 0) { playing = 0; silence(); return; }
            silence();
            idx = 0; pos = 0;
            continue;
        }
        int chunk = until - pos < (uint32_t)frames ? (int)(until - pos) : frames;
        render_span(mono, chunk);
        mono += chunk; frames -= chunk; pos += chunk;
    }
}
