// Bare-metal Doom: boots straight into the game. Doom itself (../doomgeneric)
// is compiled unchanged; doomgeneric_baremetal.c connects it to this file,
// which is the machine: boot information, memory, the framebuffer, the
// keyboard, mouse, timer, sound and restart. libc.cpp + storage.cpp put the
// WAD, the config and saved games on a FAT32 RAM disk (doom.img, handed
// over by the boot loader).
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "hw.hpp"
#include "audio.hpp"
#include "pci.hpp"
#include "usb.hpp"
#include "storage.hpp"
#include "fat32.hpp"
#include "mixer.hpp"
#include "music.h"
#include "font.h"
#include "bm.h"
#include "bm_audio.h"
#include "doomkeys.h"

// ---------------------------------------------------------------- serial
#define COM1 0x3F8
extern "C" void serial_putc(char c) {
    if (c == '\n') serial_putc('\r');
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++) {}
    outb(COM1, (uint8_t)c);
}
extern "C" void serial_puts(const char* s) { while (*s) serial_putc(*s++); }
static void serial_init() {
    outb(COM1 + 1, 0x00); outb(COM1 + 3, 0x80); outb(COM1 + 0, 0x01);
    outb(COM1 + 1, 0x00); outb(COM1 + 3, 0x03); outb(COM1 + 2, 0xC7);
}

// ---------------------------------------------------------------- multiboot
struct __attribute__((packed)) MultibootInfo {
    uint32_t flags, mem_lower, mem_upper, boot_device, cmdline;
    uint32_t mods_count, mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length, mmap_addr, drives_length, drives_addr;
    uint32_t config_table, boot_loader_name, apm_table;
    uint32_t vbe_control_info, vbe_mode_info;
    uint16_t vbe_mode, vbe_interface_seg, vbe_interface_off, vbe_interface_len;
    uint64_t fb_addr;
    uint32_t fb_pitch, fb_width, fb_height;
    uint8_t fb_bpp, fb_type;
    uint8_t fb_pad[2];      // GRUB aligns what follows to 4 bytes
    uint8_t fb_color[6];    // type 1 (RGB): red, green, blue (position, size) pairs
};
struct __attribute__((packed)) MultibootMmap { uint32_t size; uint64_t addr, len; uint32_t type; };
struct __attribute__((packed)) MultibootModule { uint32_t start, end, string, reserved; };
extern "C" uint32_t mb_magic, mb_info;
extern "C" char __kernel_start[], __kernel_end[];
extern "C" void heap_add(void* p, size_t n);
extern "C" size_t heap_free_bytes(void), heap_largest_free(void);
extern "C" uint64_t phys_limit;
uint64_t phys_limit = 0x100000000ull;

// The modules (the disk image), copied before the heap can reuse anything.
#define MAX_MODS 8
static struct { uint64_t start, end; char name[64]; } mods[MAX_MODS];
static int nmods;

// ---------------------------------------------------------------- memory
// Give the heap every usable RAM region the boot loader reports, minus the
// kernel image, the modules and anything below 1 MiB.
static void heap_init(const MultibootInfo* mbi) {
    struct Hole { uint64_t a, e; } holes[MAX_MODS + 1];
    int nh = 0;
    holes[nh++] = { (uintptr_t)__kernel_start & ~0xFFFull, ((uintptr_t)__kernel_end + 0xFFF) & ~0xFFFull };
    for (int i = 0; i < nmods; i++) holes[nh++] = { mods[i].start & ~0xFFFull, (mods[i].end + 0xFFF) & ~0xFFFull };
    uint64_t total = 0;
    // Add [a, e) minus every hole, recursively splitting around them.
    struct Adder {
        Hole* h; int n; uint64_t* total;
        void add(uint64_t a, uint64_t e, int from) {
            if (e > phys_limit) e = phys_limit;
            if (e > (uint64_t)(uintptr_t)-1) e = (uint64_t)(uintptr_t)-1;   // i386: 32-bit pointers
            if (a < 0x100000) a = 0x100000;
            if (e <= a) return;
            for (int i = from; i < n; i++) {
                if (a < h[i].e && e > h[i].a) {
                    add(a, h[i].a < a ? a : h[i].a, i + 1);
                    add(h[i].e > e ? e : h[i].e, e, i + 1);
                    return;
                }
            }
            heap_add((void*)(uintptr_t)a, (size_t)(e - a));
            *total += e - a;
        }
    } adder{holes, nh, &total};
    if (mbi->flags & (1 << 6)) {
        uintptr_t p = mbi->mmap_addr, end = p + mbi->mmap_length;
        // Copy the map first: the heap may be handed the memory it sits in.
        static MultibootMmap map[128];
        int n = 0;
        while (p < end && n < 128) {
            const MultibootMmap* m = (const MultibootMmap*)p;
            map[n++] = *m;
            p += m->size + 4;
        }
        for (int i = 0; i < n; i++)
            if (map[i].type == 1) adder.add(map[i].addr, map[i].addr + map[i].len, 0);
    } else if (mbi->flags & 1) {
        adder.add(0x100000, 0x100000 + (uint64_t)mbi->mem_upper * 1024, 0);
    }
    printf("Heap: %lu MB of RAM\n", (unsigned long)(total >> 20));
}

// ---------------------------------------------------------------- video
// The mode to set when no boot loader set one (QEMU -kernel, via Bochs VBE).
#ifndef FB_W
#define FB_W 640
#define FB_H 480
#define FB_BPP 32
#endif

static uint8_t* fb;
static uint32_t fb_w, fb_h, fb_pitch;   // pitch in bytes
static int fb_bytes = 4;                // per pixel
static bool fb_indexed;                 // 8 bits: Doom's palette goes straight into the VGA DAC
static uint8_t r_pos = 16, r_len = 8, g_pos = 8, g_len = 8, b_pos = 0, b_len = 8;
static bool video_ready;

static void dac_set(int i, uint32_t c) {
    outb(0x3C8, (uint8_t)i);
    outb(0x3C9, (c >> 18) & 63); outb(0x3C9, (c >> 10) & 63); outb(0x3C9, (c >> 2) & 63);
}
static uint32_t native(uint32_t c) {
    uint32_t r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    return (r >> (8 - r_len)) << r_pos | (g >> (8 - g_len)) << g_pos | (b >> (8 - b_len)) << b_pos;
}
// Video memory can sit on a slow bus: move dwords.
static void copy_out(void* d, const void* s, size_t n) {
    size_t words = n / 4, rest = n % 4;
    __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(words) :: "memory");
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(rest) :: "memory");
}

static bool bga_init(uint32_t w, uint32_t h, uint32_t bpp) {
    uint32_t base = 0;
    PciDevice vga;
    if (pci_find_id(0x1234, 0x1111, &vga))                  // QEMU/Bochs std VGA
        base = pci_read(vga, 0x10) & 0xFFFFFFF0;
    outw(0x1CE, 0); if (!base || inw(0x1CF) < 0xB0C0) return false;
    auto w16 = [](uint16_t i, uint16_t v) { outw(0x1CE, i); outw(0x1CF, v); };
    w16(4, 0); w16(1, w); w16(2, h); w16(3, bpp); w16(4, 0x41);
    fb = (uint8_t*)(uintptr_t)base;
    fb_bytes = (bpp + 7) / 8;
    fb_w = w; fb_h = h; fb_pitch = w * fb_bytes;
    fb_indexed = bpp == 8;
    if (bpp == 16) { r_pos = 11; r_len = 5; g_pos = 5; g_len = 6; b_pos = 0; b_len = 5; }
    if (bpp == 15) { r_pos = 10; r_len = 5; g_pos = 5; g_len = 5; b_pos = 0; b_len = 5; }
    return true;
}

static bool video_init(const MultibootInfo* mbi) {
    bool ok = false;
    if ((mbi->flags & (1 << 12)) && mbi->fb_addr < phys_limit) {
        int bpp = mbi->fb_bpp;
        if (mbi->fb_type == 0 && bpp == 8) {
            fb_indexed = ok = true;
        } else if (mbi->fb_type == 1 && (bpp == 15 || bpp == 16 || bpp == 24 || bpp == 32)) {
            const uint8_t* c = mbi->fb_color;
            r_pos = c[0]; r_len = c[1]; g_pos = c[2]; g_len = c[3]; b_pos = c[4]; b_len = c[5];
            ok = r_len && r_len <= 8 && g_len && g_len <= 8 && b_len && b_len <= 8;
        }
        if (ok) {
            fb = (uint8_t*)(uintptr_t)mbi->fb_addr;
            fb_w = mbi->fb_width; fb_h = mbi->fb_height; fb_pitch = mbi->fb_pitch;
            fb_bytes = (bpp + 7) / 8;
            printf("Using bootloader framebuffer %ux%u, %d bits\n", fb_w, fb_h, bpp);
        }
    }
    if (!ok && bga_init(FB_W, FB_H, FB_BPP)) {
        printf("Using Bochs/QEMU VBE %ux%u, %d bits\n", FB_W, FB_H, FB_BPP);
        ok = true;
    }
    return ok;
}

static void fb_put(uint8_t* p, uint32_t v) {
    switch (fb_bytes) {
    case 1: *p = (uint8_t)v; break;
    case 2: *(uint16_t*)p = (uint16_t)v; break;
    case 3: p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); break;
    default: *(uint32_t*)p = v; break;
    }
}
static void fb_clear() {
    for (uint32_t y = 0; y < fb_h; y++) memset(fb + y * fb_pitch, 0, (size_t)fb_w * fb_bytes);
}

// Doom's picture (320x200, or wider or larger: bm_draw), scaled up to the
// largest rectangle of its shape that fits -- 4:3 or 16:9, its pixels
// being 1.2 times taller than wide, as on the monitors Doom was drawn
// for -- centred, black around it.
static uint32_t pal_native[256];
static uint16_t* xmap;                  // screen column -> Doom column
static uint8_t* line;                   // one screen row, in the screen's format
static uint32_t dst_x, dst_y, dst_w, dst_h;
static int src_w, src_h;                // the frame the mapping is for
static bool scaler_ready;

static void scaler_init() {
    xmap = (uint16_t*)malloc(fb_w * sizeof(uint16_t));
    line = (uint8_t*)malloc((size_t)fb_w * 4);
    if (!xmap || !line) return;
    scaler_ready = true;
}

static void scaler_setup(int w, int h) {
    uint64_t aw = (uint64_t)w * 5, ah = (uint64_t)h * 6;    // w : 1.2h
    dst_w = fb_w; dst_h = (uint32_t)(fb_w * ah / aw);
    if (dst_h > fb_h) { dst_h = fb_h; dst_w = (uint32_t)(fb_h * aw / ah); }
    if (dst_w > fb_w) dst_w = fb_w;
    dst_x = (fb_w - dst_w) / 2; dst_y = (fb_h - dst_h) / 2;
    for (uint32_t x = 0; x < dst_w; x++) xmap[x] = (uint16_t)(x * (uint32_t)w / dst_w);
    src_w = w; src_h = h;
    fb_clear();
    printf("Video: Doom's %dx%d scaled to %ux%u at (%u,%u)\n", w, h, dst_w, dst_h, dst_x, dst_y);
}

extern "C" void bm_set_palette(const uint32_t* rgb) {
    for (int i = 0; i < 256; i++) {
        if (fb_indexed) { dac_set(i, rgb[i]); pal_native[i] = (uint32_t)i; }
        else pal_native[i] = native(rgb[i]);
    }
}

extern "C" void bm_draw(const uint8_t* src, int w, int h) {
    if (!scaler_ready || w <= 0 || h <= 0) return;
    if (w != src_w || h != src_h) scaler_setup(w, h);
    int last = -1;
    for (uint32_t y = 0; y < dst_h; y++) {
        int sy = (int)(y * (uint32_t)h / dst_h);
        if (sy != last) {                                  // a new source row: convert it
            last = sy;
            const uint8_t* s = src + sy * w;
            switch (fb_bytes) {
            case 1: for (uint32_t x = 0; x < dst_w; x++) line[x] = s[xmap[x]]; break;
            case 2: for (uint32_t x = 0; x < dst_w; x++) ((uint16_t*)line)[x] = (uint16_t)pal_native[s[xmap[x]]]; break;
            case 4: for (uint32_t x = 0; x < dst_w; x++) ((uint32_t*)line)[x] = pal_native[s[xmap[x]]]; break;
            default: for (uint32_t x = 0; x < dst_w; x++) fb_put(line + x * 3, pal_native[s[xmap[x]]]); break;
            }
        }
        copy_out(fb + (dst_y + y) * fb_pitch + dst_x * fb_bytes, line, (size_t)dst_w * fb_bytes);
    }
}

// Lines of text on a black screen (errors, the quit message).
static void text_screen(const char* const* lines, const uint32_t* colours, int n) {
    if (!fb) return;
    uint32_t col[8];
    for (int l = 0; l < n && l < 8; l++) {
        if (fb_indexed) { dac_set(l + 1, colours[l]); col[l] = (uint32_t)(l + 1); }
        else col[l] = native(colours[l]);
    }
    if (fb_indexed) dac_set(0, 0);
    fb_clear();
    int sc = fb_w >= 1280 ? 2 : 1;
    uint32_t maxc = fb_w / (8 * sc) - 2;
    uint32_t y = 16 * sc;
    for (int l = 0; l < n && l < 8; l++) {
        const char* t = lines[l];
        // Wrap long lines (and break at newlines).
        while (*t && y + 16 * sc <= fb_h) {
            uint32_t len = 0;
            while (t[len] && t[len] != '\n' && len < maxc) len++;
            for (uint32_t c = 0; c < len; c++) {
                char ch = t[c] < 32 || t[c] > 126 ? '?' : t[c];
                const unsigned char* g = font8x16[ch - 32];
                for (int r = 0; r < 16 * sc; r++)
                    for (int b = 0; b < 8 * sc; b++)
                        if (g[r / sc] & (0x80 >> (b / sc)))
                            fb_put(fb + (y + r) * fb_pitch + (8 * sc + c * 8 * sc + b) * fb_bytes, col[l]);
            }
            t += len;
            if (*t == '\n') t++;
            y += 18 * sc;
        }
        y += 8 * sc;
    }
}

// ---------------------------------------------------------------- interrupts
#ifdef __x86_64__
struct __attribute__((packed)) IdtEntry {
    uint16_t off_lo, sel; uint8_t ist, type; uint16_t off_mid; uint32_t off_hi, zero;
};
#else
struct __attribute__((packed)) IdtEntry {         // 32-bit interrupt gate
    uint16_t off_lo, sel; uint8_t zero, type; uint16_t off_hi;
};
#endif
static IdtEntry idt[256];
extern "C" void isr_timer(), isr_keyboard(), isr_mouse(), isr_spurious(), isr_fault();

static void set_gate(int n, void (*h)()) {
    uintptr_t a = (uintptr_t)h;
#ifdef __x86_64__
    idt[n] = { (uint16_t)a, 0x08, 0, 0x8E, (uint16_t)(a >> 16), (uint32_t)(a >> 32), 0 };
#else
    idt[n] = { (uint16_t)a, 0x08, 0, 0x8E, (uint16_t)(a >> 16) };
#endif
}

extern volatile uint32_t ticks, fine_ticks;
extern volatile uint8_t kbd_buf[256];
extern volatile uint8_t kbd_head, kbd_tail;
#define TICK_HZ 60
#define PIT_HZ  1193182u
#define PIT_DIV (PIT_HZ / (TICK_HZ * 4))

static void interrupts_init() {
    for (int i = 0; i < 32; i++) set_gate(i, isr_fault);
    for (int i = 32; i < 256; i++) set_gate(i, isr_spurious);
    set_gate(32, isr_timer);
    set_gate(33, isr_keyboard);
    set_gate(44, isr_mouse);
    struct __attribute__((packed)) { uint16_t lim; uintptr_t base; } idtr = { sizeof(idt) - 1, (uintptr_t)idt };
    __asm__ volatile("lidt %0" ::"m"(idtr));

    // Remap the PICs to vectors 32..47; unmask the timer, keyboard, the
    // cascade to the second PIC, and the PS/2 mouse (IRQ 12) on it.
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 32);   outb(0xA1, 40);
    outb(0x21, 4);    outb(0xA1, 2);
    outb(0x21, 1);    outb(0xA1, 1);
    outb(0x21, 0xF8); outb(0xA1, 0xEF);

    // Mode 2 (rate generator) at 240 Hz (irq.cpp).
    outb(0x43, 0x34); outb(0x40, PIT_DIV & 0xFF); outb(0x40, PIT_DIV >> 8);

    for (int i = 0; i < 64 && (inb(0x64) & 1); i++) inb(0x60);
    __asm__ volatile("sti");
}

[[noreturn]] void platform_reboot() {
    printf("Rebooting\n");
    __asm__ volatile("cli");
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) {}
    outb(0x64, 0xFE);                                  // i8042 pulse reset line
    outb(0xCF9, 0x02); outb(0xCF9, 0x06);              // PCI reset control
    struct __attribute__((packed)) { uint16_t lim; uintptr_t base; } none = { 0, 0 };
    __asm__ volatile("lidt %0; int3" ::"m"(none));     // triple fault
    for (;;) __asm__ volatile("hlt");
}

// ---------------------------------------------------------------- time
// The PIT runs at 240 Hz (irq.cpp).
uint32_t platform_ms() { return (uint32_t)((uint64_t)fine_ticks * 1000 / (TICK_HZ * 4)); }

// Microseconds since boot: the ticks so far plus how far the PIT has
// counted into the current one.
uint64_t platform_us() {
    static uint64_t last;
    uintptr_t fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    uint32_t t = fine_ticks;
    outb(0x43, 0x00);                                  // latch channel 0's count
    uint8_t lo = inb(0x40), hi = inb(0x40);
    outb(0x20, 0x0A);                                  // read the PIC's request register:
    bool pending = inb(0x20) & 1;                      // a tick not yet counted?
    __asm__ volatile("push %0; popf" :: "r"(fl) : "memory", "cc");
    uint32_t cnt = (uint32_t)lo | ((uint32_t)hi << 8);
    uint32_t into = cnt <= PIT_DIV ? PIT_DIV - cnt : 0;
    if (pending && into < PIT_DIV / 2) t++;            // the count wrapped before we read it
    uint64_t us = (uint64_t)t * 1000000u / (TICK_HZ * 4) + (uint64_t)into * 1000000u / PIT_HZ;
    if (us < last) us = last;                          // never run backwards
    last = us;
    return us;
}

// ---------------------------------------------------------------- keyboard
// PS/2 (IRQ 1, irq.cpp) and USB keyboards queue set-1 scancodes; this turns
// them into Doom's key codes (doomkeys.h), presses and releases.
void kbd_push(uint8_t b) {                              // usb.cpp
    uintptr_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    kbd_buf[kbd_head] = b;
    kbd_head = kbd_head + 1;
    __asm__ volatile("push %0; popf" :: "r"(flags) : "memory", "cc");
}

static uint16_t keyq[256];                              // pressed << 8 | key
static uint8_t keyq_head, keyq_tail;
static void key_post(bool down, int k) {
    if (!k || (uint8_t)(keyq_head + 1) == keyq_tail) return;
    keyq[keyq_head++] = (uint16_t)((down ? 0x100 : 0) | (k & 0xFF));
}

static bool ctrl_down, alt_down;
static int doom_key(bool ext, uint8_t code) {
    if (ext) {
        switch (code) {
        case 0x1C: return KEY_ENTER;                       // keypad Enter
        case 0x1D: return KEY_RCTRL;                       // right Ctrl: fire
        case 0x35: return '/';
        case 0x38: return KEY_RALT;                        // right Alt: strafe
        case 0x47: return KEY_HOME;
        case 0x48: return KEY_UPARROW;
        case 0x49: return KEY_PGUP;
        case 0x4B: return KEY_LEFTARROW;
        case 0x4D: return KEY_RIGHTARROW;
        case 0x4F: return KEY_END;
        case 0x50: return KEY_DOWNARROW;
        case 0x51: return KEY_PGDN;
        case 0x52: return KEY_INS;
        case 0x53: return KEY_DEL;
        case 0x46: return KEY_PAUSE;                       // Ctrl+Break
        }
        return 0;
    }
    switch (code) {
    case 0x01: return KEY_ESCAPE;
    case 0x0E: return KEY_BACKSPACE;
    case 0x0F: return KEY_TAB;
    case 0x1C: return KEY_ENTER;
    case 0x1D: return KEY_RCTRL;                           // Ctrl: fire
    case 0x2A: case 0x36: return KEY_RSHIFT;               // Shift: run
    case 0x38: return KEY_LALT;                            // Alt: strafe
    case 0x39: return ' ';                                 // Space: use
    case 0x3A: return KEY_CAPSLOCK;
    case 0x45: return KEY_NUMLOCK;
    case 0x46: return KEY_SCRLCK;
    case 0x57: return KEY_F11;
    case 0x58: return KEY_F12;
    case 0x37: return '*';
    case 0x4A: return '-';
    case 0x4E: return '+';
    case 0x47: return KEY_HOME;                            // keypad, as Doom's KEYP_*
    case 0x48: return KEY_UPARROW;
    case 0x49: return KEY_PGUP;
    case 0x4B: return KEY_LEFTARROW;
    case 0x4C: return '5';
    case 0x4D: return KEY_RIGHTARROW;
    case 0x4F: return KEY_END;
    case 0x50: return KEY_DOWNARROW;
    case 0x51: return KEY_PGDN;
    case 0x52: return KEY_INS;
    case 0x53: return KEY_DEL;
    }
    if (code >= 0x3B && code <= 0x44) return 0x80 + code;   // F1..F10
    static const char plain[] = "\0\0" "1234567890-=\0\0" "qwertyuiop[]\0\0" "asdfghjkl;'`\0\\" "zxcvbnm,./";
    if (code < sizeof plain - 1) return (unsigned char)plain[code];
    return 0;
}

// ---------------------------------------------------------------- mouse
// A PS/2 mouse (IRQ 12) or a USB one (usb.cpp): motion and buttons build
// up here until Doom asks (bm_mouse).
extern volatile uint8_t mouse_buf[256];
extern volatile uint8_t mouse_head, mouse_tail;
static int ps2_packet = 3;               // bytes per packet: 4 with a wheel
static int mouse_dx, mouse_dy, mouse_buttons;
static bool mouse_news;

static bool i8042_wait_write() { for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 2)) return true; return false; }
static bool i8042_wait_read()  { for (int i = 0; i < 100000; i++) if (inb(0x64) & 1) return true; return false; }
static bool mouse_send(uint8_t b) {
    i8042_wait_write(); outb(0x64, 0xD4);              // next byte to the aux device
    i8042_wait_write(); outb(0x60, b);
    for (int tries = 0; tries < 4; tries++) {          // skip stray bytes until the ACK
        if (!i8042_wait_read()) return false;
        if (inb(0x60) == 0xFA) return true;
    }
    return false;
}
static void ps2_mouse_init() {
    i8042_wait_write(); outb(0x64, 0xA8);              // enable the aux port
    i8042_wait_write(); outb(0x64, 0x20);              // read the controller config
    if (!i8042_wait_read()) { printf("PS/2 mouse: none\n"); return; }
    uint8_t cfg = inb(0x60);
    cfg = (cfg | 0x02) & ~0x20;                        // aux interrupt on, aux clock on
    i8042_wait_write(); outb(0x64, 0x60);
    i8042_wait_write(); outb(0x60, cfg);
    if (!mouse_send(0xF6)) { printf("PS/2 mouse: none\n"); return; }   // defaults
    static const uint8_t knock[] = {200, 100, 80};     // IntelliMouse: 4-byte packets
    bool ok = true;
    for (uint8_t r : knock) ok = ok && mouse_send(0xF3) && mouse_send(r);
    if (ok && mouse_send(0xF2) && i8042_wait_read() && inb(0x60) == 3) ps2_packet = 4;
    mouse_send(0xF3); mouse_send(100);
    if (!mouse_send(0xF4)) { printf("PS/2 mouse: none\n"); return; }   // start streaming
    printf("PS/2 mouse: ready\n");
}

// Relative motion (x right, y down), buttons (1 left, 2 right, 4 middle).
void mouse_push(int dx, int dy, int buttons, int) {
    mouse_dx += dx; mouse_dy += dy;
    mouse_buttons = buttons & 7;
    mouse_news = true;
}

static void poll_ps2_mouse() {
    static uint8_t pkt[4];
    static int n;
    while (mouse_tail != mouse_head) {
        uint8_t b = mouse_buf[mouse_tail++];
        if (n == 0 && !(b & 0x08)) continue;           // resync: byte 0 always has bit 3 set
        pkt[n++] = b;
        if (n < ps2_packet) continue;
        n = 0;
        if (pkt[0] & 0xC0) continue;                   // overflow: drop the packet
        int dx = pkt[1] - ((pkt[0] << 4) & 0x100);
        int dy = pkt[2] - ((pkt[0] << 3) & 0x100);
        mouse_push(dx, -dy, pkt[0] & 7, 0);           // PS/2: y grows upward
    }
}

extern "C" void bm_poll(void) {
    usb_poll();
    poll_ps2_mouse();
    static bool ext;
    static int pause_skip;
    while (kbd_tail != kbd_head) {
        uint8_t b = kbd_buf[kbd_tail++];
        if (pause_skip) { pause_skip--; continue; }
        if (b == 0xE1) { pause_skip = 5; key_post(true, KEY_PAUSE); key_post(false, KEY_PAUSE); continue; }
        if (b == 0xE0) { ext = true; continue; }
        bool e = ext;
        ext = false;
        uint8_t code = b & 0x7F;
        bool down = !(b & 0x80);
        if (e && (code == 0x2A || code == 0x36)) continue;   // fake shifts around E0 keys
        if (code == 0x1D) ctrl_down = down;
        if (code == 0x38) alt_down = down;
        if (down && code == 0x53 && ctrl_down && alt_down) platform_reboot();   // Ctrl+Alt+Del
        key_post(down, doom_key(e, code));
    }
}

extern "C" int bm_key(int* pressed, unsigned char* key) {
    bm_poll();
    if (keyq_tail == keyq_head) return 0;
    uint16_t k = keyq[keyq_tail++];
    *pressed = k >> 8;
    *key = (unsigned char)k;
    return 1;
}

extern "C" int bm_mouse(int* buttons, int* dx, int* dy) {
    if (!mouse_news) return 0;
    uintptr_t fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    *buttons = mouse_buttons; *dx = mouse_dx; *dy = mouse_dy;
    mouse_dx = mouse_dy = 0;
    mouse_news = false;
    __asm__ volatile("push %0; popf" :: "r"(fl) : "memory", "cc");
    return 1;
}

extern "C" uint32_t bm_ms(void) { return platform_ms(); }

// Waiting: keep the input drained and let the CPU sleep between ticks.
void platform_sleep_ms(uint32_t ms) {
    uint32_t t0 = platform_ms();
    do { bm_poll(); __asm__ volatile("hlt"); } while (platform_ms() - t0 < ms);
}
extern "C" void bm_sleep_ms(uint32_t ms) { platform_sleep_ms(ms); }

// ---------------------------------------------------------------- sound card
// The outputs audio.cpp found at boot (HD Audio jacks and monitors, AC'97,
// Sound Blaster, and last the PC speaker, which for Doom means off), for
// the Sound Volume menu. Switching stops the mixer first; it runs in the
// timer interrupt, so once we're here it isn't in the middle of a batch.
extern "C" int bm_audio_count(void) { return audio_output_count(); }
extern "C" int bm_audio_current(void) { int c = audio_output_current(); return c < 0 ? 0 : c; }
extern "C" const char* bm_audio_label(int i) {
    static char buf[64];
    const char* n = audio_output_name(i);
    if (!strcmp(n, "PC speaker")) return "OFF";
    strncpy(buf, n, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    char* p = strstr(buf, " (");                       // drop the PCI address
    if (p && strchr(p, ':')) *p = 0;
    return buf;
}
extern "C" int bm_audio_select(int i) {
    mixer_init(0);
    bool ok = audio_select(i);
    mixer_init(audio_name()[0] != 'n');                 // "none": the PC speaker (off)
    printf("Sound: %s%s\n", audio_output_name(i), ok ? "" : " (couldn't start)");
    return ok;
}

// ---------------------------------------------------------------- console
// The kernel's and Doom's messages (printf, stdout, stderr) go to the serial
// port, and the last 16 KB are kept for the error screen.
static char klog[16384];
static size_t klog_len;                         // total ever written
extern "C" void klog_add(const char* s, size_t n) {    // libc.cpp: printf
    for (size_t i = 0; i < n; i++) klog[klog_len++ % sizeof klog] = s[i];
}
// The last `max` bytes of the log, starting at a line.
static const char* klog_tail(size_t max) {
    static char out[sizeof klog + 1];
    size_t n = klog_len < sizeof klog ? klog_len : sizeof klog;
    if (n > max) n = max;
    size_t start = klog_len - n;
    for (size_t i = 0; i < n; i++) out[i] = klog[(start + i) % sizeof klog];
    out[n] = 0;
    char* p = out;
    if (start) while (*p && *p != '\n') p++;
    return *p == '\n' ? p + 1 : p;
}
// What Doom writes to stderr (I_Error's message), kept on its own.
static char errlog[1024];
static size_t errlog_len;
void console_write(const char* s, size_t n, bool is_stderr) {
    klog_add(s, n);
    for (size_t i = 0; i < n; i++) serial_putc(s[i]);
    if (is_stderr)
        for (size_t i = 0; i < n && errlog_len < sizeof errlog - 1; i++) errlog[errlog_len++] = s[i];
}
int console_getchar() { return -1; }

// ---------------------------------------------------------------- exit
// Doom quits with exit(0) and stops on an error (I_Error) with exit(-1),
// having written the message to stderr. Either way: say so, then restart
// the machine on a key press.
static bool game_running;
[[noreturn]] static void wait_key_and_reboot() {
    __asm__ volatile("sti");
    mixer_init(0);
    keyq_tail = keyq_head;
    for (;;) {
        int p; unsigned char k;
        if (bm_key(&p, &k) && p) platform_reboot();
        __asm__ volatile("hlt");
    }
}
extern "C" void exit(int code) noexcept {
    if (code == 0 && game_running) {
        printf("exit(0)\n");
        static const char* l[] = {"Doom has quit.", "Press any key to restart the computer."};
        static const uint32_t c[] = {0xFFFFFF, 0xAAAAAA};
        text_screen(l, c, 2);
    } else {
        // Doom's message (stderr), or else the end of the log.
        static char msg[1200];
        strncpy(msg, errlog_len ? errlog : klog_tail(600), sizeof msg - 1);
        size_t n = strlen(msg);
        while (n && (msg[n - 1] == '\n' || msg[n - 1] == ' ')) msg[--n] = 0;
        printf("exit(%d)\n", code);
        static const char* l[] = {"DOOM STOPPED", msg, "Press any key to restart the computer."};
        static const uint32_t c[] = {0xFF5555, 0xAAAAAA, 0xFFFFFF};
        text_screen(l, c, 3);
    }
    wait_key_and_reboot();
}

[[noreturn]] static void halt_screen(const char* l1, const char* l2) {
    printf("%s\n%s\n", l1, l2);
    const char* l[] = {l1, l2, "Press any key to restart the computer."};
    static const uint32_t c[] = {0xFF5555, 0xAAAAAA, 0xFFFFFF};
    text_screen(l, c, 3);
    wait_key_and_reboot();
}

extern "C" void fault_handler() {
    printf("CPU exception - halted\n");
    static const char* l[] = {"CPU EXCEPTION", "The machine has stopped. Restart it to play again."};
    static const uint32_t c[] = {0xFF5555, 0xAAAAAA};
    text_screen(l, c, 2);
}

extern "C" void (*__init_array_start[])(), (*__init_array_end[])();
#ifndef __x86_64__
extern "C" uint32_t fpu_present;                       // boot32.S
#endif

// Boot progress: stage n draws blocks 1..n along the bottom of the screen
// straight into the framebuffer, so a machine that hangs before the game
// starts still shows how far it got. Block 1 is the UEFI loader's.
static const MultibootInfo* boot_fb;
static void boot_mark(int n) {
    static const uint32_t colour[] = {
        0xFF0000, 0xFF8000, 0xFFFF00, 0x00FF00, 0x00FFFF,
        0x0060FF, 0x8000FF, 0xFF00FF, 0xFFFFFF, 0xA0A0A0,
    };
    const MultibootInfo* m = boot_fb;
    if (!m || !(m->flags & (1 << 12)) || m->fb_type != 1 || m->fb_bpp != 32 || m->fb_addr >= phys_limit) return;
    if (m->fb_height < 40) return;
    const uint8_t* c = m->fb_color;
    for (int i = 1; i <= n && i <= 10; i++) {
        uint32_t x0 = 8 + (uint32_t)(i - 1) * 24, y0 = m->fb_height - 24;
        if (x0 + 16 > m->fb_width) return;
        uint32_t rgb = colour[i - 1];
        uint32_t px = ((rgb >> 16 & 0xFF) << c[0]) | ((rgb >> 8 & 0xFF) << c[2]) | ((rgb & 0xFF) << c[4]);
        for (uint32_t y = y0; y < y0 + 16; y++) {
            uint32_t* row = (uint32_t*)(uintptr_t)(m->fb_addr + (uint64_t)y * m->fb_pitch);
            for (uint32_t x = x0; x < x0 + 16; x++) row[x] = px;
        }
    }
}

// ---------------------------------------------------------------- arguments
// Doom's command line: the words of the boot command line that aren't the
// kernel's own options (audio=, hda=, sb=, latency=, usb=, usbhc=), e.g.
//   multiboot /boot/doom.elf -iwad doom2.wad -warp 1 1 audio=sb
#define MAX_ARGS 32
static char* doom_argv[MAX_ARGS + 1];
static int doom_argc;
static bool has_arg(const char* a) {
    for (int i = 1; i < doom_argc; i++) if (!strcmp(doom_argv[i], a)) return true;
    return false;
}
static void add_arg(const char* a) {
    if (doom_argc < MAX_ARGS) doom_argv[doom_argc++] = strdup(a);
}
static void build_args(const char* cmdline) {
    add_arg("doom");
    bool first = true;
    for (const char* p = cmdline; p && *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char* e = p;
        while (*e && *e != ' ') e++;
        char w[256];
        size_t n = (size_t)(e - p) < sizeof w - 1 ? (size_t)(e - p) : sizeof w - 1;
        memcpy(w, p, n); w[n] = 0;
        p = e;
        // The boot loader puts the kernel's own path first.
        if (first && w[0] != '-' && !strchr(w, '=')) { first = false; continue; }
        first = false;
        if (strchr(w, '=') && w[0] != '-') continue;     // a kernel option
        if (!strcmp(w, "debug") || !strcmp(w, "pause")) continue;
        add_arg(w);
    }
}

// The WAD: Doom looks for the usual IWAD names itself (doom2.wad,
// doom.wad, doom1.wad, freedoom2.wad...) in the current directory, the root
// of the RAM disk. Check there's one, to say so clearly if not.
static bool any_wad(const char* name, uint32_t, bool is_dir, void* found) {
    size_t n = strlen(name);
    if (!is_dir && n > 4 && !strcasecmp(name + n - 4, ".wad")) { *(bool*)found = true; return false; }
    return true;
}

extern "C" void doomgeneric_Create(int argc, char** argv);
extern "C" void doomgeneric_Tick(void);

extern "C" void kmain() {
    serial_init();
    printf("\nDoom - bare metal\n");
    const MultibootInfo* mbi = (const MultibootInfo*)(uintptr_t)mb_info;
    if (mb_magic != 0x2BADB002) { printf("Not booted by a Multiboot loader\n"); return; }
    static MultibootInfo info;
    info = *mbi;
    boot_fb = &info;
    boot_mark(2);                                       // kernel started
    static char cmdbuf[1024];
    const char* cmdline = nullptr;
    if (info.flags & (1 << 2)) {
        strncpy(cmdbuf, (const char*)(uintptr_t)info.cmdline, sizeof(cmdbuf) - 1);
        cmdline = cmdbuf;
    }
    if ((info.flags & (1 << 3)) && info.mods_count) {
        const MultibootModule* m = (const MultibootModule*)(uintptr_t)info.mods_addr;
        for (uint32_t i = 0; i < info.mods_count && nmods < MAX_MODS; i++, nmods++) {
            mods[nmods].start = m[i].start;
            mods[nmods].end = m[i].end;
            if (m[i].string) strncpy(mods[nmods].name, (const char*)(uintptr_t)m[i].string, sizeof mods[0].name - 1);
        }
    }
    heap_init(&info);
    boot_mark(3);                                       // memory
    if (!video_init(&info)) { printf("No usable framebuffer found\n"); return; }
    video_ready = true;
    boot_mark(4);                                       // video
    for (auto f = __init_array_start; f != __init_array_end; f++) (*f)();
    boot_mark(5);

#ifndef __x86_64__
    if (!fpu_present)
        halt_screen("DOOM NEEDS A MATH COPROCESSOR", "This PC has no 387 (or 486DX) floating-point unit.");
#endif

    AudioDriver drv = audio_init(cmdline);
    mixer_init(drv != AUDIO_NONE);
    music_init();                                       // OPL3 tables (before the timer runs)
    boot_mark(6);                                       // sound
    printf("Sound: %s\n", drv != AUDIO_NONE ? audio_name() : "none (no sound card found)");
    usb_init(cmdline);
    boot_mark(7);                                       // USB
    ps2_mouse_init();
    boot_mark(8);                                       // PS/2 mouse
    interrupts_init();
    boot_mark(9);                                       // interrupts on

    // The disk image: the module named *.img, else the first one.
    int disk = nmods ? 0 : -1;
    for (int i = 0; i < nmods; i++) {
        size_t n = strlen(mods[i].name);
        if (n >= 4 && !strcasecmp(mods[i].name + n - 4, ".img")) { disk = i; break; }
    }
    if (disk >= 0) storage_init((const void*)(uintptr_t)mods[disk].start, mods[disk].end - mods[disk].start);
    else storage_init(nullptr, 0);
    boot_mark(10);                                      // disk
    if (!storage_ready()) {
        // Say exactly what arrived, so a screenshot is enough to tell why.
        static char why[600];
        size_t w = 0;
        auto add = [&](const char* f, auto... a) {
            if (w < sizeof why) w += snprintf(why + w, sizeof why - w, f, a...);
        };
        if (!(info.flags & (1 << 3)))
            add("The boot loader passed no modules at all (Multiboot flags %X).", info.flags);
        else if (!nmods)
            add("The boot loader passed 0 modules: GRUB's \"module\" line failed (wrong path, or not enough memory?).");
        else {
            add("%d module(s):", nmods);
            for (int i = 0; i < nmods; i++)
                add(" [%s] %lu bytes at %lX;", mods[i].name[0] ? mods[i].name : "no name",
                    (unsigned long)(mods[i].end - mods[i].start), (unsigned long)mods[i].start);
            if (disk >= 0) {
                const uint8_t* b = (const uint8_t*)(uintptr_t)mods[disk].start;
                uint64_t sz = mods[disk].end - mods[disk].start;
                if (sz < 512) add(" Too small to be a disk image.");
                else if (b[0] == 0x1F && b[1] == 0x8B) add(" It's still gzip-compressed: the boot loader didn't unpack it.");
                else {
                    add(" Boot sector: signature %02X%02X, %u bytes/sector, FAT16 size %u, FAT32 size %u, type \"%.8s\".",
                        b[510], b[511], b[11] | b[12] << 8, b[22] | b[23] << 8,
                        (unsigned)(b[36] | b[37] << 8 | b[38] << 16 | (uint32_t)b[39] << 24), (const char*)b + 82);
                }
            }
        }
        printf("%s\n", why);
        halt_screen("NO DOOM DISK", why);
    }
    bool wad = false;
    fat_list("/", any_wad, &wad);
    if (!wad) halt_screen("NO WAD FILE", "doom.img has no .wad file in its root directory. Put doom1.wad, doom.wad or doom2.wad there.");

    build_args(cmdline);
    // Doom's zone: 6 MB unless told (-mb). Take more when there's room, for
    // big levels and PWADs; the WAD itself is read into the heap too.
    if (!has_arg("-mb")) {
        size_t big = heap_largest_free() >> 20;
        if (big >= 192) { add_arg("-mb"); add_arg("64"); }
        else if (big >= 96) { add_arg("-mb"); add_arg("32"); }
        else if (big >= 48) { add_arg("-mb"); add_arg("16"); }
    }
    printf("Heap: %lu KB free, largest block %lu KB\n",
           (unsigned long)(heap_free_bytes() >> 10), (unsigned long)(heap_largest_free() >> 10));
    printf("Doom's command line:");
    for (int i = 0; i < doom_argc; i++) printf(" %s", doom_argv[i]);
    printf("\n");

    scaler_init();
    game_running = true;
    doomgeneric_Create(doom_argc, doom_argv);
    for (;;) doomgeneric_Tick();
}
