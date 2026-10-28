// Audio output for the bare-metal build: Intel HD Audio, Intel AC'97 and the
// Sound Blaster (Pro 2.0 and later). HD Audio and AC'97 play from the same
// looping DMA ring (16-bit stereo, 48 kHz); the Sound Blaster from its own
// (8-bit mono, 22 kHz, ISA DMA). Each frame the main loop hands us the
// game's mono samples and we write them a little ahead of the hardware's
// play position. No interrupts are used.
// (Driver shared with the Felix BASIC and Super Mario Bros. bare-metal builds.)
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "hw.hpp"
#include "pci.hpp"
#include "audio.hpp"

static const int kRate = 48000;

static AudioDriver driver = AUDIO_NONE;

// Shared ring: RING_BUFS chunks of CHUNK_FRAMES stereo frames.
#define RING_BUFS    32
#define CHUNK_FRAMES 512
#define RING_FRAMES  (RING_BUFS * CHUNK_FRAMES)
static int16_t ring[RING_FRAMES * 2] __attribute__((aligned(4096)));
static uint32_t write_pos;        // next frame we will write
static uint32_t target_ahead;     // desired latency in frames

static void io_delay(int n) { while (n--) inb(0x80); }   // ~1 us each

// ================================================================ AC'97
struct __attribute__((packed)) AcBDL { uint32_t addr; uint16_t samples; uint16_t flags; };
static AcBDL ac_bdl[RING_BUFS] __attribute__((aligned(8)));
static uint16_t ac_nam, ac_nabm;

static bool ac97_init() {
    PciDevice d;
    if (!pci_find_class(0x04, 0x01, -1, &d)) return false;
    ac_nam  = pci_read(d, 0x10) & 0xFFFC;
    ac_nabm = pci_read(d, 0x14) & 0xFFFC;
    if (!ac_nam || !ac_nabm) return false;
    pci_write(d, 0x04, pci_read(d, 0x04) | 0x05);     // I/O + bus master

    outl(ac_nabm + 0x2C, 0x00000002);                 // cold reset release
    io_delay(20000);
    outw(ac_nam + 0x00, 0);                           // codec reset
    io_delay(20000);
    outw(ac_nam + 0x02, 0x0000);                      // master volume: max, unmuted
    outw(ac_nam + 0x18, 0x0808);                      // PCM out volume
    if (inw(ac_nam + 0x28) & 1) {                     // variable rate: ask for 48 kHz
        outw(ac_nam + 0x2A, inw(ac_nam + 0x2A) | 1);
        outw(ac_nam + 0x2C, 48000);
    }

    for (int i = 0; i < RING_BUFS; i++) {
        ac_bdl[i].addr = (uint32_t)(uintptr_t)&ring[i * CHUNK_FRAMES * 2];
        ac_bdl[i].samples = CHUNK_FRAMES * 2;         // counted in 16-bit samples
        ac_bdl[i].flags = 0;
    }
    outb(ac_nabm + 0x1B, 0x02);                       // reset PCM-out engine
    io_delay(1000);
    outl(ac_nabm + 0x10, (uint32_t)(uintptr_t)ac_bdl);
    outb(ac_nabm + 0x15, RING_BUFS - 1);              // last valid index
    outb(ac_nabm + 0x1B, 0x01);                       // run
    return true;
}

static uint32_t ac97_play_pos() {
    uint8_t civ = inb(ac_nabm + 0x14) & 31;
    outb(ac_nabm + 0x15, (civ + RING_BUFS - 1) & 31); // keep LVI behind us so it loops forever
    outw(ac_nabm + 0x16, 0x1C);                       // clear status bits
    uint32_t left = inw(ac_nabm + 0x18) / 2;          // frames left in current buffer
    if (left > CHUNK_FRAMES) left = CHUNK_FRAMES;
    return civ * CHUNK_FRAMES + (CHUNK_FRAMES - left);
}

static void ac97_stop() {
    if (ac_nabm) outb(ac_nabm + 0x1B, 0x00);         // PCM-out engine stopped
}

// ================================================================ HD Audio
// Controller: CORB/RIRB for codec verbs, one output stream whose BDL covers
// the ring. Codec: walk the audio function group, find every connected
// output pin (line out, speaker, headphone), follow its connections to a
// DAC, unmute everything on the way and point every DAC at our stream.
static volatile uint8_t* hda;
static uint32_t corb[256] __attribute__((aligned(1024)));
static uint64_t rirb[256] __attribute__((aligned(2048)));
struct __attribute__((packed)) HdaBDL { uint64_t addr; uint32_t len; uint32_t ioc; };
static HdaBDL hda_bdl[RING_BUFS] __attribute__((aligned(128)));
static uint16_t rirb_rp;
static uint32_t sd;               // output stream descriptor offset

static inline uint8_t  r8 (uint32_t o) { return *(volatile uint8_t*)(hda + o); }
static inline uint16_t r16(uint32_t o) { return *(volatile uint16_t*)(hda + o); }
static inline uint32_t r32(uint32_t o) { return *(volatile uint32_t*)(hda + o); }
static inline void w8 (uint32_t o, uint8_t v)  { *(volatile uint8_t*)(hda + o) = v; }
static inline void w16(uint32_t o, uint16_t v) { *(volatile uint16_t*)(hda + o) = v; }
static inline void w32(uint32_t o, uint32_t v) { *(volatile uint32_t*)(hda + o) = v; }

static bool wait_bits32(uint32_t off, uint32_t mask, uint32_t want, int us) {
    while (us-- > 0) { if ((r32(off) & mask) == want) return true; io_delay(1); }
    return false;
}

// Send one verb and wait for its response. Returns false on timeout.
static bool hda_cmd(uint32_t verb, uint32_t* resp) {
    uint16_t wp = (r16(0x48) + 1) & 0xFF;
    corb[wp] = verb;
    w16(0x48, wp);
    for (int i = 0; i < 20000; i++) {
        uint16_t rwp = r16(0x58) & 0xFF;
        if (rwp != rirb_rp) {
            rirb_rp = (rirb_rp + 1) & 0xFF;
            if (resp) *resp = (uint32_t)rirb[rirb_rp];
            w8(0x5D, 0x05);                           // clear RIRB status
            return true;
        }
        io_delay(1);
    }
    return false;
}
static uint32_t verb(int cad, int nid, uint32_t v, uint32_t payload) {   // 12-bit verb, 8-bit payload
    uint32_t r = 0;
    hda_cmd((uint32_t)cad << 28 | (uint32_t)nid << 20 | v << 8 | payload, &r);
    return r;
}
static uint32_t verb4(int cad, int nid, uint32_t v, uint32_t payload) {  // 4-bit verb, 16-bit payload
    uint32_t r = 0;
    hda_cmd((uint32_t)cad << 28 | (uint32_t)nid << 20 | v << 16 | payload, &r);
    return r;
}
static uint32_t param(int cad, int nid, int p) { return verb(cad, nid, 0xF00, p); }

struct Widget { uint8_t type; uint8_t nconn; uint8_t conn[16]; uint32_t caps; };
static Widget widgets[128];

static void unmute_out(int cad, int nid) {
    uint32_t caps = param(cad, nid, 0x12);            // output amp caps
    uint32_t gain = caps & 0x7F;                      // offset = 0 dB
    verb4(cad, nid, 0x3, 0xB000 | gain);              // output, L+R
}
static void unmute_in(int cad, int nid, int index) {
    uint32_t caps = param(cad, nid, 0x0D);            // input amp caps
    uint32_t gain = caps & 0x7F;
    verb4(cad, nid, 0x3, 0x7000 | (index << 8) | gain);
}

// Depth-first search from a pin to a DAC; on success configures the path.
static int route_to_dac(int cad, int nid, int depth) {
    if (depth > 6 || nid >= 128) return -1;
    Widget& w = widgets[nid];
    if (w.type == 0) return nid;                      // audio output (DAC)
    for (int i = 0; i < w.nconn; i++) {
        int dac = route_to_dac(cad, w.conn[i], depth + 1);
        if (dac < 0) continue;
        if (w.type != 2 && w.nconn > 1)
            verb(cad, nid, 0x701, i);                 // connection select (mixers sum instead)
        if (w.caps & (1 << 1)) unmute_in(cad, nid, i);
        if (w.caps & (1 << 2)) unmute_out(cad, nid);
        verb(cad, nid, 0x705, 0);                     // power D0
        return dac;
    }
    return -1;
}


// ---- HDMI / DisplayPort output (the graphics card's or chipset's codec)
// A digital pin carries sound only when a monitor is attached (presence
// detect), its converter is switched to digital, and the monitor is told
// what's coming in an audio InfoFrame. As Linux's generic HDMI codec code.
static bool hdmi_present(int cad, int pin, uint32_t pcaps) {
    if (!(pcaps & (1 << 2))) return true;                     // can't tell: assume so
    if (pcaps & (1 << 1)) { verb(cad, pin, 0x709, 0); io_delay(2000); }   // pin sense trigger
    return verb(cad, pin, 0xF09, 0) & (1u << 31);
}
// The monitor's ELD (from its EDID): connection type and name, for the boot
// messages. Returns 1 for DisplayPort, 0 for HDMI, -1 if it can't be read.
static int hdmi_eld(int cad, int pin, char* name, int cap) {
    name[0] = 0;
    int size = (int)(verb(cad, pin, 0xF2E, 0x08) & 0xFF);     // ELD buffer size
    if (size < 20) return -1;
    uint8_t eld[96];
    if (size > (int)sizeof eld) size = sizeof eld;
    for (int i = 0; i < size; i++) {
        uint32_t r = verb(cad, pin, 0xF2F, i);
        if (!(r & (1u << 31))) return -1;                     // not valid (yet)
        eld[i] = r & 0xFF;
    }
    int mnl = eld[4] & 0x1F;                                  // monitor name length
    int n = 0;
    for (int i = 0; i < mnl && 20 + i < size && n < cap - 1; i++)
        if (eld[20 + i] >= 32 && eld[20 + i] < 127) name[n++] = (char)eld[20 + i];
    name[n] = 0;
    return (eld[5] >> 2) & 3;                                 // conn_type: 0 HDMI, 1 DP
}
// Audio InfoFrame (2-channel PCM, "refer to stream header") through the
// pin's data island packet buffer 0.
static void hdmi_infoframe(int cad, int pin, bool dp) {
    uint8_t f[9];
    int n;
    if (dp) {
        const uint8_t d[8] = {0x84, 0x1B, 0x11 << 2, 0x01, 0, 0, 0, 0};   // type, len, ver, CC = 2ch
        memcpy(f, d, 8); n = 8;
    } else {
        const uint8_t h[9] = {0x84, 0x01, 0x0A, 0, 0x01, 0, 0, 0, 0};     // type, ver, len, sum, CC = 2ch
        memcpy(f, h, 9); n = 9;
        uint8_t sum = 0;
        for (int i = 0; i < n; i++) sum += f[i];
        f[3] = (uint8_t)(0x100 - sum);                        // all bytes sum to 0
    }
    verb(cad, pin, 0x730, 0);                                 // DIP index: packet 0, byte 0
    verb(cad, pin, 0x732, 0x00);                              // stop sending it
    verb(cad, pin, 0x730, 0);
    for (int i = 0; i < n; i++) verb(cad, pin, 0x731, f[i]);
    for (int i = n; i < 32; i++) verb(cad, pin, 0x731, 0);    // clear the rest of the buffer
    verb(cad, pin, 0x730, 0);
    verb(cad, pin, 0x732, 0xC0);                              // send it, best effort
}
// What hda_setup_codec does with each audio function group it finds.
enum HdaMode { HDA_ANALOG, HDA_DIGITAL, HDA_LIST };
static HdaMode hda_mode;
static int want_cad, want_pin;                  // HDA_DIGITAL: the pin to play through
// HDA_LIST results: whether there's an analog output, and every HDMI/DP pin.
struct HdaPin { uint8_t cad, nid; bool dp, present; char name[32]; };
static HdaPin list_pins[16];
static int list_npins;
static bool list_analog;

static bool is_digital_out(uint32_t pcaps) { return (pcaps & (1 << 4)) && (pcaps & ((1 << 7) | (1u << 24))); }
static bool is_analog_out(int cad, int n, uint32_t pcaps) {
    if (!(pcaps & (1 << 4)) || (pcaps & ((1 << 7) | (1u << 24)))) return false;
    uint32_t cfg = verb(cad, n, 0xF1C, 0);
    int conn = cfg >> 30, dev = (cfg >> 20) & 0xF;
    return conn != 1 && (dev == 0x0 || dev == 0x1 || dev == 0x2);   // line out, speaker, HP
}

// Digital pin n: route it from a converter, switch the converter to digital
// and tell the monitor (if any) what's coming. Played through whether or not
// a monitor answers: some only show up once there's a signal.
static bool hdmi_config_pin(int cad, int n, uint8_t stream_tag, uint16_t fmt) {
    Widget& w = widgets[n];
    uint32_t pcaps = param(cad, n, 0x0C);
    char name[32];
    int type = hdmi_eld(cad, n, name, sizeof name);
    bool dp = type == 1 || (type < 0 && !(pcaps & (1 << 7)));
    verb(cad, n, 0x705, 0);                               // pin to D0
    int cvt = route_to_dac(cad, n, 0);
    if (cvt < 0) { printf("HDA: codec %d pin %d: no converter reaches it\n", cad, n); return false; }
    verb4(cad, cvt, 0x2, fmt);                            // converter format
    verb(cad, cvt, 0x72D, 1);                             // 2 channels
    verb(cad, cvt, 0x706, stream_tag << 4);               // stream tag, channel 0
    verb(cad, cvt, 0x70D, 0x01);                          // digital converter on
    verb(cad, cvt, 0x705, 0);
    unmute_out(cad, cvt);
    verb(cad, n, 0x734, 0x00);                            // channel 0 -> slot 0
    verb(cad, n, 0x734, 0x11);                            // channel 1 -> slot 1
    if (w.caps & (1 << 2)) unmute_out(cad, n);
    verb(cad, n, 0x707, 0x40);                            // pin out enable
    hdmi_infoframe(cad, n, dp);
    printf("HDA: codec %d pin %d -> converter %d (%s%s%s%s)\n", cad, n, cvt, dp ? "DisplayPort" : "HDMI",
           name[0] ? ", \"" : "", name, name[0] ? "\"" : "");
    return true;
}

static bool hda_setup_codec(int cad, uint8_t stream_tag, uint16_t fmt) {
    uint32_t sub = param(cad, 0, 0x04);
    int fg_start = (sub >> 16) & 0xFF, fg_count = sub & 0xFF;
    bool any = false;
    for (int fg = fg_start; fg < fg_start + fg_count; fg++) {
        if ((param(cad, fg, 0x05) & 0xFF) != 0x01) continue;  // audio function group only
        verb(cad, fg, 0x705, 0);                               // power up the group
        io_delay(10000);
        uint32_t ws = param(cad, fg, 0x04);
        int start = (ws >> 16) & 0xFF, count = ws & 0xFF;

        // Read every widget's type and connection list.
        for (int n = start; n < start + count && n < 128; n++) {
            Widget& w = widgets[n];
            w.caps = param(cad, n, 0x09);
            w.type = (w.caps >> 20) & 0xF;
            uint32_t cl = param(cad, n, 0x0E);
            int len = cl & 0x7F;
            bool lng = cl & 0x80;
            w.nconn = 0;
            if (!(w.caps & (1 << 8)) || lng) continue;          // no list, or long form (rare)
            for (int i = 0; i < len && w.nconn < 16; i += 4) {
                uint32_t e = verb(cad, n, 0xF02, i);
                for (int k = 0; k < 4 && i + k < len; k++)
                    w.conn[w.nconn++] = (e >> (8 * k)) & 0xFF;
            }
        }

        if (hda_mode == HDA_DIGITAL) {                        // one HDMI / DisplayPort pin
            if (cad == want_cad && want_pin >= start && want_pin < start + count && widgets[want_pin].type == 4)
                return hdmi_config_pin(cad, want_pin, stream_tag, fmt);
            continue;
        }

        for (int n = start; n < start + count && n < 128; n++) {
            Widget& w = widgets[n];
            if (w.type != 4) continue;                               // pin complex
            uint32_t pcaps = param(cad, n, 0x0C);
            if (hda_mode == HDA_LIST) {                              // just note what's there
                if (is_analog_out(cad, n, pcaps)) list_analog = true;
                else if (is_digital_out(pcaps) && (verb(cad, n, 0xF1C, 0) >> 30) != 1 &&
                         list_npins < (int)(sizeof list_pins / sizeof list_pins[0])) {
                    HdaPin& p = list_pins[list_npins++];
                    p.cad = (uint8_t)cad; p.nid = (uint8_t)n;
                    p.present = hdmi_present(cad, n, pcaps);
                    int type = hdmi_eld(cad, n, p.name, sizeof p.name);
                    p.dp = type == 1 || (type < 0 && !(pcaps & (1 << 7)));
                }
                continue;
            }
            // HDA_ANALOG: route every connected analog output pin.
            if (!is_analog_out(cad, n, pcaps)) continue;
            uint32_t cfg = verb(cad, n, 0xF1C, 0);
            int dev = (cfg >> 20) & 0xF;
            int dac = route_to_dac(cad, n, 0);
            if (dac < 0) continue;
            verb(cad, n, 0x707, dev == 0x2 ? 0xC0 : 0x40);           // out enable (+HP amp)
            if (pcaps & (1 << 16)) verb(cad, n, 0x70C, 0x02);        // EAPD on
            verb4(cad, dac, 0x2, fmt);                               // converter format
            verb(cad, dac, 0x706, stream_tag << 4);                  // stream tag, channel 0
            unmute_out(cad, dac);
            verb(cad, dac, 0x705, 0);
            printf("HDA: codec %d pin %d -> DAC %d (%s)\n", cad, n, dac,
                   dev == 0 ? "line out" : dev == 1 ? "speaker" : "headphone");
            any = true;
        }
    }
    return any;
}

// Reset controller d and start its command rings. Returns its codecs (a bit
// each), 0 if it won't come up.
static uint16_t hda_open(const PciDevice& d) {
    uint32_t id = pci_read(d, 0x00);
    uint16_t vendor = id & 0xFFFF;
    uint32_t bar = pci_read(d, 0x10);
    uint64_t base = bar & 0xFFFFFFF0;
    if ((bar & 0x6) == 0x4) base |= (uint64_t)pci_read(d, 0x14) << 32;
    if (base == 0 || base >= phys_limit) { printf("HDA: %02x:%02x.%x: BAR out of reach\n", d.bus, d.dev, d.fn); return 0; }
    hda = (volatile uint8_t*)(uintptr_t)base;
    pci_write(d, 0x04, pci_read(d, 0x04) | 0x06);    // memory + bus master
    // AMD/ATI controllers: have the controller snoop the CPU caches, as
    // Linux does (misc control 2, offset 0x42), or it can read stale
    // commands and sound from memory.
    if (vendor == 0x1022 || vendor == 0x1002) {
        uint32_t v = pci_read(d, 0x40);
        pci_write(d, 0x40, (v & ~(0x07u << 16)) | (0x02u << 16));
    }

    // Controller reset.
    w32(0x08, r32(0x08) & ~1u);
    if (!wait_bits32(0x08, 1, 0, 100000)) return 0;
    w32(0x08, r32(0x08) | 1);
    if (!wait_bits32(0x08, 1, 1, 100000)) return 0;
    io_delay(2000);                                   // codecs report in within 521 us
    uint16_t codecs = r16(0x0E);
    if (!codecs) { printf("HDA: %02x:%02x.%x: no codecs\n", d.bus, d.dev, d.fn); return 0; }

    // CORB / RIRB, 256 entries each.
    w8(0x4C, 0); w8(0x5C, 0);                         // stop DMA engines
    io_delay(1000);
    w32(0x40, (uint32_t)(uintptr_t)corb); w32(0x44, 0);
    w8(0x4E, 0x02);
    w16(0x48, 0);
    w16(0x4A, 0x8000);                                // reset read pointer
    for (int i = 0; i < 1000 && !(r16(0x4A) & 0x8000); i++) io_delay(1);
    w16(0x4A, 0);
    for (int i = 0; i < 1000 && (r16(0x4A) & 0x8000); i++) io_delay(1);
    w32(0x50, (uint32_t)(uintptr_t)rirb); w32(0x54, 0);
    w8(0x5E, 0x02);
    w16(0x58, 0x8000);                                // reset write pointer
    w16(0x5A, 0xFF);                                  // response interrupt count (unused)
    rirb_rp = 0;
    w8(0x4C, 0x02);                                   // CORB run
    w8(0x5C, 0x02);                                   // RIRB run

    // First output stream: descriptors follow the input streams.
    uint16_t gcap = r16(0x00);
    int iss = (gcap >> 8) & 0xF, oss = (gcap >> 12) & 0xF;
    if (!oss) { printf("HDA: %02x:%02x.%x: no output streams\n", d.bus, d.dev, d.fn); return 0; }
    sd = 0x80 + iss * 0x20;
    return codecs;
}

// What controller d can play through: list_analog, list_pins.
static void hda_list(const PciDevice& d) {
    list_analog = false;
    list_npins = 0;
    uint16_t codecs = hda_open(d);
    if (!codecs) return;
    hda_mode = HDA_LIST;
    for (int cad = 0; cad < 15; cad++)
        if (codecs & (1 << cad)) hda_setup_codec(cad, 1, 0x0011);
    w8(0x4C, 0); w8(0x5C, 0);                         // CORB / RIRB stopped
}

// Start playing through controller d: its analog outputs, or (digital) the
// HDMI/DP pin want_cad/want_pin.
static bool hda_try(const PciDevice& d, bool digital) {
    uint16_t codecs = hda_open(d);
    if (!codecs) return false;
    const uint8_t tag = 1;
    const uint16_t fmt = 0x0011;                      // 48 kHz, 16-bit, 2 channels
    hda_mode = digital ? HDA_DIGITAL : HDA_ANALOG;
    bool routed = false;
    for (int cad = 0; cad < 15 && !(digital && routed); cad++)
        if (codecs & (1 << cad)) routed |= hda_setup_codec(cad, tag, fmt);
    if (!routed) {
        w8(0x4C, 0); w8(0x5C, 0);
        printf("HDA: %02x:%02x.%x: nothing to play through\n", d.bus, d.dev, d.fn);
        return false;
    }

    // Stream reset, then program the BDL over the shared ring.
    w8(sd + 0, r8(sd + 0) | 1);
    for (int i = 0; i < 1000 && !(r8(sd + 0) & 1); i++) io_delay(1);
    w8(sd + 0, r8(sd + 0) & ~1);
    for (int i = 0; i < 1000 && (r8(sd + 0) & 1); i++) io_delay(1);

    for (int i = 0; i < RING_BUFS; i++) {
        hda_bdl[i].addr = (uint64_t)(uintptr_t)&ring[i * CHUNK_FRAMES * 2];
        hda_bdl[i].len = CHUNK_FRAMES * 4;
        hda_bdl[i].ioc = 0;
    }
    w32(sd + 0x18, (uint32_t)(uintptr_t)hda_bdl);
    w32(sd + 0x1C, 0);
    w32(sd + 0x08, sizeof(ring));                     // cyclic buffer length
    w16(sd + 0x0C, RING_BUFS - 1);                    // last valid index
    w16(sd + 0x12, fmt);
    w8(sd + 0x02, tag << 4);                          // stream number (CTL bits 23:20)
    w8(sd + 0x00, 0x02);                              // run
    return true;
}

static void hda_stop() {
    if (!hda) return;
    w8(sd + 0, 0);                                    // stream stopped
    for (int i = 0; i < 1000 && (r8(sd + 0) & 2); i++) io_delay(1);
    w8(0x4C, 0); w8(0x5C, 0);                         // CORB / RIRB stopped
}

// hda=BB:DD.F (hex, as lspci prints it). Returns false if absent or malformed.
static bool hda_address(const char* cmdline, int* bus, int* dev, int* fn) {
    for (const char* p = cmdline; p && *p; p++) {
        if (strncmp(p, "hda=", 4) != 0 || (p != cmdline && p[-1] != ' ')) continue;
        int v[3] = {0, 0, 0}, k = 0;
        for (const char* q = p + 4; *q && *q != ' ' && k < 3; q++) {
            char c = *q;
            if (c == ':' || c == '.') { k++; continue; }
            int x = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
            if (x < 0) return false;
            v[k] = v[k] * 16 + x;
        }
        if (k != 2) return false;
        *bus = v[0]; *dev = v[1]; *fn = v[2];
        return true;
    }
    return false;
}

static uint32_t hda_play_pos() {
    return (r32(sd + 0x04) / 4) % RING_FRAMES;       // link position in buffer
}


// ================================================================ Sound Blaster
// The Sound Blaster Pro 2.0's DSP (version 3.xx; the SB16 and its clones
// speak it too): 8-bit unsigned mono from an auto-initialising ISA DMA
// ring on an 8-bit channel, the rate set by a time constant. The DSP's
// interrupt is left masked at the PIC; the play position comes from the
// DMA controller's count, as with the other cards.
static uint16_t sb_base = 0x220;
static int sb_dma = 1;
static uint32_t sb_rate;                     // what the time constant gives
#define SB_RING 16384                        // bytes (frames): 0.74 s at 22 kHz
static uint8_t sb_ring[SB_RING] __attribute__((aligned(65536)));   // in one 64 KB DMA page
static uint32_t sb_write;                    // next frame we will write
static uint32_t sb_ahead;                    // desired latency in frames
static uint32_t sb_phase;                    // 48 kHz -> sb_rate resampling
static int32_t  sb_acc, sb_accn;

static bool sb_wait_write() {
    for (int i = 0; i < 20000; i++) if (!(inb(sb_base + 0xC) & 0x80)) return true;
    return false;
}
static bool sb_wait_read() {
    for (int i = 0; i < 20000; i++) if (inb(sb_base + 0xE) & 0x80) return true;
    return false;
}
static void sb_out(uint8_t v) { if (sb_wait_write()) outb(sb_base + 0xC, v); }
static int sb_in() { return sb_wait_read() ? inb(sb_base + 0xA) : -1; }

static bool sb_reset() {
    outb(sb_base + 0x6, 1);
    io_delay(10);
    outb(sb_base + 0x6, 0);
    for (int i = 0; i < 100; i++) {
        if ((inb(sb_base + 0xE) & 0x80) && inb(sb_base + 0xA) == 0xAA) return true;
        io_delay(10);
    }
    return false;
}

// 8237 registers for 8-bit channels 0-3: address, count, page.
static const uint8_t kDmaAddr[4] = {0x00, 0x02, 0x04, 0x06};
static const uint8_t kDmaCount[4] = {0x01, 0x03, 0x05, 0x07};
static const uint8_t kDmaPage[4] = {0x87, 0x83, 0x81, 0x82};

static void sb_stop() {
    sb_reset();                                       // ends auto-init output, speaker off
    outb(0x0A, 0x04 | sb_dma);                        // mask its DMA channel
}

static bool sb_init(const char* cmdline) {
    // sb=220,1  (port, 8-bit DMA channel)
    for (const char* p = cmdline; p && *p; p++)
        if (!strncmp(p, "sb=", 3)) {
            unsigned v = 0; const char* q = p + 3;
            for (; (*q >= '0' && *q <= '9') || ((*q | 32) >= 'a' && (*q | 32) <= 'f'); q++)
                v = v * 16 + (*q <= '9' ? *q - '0' : (*q | 32) - 'a' + 10);
            if (v >= 0x200 && v <= 0x280) sb_base = (uint16_t)v;
            if (*q == ',' && q[1] >= '0' && q[1] <= '3') sb_dma = q[1] - '0';
        }
    if (!sb_reset()) return false;
    sb_out(0xE1);                                     // DSP version
    int major = sb_in(), minor = sb_in();
    if (major < 2) return false;                      // the SB 1.x has no auto-init DMA
    uintptr_t phys = (uintptr_t)sb_ring;
    if (phys + SB_RING > 0x1000000) return false;     // ISA DMA reaches the first 16 MB only

    // Mixer (SB Pro and later): mono output, filter on, voice and master full.
    if (major >= 3) {
        outb(sb_base + 4, 0x0E); outb(sb_base + 5, 0x00);
        outb(sb_base + 4, 0x04); outb(sb_base + 5, 0xFF);
        outb(sb_base + 4, 0x22); outb(sb_base + 5, 0xFF);
    }
    memset(sb_ring, 0x80, sizeof sb_ring);            // silence is 128

    // DMA: single mode, auto-init, memory to device, over the whole ring.
    int ch = sb_dma;
    outb(0x0A, 0x04 | ch);                            // mask the channel
    outb(0x0C, 0);                                    // clear the byte flip-flop
    outb(0x0B, 0x58 | ch);
    outb(kDmaAddr[ch], phys & 0xFF);
    outb(kDmaAddr[ch], (phys >> 8) & 0xFF);
    outb(kDmaPage[ch], (phys >> 16) & 0xFF);
    outb(kDmaCount[ch], (SB_RING - 1) & 0xFF);
    outb(kDmaCount[ch], ((SB_RING - 1) >> 8) & 0xFF);
    outb(0x0A, ch);                                   // unmask

    // DSP: speaker on, 22 kHz (time constant 256 - 1000000/rate), a block
    // the length of the ring, 8-bit auto-init output.
    int tc = 211;
    sb_rate = 1000000 / (256 - tc);                   // 22222 Hz
    sb_out(0xD1);
    sb_out(0x40); sb_out((uint8_t)tc);
    sb_out(0x48); sb_out((SB_RING - 1) & 0xFF); sb_out(((SB_RING - 1) >> 8) & 0xFF);
    sb_out(0x1C);
    printf("Audio: Sound Blaster DSP %d.%02d at %Xh, DMA %d\n", major, minor < 0 ? 0 : minor, sb_base, ch);
    return true;
}

// Frames the DMA controller has played into the ring.
static uint32_t sb_play_pos() {
    int ch = sb_dma;
    inb(sb_base + 0xE);                               // acknowledge the DSP's block interrupt
    uint32_t c1, c2;
    do {
        outb(0x0C, 0);
        c1 = inb(kDmaCount[ch]); c1 |= inb(kDmaCount[ch]) << 8;
        outb(0x0C, 0);
        c2 = inb(kDmaCount[ch]); c2 |= inb(kDmaCount[ch]) << 8;
    } while (c1 - c2 > 4 && c2 - c1 > 4);             // read twice: the count was changing
    uint32_t left = (c2 + 1) & 0xFFFF;                // bytes left in this pass
    if (left > SB_RING) left = SB_RING;
    return (SB_RING - left) % SB_RING;
}

// As audio_frames_wanted, in the Sound Blaster's frames; `nominal` and the
// answer are 48 kHz frames.
static int sb_frames_wanted(int nominal, uint32_t* underruns) {
    uint32_t play = sb_play_pos();
    uint32_t ahead = (sb_write + SB_RING - play) % SB_RING;
    int nom = (int)((uint32_t)nominal * sb_rate / 48000);
    if (ahead < sb_ahead / 8 || ahead > sb_ahead * 2) {
        if (ahead < sb_ahead / 8 || ahead >= SB_RING / 2) (*underruns)++;
        sb_write = (play + sb_ahead + SB_RING - nom % SB_RING) % SB_RING;
        return nominal;
    }
    int err = (int)(ahead + nom) - (int)sb_ahead;
    int m = nom - err / 8, lim = nom / 32 + 1;
    m = m < nom - lim ? nom - lim : m > nom + lim ? nom + lim : m;
    return (int)((uint32_t)m * 48000 / sb_rate);
}

// 48 kHz signed 16-bit in, sb_rate 8-bit unsigned out: each output sample
// is the average of the input samples that fall in it.
static void sb_submit(const int16_t* samples, int n) {
    for (int i = 0; i < n; i++) {
        sb_acc += samples[i]; sb_accn++;
        sb_phase += sb_rate;
        if (sb_phase >= 48000) {
            sb_phase -= 48000;
            int v = sb_acc / sb_accn;
            sb_acc = 0; sb_accn = 0;
            sb_ring[sb_write] = (uint8_t)((v >> 8) + 128);
            sb_write = (sb_write + 1) % SB_RING;
        }
    }
}

// ================================================================ common
static bool arg_is(const char* cmdline, const char* want) {
    if (!cmdline) return false;
    for (const char* p = cmdline; *p; p++)
        if (strncmp(p, "audio=", 6) == 0 && strncmp(p + 6, want, strlen(want)) == 0) return true;
    return false;
}

// ---------------------------------------------------------------- outputs
// Every way of making sound this machine has, found once at boot: each HD
// Audio controller's analog jacks and its HDMI/DisplayPort output (when a
// monitor is on it), AC'97, a Sound Blaster, and the PC speaker. One plays
// at a time; audio_select() switches.
enum OutKind : uint8_t { OUT_SPEAKER, OUT_HDA_ANALOG, OUT_HDA_DIGITAL, OUT_AC97, OUT_SB };
struct AudioOut { OutKind kind; PciDevice d; uint8_t cad, pin; bool monitor; char name[64]; };
#define MAX_OUTS 24
static AudioOut outs[MAX_OUTS];
static int nouts, cur_out = -1;
static int latency_ms;
static const char* boot_cmdline;                      // for sb= when the Sound Blaster restarts

static const char* hda_vendor(const PciDevice& d) {
    switch (pci_read(d, 0x00) & 0xFFFF) {
    case 0x10DE: return "NVIDIA";
    case 0x1002: case 0x1022: return "AMD";
    case 0x8086: return "Intel";
    default: return "HD Audio";
    }
}

static AudioOut* add_out(OutKind k, const PciDevice* d, const char* name) {
    if (nouts >= MAX_OUTS) return nullptr;
    AudioOut& o = outs[nouts++];
    o.kind = k;
    if (d) o.d = *d;
    snprintf(o.name, sizeof o.name, "%s", name);
    return &o;
}

static void stop_current() {
    switch (driver) {
    case AUDIO_HDA:  hda_stop(); break;
    case AUDIO_AC97: ac97_stop(); break;
    case AUDIO_SB:   sb_stop(); break;
    default: break;
    }
    driver = AUDIO_NONE;
}

// Start output i; the ring starts out silent and the write position a
// latency ahead of the card.
static bool start_out(int i) {
    stop_current();
    memset(ring, 0, sizeof(ring));
    const AudioOut& o = outs[i];
    bool ok = false;
    switch (o.kind) {
    case OUT_HDA_ANALOG:  ok = hda_try(o.d, false); if (ok) driver = AUDIO_HDA; break;
    case OUT_HDA_DIGITAL:
        want_cad = o.cad; want_pin = o.pin;
        ok = hda_try(o.d, true); if (ok) driver = AUDIO_HDA; break;
    case OUT_AC97:        ok = ac97_init();         if (ok) driver = AUDIO_AC97; break;
    case OUT_SB:          ok = sb_init(boot_cmdline); if (ok) driver = AUDIO_SB; break;
    case OUT_SPEAKER:     ok = true; break;
    }
    if (!ok) return false;
    cur_out = i;
    if (driver == AUDIO_SB) {
        sb_ahead = sb_rate * latency_ms / 1000;
        sb_write = (sb_play_pos() + sb_ahead) % SB_RING;
    } else if (driver != AUDIO_NONE) {
        target_ahead = (uint32_t)(kRate * latency_ms / 1000);
        write_pos = (audio_play_pos() + target_ahead) % RING_FRAMES;
    }
    printf("Audio: %s\n", o.name);
    return true;
}

int audio_output_count() { return nouts; }
int audio_output_current() { return cur_out; }
const char* audio_output_name(int i) { return i >= 0 && i < nouts ? outs[i].name : ""; }
// Switch to output i (the note player must be held meanwhile: it calls in
// from the timer interrupt). On failure the previous one is restarted.
bool audio_select(int i) {
    if (i < 0 || i >= nouts) return false;
    int was = cur_out;
    if (start_out(i)) return true;
    printf("Audio: couldn't start %s\n", outs[i].name);
    if (was >= 0) start_out(was);
    return false;
}

AudioDriver audio_init(const char* cmdline) {
    boot_cmdline = cmdline;
    bool want_off = arg_is(cmdline, "off") || arg_is(cmdline, "speaker");
    bool want_hdmi = arg_is(cmdline, "hdmi");
    bool want_hda = arg_is(cmdline, "hda") || want_hdmi || arg_is(cmdline, "analog");
    bool want_ac  = arg_is(cmdline, "ac97");
    bool want_sb  = arg_is(cmdline, "sb");
    int hb = -1, hd = 0, hf = 0;
    if (hda_address(cmdline, &hb, &hd, &hf)) want_hda = true;
    bool any = !want_hda && !want_ac && !want_sb;

    // Sound is topped up at 240 Hz (kernel.cpp), so a short cushion is enough:
    // 20 ms. A 386 can spend longer than that on one frame, so it gets 50 ms.
#ifdef __x86_64__
    latency_ms = 20;
#else
    latency_ms = 50;
#endif
    // Boot option latency=N (milliseconds, 5..200) overrides it: lower if
    // sound feels late, higher if it crackles (the debug heartbeat counts underruns).
    for (const char* p = cmdline; p && *p; p++)
        if (!strncmp(p, "latency=", 8)) {
            int v = 0;
            for (const char* q = p + 8; *q >= '0' && *q <= '9'; q++) v = v * 10 + (*q - '0');
            if (v >= 5 && v <= 200) latency_ms = v;
        }

    // Find every output. HD Audio: each controller's analog jacks, and every
    // HDMI/DisplayPort pin with a monitor the codec knows about.
    PciDevice d;
    for (int i = 0; i < 8 && pci_find_class(0x04, 0x03, -1, &d, i); i++) {
        char name[64];
        hda_list(d);
        if (list_analog) {
            snprintf(name, sizeof name, "%s speakers/headphones (%02x:%02x.%x)", hda_vendor(d), d.bus, d.dev, d.fn);
            add_out(OUT_HDA_ANALOG, &d, name);
        }
        for (int p = 0; p < list_npins; p++) {
            const HdaPin& hp = list_pins[p];
            printf("HDA: %02x:%02x.%x codec %d pin %d: %s%s%s\n", d.bus, d.dev, d.fn, hp.cad, hp.nid,
                   hp.present ? "monitor present" : "no monitor seen", hp.name[0] ? ", " : "", hp.name);
            // Only connectors with a monitor the codec knows about: without a
            // graphics driver to tell it, it usually doesn't (and nothing
            // would reach the monitor anyway).
            if (!hp.present && !hp.name[0]) continue;
            if (hp.name[0]) snprintf(name, sizeof name, "%s %s %d \"%s\" (%02x:%02x.%x)", hda_vendor(d),
                                     hp.dp ? "DP" : "HDMI", p + 1, hp.name, d.bus, d.dev, d.fn);
            else snprintf(name, sizeof name, "%s %s %d%s (%02x:%02x.%x)", hda_vendor(d), hp.dp ? "DP" : "HDMI",
                          p + 1, hp.present ? "" : ", no monitor seen", d.bus, d.dev, d.fn);
            AudioOut* o = add_out(OUT_HDA_DIGITAL, &d, name);
            if (o) { o->cad = hp.cad; o->pin = hp.nid; o->monitor = true; }
        }
    }
    if (ac97_init()) { ac97_stop(); add_out(OUT_AC97, nullptr, "AC'97"); }
    if (sb_init(cmdline)) { sb_stop(); add_out(OUT_SB, nullptr, "Sound Blaster"); }
    add_out(OUT_SPEAKER, nullptr, "PC speaker");

    // The one to start with: what the boot options ask for, else the
    // built-in speakers/headphones, else a monitor, else AC'97, else a
    // Sound Blaster, else the PC speaker.
    static const OutKind analog_first[] = {OUT_HDA_ANALOG, OUT_HDA_DIGITAL, OUT_AC97, OUT_SB};
    static const OutKind hdmi_first[]   = {OUT_HDA_DIGITAL, OUT_HDA_ANALOG, OUT_AC97, OUT_SB};
    const OutKind* order = want_hdmi ? hdmi_first : analog_first;
    if (!want_off)
        for (int k = 0; k < 4 && cur_out < 0; k++) {
            OutKind kind = order[k];
            bool hda_kind = kind == OUT_HDA_ANALOG || kind == OUT_HDA_DIGITAL;
            if (!any && !(hda_kind && want_hda) && !(kind == OUT_AC97 && want_ac) && !(kind == OUT_SB && want_sb)) continue;
            for (int pass = 0; pass < 2 && cur_out < 0; pass++)    // digital: monitors that answered first
            for (int i = 0; i < nouts && cur_out < 0; i++) {
                const AudioOut& o = outs[i];
                if (o.kind != kind) continue;
                if (kind == OUT_HDA_DIGITAL && pass == 0 && !o.monitor) continue;
                if (kind != OUT_HDA_DIGITAL && pass == 1) continue;
                if (hda_kind && hb >= 0 && (o.d.bus != hb || o.d.dev != hd || o.d.fn != hf)) continue;
                start_out(i);
            }
        }
    if (cur_out < 0) start_out(nouts - 1);            // the PC speaker
    printf("Audio: %d output%s\n", nouts, nouts == 1 ? "" : "s");
    return driver;
}

uint32_t audio_play_pos() {
    switch (driver) {
    case AUDIO_HDA:  return hda_play_pos();
    case AUDIO_AC97: return ac97_play_pos();
    case AUDIO_SB:   return sb_play_pos();
    default:         return 0;
    }
}

// How many frames to write now, for `nominal` frames' worth of game time,
// so the write position stays a steady target_ahead in front of the card.
// The game runs on the PIT's 60 Hz and the card plays on its own crystal;
// left alone, the gap drifts (and with it the delay you hear). So each batch
// is stretched or squeezed by up to ~3% to pull the gap back, and only a
// real stall forces a jump.
static uint32_t underruns;
uint32_t audio_underruns() { return underruns; }
int audio_frames_wanted(int nominal) {
    if (driver == AUDIO_NONE || nominal <= 0) return nominal;
    if (driver == AUDIO_SB) return sb_frames_wanted(nominal, &underruns);
    uint32_t play = audio_play_pos();
    uint32_t ahead = (write_pos + RING_FRAMES - play) % RING_FRAMES;
    if (ahead < target_ahead / 8 || ahead > target_ahead * 2) {
        if (ahead < target_ahead / 8 || ahead >= RING_FRAMES / 2) underruns++;   // ran dry: a gap you can hear
        write_pos = (play + target_ahead + RING_FRAMES - nominal % RING_FRAMES) % RING_FRAMES;   // restart the gap
        return nominal;
    }
    int err = (int)(ahead + nominal) - (int)target_ahead;      // + : more delay than wanted
    int m = nominal - err / 8, lim = nominal / 32 + 1;
    return m < nominal - lim ? nominal - lim : m > nominal + lim ? nominal + lim : m;
}

// Volume, 0..100 % (and mute), applied to the samples on their way to the
// card, so it works the same on every output; the PC speaker can only mute.
static int volume = 100;
static bool muted;
int audio_volume() { return muted ? 0 : volume; }
int audio_volume_setting() { return volume; }
bool audio_muted() { return muted; }
void audio_set_volume(int v) { volume = v < 0 ? 0 : v > 100 ? 100 : v; }
void audio_set_muted(bool m) { muted = m; }

void audio_submit(const int16_t* samples, int n) {
    if (driver == AUDIO_NONE) return;
    int v = audio_volume();
    // Perceived loudness is roughly logarithmic: square the setting so the
    // steps sound even (50 % is about a quarter of the amplitude).
    int gain = v * v;                                  // 0 .. 10000
    if (driver == AUDIO_SB) {
        static int16_t tmp[2048];
        if (n > 2048) n = 2048;
        for (int i = 0; i < n; i++) tmp[i] = (int16_t)(samples[i] * gain / 10000);
        sb_submit(tmp, n);
        return;
    }
    for (int i = 0; i < n; i++) {
        int16_t x = (int16_t)(samples[i] * gain / 10000);
        ring[write_pos * 2] = x;
        ring[write_pos * 2 + 1] = x;
        write_pos = (write_pos + 1) % RING_FRAMES;
    }
}

uint32_t audio_delay_ms() {
    if (driver == AUDIO_NONE) return 0;
    if (driver == AUDIO_SB) return (sb_write + SB_RING - sb_play_pos()) % SB_RING * 1000 / sb_rate;
    return (write_pos + RING_FRAMES - audio_play_pos()) % RING_FRAMES * 1000 / kRate;
}

const char* audio_name() {
    return driver == AUDIO_HDA ? "HD Audio" : driver == AUDIO_AC97 ? "AC97" :
           driver == AUDIO_SB ? "Sound Blaster" : "none";
}
