// MS-DOS (DJGPP, 32-bit DPMI) platform layer for Doom.
//
// Stands in for ../baremetal/kernel.cpp and audio.cpp: the same interface
// (bm.h, bm_audio.h, the parts of audio.hpp the mixer uses), so the port
// glue (doomgeneric_baremetal.c, i_sound_baremetal.c), the mixer and the
// OPL3 General MIDI synth from ../baremetal run unchanged. DOS and DJGPP
// supply the C library, files (the WAD, config and saved games in the
// current directory) and memory; this file supplies:
//
//   video     VGA mode 13h: 320x200, 256 colours, Doom's own resolution,
//             so the frame is copied straight to A000:0000 and the palette
//             goes into the DAC
//   keyboard  IRQ 1, scancodes straight from the controller (Doom needs
//             key releases, which the BIOS doesn't give)
//   mouse     INT 33h driver (CTMOUSE, MOUSE.COM, DOSBox's own)
//   time      uclock() (the PIT, to the microsecond)
//   sound     Sound Blaster (2.0 / Pro / 16 and clones) from BLASTER=,
//             8-bit mono auto-init DMA from a buffer in conventional memory.
//             The ring is topped up whenever Doom polls for input or draws,
//             so there is no sound interrupt handler to go wrong.
//   console   Doom's messages go to the DOS screen until the game switches
//             to graphics; after that they're kept and shown on exit
#include <dpmi.h>
#include <go32.h>
#include <pc.h>
#include <crt0.h>
#include <sys/farptr.h>
#include <sys/movedata.h>
#include <sys/exceptn.h>
#include <sys/fsext.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern "C" {
#include "doomkeys.h"
}
#include "bm.h"
#include "bm_audio.h"
#include "audio.hpp"
#include "mixer.hpp"
#include "music.h"
extern "C" {
#include "d_doomcom.h"
}
#include <stddef.h>

// The keyboard handler runs at interrupt time and touches our data: lock
// everything (code, data, stack, heap) so the DPMI host never pages it out.
int _crt0_startup_flags = _CRT0_FLAG_LOCK_MEMORY;

// ---------------------------------------------------------------- time
static uclock_t t_start;
static uint32_t now_ms() { return (uint32_t)((uclock() - t_start) * 1000 / UCLOCKS_PER_SEC); }

// ---------------------------------------------------------------- console
// Before the game switches to graphics, printf goes to the DOS screen as
// usual. After that, it's kept here and printed when we're back in text
// mode (an I_Error message, say).
static char con_log[8192];
static size_t con_len;
static bool graphics_on;

static int con_hook(__FSEXT_Fnumber n, int* rv, va_list args) {
    if (n != __FSEXT_write || !graphics_on) return 0;
    va_arg(args, int);
    const char* s = va_arg(args, const char*);
    size_t len = va_arg(args, size_t);
    for (size_t i = 0; i < len; i++) con_log[con_len++ % sizeof con_log] = s[i];
    *rv = (int)len;
    return 1;
}

// ---------------------------------------------------------------- video
// 320x200 (the default): VGA mode 13h, Doom's own resolution, a straight
// copy. Anything else (-hires, -widescreen): a VESA mode, Doom's frame
// scaled up to the largest rectangle of its shape -- its pixels being 1.2
// times taller than wide -- black around it. The mode is picked from the
// card's list (or -vesa WxH / DOOMVID=WxH); 8-bit palette modes, or 15, 16,
// 24 or 32 bits through a palette lookup; linear framebuffer, or a banked
// window on cards without one.
enum VMode { V_NONE, V_13H, V_LFB, V_BANKED };
static VMode vmode = V_NONE;
static uint32_t fb_w, fb_h, fb_pitch;
static int fb_bytes = 1;
static bool fb_indexed = true;
static uint8_t r_pos, r_len, g_pos, g_len, b_pos, b_len;
static int lfb_sel = -1;
static __dpmi_meminfo lfb_map;
static uint32_t win_seg = 0xA000, win_gran = 65536, win_size = 65536;
static int cur_bank = -1;
static int frame_w, frame_h;            // the frame the scaler is set up for
static uint32_t dst_x, dst_y, dst_w, dst_h;
static uint16_t* xmap;                  // screen column -> frame column
static uint8_t* line;                   // one screen row, in the screen's format
static uint32_t pal_native[256];
static uint32_t pal_rgb[256];
static int want_w, want_h;              // -vesa / DOOMVID
static char mode_msg[320];              // which mode, shown after the game (con_log)

static void vga_mode(int mode) {
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = (unsigned short)mode;
    __dpmi_int(0x10, &r);
}

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static bool vbe_mode_info(int mode, uint8_t mi[256]) {
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x4F01; r.x.cx = (unsigned short)mode;
    r.x.es = __tb >> 4; r.x.di = __tb & 15;
    __dpmi_int(0x10, &r);
    if (r.x.ax != 0x004F) return false;
    dosmemget(__tb, 256, mi);
    return true;
}

// A VESA mode for a w x h frame. False: no VESA BIOS, or nothing usable.
static bool vesa_init(int w, int h) {
    uint8_t info[512];
    memset(info, 0, sizeof info);
    memcpy(info, "VBE2", 4);
    dosmemput(info, sizeof info, __tb);
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x4F00; r.x.es = __tb >> 4; r.x.di = __tb & 15;
    __dpmi_int(0x10, &r);
    if (r.x.ax != 0x004F) return false;
    dosmemget(__tb, sizeof info, info);
    if (memcmp(info, "VESA", 4) != 0) return false;
    int ver = rd16(info + 4);
    uint32_t lp = rd32(info + 14);
    uint32_t list_lin = ((lp >> 16) << 4) + (lp & 0xFFFF);
    uint16_t modes[256];
    int nm = 0;
    for (; nm < 255; nm++) {
        uint16_t m = _farpeekw(_dos_ds, list_lin + nm * 2);
        if (m == 0xFFFF) break;
        modes[nm] = m;
    }

    // The frame as shown: w x 1.2h. Best: the mode its shape fills most
    // (a 16:9 one for widescreen: 848x480 in DOSBox Staging with
    // vesa_modes=all), big enough (a few pixels short is fine: scaled to
    // fit), then the fewest pixels to write; 8 bits (the palette is the
    // DAC's) before 16, 15, 32, 24.
    double need_w = w, need_h = h * 1.2;
    int best = -1;
    double best_score = -1e9;
    uint8_t mi[256];
    for (int i = 0; i < nm; i++) {
        if (!vbe_mode_info(modes[i], mi)) continue;
        uint16_t attr = rd16(mi);
        if ((attr & 0x19) != 0x19) continue;            // supported, colour, graphics
        int mw = rd16(mi + 18), mh = rd16(mi + 20), bpp = mi[25], model = mi[27];
        if (mw < 320 || mh < 200) continue;
        if (bpp == 8 ? model != 4 : model != 6) continue;
        if (bpp != 8 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32) continue;
        bool lfb = (attr & 0x80) && ver >= 0x200 && rd32(mi + 40);
        if (!lfb && (mi[2] & 5) != 5) continue;         // window A must be writable
        // 640x400 is shown at 4:3 (by DOSBox and on a CRT): taller pixels.
        double mh_shown = (mw == 640 && mh == 400) ? 480 : mh;
        double sc = mw / need_w < mh_shown / need_h ? mw / need_w : mh_shown / need_h;
        double usage = (need_w * sc) * (need_h * sc) / ((double)mw * mh_shown);
        double score;
        if (want_w && mw == want_w && mh == want_h) score = 100000;
        else if (sc < 0.97) score = sc * 1000;          // too small
        else score = 5000 + usage * 4000 - ((double)mw * mh) / (need_w * need_h) * 20;
        static const int pref[] = {8, 16, 15, 32, 24};
        for (int k = 0; k < 5; k++) if (pref[k] == bpp) score += 40 - k * 8;
        if (lfb) score += 5;
        if (score > best_score) { best_score = score; best = modes[i]; }
    }
    if (best < 0 || !vbe_mode_info(best, mi)) return false;

    uint16_t attr = rd16(mi);
    fb_w = rd16(mi + 18); fb_h = rd16(mi + 20);
    int bpp = mi[25];
    fb_bytes = (bpp + 7) / 8;
    fb_indexed = bpp == 8;
    fb_pitch = rd16(mi + 16);
    if (!fb_indexed) {
        r_len = mi[31]; r_pos = mi[32]; g_len = mi[33]; g_pos = mi[34]; b_len = mi[35]; b_pos = mi[36];
        if (!r_len || !g_len || !b_len) {               // VBE 1.x leaves the masks out
            if (bpp == 15) { r_pos = 10; r_len = 5; g_pos = 5; g_len = 5; b_pos = 0; b_len = 5; }
            else if (bpp == 16) { r_pos = 11; r_len = 5; g_pos = 5; g_len = 6; b_pos = 0; b_len = 5; }
            else { r_pos = 16; r_len = 8; g_pos = 8; g_len = 8; b_pos = 0; b_len = 8; }
        }
    }
    bool lfb = (attr & 0x80) && ver >= 0x200 && rd32(mi + 40);
    if (lfb) {
        if (ver >= 0x300 && rd16(mi + 50)) fb_pitch = rd16(mi + 50);   // LinBytesPerScanLine
        lfb_map.address = rd32(mi + 40);
        lfb_map.size = (fb_pitch * fb_h + 0xFFF) & ~0xFFFu;
        if (__dpmi_physical_address_mapping(&lfb_map) != 0) lfb = false;
        else {
            lfb_sel = __dpmi_allocate_ldt_descriptors(1);
            if (lfb_sel < 0) lfb = false;
            else {
                __dpmi_set_segment_base_address(lfb_sel, lfb_map.address);
                __dpmi_set_segment_limit(lfb_sel, (lfb_map.size - 1) | 0xFFF);
            }
        }
    }
    if (!lfb) {
        if ((mi[2] & 5) != 5) return false;
        win_gran = (uint32_t)rd16(mi + 4) * 1024;
        win_size = (uint32_t)rd16(mi + 6) * 1024;
        win_seg = rd16(mi + 8);
        if (!win_gran || !win_size || !win_seg) return false;
    }
    memset(&r, 0, sizeof r);
    r.x.ax = 0x4F02; r.x.bx = (unsigned short)(best | (lfb ? 0x4000 : 0));
    __dpmi_int(0x10, &r);
    if (r.x.ax != 0x004F) return false;
    vmode = lfb ? V_LFB : V_BANKED;
    cur_bank = -1;
    snprintf(mode_msg, sizeof mode_msg, "Video: VESA mode %Xh, %ux%u, %d bits, %s\n", best, fb_w, fb_h, bpp,
             lfb ? "linear framebuffer" : "banked window");
    // A widescreen picture in a mode of another shape has bars: say why.
    if (w * 5 > h * 9 && (fb_w * 9 < fb_h * 15 || (fb_w == 640 && fb_h == 400))) {   // a wide picture, a 4:3 screen
        size_t n = strlen(mode_msg);
        snprintf(mode_msg + n, sizeof mode_msg - n,
                 "Video: no widescreen VESA mode on this card, so the picture has bars.\n"
                 "       DOSBox Staging: vesa_modes = all (848x480). Or -vesa WxH.\n");
    }
    return true;
}

static uint32_t native(uint32_t c) {
    uint32_t r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    return (r >> (8 - r_len)) << r_pos | (g >> (8 - g_len)) << g_pos | (b >> (8 - b_len)) << b_pos;
}

// Bytes to the screen at byte offset `off`: the linear framebuffer, or
// the banked window a piece at a time.
static void fb_write(uint32_t off, const void* src, uint32_t n) {
    if (vmode == V_LFB) { movedata(_my_ds(), (unsigned)src, lfb_sel, off, n); return; }
    const uint8_t* s = (const uint8_t*)src;
    while (n) {
        int bank = (int)(off / win_gran);
        uint32_t in = off % win_gran;
        uint32_t k = win_size - in;
        if (k > n) k = n;
        if (bank != cur_bank) {
            __dpmi_regs r;
            memset(&r, 0, sizeof r);
            r.x.ax = 0x4F05; r.x.bx = 0; r.x.dx = (unsigned short)bank;
            __dpmi_int(0x10, &r);
            cur_bank = bank;
        }
        dosmemput(s, k, win_seg * 16 + in);
        s += k; off += k; n -= k;
    }
}

static void fb_clear() {
    static uint8_t zero[4096];
    for (uint32_t y = 0; y < fb_h; y++)
        for (uint32_t x = 0; x < fb_pitch; x += sizeof zero)
            fb_write(y * fb_pitch + x, zero, fb_pitch - x < sizeof zero ? fb_pitch - x : sizeof zero);
}

// The largest rectangle of the frame's shape (w : 1.2h) that fits. Modes
// shown at 4:3 with non-square pixels (320x200, 640x400) count their
// pixels as 1.2 times taller.
static void scaler_setup(int w, int h) {
    bool tall = (fb_w == 320 && fb_h == 200) || (fb_w == 640 && fb_h == 400);
    uint64_t aw = (uint64_t)w * 5, ah = (uint64_t)h * (tall ? 5 : 6);
    dst_w = fb_w; dst_h = (uint32_t)(fb_w * ah / aw);
    if (dst_h > fb_h) { dst_h = fb_h; dst_w = (uint32_t)(fb_h * aw / ah); }
    if (dst_w > fb_w) dst_w = fb_w;
    dst_x = (fb_w - dst_w) / 2; dst_y = (fb_h - dst_h) / 2;
    free(xmap); free(line);
    xmap = (uint16_t*)malloc(dst_w * sizeof(uint16_t));
    line = (uint8_t*)malloc((size_t)dst_w * 4);
    for (uint32_t x = 0; x < dst_w; x++) xmap[x] = (uint16_t)(x * (uint32_t)w / dst_w);
    frame_w = w; frame_h = h;
    fb_clear();
}

// The first frame picks the mode (so Doom's startup text stays readable).
static void graphics_start(int w, int h) {
    if (graphics_on) return;
    if (!(w == 320 && h == 200 && !want_w) && vesa_init(w, h)) {
        graphics_on = true;
        scaler_setup(w, h);
        printf("%s", mode_msg);                         // kept, shown on exit
    } else {
        if (w != 320 || h != 200)
            printf("Video: no VESA mode for %dx%d; showing it in mode 13h\n", w, h);
        vga_mode(0x13);
        vmode = V_13H;
        fb_w = 320; fb_h = 200; fb_pitch = 320; fb_bytes = 1; fb_indexed = true;
        graphics_on = true;
        if (w != 320 || h != 200) scaler_setup(w, h);
    }
}

static void graphics_stop() {
    if (!graphics_on) return;
    graphics_on = false;
    vga_mode(0x03);
    vmode = V_NONE;
    if (lfb_sel >= 0) { __dpmi_free_ldt_descriptor(lfb_sel); lfb_sel = -1; }
    if (lfb_map.size) { __dpmi_free_physical_address_mapping(&lfb_map); lfb_map.size = 0; }
}

static void dac_load(const uint32_t* rgb) {
    outportb(0x3C8, 0);
    for (int i = 0; i < 256; i++) {
        uint32_t c = rgb[i];
        outportb(0x3C9, (c >> 18) & 63);
        outportb(0x3C9, (c >> 10) & 63);
        outportb(0x3C9, (c >> 2) & 63);
    }
}

extern "C" int SCREENWIDTH, SCREENHEIGHT;      // i_video.c: the frame's size

extern "C" void bm_set_palette(const uint32_t* rgb) {
    graphics_start(SCREENWIDTH, SCREENHEIGHT);      // setting the mode resets the DAC: first
    memcpy(pal_rgb, rgb, sizeof pal_rgb);
    if (fb_indexed) dac_load(rgb);
    else for (int i = 0; i < 256; i++) pal_native[i] = native(rgb[i]);
}

static bool vsync;
extern "C" void bm_draw(const uint8_t* pixels, int w, int h) {
    graphics_start(w, h);
    if (vsync) {                                        // -vsync: no tearing, at most 70 fps
        while (inportb(0x3DA) & 8) {}
        while (!(inportb(0x3DA) & 8)) {}
    }
    if (vmode == V_13H && w == 320 && h == 200) {       // the classic path: a straight copy
        dosmemput(pixels, 320 * 200, 0xA0000);
        return;
    }
    if (w != frame_w || h != frame_h) scaler_setup(w, h);
    int last = -1;
    for (uint32_t y = 0; y < dst_h; y++) {
        int sy = (int)(y * (uint32_t)h / dst_h);
        if (sy != last) {                               // a new source row: convert it
            last = sy;
            const uint8_t* s = pixels + sy * w;
            switch (fb_bytes) {
            case 1: for (uint32_t x = 0; x < dst_w; x++) line[x] = s[xmap[x]]; break;
            case 2: for (uint32_t x = 0; x < dst_w; x++) ((uint16_t*)line)[x] = (uint16_t)pal_native[s[xmap[x]]]; break;
            case 4: for (uint32_t x = 0; x < dst_w; x++) ((uint32_t*)line)[x] = pal_native[s[xmap[x]]]; break;
            default:
                for (uint32_t x = 0; x < dst_w; x++) {
                    uint32_t v = pal_native[s[xmap[x]]];
                    line[x * 3] = (uint8_t)v; line[x * 3 + 1] = (uint8_t)(v >> 8); line[x * 3 + 2] = (uint8_t)(v >> 16);
                }
            }
        }
        if (vmode == V_13H) dosmemput(line, dst_w, 0xA0000 + (dst_y + y) * 320 + dst_x);
        else fb_write((dst_y + y) * fb_pitch + dst_x * fb_bytes, line, dst_w * fb_bytes);
    }
}

// ---------------------------------------------------------------- keyboard
// IRQ 1 queues the raw bytes; bm_poll turns them into Doom's keys.
static volatile uint8_t kbd_buf[256];
static volatile uint8_t kbd_head, kbd_tail;
static _go32_dpmi_seginfo kbd_old, kbd_new;
static bool kbd_hooked;

static void kbd_isr() {
    uint8_t b = inportb(0x60);
    kbd_buf[kbd_head] = b;
    kbd_head = kbd_head + 1;
    outportb(0x20, 0x20);
}
static void kbd_isr_end() {}

static void kbd_init() {
    _go32_dpmi_lock_code((void*)kbd_isr, (char*)kbd_isr_end - (char*)kbd_isr);
    _go32_dpmi_lock_data((void*)kbd_buf, sizeof kbd_buf);
    _go32_dpmi_get_protected_mode_interrupt_vector(9, &kbd_old);
    kbd_new.pm_offset = (unsigned long)kbd_isr;
    kbd_new.pm_selector = _go32_my_cs();
    if (_go32_dpmi_allocate_iret_wrapper(&kbd_new) != 0) { printf("Keyboard: can't hook IRQ 1\n"); return; }
    _go32_dpmi_set_protected_mode_interrupt_vector(9, &kbd_new);
    kbd_hooked = true;
}

static void kbd_done() {
    if (!kbd_hooked) return;
    _go32_dpmi_set_protected_mode_interrupt_vector(9, &kbd_old);
    _go32_dpmi_free_iret_wrapper(&kbd_new);
    kbd_hooked = false;
    // Leave the BIOS a clean slate: no stuck Shift/Ctrl/Alt, an empty buffer.
    _farpokeb(_dos_ds, 0x417, _farpeekb(_dos_ds, 0x417) & 0xF0);
    _farpokeb(_dos_ds, 0x418, 0);
    _farpokew(_dos_ds, 0x41A, _farpeekw(_dos_ds, 0x41C));
}

static uint16_t keyq[256];                              // pressed << 8 | key
static uint8_t keyq_head, keyq_tail;
static void key_post(bool down, int k) {
    if (!k || (uint8_t)(keyq_head + 1) == keyq_tail) return;
    keyq[keyq_head++] = (uint16_t)((down ? 0x100 : 0) | (k & 0xFF));
}

// Set-1 scancodes to doomkeys.h, as kernel.cpp.
static int doom_key(bool ext, uint8_t code) {
    if (ext) {
        switch (code) {
        case 0x1C: return KEY_ENTER;
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
    case 0x47: return KEY_HOME;
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

static void audio_pump();

// ---------------------------------------------------------------- mouse
static bool mouse_ok;
static int mouse_dx, mouse_dy, mouse_buttons = -1;
static bool mouse_news;
static uint32_t mouse_last;

static void mouse_init() {
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x0000;                                    // reset / detect
    __dpmi_int(0x33, &r);
    mouse_ok = r.x.ax == 0xFFFF;
    printf("Mouse: %s\n", mouse_ok ? "INT 33h driver" : "none (load CTMOUSE or MOUSE.COM for one)");
    if (!mouse_ok) return;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x000B;                                    // clear the motion counters
    __dpmi_int(0x33, &r);
}

static void mouse_poll() {
    if (!mouse_ok) return;
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x000B;                                    // motion since last call, in mickeys
    __dpmi_int(0x33, &r);
    int dx = (short)r.x.cx, dy = (short)r.x.dx;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x0003;                                    // buttons: 1 left, 2 right, 4 middle
    __dpmi_int(0x33, &r);
    int b = r.x.bx & 7;
    if (dx || dy || b != mouse_buttons) {
        mouse_dx += dx; mouse_dy += dy;
        mouse_buttons = b;
        mouse_news = true;
    }
}

// ---------------------------------------------------------------- input
extern "C" void bm_poll(void) {
    static bool ext;
    static int pause_skip;
    while (kbd_tail != kbd_head) {
        uint8_t b = kbd_buf[kbd_tail];
        kbd_tail = kbd_tail + 1;
        if (pause_skip) { pause_skip--; continue; }
        if (b == 0xE1) { pause_skip = 5; key_post(true, KEY_PAUSE); key_post(false, KEY_PAUSE); continue; }
        if (b == 0xE0) { ext = true; continue; }
        bool e = ext;
        ext = false;
        uint8_t code = b & 0x7F;
        bool down = !(b & 0x80);
        if (e && (code == 0x2A || code == 0x36)) continue;   // fake shifts around E0 keys
        key_post(down, doom_key(e, code));
    }
    uint32_t t = now_ms();
    if (t - mouse_last >= 5) { mouse_last = t; mouse_poll(); }   // INT 33h is a real-mode trip: not too often
    audio_pump();
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
    *buttons = mouse_buttons; *dx = mouse_dx; *dy = mouse_dy;
    mouse_dx = mouse_dy = 0;
    mouse_news = false;
    return 1;
}

extern "C" uint32_t bm_ms(void) { return now_ms(); }

extern "C" void bm_sleep_ms(uint32_t ms) {
    uint32_t t0 = now_ms();
    do { bm_poll(); __dpmi_yield(); } while (now_ms() - t0 < ms);
}

// ================================================================ sound
// The Sound Blaster's DSP 2.0+ (the SB Pro and SB16 and their clones speak
// it too): 8-bit unsigned mono, ~22 kHz, auto-initialising ISA DMA over a
// ring in conventional memory (ISA DMA reaches the first 16 MB only, and
// mustn't cross a 64 KB page). As audio.cpp, but polled from the main loop.
static uint16_t sb_base = 0x220;
static int sb_irq = 5, sb_dma = 1;
static bool sb_found, sb_running;
static int sb_major, sb_minor;
static uint32_t sb_rate;
#define SB_RING 16384
static uint32_t sb_phys;                     // the ring's physical (= linear) address
static int sb_dos_sel = -1;
static uint32_t sb_write;                    // next frame we write
static uint32_t sb_ahead;                    // how far ahead of the DMA we keep it
static uint32_t sb_phase;
static int32_t sb_acc, sb_accn;
static uint32_t sb_underruns;
static uint8_t pic_saved;

static bool sb_wait_write() { for (int i = 0; i < 65536; i++) if (!(inportb(sb_base + 0xC) & 0x80)) return true; return false; }
static bool sb_wait_read()  { for (int i = 0; i < 65536; i++) if (inportb(sb_base + 0xE) & 0x80) return true; return false; }
static void sb_out(uint8_t v) { if (sb_wait_write()) outportb(sb_base + 0xC, v); }
static int sb_in() { return sb_wait_read() ? inportb(sb_base + 0xA) : -1; }

static bool sb_reset() {
    outportb(sb_base + 0x6, 1);
    uclock_t t = uclock();
    while (uclock() - t < UCLOCKS_PER_SEC / 10000 + 1) {}      // >= 3 us
    outportb(sb_base + 0x6, 0);
    t = uclock();
    while (uclock() - t < UCLOCKS_PER_SEC / 50)                // up to 20 ms
        if ((inportb(sb_base + 0xE) & 0x80) && inportb(sb_base + 0xA) == 0xAA) return true;
    return false;
}

// BLASTER=A220 I5 D1 H5 T6
static void sb_parse_blaster() {
    const char* e = getenv("BLASTER");
    if (!e) return;
    for (const char* p = e; *p; p++) {
        char c = *p & ~0x20;
        if (p != e && p[-1] != ' ' && p[-1] != '\t') continue;
        if (c == 'A') sb_base = (uint16_t)strtoul(p + 1, nullptr, 16);
        else if (c == 'I') sb_irq = atoi(p + 1);
        else if (c == 'D') sb_dma = atoi(p + 1);
    }
}

static const uint8_t kDmaAddr[4]  = {0x00, 0x02, 0x04, 0x06};
static const uint8_t kDmaCount[4] = {0x01, 0x03, 0x05, 0x07};
static const uint8_t kDmaPage[4]  = {0x87, 0x83, 0x81, 0x82};

static void ring_fill(uint32_t at, uint8_t v, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) _farpokeb(_dos_ds, sb_phys + (at + i) % SB_RING, v);
}

static bool sb_start() {
    if (!sb_found || sb_running) return sb_running;
    if (!sb_reset()) return false;
    ring_fill(0, 0x80, SB_RING);                      // silence is 128
    // Keep the card's interrupt masked: we read the DMA position instead.
    int irq = sb_irq & 15;
    uint16_t pic = irq < 8 ? 0x21 : 0xA1;
    pic_saved = inportb(pic);
    outportb(pic, pic_saved | (1 << (irq & 7)));
    if (sb_major >= 3) {                              // SB Pro mixer: mono, voice and master up
        outportb(sb_base + 4, 0x0E); outportb(sb_base + 5, 0x00);
        outportb(sb_base + 4, 0x04); outportb(sb_base + 5, 0xFF);
        outportb(sb_base + 4, 0x22); outportb(sb_base + 5, 0xFF);
    }
    // DMA: single mode, auto-init, memory to card, over the whole ring.
    int ch = sb_dma;
    outportb(0x0A, 0x04 | ch);                        // mask
    outportb(0x0C, 0);                                // clear the flip-flop
    outportb(0x0B, 0x58 | ch);
    outportb(kDmaAddr[ch], sb_phys & 0xFF);
    outportb(kDmaAddr[ch], (sb_phys >> 8) & 0xFF);
    outportb(kDmaPage[ch], (sb_phys >> 16) & 0xFF);
    outportb(kDmaCount[ch], (SB_RING - 1) & 0xFF);
    outportb(kDmaCount[ch], ((SB_RING - 1) >> 8) & 0xFF);
    outportb(0x0A, ch);                               // unmask
    int tc = 211;                                     // 256 - 1000000 / 22222
    sb_rate = 1000000 / (256 - tc);
    sb_out(0xD1);                                     // speaker on
    sb_out(0x40); sb_out((uint8_t)tc);
    sb_out(0x48); sb_out((SB_RING - 1) & 0xFF); sb_out(((SB_RING - 1) >> 8) & 0xFF);
    sb_out(0x1C);                                     // 8-bit auto-init output
    sb_write = 0; sb_phase = 0; sb_acc = sb_accn = 0;
    sb_running = true;
    return true;
}

static void sb_stop() {
    if (!sb_running) return;
    sb_running = false;
    sb_out(0xD0);                                     // pause DMA
    sb_reset();                                       // ends auto-init, speaker off
    outportb(0x0A, 0x04 | sb_dma);
    int irq = sb_irq & 15;
    outportb(irq < 8 ? 0x21 : 0xA1, pic_saved);
    inportb(sb_base + 0xE);                           // acknowledge whatever's pending
}

static bool sb_init() {
    sb_parse_blaster();
    if (!getenv("BLASTER")) { printf("Sound: no BLASTER= variable, trying a Sound Blaster at 220h\n"); }
    if (sb_dma < 0 || sb_dma > 3) { printf("Sound: BLASTER's D%d isn't an 8-bit DMA channel\n", sb_dma); return false; }
    if (!sb_reset()) { printf("Sound: no Sound Blaster at %Xh\n", sb_base); return false; }
    sb_out(0xE1);
    sb_major = sb_in(); sb_minor = sb_in();
    if (sb_major < 2) { printf("Sound: Sound Blaster DSP %d.x has no auto-init DMA\n", sb_major); return false; }
    // Twice the ring in conventional memory, then the half that doesn't
    // cross a 64 KB page.
    int seg = __dpmi_allocate_dos_memory((SB_RING * 2 + 15) / 16, &sb_dos_sel);
    if (seg == -1) { printf("Sound: no conventional memory for the DMA buffer\n"); return false; }
    uint32_t a = (uint32_t)seg << 4;
    if ((a >> 16) != ((a + SB_RING - 1) >> 16)) a = (a + 0xFFFF) & ~0xFFFFu;
    sb_phys = a;
    sb_found = true;
    printf("Sound: Sound Blaster DSP %d.%02d at %Xh, IRQ %d, DMA %d\n", sb_major, sb_minor < 0 ? 0 : sb_minor,
           sb_base, sb_irq, sb_dma);
    return true;
}

static uint32_t sb_play_pos() {
    int ch = sb_dma;
    inportb(sb_base + 0xE);                           // acknowledge the DSP's block interrupt
    uint32_t c1, c2;
    do {
        outportb(0x0C, 0);
        c1 = inportb(kDmaCount[ch]); c1 |= (uint32_t)inportb(kDmaCount[ch]) << 8;
        outportb(0x0C, 0);
        c2 = inportb(kDmaCount[ch]); c2 |= (uint32_t)inportb(kDmaCount[ch]) << 8;
    } while (c1 - c2 + 1 > 2);                        // read twice: the low byte may have rolled
    uint32_t left = (c2 + 1) & 0xFFFF;
    if (left > SB_RING) left = SB_RING;
    return (SB_RING - left) % SB_RING;
}

// audio.hpp, for mixer.cpp: frames (at 48 kHz) to top the ring up to
// sb_ahead ahead of the DMA. `nominal` is ignored: we're polled, not timed.
static int pump_frames;
int audio_frames_wanted(int) { return pump_frames; }

void audio_submit(const int16_t* samples, int n) {
    static uint8_t out[1024];
    int k = 0;
    for (int i = 0; i < n; i++) {
        sb_acc += samples[i]; sb_accn++;
        sb_phase += sb_rate;
        if (sb_phase >= 48000) {
            sb_phase -= 48000;
            int v = sb_acc / sb_accn;
            sb_acc = 0; sb_accn = 0;
            out[k++] = (uint8_t)((v >> 8) + 128);
        }
    }
    // Into the ring, in up to two pieces.
    uint32_t first = SB_RING - sb_write;
    if ((uint32_t)k <= first) dosmemput(out, k, sb_phys + sb_write);
    else { dosmemput(out, first, sb_phys + sb_write); dosmemput(out + first, k - first, sb_phys); }
    sb_write = (sb_write + k) % SB_RING;
}

const char* audio_name() { return sb_running ? "Sound Blaster" : "none"; }

static void audio_pump() {
    if (!sb_running) return;
    for (int round = 0; round < 32; round++) {
        uint32_t play = sb_play_pos();
        uint32_t ahead = (sb_write + SB_RING - play) % SB_RING;
        if (ahead > SB_RING / 2) {                    // the DMA passed us: start over just ahead of it
            sb_underruns++;
            ring_fill(play, 0x80, SB_RING / 2);
            sb_write = (play + 64) % SB_RING;
            ahead = 64;
        }
        if (ahead >= sb_ahead) break;
        uint32_t need = sb_ahead - ahead;
        if (need > 450) need = 450;                   // < 1024 frames at 48 kHz per mixer_tick
        pump_frames = (int)(need * 48000 / sb_rate) + 1;
        mixer_tick();
    }
    pump_frames = 0;
}

// bm_audio.h: the Sound Volume menu's CARD line. Sound Blaster, then OFF.
extern "C" int bm_audio_count(void) { return sb_found ? 2 : 1; }
extern "C" int bm_audio_current(void) { return sb_found && sb_running ? 0 : bm_audio_count() - 1; }
extern "C" const char* bm_audio_label(int i) { return sb_found && i == 0 ? "SOUND BLASTER" : "OFF"; }
extern "C" int bm_audio_select(int i) {
    mixer_init(0);
    if (sb_found && i == 0) {
        bool ok = sb_start();
        mixer_init(ok);
        return ok;
    }
    sb_stop();
    return 1;
}

// ================================================================ start / exit
static void shutdown() {
    sb_stop();
    kbd_done();
    graphics_stop();
    if (sb_dos_sel >= 0) { __dpmi_free_dos_memory(sb_dos_sel); sb_dos_sel = -1; }
    __FSEXT_set_function(1, nullptr);
    __FSEXT_set_function(2, nullptr);
    // What Doom said while the game was on screen (an error, say).
    if (con_len) {
        size_t n = con_len < sizeof con_log ? con_len : sizeof con_log;
        size_t start = con_len - n;
        con_len = 0;
        fflush(stdout);
        for (size_t i = 0; i < n; i++) fputc(con_log[(start + i) % sizeof con_log], stderr);
        fflush(stderr);
    }
}

static void on_fault(int sig) {
    shutdown();
    signal(sig, SIG_DFL);
    raise(sig);
}

static bool has_arg(int argc, char** argv, const char* a) {
    for (int i = 1; i < argc; i++) if (!strcasecmp(argv[i], a)) return true;
    return false;
}

// ================================================================ network
// The original network drivers (IPXSETUP.EXE, SERSETUP.EXE) start the game
// with "-net <address>": the linear address of their doomcom_t, in
// conventional memory. Doom (d_net.c) works on a copy; each command is
// copied across, run by the driver's real-mode interrupt, and copied back.
static uint32_t net_addr;
static doomcom_t net_dc;
// The vanilla packet plus the free-look extension, which follows it.
#define NET_DATA_MAX ((int)(sizeof(doomcom_t) - offsetof(doomcom_t, data)))
static_assert(offsetof(doomcom_t, dataext) == offsetof(doomcom_t, data) + sizeof(doomdata_t),
              "the extension must follow the packet");

static doomcom_t* dos_net_init() {
    if (!net_addr) return nullptr;
    dosmemget(net_addr, sizeof net_dc, &net_dc);
    printf("Network: driver at %lXh, interrupt %Xh, %d node(s)\n", (unsigned long)net_addr,
           (unsigned)(uint16_t)net_dc.intnum, net_dc.numnodes);
    return &net_dc;
}

static void dos_net_cmd() {
    const uint32_t a = net_addr;
    _farpokew(_dos_ds, a + offsetof(doomcom_t, command), (uint16_t)net_dc.command);
    if (net_dc.command == CMD_SEND) {
        _farpokew(_dos_ds, a + offsetof(doomcom_t, remotenode), (uint16_t)net_dc.remotenode);
        _farpokew(_dos_ds, a + offsetof(doomcom_t, datalength), (uint16_t)net_dc.datalength);
        int n = net_dc.datalength < 0 ? 0 : net_dc.datalength > NET_DATA_MAX ? NET_DATA_MAX : net_dc.datalength;
        dosmemput(&net_dc.data, n, a + offsetof(doomcom_t, data));
    }
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    __dpmi_int((uint8_t)net_dc.intnum, &r);
    if (net_dc.command == CMD_GET) {
        net_dc.remotenode = (int16_t)_farpeekw(_dos_ds, a + offsetof(doomcom_t, remotenode));
        net_dc.datalength = (int16_t)_farpeekw(_dos_ds, a + offsetof(doomcom_t, datalength));
        if (net_dc.remotenode != -1) {
            int n = net_dc.datalength;
            if (n < 0) n = 0;
            if (n > NET_DATA_MAX) n = NET_DATA_MAX;
            dosmemget(a + offsetof(doomcom_t, data), n, &net_dc.data);
        }
    }
}

extern "C" void doomgeneric_Create(int argc, char** argv);
extern "C" void doomgeneric_Tick(void);

int main(int argc, char** argv) {
    t_start = uclock();
    __djgpp_set_ctrl_c(0);                            // Ctrl+C is a key, not SIGINT
    printf("Doom for MS-DOS (doomgeneric, DJGPP)\n");

    vsync = has_arg(argc, argv, "-vsync");
    // The VESA mode for -hires / -widescreen: -vesa WxH or DOOMVID=WxH
    // (otherwise the best fit from the card's list).
    if (const char* e = getenv("DOOMVID")) sscanf(e, "%dx%d", &want_w, &want_h);
    for (int i = 1; i + 1 < argc; i++)
        if (!strcasecmp(argv[i], "-vesa")) sscanf(argv[i + 1], "%dx%d", &want_w, &want_h);
    for (int i = 1; i + 1 < argc; i++)
        if (!strcasecmp(argv[i], "-net")) {
            unsigned long a = strtoul(argv[i + 1], nullptr, 10);
            if (a && a < 0x100000 - sizeof(doomcom_t)) net_addr = (uint32_t)a;
            else printf("Network: -net %s isn't a conventional-memory address\n", argv[i + 1]);
        }
    I_VanillaNetInit = dos_net_init;
    I_VanillaNetCmd = dos_net_cmd;
    // Sound latency: -latency MS (default 120). More on a slow machine if it crackles.
    uint32_t latency_ms = 120;
    for (int i = 1; i + 1 < argc; i++)
        if (!strcasecmp(argv[i], "-latency")) { int v = atoi(argv[i + 1]); if (v >= 20 && v <= 600) latency_ms = (uint32_t)v; }

    bool want_sound = !has_arg(argc, argv, "-nosound") && !(has_arg(argc, argv, "-nosfx") && has_arg(argc, argv, "-nomusic"));
    bool sound = want_sound && sb_init() && sb_start();
    sb_ahead = sb_rate * latency_ms / 1000;
    if (sb_ahead > SB_RING / 3) sb_ahead = SB_RING / 3;
    mixer_init(sound);
    music_init();                                     // OPL3 tables: needs the FPU (or EMU387)
    mouse_init();
    kbd_init();

    atexit(shutdown);
    signal(SIGSEGV, on_fault); signal(SIGFPE, on_fault); signal(SIGILL, on_fault);
    signal(SIGABRT, on_fault); signal(SIGINT, on_fault);
    __FSEXT_set_function(1, con_hook);
    __FSEXT_set_function(2, con_hook);

    // Doom's zone: 6 MB unless told (-mb); more when DPMI has plenty, for
    // big levels and PWADs.
    static char* args[256];
    int n = 0;
    for (int i = 0; i < argc && n < 250; i++) args[n++] = argv[i];
    if (!has_arg(argc, argv, "-mb")) {
        __dpmi_free_mem_info fi;
        unsigned long big = 0;
        if (__dpmi_get_free_memory_information(&fi) == 0 && fi.largest_available_free_block_in_bytes != 0xFFFFFFFFul)
            big = fi.largest_available_free_block_in_bytes >> 20;
        if (big >= 48) { args[n++] = (char*)"-mb"; args[n++] = (char*)"32"; }
        else if (big >= 24) { args[n++] = (char*)"-mb"; args[n++] = (char*)"16"; }
        printf("Memory: %lu MB free\n", big);
    }
    args[n] = nullptr;

    doomgeneric_Create(n, args);
    for (;;) doomgeneric_Tick();
}
