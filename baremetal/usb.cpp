// xHCI USB keyboard driver (polled, boot protocol).
//
// Takes every xHCI controller from the BIOS, resets it, enumerates keyboards and
// mice on the root ports and behind USB 2 hubs (hubs in hubs too, and ones
// plugged in later), and turns keyboards' 8-byte boot reports into the same
// PS/2 set-1 scancodes the rest of the kernel already handles. A USB 3
// hub's SuperSpeed half is left alone: keyboards and mice show up on its
// USB 2 half, which is an ordinary hub.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "hw.hpp"
#include "pci.hpp"
#include "usb.hpp"

void kbd_push(uint8_t b);                 // kernel.cpp: feeds the scancode queue
void mouse_push(int dx, int dy, int buttons, int wheel);   // kernel.cpp: relative motion, buttons, wheel

static void io_delay(int n) { while (n--) inb(0x80); }   // ~1 us each
static void delay_ms(int ms) { io_delay(ms * 1000); }
#define barrier() __asm__ volatile("" ::: "memory")

// ---------------------------------------------------------------- memory
// All controller-visible structures come from one zeroed, identity-mapped pool.
static uint8_t pool[2 << 20] __attribute__((aligned(4096)));
static size_t pool_used;
static void* dma_alloc(size_t size, size_t align) {
    pool_used = (pool_used + align - 1) & ~(align - 1);
    if (pool_used + size > sizeof(pool)) return nullptr;
    void* p = pool + pool_used;
    pool_used += size;
    memset(p, 0, size);
    return p;
}
static inline uint64_t phys(const volatile void* p) { return (uint64_t)(uintptr_t)p; }

// ---------------------------------------------------------------- registers
static inline uint32_t rd(volatile uint8_t* b, uint32_t o) { return *(volatile uint32_t*)(b + o); }
static inline void wr(volatile uint8_t* b, uint32_t o, uint32_t v) { *(volatile uint32_t*)(b + o) = v; }
static inline void wr64(volatile uint8_t* b, uint32_t o, uint64_t v) { wr(b, o, (uint32_t)v); wr(b, o + 4, (uint32_t)(v >> 32)); }

// Bits that must be written back unchanged when touching PORTSC; everything
// else (the write-1-to-clear change bits, PED) is written as 0.
static const uint32_t PORT_KEEP = (1u << 0) | (1u << 3) | (0xFu << 10) | (1u << 30) |
                                  (0xFu << 5) | (1u << 9) | (3u << 14) | (7u << 25);
enum {
    PORT_CCS = 1 << 0, PORT_PED = 1 << 1, PORT_PR = 1 << 4, PORT_PP = 1 << 9,
    PORT_CSC = 1 << 17, PORT_PRC = 1 << 21, PORT_CHANGES = 0x7F << 17,
};

// ---------------------------------------------------------------- rings
struct Trb { uint32_t d0, d1, d2, d3; };
enum {
    TRB_NORMAL = 1, TRB_SETUP = 2, TRB_DATA = 3, TRB_STATUS = 4, TRB_LINK = 6,
    TRB_ENABLE_SLOT = 9, TRB_ADDRESS_DEVICE = 11, TRB_CONFIGURE_EP = 12, TRB_EVALUATE_CTX = 13,
    TRB_TRANSFER_EVENT = 32, TRB_CMD_COMPLETION = 33, TRB_PORT_STATUS = 34,
};
#define RING_TRBS 64

struct Ring {
    volatile Trb* trb;
    int enq;
    uint32_t cycle;
};

static bool ring_init(Ring& r) {
    r.trb = (volatile Trb*)dma_alloc(RING_TRBS * sizeof(Trb), 64);
    if (!r.trb) return false;
    r.enq = 0;
    r.cycle = 1;
    volatile Trb& link = r.trb[RING_TRBS - 1];     // last TRB links back, toggling cycle
    link.d0 = (uint32_t)phys(r.trb);
    link.d1 = (uint32_t)(phys(r.trb) >> 32);
    link.d3 = TRB_LINK << 10 | (1 << 1);
    return true;
}

/* A ring in memory that's already allocated (reused across plug-ins). */
static void ring_attach(Ring& r, volatile Trb* mem) {
    r.trb = mem;
    memset((void*)mem, 0, RING_TRBS * sizeof(Trb));
    r.enq = 0;
    r.cycle = 1;
    volatile Trb& link = r.trb[RING_TRBS - 1];
    link.d0 = (uint32_t)phys(r.trb);
    link.d1 = (uint32_t)(phys(r.trb) >> 32);
    link.d3 = TRB_LINK << 10 | (1 << 1);
}

static uint64_t ring_push(Ring& r, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3) {
    volatile Trb& t = r.trb[r.enq];
    uint64_t addr = phys(&t);
    t.d0 = d0; t.d1 = d1; t.d2 = d2;
    barrier();
    t.d3 = (d3 & ~1u) | r.cycle;
    if (++r.enq == RING_TRBS - 1) {
        volatile Trb& link = r.trb[RING_TRBS - 1];
        link.d3 = (link.d3 & ~1u) | r.cycle;       // hand the link TRB over too
        r.enq = 0;
        r.cycle ^= 1;
    }
    return addr;
}

#define EVT_TRBS 256

// One xHCI controller. A PC can have several (the chipset's plus add-in
// cards, or separate USB 2/USB 3 controllers on some boards); each gets its
// own registers, command and event rings and device slots. H is the one
// being worked on: set it before touching a controller or its devices.
struct Hc {
    volatile uint8_t *cap, *op, *rt;
    volatile uint32_t* db;
    Ring cmd_ring;
    volatile Trb* evt;
    int evt_deq;
    uint32_t evt_cycle;
    size_t csz;                            // context size: 32 or 64 bytes
    volatile uint64_t* dcbaa;
    int max_slots, num_ports;
    bool port_dirty[256];
};
#define MAX_HC 8
static Hc hcs[MAX_HC];
static int num_hc;
static Hc* H;

static inline uint32_t portsc(int port) { return rd(H->op, 0x400 + 0x10 * (port - 1)); }
static inline void set_portsc(int port, uint32_t v) { wr(H->op, 0x400 + 0x10 * (port - 1), v); }

static bool next_event(Trb* out) {
    volatile Trb& e = H->evt[H->evt_deq];
    if ((e.d3 & 1) != H->evt_cycle) return false;
    barrier();
    out->d0 = e.d0; out->d1 = e.d1; out->d2 = e.d2; out->d3 = e.d3;
    if (++H->evt_deq == EVT_TRBS) { H->evt_deq = 0; H->evt_cycle ^= 1; }
    wr64(H->rt, 0x38, phys(&H->evt[H->evt_deq]) | (1 << 3));  // ERDP, clear busy
    return true;
}

// ---------------------------------------------------------------- devices

// Everything plugged in that we drive: keyboards, mice and the hubs they
// hang off. Where a device is: the root port its tree starts at, the route
// string down through the hubs (4 bits a tier), and, for a low/full-speed
// device behind a high-speed hub, that hub's slot and port (its transaction
// translator).
enum Kind { KBD, MOUSE, HUB };
struct Keyboard {
    bool active;
    Hc* hc;                                // the controller it's on
    Kind kind;
    bool mouse;                            // kind == MOUSE (kept for the report code)
    int port;                              // root port
    uint32_t route;
    int depth;                             // hub tier below the root (0: on a root port)
    int parent;                            // index of the hub it's on, -1: a root port
    int hub_port;                          // its port on that hub
    int tt_slot, tt_port;                  // transaction translator, 0: none
    int slot, speed, dci, mps;
    volatile uint8_t* out_ctx;
    volatile uint8_t* in_ctx;
    Ring ep0, intr;
    volatile uint8_t* reports;             // one 8-byte buffer per ring slot
    uint8_t prev[8];
    // hubs
    int nports, ttt;
    uint32_t dirty;                        // ports with a change waiting (bit n = port n)
    uint32_t power_good_ms;
    char where[24];                        // "port 3" / "port 3.2" for messages
};
#define MAX_KBD 32
static Keyboard kbds[MAX_KBD];
/* Each table entry's controller-visible memory, allocated the first time
 * the entry is used and reused by whatever is plugged in next: the pool is
 * never freed, so allocating per plug-in ran it dry after a few hundred. */
static struct {
    volatile uint8_t *out_ctx, *in_ctx, *reports;
    volatile Trb *ep0, *intr;
} devmem[MAX_KBD];

static inline volatile uint32_t* ctx(volatile uint8_t* base, int idx) {
    return (volatile uint32_t*)(base + idx * H->csz);
}

// ---------------------------------------------------------------- key reports
// HID usage -> PS/2 set-1 scancode (0x100 flag = E0-prefixed): letters,
// digits, punctuation, Enter/Esc/Backspace/Tab/Space, F1-F12, arrows, the
// editing keys and the keypad.
static const struct { uint8_t usage; uint16_t code; } keymap[] = {
    {0x04, 0x1E}, {0x05, 0x30}, {0x06, 0x2E}, {0x07, 0x20}, {0x08, 0x12}, {0x09, 0x21},
    {0x0A, 0x22}, {0x0B, 0x23}, {0x0C, 0x17}, {0x0D, 0x24}, {0x0E, 0x25}, {0x0F, 0x26},
    {0x10, 0x32}, {0x11, 0x31}, {0x12, 0x18}, {0x13, 0x19}, {0x14, 0x10}, {0x15, 0x13},
    {0x16, 0x1F}, {0x17, 0x14}, {0x18, 0x16}, {0x19, 0x2F}, {0x1A, 0x11}, {0x1B, 0x2D},
    {0x1C, 0x15}, {0x1D, 0x2C}, {0x1E, 0x2}, {0x1F, 0x3}, {0x20, 0x4}, {0x21, 0x5},
    {0x22, 0x6}, {0x23, 0x7}, {0x24, 0x8}, {0x25, 0x9}, {0x26, 0xA}, {0x27, 0xB},
    {0x28, 0x1C}, {0x29, 0x1}, {0x2A, 0xE}, {0x2B, 0xF}, {0x2C, 0x39}, {0x2D, 0xC},
    {0x2E, 0xD}, {0x2F, 0x1A}, {0x30, 0x1B}, {0x31, 0x2B}, {0x33, 0x27}, {0x34, 0x28},
    {0x35, 0x29}, {0x36, 0x33}, {0x37, 0x34}, {0x38, 0x35}, {0x3A, 0x3B}, {0x3B, 0x3C},
    {0x3C, 0x3D}, {0x3D, 0x3E}, {0x3E, 0x3F}, {0x3F, 0x40}, {0x40, 0x41}, {0x41, 0x42},
    {0x42, 0x43}, {0x43, 0x44}, {0x44, 0x57}, {0x45, 0x58}, {0x4F, 0x14D}, {0x50, 0x14B},
    {0x51, 0x150}, {0x52, 0x148}, {0x58, 0x11C},
    // Caps Lock; Insert, Home, Page Up, Delete, End, Page Down
    {0x39, 0x3A}, {0x49, 0x152}, {0x4A, 0x147}, {0x4B, 0x149}, {0x4C, 0x153}, {0x4D, 0x14F}, {0x4E, 0x151},
    // keypad: Num Lock, / * - +, 1-9, 0, .
    {0x53, 0x45}, {0x54, 0x135}, {0x55, 0x37}, {0x56, 0x4A}, {0x57, 0x4E},
    {0x59, 0x4F}, {0x5A, 0x50}, {0x5B, 0x51}, {0x5C, 0x4B}, {0x5D, 0x4C}, {0x5E, 0x4D},
    {0x5F, 0x47}, {0x60, 0x48}, {0x61, 0x49}, {0x62, 0x52}, {0x63, 0x53},
    {0x64, 0x56},                                       // the key left of Z on ISO boards
};

static void emit(uint16_t code, bool down) {
    if (code & 0x100) kbd_push(0xE0);
    kbd_push((uint8_t)(code & 0x7F) | (down ? 0 : 0x80));
}
static void emit_usage(uint8_t usage, bool down) {
    for (auto& k : keymap) if (k.usage == usage) { emit(k.code, down); return; }
}

static void handle_report(Keyboard& k, const volatile uint8_t* r) {
    if (k.mouse) {                                         // buttons, dx, dy (boot protocol)
        // Most mice add the wheel as a 4th byte even in the boot protocol;
        // a 3-byte report leaves it 0 (queue_report clears the buffer).
        mouse_push((int8_t)r[1], (int8_t)r[2], r[0] & 7, (int8_t)r[3]);
        return;
    }
    uint8_t cur[8];
    for (int i = 0; i < 8; i++) cur[i] = r[i];
    if (cur[2] == 1) return;                               // rollover error: ignore
    uint8_t mchg = cur[0] ^ k.prev[0];
    if (mchg & 0x02) emit(0x2A, cur[0] & 0x02);            // left shift
    if (mchg & 0x20) emit(0x36, cur[0] & 0x20);            // right shift
    if (mchg & 0x01) emit(0x1D, cur[0] & 0x01);            // left ctrl
    if (mchg & 0x10) emit(0x11D, cur[0] & 0x10);           // right ctrl
    if (mchg & 0x04) emit(0x38, cur[0] & 0x04);            // left alt
    if (mchg & 0x40) emit(0x138, cur[0] & 0x40);           // right alt
    for (int i = 2; i < 8; i++) {                          // releases
        uint8_t u = k.prev[i];
        if (!u) continue;
        bool still = false;
        for (int j = 2; j < 8; j++) still |= cur[j] == u;
        if (!still) emit_usage(u, false);
    }
    for (int i = 2; i < 8; i++) {                          // presses
        uint8_t u = cur[i];
        if (!u) continue;
        bool was = false;
        for (int j = 2; j < 8; j++) was |= k.prev[j] == u;
        if (!was) emit_usage(u, true);
    }
    memcpy(k.prev, cur, 8);
}

static void queue_report(Keyboard& k) {
    int idx = k.intr.enq;
    for (int i = 0; i < 8; i++) k.reports[idx * 8 + i] = 0;
    uint64_t buf = phys(k.reports + idx * 8);
    ring_push(k.intr, (uint32_t)buf, (uint32_t)(buf >> 32), 8,
              TRB_NORMAL << 10 | (1 << 5) | (1 << 2));     // IOC, ISP
}

static void release_all(Keyboard& k) {
    if (k.mouse) { mouse_push(0, 0, 0, 0); return; }
    uint8_t empty[8] = {0};
    handle_report(k, empty);
}

// Events that arrive while we're waiting for something else.
static void dispatch(const Trb& e) {
    int type = (e.d3 >> 10) & 0x3F;
    if (type == TRB_PORT_STATUS) {
        int port = (e.d0 >> 24) & 0xFF;
        if (port >= 1 && port <= H->num_ports) H->port_dirty[port] = true;
        return;
    }
    if (type != TRB_TRANSFER_EVENT) return;
    int slot = e.d3 >> 24, ep = (e.d3 >> 16) & 0x1F, cc = e.d2 >> 24;
    for (auto& k : kbds) {
        if (!k.active || k.hc != H || k.slot != slot || k.dci != ep) continue;
        uint64_t trb = (uint64_t)e.d1 << 32 | e.d0;
        int idx = (int)((trb - phys(k.intr.trb)) / sizeof(Trb));
        if (idx >= 0 && idx < RING_TRBS && (cc == 1 || cc == 13)) {
            const volatile uint8_t* r = k.reports + idx * 8;
            if (k.kind == HUB) k.dirty |= (uint32_t)(r[0] | r[1] << 8 | r[2] << 16) & ~1u;   // bit 0: the hub itself
            else handle_report(k, r);
        }
        if (cc == 1 || cc == 13) queue_report(k);
        else {
            if (k.kind != HUB) release_all(k);
            k.active = false;
            printf("USB: %s on %s stopped (code %d)\n", k.kind == HUB ? "hub" : k.mouse ? "mouse" : "keyboard", k.where, cc);
        }
        H->db[slot] = k.dci;
    }
}

// Wait for an event of 'type' (and, for transfers, matching slot/endpoint).
static bool wait_event(int type, int slot, int ep, Trb* out, int ms = 500) {
    for (int t = 0; t < ms * 20; t++) {
        Trb e;
        while (next_event(&e)) {
            int et = (e.d3 >> 10) & 0x3F;
            bool match = et == type;
            if (match && type == TRB_TRANSFER_EVENT)
                match = (int)(e.d3 >> 24) == slot && (int)((e.d3 >> 16) & 0x1F) == ep;
            if (match) { *out = e; return true; }
            dispatch(e);
        }
        io_delay(50);
    }
    return false;
}

static int command(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3, Trb* ev) {
    ring_push(H->cmd_ring, d0, d1, d2, d3);
    H->db[0] = 0;
    Trb e;
    if (!wait_event(TRB_CMD_COMPLETION, 0, 0, &e)) return -1;
    if (ev) *ev = e;
    return e.d2 >> 24;                                  // completion code (1 = success)
}

// A control transfer on endpoint 0. Returns true on success.
static bool control(Keyboard& k, uint8_t type, uint8_t req, uint16_t value, uint16_t index,
                    uint16_t len, volatile void* buf) {
    bool in = type & 0x80;
    uint32_t trt = len ? (in ? 3 : 2) : 0;
    ring_push(k.ep0, type | req << 8 | (uint32_t)value << 16, index | (uint32_t)len << 16, 8,
              TRB_SETUP << 10 | (1 << 6) | trt << 16);
    if (len) {
        uint64_t p = phys(buf);
        ring_push(k.ep0, (uint32_t)p, (uint32_t)(p >> 32), len, TRB_DATA << 10 | (in ? 1 << 16 : 0));
    }
    ring_push(k.ep0, 0, 0, 0, TRB_STATUS << 10 | (1 << 5) | ((len && in) ? 0 : 1 << 16));
    H->db[k.slot] = 1;
    Trb e;
    if (!wait_event(TRB_TRANSFER_EVENT, k.slot, 1, &e)) return false;
    int cc = e.d2 >> 24;
    return cc == 1 || cc == 13;
}

// ---------------------------------------------------------------- enumeration
static bool reset_port(int port) {
    uint32_t sc = portsc(port);
    if (!(sc & PORT_CCS)) return false;
    if (!(sc & PORT_PED)) {                             // USB2 ports need a reset; USB3 enable themselves
        set_portsc(port, (sc & PORT_KEEP) | PORT_PR);
        for (int i = 0; i < 500 && !(portsc(port) & PORT_PRC); i++) delay_ms(1);
    }
    sc = portsc(port);
    set_portsc(port, (sc & PORT_KEEP) | (sc & PORT_CHANGES));   // ack change bits
    delay_ms(20);                                               // reset recovery
    return portsc(port) & PORT_PED;
}

static const char* speed_name(int s) { return s == 2 ? "low" : s == 1 ? "full" : s == 3 ? "high" : "super"; }

// The slot context's words 0-2: speed, route, root port, transaction
// translator and, for a hub, its port count. `entries` is the last
// endpoint context in use.
static void fill_slot(const Keyboard& k, int entries) {
    volatile uint32_t* sl = ctx(k.in_ctx, 1);
    sl[0] = (k.route & 0xFFFFF) | (uint32_t)k.speed << 20 | (uint32_t)entries << 27 |
            (k.kind == HUB ? 1u << 26 : 0);
    sl[1] = (uint32_t)k.port << 16 | (k.kind == HUB ? (uint32_t)k.nports << 24 : 0);
    sl[2] = (uint32_t)k.tt_slot | (uint32_t)k.tt_port << 8 | (k.kind == HUB ? (uint32_t)k.ttt << 16 : 0);
    sl[3] = 0;
}

// Interrupt IN endpoint `ep_addr` as the device's report/status ring.
static bool configure_intr(Keyboard& k, int ep_addr, int ep_mps, int ep_interval) {
    k.dci = (ep_addr & 0xF) * 2 + 1;
    k.mps = ep_mps;
    int interval;
    if (k.speed == 1 || k.speed == 2) {                 // bInterval in ms -> 2^n x 125 us
        interval = 3;
        while (interval < 10 && (1 << (interval + 1)) <= ep_interval * 8) interval++;
    } else {
        interval = ep_interval ? ep_interval - 1 : 0;
        if (interval > 15) interval = 15;
    }
    memset((void*)k.in_ctx, 0, 33 * H->csz);
    ctx(k.in_ctx, 0)[1] = 1 | 1u << k.dci;
    fill_slot(k, k.dci);
    volatile uint32_t* ep = ctx(k.in_ctx, 1 + k.dci);
    ep[0] = (uint32_t)interval << 16;
    ep[1] = 3 << 1 | 7 << 3 | (uint32_t)ep_mps << 16;  // CErr 3, interrupt IN
    uint64_t ri = phys(k.intr.trb) | 1;
    ep[2] = (uint32_t)ri; ep[3] = (uint32_t)(ri >> 32);
    ep[4] = 8 | (uint32_t)ep_mps << 16;
    uint64_t ic = phys(k.in_ctx);
    int cc = command((uint32_t)ic, (uint32_t)(ic >> 32), 0, TRB_CONFIGURE_EP << 10 | (uint32_t)k.slot << 24, nullptr);
    if (cc != 1) { printf("USB: %s: configure failed (%d)\n", k.where, cc); return false; }
    return true;
}

static void hub_port_change(int h, int port);
static void forget(int i);

// A device that's just been reset and enabled, wherever it is: give it a
// slot and an address, and set it up as a keyboard, mouse or hub.
static bool setup_slot(Keyboard& k, int ki);
static void release_slot(int slot) {
    H->dcbaa[slot] = 0;
    command(0, 0, 0, 10 << 10 | (uint32_t)slot << 24, nullptr);   // Disable Slot
}

static void setup_device(int root_port, int parent, int hub_port, int speed) {
    for (auto& k : kbds)                               // already there?
        if (k.active && k.hc == H && k.port == root_port && k.parent == parent && (parent < 0 || k.hub_port == hub_port)) return;
    int ki = -1;
    for (int i = 0; i < MAX_KBD; i++) if (!kbds[i].active) { ki = i; break; }
    if (ki < 0) { printf("USB: too many devices\n"); return; }
    Keyboard& k = kbds[ki];
    memset((void*)&k, 0, sizeof(k));
    k.hc = H;
    k.port = root_port;
    k.parent = parent;
    k.hub_port = hub_port;
    k.speed = speed;
    if (parent < 0) {
        snprintf(k.where, sizeof k.where, "port %d", root_port);
    } else {
        const Keyboard& h = kbds[parent];
        k.depth = h.depth + 1;
        k.route = h.route | (uint32_t)(hub_port > 15 ? 15 : hub_port) << (4 * h.depth);
        snprintf(k.where, sizeof k.where, "%s.%d", h.where, hub_port);
        if (speed == 1 || speed == 2) {                 // low/full speed: through a translator
            if (h.speed == 3) { k.tt_slot = h.slot; k.tt_port = hub_port; }
            else { k.tt_slot = h.tt_slot; k.tt_port = h.tt_port; }
        }
    }

    Trb ev;
    if (command(0, 0, 0, TRB_ENABLE_SLOT << 10, &ev) != 1) { printf("USB: %s: no slot\n", k.where); return; }
    k.slot = ev.d3 >> 24;
    if (k.slot < 1 || k.slot > H->max_slots) return;
    // Anything that doesn't end up driven gives its slot back: kept, a USB
    // stick plugged in a few dozen times used them all up.
    if (!setup_slot(k, ki)) release_slot(k.slot);
}


// The rest of setting up a device that has slot k.slot: its memory, an
// address, and a keyboard, mouse or hub on top. False: not driven.
static bool setup_slot(Keyboard& k, int ki) {
    auto& m = devmem[ki];
    if (!m.out_ctx) {                                   // sized for 64-byte contexts, whichever controller
        m.out_ctx = (volatile uint8_t*)dma_alloc(32 * 64, 64);
        m.in_ctx  = (volatile uint8_t*)dma_alloc(33 * 64, 64);
        m.reports = (volatile uint8_t*)dma_alloc(RING_TRBS * 8, 64);
        m.ep0     = (volatile Trb*)dma_alloc(RING_TRBS * sizeof(Trb), 64);
        m.intr    = (volatile Trb*)dma_alloc(RING_TRBS * sizeof(Trb), 64);
        if (!m.out_ctx || !m.in_ctx || !m.reports || !m.ep0 || !m.intr) {
            printf("USB: %s: out of memory\n", k.where);
            m.out_ctx = nullptr;
            return false;
        }
    }
    k.out_ctx = m.out_ctx; k.in_ctx = m.in_ctx; k.reports = m.reports;
    memset((void*)k.out_ctx, 0, 32 * 64);
    memset((void*)k.in_ctx, 0, 33 * 64);
    memset((void*)k.reports, 0, RING_TRBS * 8);
    ring_attach(k.ep0, m.ep0);
    ring_attach(k.intr, m.intr);
    H->dcbaa[k.slot] = phys(k.out_ctx);

    // Address Device: slot context + endpoint 0.
    int mps0 = k.speed == 2 || k.speed == 1 ? 8 : k.speed == 3 ? 64 : 512;
    ctx(k.in_ctx, 0)[1] = 0x3;                          // add slot + EP0
    fill_slot(k, 1);
    volatile uint32_t* ep0 = ctx(k.in_ctx, 2);
    ep0[1] = 3 << 1 | 4 << 3 | (uint32_t)mps0 << 16;    // CErr 3, control, max packet
    uint64_t r0 = phys(k.ep0.trb) | 1;
    ep0[2] = (uint32_t)r0; ep0[3] = (uint32_t)(r0 >> 32);
    ep0[4] = 8;
    uint64_t ic = phys(k.in_ctx);
    int cc = command((uint32_t)ic, (uint32_t)(ic >> 32), 0, TRB_ADDRESS_DEVICE << 10 | (uint32_t)k.slot << 24, nullptr);
    if (cc != 1) { printf("USB: %s: address failed (%d)\n", k.where, cc); return false; }
    delay_ms(2);

    static volatile uint8_t desc[256] __attribute__((aligned(64)));
    if (!control(k, 0x80, 6, 0x0100, 0, 8, desc)) { printf("USB: %s: no descriptor\n", k.where); return false; }
    int mps = desc[7];
    bool hub = desc[4] == 9;                            // device class: hub
    if (mps && mps != mps0 && k.speed < 4) {            // fix EP0 max packet size
        memset((void*)ctx(k.in_ctx, 0), 0, H->csz);
        ctx(k.in_ctx, 0)[1] = 0x2;
        ep0[1] = (ep0[1] & 0xFFFF) | (uint32_t)mps << 16;
        command((uint32_t)ic, (uint32_t)(ic >> 32), 0, TRB_EVALUATE_CTX << 10 | (uint32_t)k.slot << 24, nullptr);
    }
    if (!control(k, 0x80, 6, 0x0200, 0, 9, desc)) return false;
    int total = desc[2] | desc[3] << 8;
    if (total > (int)sizeof(desc)) total = sizeof(desc);
    if (!control(k, 0x80, 6, 0x0200, 0, total, desc)) return false;
    int config = desc[5];

    // A boot keyboard or mouse interface, or a hub's, and its interrupt IN endpoint.
    int iface = -1, ep_addr = 0, ep_mps = 8, ep_interval = 10;
    bool in_iface = false, is_mouse = false;
    for (int i = 0; i + 1 < total && desc[i] >= 2; i += desc[i]) {
        uint8_t type = desc[i + 1];
        if (type == 4) {
            if (hub) in_iface = iface < 0 && desc[i + 5] == 9;
            // HID boot interface: protocol 1 = keyboard, 2 = mouse.
            else in_iface = iface < 0 && desc[i + 5] == 3 && desc[i + 6] == 1 && (desc[i + 7] == 1 || desc[i + 7] == 2);
            if (in_iface) { iface = desc[i + 2]; is_mouse = !hub && desc[i + 7] == 2; }
        } else if (type == 5 && in_iface && !ep_addr && (desc[i + 2] & 0x80) && (desc[i + 3] & 3) == 3) {
            ep_addr = desc[i + 2];
            ep_mps = (desc[i + 4] | desc[i + 5] << 8) & 0x7FF;
            ep_interval = desc[i + 6];
        }
    }
    if (iface < 0 || !ep_addr) { printf("USB: %s: not a keyboard, mouse or hub\n", k.where); return false; }
    if (hub && k.speed >= 4) {                          // a USB 3 hub's SuperSpeed half
        printf("USB: %s: USB 3 hub (its USB 2 side carries keyboards and mice)\n", k.where);
        return false;
    }
    if (hub && k.depth >= 5) { printf("USB: %s: hubs nested too deep\n", k.where); return false; }

    if (!control(k, 0x00, 9, config, 0, 0, nullptr)) return false;          // SET_CONFIGURATION

    if (hub) {
        // Hub descriptor: port count, characteristics (TT think time), power-on delay.
        if (!control(k, 0xA0, 6, 0x2900, 0, 9, desc)) { printf("USB: %s: no hub descriptor\n", k.where); return false; }
        k.kind = HUB;
        k.nports = desc[2] > 31 ? 31 : desc[2];
        k.ttt = (desc[3] >> 5) & 3;
        k.power_good_ms = desc[5] * 2u;
        if (!configure_intr(k, ep_addr, ep_mps, ep_interval)) return false;
        k.active = true;
        for (int i = 0; i < 8; i++) queue_report(k);
        H->db[k.slot] = k.dci;
        printf("USB: hub on %s (%d ports, %s speed)\n", k.where, k.nports, speed_name(k.speed));
        for (int p = 1; p <= k.nports; p++) control(k, 0x23, 3, 8, p, 0, nullptr);   // SET_FEATURE PORT_POWER
        delay_ms((int)k.power_good_ms + 100);           // power good, then connect debounce
        for (int p = 1; p <= k.nports; p++) hub_port_change(ki, p);
        return true;
    }

    control(k, 0x21, 0x0B, 0, iface, 0, nullptr);                      // SET_PROTOCOL boot
    control(k, 0x21, 0x0A, 0, iface, 0, nullptr);                      // SET_IDLE (may stall)
    if (!configure_intr(k, ep_addr, ep_mps, ep_interval)) return false;

    k.kind = is_mouse ? MOUSE : KBD;
    k.mouse = is_mouse;
    k.active = true;
    for (int i = 0; i < 8; i++) queue_report(k);
    H->db[k.slot] = k.dci;
    printf("USB: %s on %s (slot %d, %s speed)\n", k.mouse ? "mouse" : "keyboard", k.where, k.slot, speed_name(k.speed));
    return true;
}

// Drop device i and everything plugged into it (if it's a hub).
static void forget(int i) {
    Keyboard& k = kbds[i];
    if (!k.active) return;
    H = k.hc;
    for (int j = 0; j < MAX_KBD; j++)
        if (kbds[j].active && kbds[j].parent == i) forget(j);
    if (k.kind != HUB) release_all(k);
    k.active = false;
    command(0, 0, 0, 10 << 10 | (uint32_t)k.slot << 24, nullptr);   // disable slot
    printf("USB: %s on %s unplugged\n", k.kind == HUB ? "hub" : k.mouse ? "mouse" : "keyboard", k.where);
}

// Port `port` of hub h: something plugged in or out (or the first look).
static void hub_port_change(int h, int port) {
    Keyboard& hb = kbds[h];
    H = hb.hc;
    static volatile uint8_t st[4] __attribute__((aligned(64)));
    if (!hb.active || !control(hb, 0xA3, 0, 0, port, 4, st)) return;            // GET_STATUS
    uint16_t status = st[0] | st[1] << 8, change = st[2] | st[3] << 8;
    static const uint8_t clear[] = {16, 17, 18, 19, 20};                        // C_PORT_* features
    for (int b = 0; b < 5; b++)
        if (change & (1 << (b == 4 ? 4 : b))) control(hb, 0x23, 1, clear[b], port, 0, nullptr);
    int existing = -1;
    for (int j = 0; j < MAX_KBD; j++)
        if (kbds[j].active && kbds[j].parent == h && kbds[j].hub_port == port) existing = j;
    bool connected = status & 1;
    if (existing >= 0 && (!connected || (change & 1))) { forget(existing); existing = -1; }
    if (!connected || existing >= 0) return;

    // Reset the port, wait for it to finish, and see how fast the device is.
    if (!control(hb, 0x23, 3, 4, port, 0, nullptr)) return;                   // SET_FEATURE PORT_RESET
    bool done = false;
    for (int i = 0; i < 50 && !done; i++) {
        delay_ms(10);
        if (!control(hb, 0xA3, 0, 0, port, 4, st)) return;
        done = (st[2] | st[3] << 8) & (1 << 4);                                // C_PORT_RESET
    }
    control(hb, 0x23, 1, 20, port, 0, nullptr);                                 // clear C_PORT_RESET
    status = st[0] | st[1] << 8;
    if (!done || !(status & 2)) { printf("USB: %s.%d: port didn't enable\n", hb.where, port); return; }
    delay_ms(10);                                                               // reset recovery
    int speed = (status & (1 << 9)) ? 2 : (status & (1 << 10)) ? 3 : 1;         // low / high / full
    setup_device(hb.port, h, port, speed);
}

static void setup_port(int port) {
    for (auto& k : kbds) if (k.active && k.hc == H && k.port == port && k.parent < 0) return;
    if (!reset_port(port)) return;
    setup_device(port, -1, 0, (portsc(port) >> 10) & 0xF);    // 1 FS, 2 LS, 3 HS, 4+ SS
}

// ---------------------------------------------------------------- init
static bool bios_handoff() {
    uint32_t xecp = (rd(H->cap, 0x10) >> 16) * 4;
    for (int guard = 0; xecp && guard < 64; guard++) {
        uint32_t v = rd(H->cap, xecp);
        if ((v & 0xFF) == 1) {                          // USB legacy support
            wr(H->cap, xecp, v | (1u << 24));              // OS owned
            for (int i = 0; i < 1000 && (rd(H->cap, xecp) & (1u << 16)); i++) delay_ms(1);
            if (rd(H->cap, xecp) & (1u << 16)) { printf("USB: BIOS won't release the controller\n"); return false; }
            wr(H->cap, xecp + 4, 0xE0000000);              // disable SMIs, clear their status
            return true;
        }
        uint32_t next = (v >> 8) & 0xFF;
        if (!next) break;
        xecp += next * 4;
    }
    return true;
}

static bool init_controller(const PciDevice& d) {
    uint32_t bar = pci_read(d, 0x10);
    uint64_t base = bar & 0xFFFFFFF0;
    if ((bar & 0x6) == 0x4) base |= (uint64_t)pci_read(d, 0x14) << 32;
    if (!base || base >= phys_limit) { printf("USB: xHCI registers above 4 GiB\n"); return false; }
    pci_write(d, 0x04, pci_read(d, 0x04) | 0x06);      // memory + bus master

    H->cap = (volatile uint8_t*)(uintptr_t)base;
    H->op = H->cap + (rd(H->cap, 0x00) & 0xFF);
    H->rt = H->cap + (rd(H->cap, 0x18) & ~0x1Fu);
    H->db = (volatile uint32_t*)(H->cap + (rd(H->cap, 0x14) & ~0x3u));
    uint32_t hcs1 = rd(H->cap, 0x04), hcs2 = rd(H->cap, 0x08), hcc1 = rd(H->cap, 0x10);
    H->max_slots = hcs1 & 0xFF;
    H->num_ports = hcs1 >> 24;
    H->csz = (hcc1 & (1 << 2)) ? 64 : 32;
    if (H->max_slots > 32) H->max_slots = 32;

    if (!bios_handoff()) return false;

    // Halt and reset.
    wr(H->op, 0x00, rd(H->op, 0x00) & ~1u);
    for (int i = 0; i < 100 && !(rd(H->op, 0x04) & 1); i++) delay_ms(1);
    wr(H->op, 0x00, 1 << 1);
    for (int i = 0; i < 1000 && (rd(H->op, 0x00) & (1 << 1)); i++) delay_ms(1);
    for (int i = 0; i < 1000 && (rd(H->op, 0x04) & (1 << 11)); i++) delay_ms(1);
    if (rd(H->op, 0x00) & (1 << 1)) { printf("USB: controller reset timed out\n"); return false; }

    // Device context array, with scratchpad buffers if the controller wants them.
    H->dcbaa = (volatile uint64_t*)dma_alloc((H->max_slots + 1) * 8, 64);
    int scratch = ((hcs2 >> 27) & 0x1F) | (((hcs2 >> 21) & 0x1F) << 5);
    if (scratch) {
        volatile uint64_t* arr = (volatile uint64_t*)dma_alloc(scratch * 8, 64);
        for (int i = 0; i < scratch; i++) {
            void* page = dma_alloc(4096, 4096);
            if (!arr || !page) { printf("USB: out of memory for scratchpad\n"); return false; }
            arr[i] = phys(page);
        }
        H->dcbaa[0] = phys(arr);
    }
    if (!H->dcbaa || !ring_init(H->cmd_ring)) return false;

    // One event ring segment.
    H->evt = (volatile Trb*)dma_alloc(EVT_TRBS * sizeof(Trb), 64);
    volatile uint64_t* erst = (volatile uint64_t*)dma_alloc(16, 64);
    if (!H->evt || !erst) return false;
    erst[0] = phys(H->evt);
    erst[1] = EVT_TRBS;

    wr(H->op, 0x38, H->max_slots);                            // CONFIG: slots enabled
    wr64(H->op, 0x30, phys(H->dcbaa));
    wr64(H->op, 0x18, phys(H->cmd_ring.trb) | 1);             // CRCR, cycle 1
    wr(H->rt, 0x28, 1);                                    // ERSTSZ
    wr64(H->rt, 0x38, phys(H->evt));                          // ERDP
    wr64(H->rt, 0x30, phys(erst));                         // ERSTBA
    wr(H->rt, 0x20, 0);                                    // IMAN: no interrupts, we poll
    wr(H->op, 0x00, 1);                                    // run
    for (int i = 0; i < 100 && (rd(H->op, 0x04) & 1); i++) delay_ms(1);

    for (int p = 1; p <= H->num_ports; p++) {              // make sure ports are powered
        uint32_t sc = portsc(p);
        if (!(sc & PORT_PP)) set_portsc(p, (sc & PORT_KEEP) | PORT_PP);
    }
    delay_ms(100);                                      // let devices connect
    return true;
}

bool usb_init(const char* cmdline) {
    for (const char* p = cmdline; p && *p; p++)
        if (strncmp(p, "usb=off", 7) == 0) { printf("USB: disabled\n"); return false; }
    // Every xHCI controller, not just the first: keyboards and mice may be
    // on an add-in card, or on the second of a board's two controllers.
    PciDevice d;
    for (int i = 0; num_hc < MAX_HC && pci_find_class(0x0C, 0x03, 0x30, &d, i); i++) {
        H = &hcs[num_hc];
        memset((void*)H, 0, sizeof *H);
        H->evt_cycle = 1;
        printf("USB: xHCI %d at PCI %02x:%02x.%x\n", num_hc, d.bus, d.dev, d.fn);
        if (!init_controller(d)) continue;              // try the others
        num_hc++;
        for (int p = 1; p <= H->num_ports; p++)
            if (portsc(p) & PORT_CCS) setup_port(p);
        int n = 0;
        for (auto& k : kbds) n += k.active && k.hc == H && k.kind != HUB;
        printf("USB: xHCI %d with %d ports, %d keyboard/mouse device%s\n", num_hc - 1, H->num_ports, n, n == 1 ? "" : "s");
    }
    if (!num_hc) { printf("USB: no xHCI controller\n"); return false; }
    return true;
}

void usb_poll() {
    for (int c = 0; c < num_hc; c++) {
        H = &hcs[c];
        Trb e;
        for (int i = 0; i < 64 && next_event(&e); i++) dispatch(e);
        for (int p = 1; p <= H->num_ports; p++) {
            if (!H->port_dirty[p]) continue;
            H->port_dirty[p] = false;
            uint32_t sc = portsc(p);
            set_portsc(p, (sc & PORT_KEEP) | (sc & PORT_CHANGES));   // ack
            if (sc & PORT_CCS) {
                if (sc & PORT_CSC || !(sc & PORT_PED)) setup_port(p);
            } else {
                for (int i = 0; i < MAX_KBD; i++)
                    if (kbds[i].active && kbds[i].hc == H && kbds[i].port == p && kbds[i].parent < 0) forget(i);
            }
        }
    }
    for (int h = 0; h < MAX_KBD; h++) {                 // hubs' ports (hub_port_change picks the controller)
        while (kbds[h].active && kbds[h].kind == HUB && kbds[h].dirty) {
            uint32_t d = kbds[h].dirty;
            kbds[h].dirty = 0;
            for (int p = 1; p <= kbds[h].nports; p++)
                if (d & (1u << p)) hub_port_change(h, p);
        }
    }
}
