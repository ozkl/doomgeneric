// ./setup: SETUP.EXE for the Linux build. Blue text-mode screens (drawn in
// an SDL window, 80x25 like the real thing, so it can see any key: Ctrl,
// Shift and Alt on their own, which a terminal can't) to change the keys,
// the mouse, video and sound settings and the chat macros. It edits
// default.cfg and dgextra.cfg in the current directory, the files the game
// reads, keeping keys as DOS scan codes as SETUP.EXE did; anything it
// doesn't know about is left as it is.
//
//   ./setup              then Save and run DOOM, or Save and exit
//   ./setup -iwad x.wad  options after setup's own go to the game it runs
#include <SDL.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <vector>
#include "font.h"
#include "keymap.h"

// ================================================================ the screen
// 80x25 cells, the VGA's 16 colours. Characters below 32 are the
// line-drawing and shading glyphs, drawn here (font.h is ASCII only).
enum {
    C_BLACK, C_BLUE, C_GREEN, C_CYAN, C_RED, C_MAGENTA, C_BROWN, C_LGRAY,
    C_DGRAY, C_LBLUE, C_LGREEN, C_LCYAN, C_LRED, C_LMAGENTA, C_YELLOW, C_WHITE
};
static const uint32_t VGA[16] = {0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
                                 0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};
enum {
    G_TL2 = 1, G_TR2, G_BL2, G_BR2, G_H2, G_V2,     // double lines
    G_H1, G_LT, G_RT,                               // a single line across a box: ╟─╢
    G_SHADE1, G_SHADE2, G_SHADE3,                   // ░ ▒ ▓
    G_LEFT, G_RIGHT, G_UP, G_DOWN,                  // ◄ ► ▲ ▼
};

struct Cell { uint8_t ch, fg, bg; };
static const int W = 80, H = 25;
static Cell scr[H][W];

static void put(int y, int x, int ch, int fg, int bg) {
    if (y >= 0 && y < H && x >= 0 && x < W) scr[y][x] = {(uint8_t)ch, (uint8_t)fg, (uint8_t)bg};
}
static void text(int y, int x, const std::string& s, int fg, int bg) {
    for (size_t i = 0; i < s.size(); i++) put(y, x + (int)i, (unsigned char)s[i], fg, bg);
}
static void fill(int y, int x, int h, int w, int ch, int fg, int bg) {
    for (int r = 0; r < h; r++) for (int c = 0; c < w; c++) put(y + r, x + c, ch, fg, bg);
}
static void center(int y, const std::string& s, int fg, int bg) { text(y, (W - (int)s.size()) / 2, s, fg, bg); }

// A double-lined box with a title, and a shadow as SETUP.EXE's.
static void box(int y, int x, int h, int w, const std::string& title, int fg = C_WHITE, int bg = C_BLUE) {
    for (int r = 1; r <= h; r++) {                 // shadow: dark, right and below
        for (int c = 2; c <= w + 1; c++) {
            int yy = y + r, xx = x + c;
            if ((r == h || c >= w) && yy < H && xx < W) { scr[yy][xx].fg = C_DGRAY; scr[yy][xx].bg = C_BLACK; }
        }
    }
    fill(y, x, h, w, ' ', fg, bg);
    put(y, x, G_TL2, fg, bg); put(y, x + w - 1, G_TR2, fg, bg);
    put(y + h - 1, x, G_BL2, fg, bg); put(y + h - 1, x + w - 1, G_BR2, fg, bg);
    for (int c = 1; c < w - 1; c++) { put(y, x + c, G_H2, fg, bg); put(y + h - 1, x + c, G_H2, fg, bg); }
    for (int r = 1; r < h - 1; r++) { put(y + r, x, G_V2, fg, bg); put(y + r, x + w - 1, G_V2, fg, bg); }
    if (!title.empty()) text(y, x + (w - (int)title.size() - 2) / 2, " " + title + " ", C_YELLOW, bg);
}
static void rule(int y, int x, int w, int fg, int bg) {
    put(y, x, G_LT, fg, bg); put(y, x + w - 1, G_RT, fg, bg);
    for (int c = 1; c < w - 1; c++) put(y, x + c, G_H1, fg, bg);
}

// The backdrop: a shaded blue field, a title bar, a help line.
static void backdrop(const std::string& help) {
    fill(0, 0, H, W, G_SHADE1, C_LBLUE, C_BLUE);
    fill(0, 0, 1, W, ' ', C_BLACK, C_LGRAY);
    text(0, 1, "DOOM Setup", C_BLACK, C_LGRAY);
    text(0, W - 22, "baremetaldoom, Linux", C_BLACK, C_LGRAY);
    fill(H - 1, 0, 1, W, ' ', C_BLACK, C_LGRAY);
    text(H - 1, 1, help, C_BLACK, C_LGRAY);
}

// ---------------------------------------------------------------- drawing it
static uint8_t glyph_row(int ch, int y) {
    if (ch >= 32 && ch < 127) return font8x16[ch - 32][y];
    auto hl = [&](int row, int x0, int x1) -> uint8_t {
        if (y != row) return 0;
        uint8_t m = 0;
        for (int x = x0; x <= x1; x++) m |= 0x80 >> x;
        return m;
    };
    auto vl = [&](int col, int y0, int y1) -> uint8_t { return y >= y0 && y <= y1 ? (uint8_t)(0x80 >> col) : 0; };
    switch (ch) {          // double lines at rows 6 and 9, columns 2 and 5
    case G_H2: return hl(6, 0, 7) | hl(9, 0, 7);
    case G_V2: return vl(2, 0, 15) | vl(5, 0, 15);
    case G_TL2: return hl(6, 2, 7) | hl(9, 5, 7) | vl(2, 6, 15) | vl(5, 9, 15);
    case G_TR2: return hl(6, 0, 5) | hl(9, 0, 2) | vl(5, 6, 15) | vl(2, 9, 15);
    case G_BL2: return hl(9, 2, 7) | hl(6, 5, 7) | vl(2, 0, 9) | vl(5, 0, 6);
    case G_BR2: return hl(9, 0, 5) | hl(6, 0, 2) | vl(5, 0, 9) | vl(2, 0, 6);
    case G_H1: return hl(8, 0, 7);
    case G_LT: return vl(2, 0, 15) | vl(5, 0, 15) | hl(8, 5, 7);
    case G_RT: return vl(2, 0, 15) | vl(5, 0, 15) | hl(8, 0, 2);
    case G_SHADE1: return (y % 2 == 0) ? (y % 4 == 0 ? 0x88 : 0x22) : 0;
    case G_SHADE2: return (y % 2 == 0) ? 0xAA : 0x55;
    case G_SHADE3: return (uint8_t)~((y % 2 == 0) ? (y % 4 == 0 ? 0x88 : 0x22) : 0);
    case G_RIGHT: case G_LEFT: {               // 9 rows tall, up to 5 wide
        int d = y - 4;
        if (d < 0 || d > 8) return 0;
        int n = 5 - abs(d - 4);                // 1..5 columns
        uint8_t m = 0;
        for (int i = 0; i < n; i++) m |= ch == G_RIGHT ? 0x80 >> (1 + i) : 0x80 >> (6 - i);
        return m;
    }
    case G_UP: { int d = y - 5; if (d < 0 || d > 4) return 0; return (uint8_t)(((0xFF >> (7 - 2 * d)) & 0xFF) << (3 - d)); }
    case G_DOWN: { int d = 9 - y; if (d < 0 || d > 4) return 0; return (uint8_t)(((0xFF >> (7 - 2 * d)) & 0xFF) << (3 - d)); }
    }
    return 0;
}

static SDL_Window* win;
static SDL_Renderer* ren;
static SDL_Texture* tex;

static void present() {
    void* px;
    int pitch;
    if (SDL_LockTexture(tex, nullptr, &px, &pitch) != 0) return;
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++) {
            const Cell& k = scr[r][c];
            uint32_t fg = 0xFF000000u | VGA[k.fg & 15], bg = 0xFF000000u | VGA[k.bg & 15];
            for (int y = 0; y < 16; y++) {
                uint8_t bits = glyph_row(k.ch, y);
                uint32_t* d = (uint32_t*)((uint8_t*)px + (r * 16 + y) * pitch) + c * 8;
                for (int x = 0; x < 8; x++) d[x] = (bits & (0x80 >> x)) ? fg : bg;
            }
        }
    SDL_UnlockTexture(tex);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, nullptr, nullptr);
    SDL_RenderPresent(ren);
}

// ================================================================ the settings
// What the game keeps (m_config.c), with the defaults it would write. File 0:
// default.cfg, 1: dgextra.cfg.
struct Setting { const char* name; int file; const char* def; };
static const Setting SETTINGS[] = {
    {"mouse_sensitivity", 0, "5"}, {"mouse_freelook", 0, "1"}, {"mouse_invert", 0, "0"},
    {"sfx_volume", 0, "8"}, {"music_volume", 0, "8"}, {"show_messages", 0, "1"},
    {"key_right", 0, "77"}, {"key_left", 0, "75"}, {"key_up", 0, "17"}, {"key_down", 0, "31"},
    {"key_strafeleft", 0, "30"}, {"key_straferight", 0, "32"}, {"key_fire", 0, "29"}, {"key_use", 0, "57"},
    {"key_strafe", 0, "56"}, {"key_speed", 0, "54"},
    {"mouseb_fire", 0, "0"}, {"mouseb_strafe", 0, "1"}, {"mouseb_forward", 0, "2"},
    {"screenblocks", 0, "10"}, {"screen_scale", 0, "1"}, {"screen_widescreen", 0, "0"},
    {"fullscreen", 0, "0"}, {"key_fullscreen", 0, "41"}, {"detaillevel", 0, "0"}, {"snd_channels", 0, "8"},
    {"chatmacro0", 0, "\"No\""}, {"chatmacro1", 0, "\"I'm ready to kick butt!\""}, {"chatmacro2", 0, "\"I'm OK.\""},
    {"chatmacro3", 0, "\"I'm not looking too good!\""}, {"chatmacro4", 0, "\"Help!\""},
    {"chatmacro5", 0, "\"You suck!\""}, {"chatmacro6", 0, "\"Next time, scumbag...\""},
    {"chatmacro7", 0, "\"Come here!\""}, {"chatmacro8", 0, "\"I'll take care of it.\""}, {"chatmacro9", 0, "\"Yes\""},
    {"mouseb_use", 1, "-1"}, {"mouseb_backward", 1, "-1"}, {"mouseb_strafeleft", 1, "-1"},
    {"mouseb_straferight", 1, "-1"}, {"mouseb_prevweapon", 1, "-1"}, {"mouseb_nextweapon", 1, "-1"},
    {"dclick_use", 1, "1"}, {"key_pause", 1, "69"}, {"key_map_toggle", 1, "15"},
    {"key_weapon1", 1, "2"}, {"key_weapon2", 1, "3"}, {"key_weapon3", 1, "4"}, {"key_weapon4", 1, "5"},
    {"key_weapon5", 1, "6"}, {"key_weapon6", 1, "7"}, {"key_weapon7", 1, "8"}, {"key_weapon8", 1, "9"},
    {"key_prevweapon", 1, "0"}, {"key_nextweapon", 1, "0"}, {"key_multi_msg", 1, "20"},
    {"key_menu_quit", 1, "68"}, {"key_menu_qsave", 1, "64"}, {"key_menu_qload", 1, "67"},
    {"key_menu_gamma", 1, "87"}, {"key_spy", 1, "88"}, {"key_menu_screenshot", 1, "0"}, {"show_endoom", 1, "1"},
};
static const char* const FILES[2] = {"default.cfg", "dgextra.cfg"};

struct Line { std::string name, value, raw; };
static std::vector<Line> lines[2];
static std::vector<std::string> values;          // per SETTINGS entry
static std::vector<std::string> loaded;          // as read, to tell if anything changed
static const int NSET = sizeof SETTINGS / sizeof SETTINGS[0];

static int idx(const char* name) {
    for (int i = 0; i < NSET; i++) if (!strcmp(SETTINGS[i].name, name)) return i;
    fprintf(stderr, "setup: unknown setting %s\n", name);
    abort();
}
static int geti(const char* name) { return atoi(values[idx(name)].c_str()); }
static void seti(const char* name, int v) { values[idx(name)] = std::to_string(v); }
static std::string gets(const char* name) {
    std::string v = values[idx(name)];
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    return v;
}
static void sets(const char* name, const std::string& v) { values[idx(name)] = "\"" + v + "\""; }

static void load_config() {
    values.clear();
    for (int i = 0; i < NSET; i++) values.push_back(SETTINGS[i].def);
    for (int f = 0; f < 2; f++) {
        lines[f].clear();
        FILE* fp = fopen(FILES[f], "r");
        if (!fp) continue;
        char buf[1024];
        while (fgets(buf, sizeof buf, fp)) {
            std::string raw = buf;
            while (!raw.empty() && (raw.back() == '\n' || raw.back() == '\r')) raw.pop_back();
            Line l;
            l.raw = raw;
            size_t a = raw.find_first_not_of(" \t");
            size_t b = a == std::string::npos ? a : raw.find_first_of(" \t", a);
            if (b != std::string::npos) {
                l.name = raw.substr(a, b - a);
                size_t c = raw.find_first_not_of(" \t", b);
                l.value = c == std::string::npos ? "" : raw.substr(c);
                for (int i = 0; i < NSET; i++)
                    if (SETTINGS[i].file == f && l.name == SETTINGS[i].name) values[i] = l.value;
            }
            lines[f].push_back(l);
        }
        fclose(fp);
    }
    loaded = values;
}

static bool save_config() {
    for (int f = 0; f < 2; f++) {
        std::vector<bool> done(NSET, false);
        std::vector<std::string> out;
        for (auto& l : lines[f]) {
            int i = -1;
            for (int k = 0; k < NSET; k++) if (SETTINGS[k].file == f && l.name == SETTINGS[k].name) i = k;
            if (i < 0) { out.push_back(l.raw); continue; }
            done[i] = true;
            char b[1200];
            snprintf(b, sizeof b, "%-30s%s", l.name.c_str(), values[i].c_str());
            out.push_back(b);
        }
        // Not in the file yet: only what's been changed from the default
        // (the game writes the rest itself when it quits).
        for (int i = 0; i < NSET; i++) {
            if (SETTINGS[i].file != f || done[i] || values[i] == SETTINGS[i].def) continue;
            char b[1200];
            snprintf(b, sizeof b, "%-30s%s", SETTINGS[i].name, values[i].c_str());
            out.push_back(b);
        }
        if (out.empty()) continue;
        std::string tmp = std::string(FILES[f]) + ".new";
        FILE* fp = fopen(tmp.c_str(), "w");
        if (!fp) return false;
        for (auto& s : out) fprintf(fp, "%s\n", s.c_str());
        if (fclose(fp) != 0 || rename(tmp.c_str(), FILES[f]) != 0) return false;
    }
    load_config();
    return true;
}

static bool changed() { return values != loaded; }

// ---------------------------------------------------------------- key names
// As SETUP.EXE named them. 0: none.
static std::string key_name(int sc) {
    static const char* const names[0x59] = {
        "NONE", "ESC", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "BACKSPACE", "TAB",
        "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "[", "]", "ENTER", "CTRL", "A", "S",
        "D", "F", "G", "H", "J", "K", "L", ";", "'", "` (BACKQUOTE)", "LEFT SHIFT", "\\", "Z", "X", "C", "V",
        "B", "N", "M", ",", ".", "/", "SHIFT", "KEYPAD *", "ALT", "SPACE", "CAPS LOCK", "F1", "F2", "F3", "F4", "F5",
        "F6", "F7", "F8", "F9", "F10", "PAUSE", "SCROLL LOCK", "HOME", "UP ARROW", "PAGE UP", "KEYPAD -", "LEFT ARROW", "KEYPAD 5", "RIGHT ARROW", "KEYPAD +", "END",
        "DOWN ARROW", "PAGE DOWN", "INSERT", "DELETE", "", "", "", "F11", "F12",
    };
    if (sc >= 0 && sc < 0x59 && names[sc][0]) return names[sc];
    char b[16];
    snprintf(b, sizeof b, "KEY %d", sc);
    return b;
}

// A key pressed in the window, as default.cfg wants it. -1: Esc (cancel),
// -2: a key the game can't take from the file (keypad *, 5, +, Num Lock:
// its table maps those to other keys), 0: nothing usable.
static int scan_of(const SDL_Keysym& k) {
    if (k.scancode == SDL_SCANCODE_ESCAPE) return -1;
    if (k.scancode == SDL_SCANCODE_PAUSE) return 0x45;          // the file's PAUSE (m_config.c: scantokey)
    if (k.scancode == SDL_SCANCODE_NUMLOCKCLEAR) return -2;
    bool ext;
    int sc = set1(k.scancode, &ext);
    if (!sc) return 0;
    if (sc == 0x2A) sc = 0x36;                                  // either Shift: the game has one (RSHIFT, 54)
    if (!ext && (sc == 0x37 || sc == 0x4C || sc == 0x4E)) return -2;
    return sc;
}

// ================================================================ menus
struct Item {
    enum Kind { KEY, CHOICE, NUMBER, TEXT, BUTTON, HEADER, GAP } kind;
    std::string label;
    const char* var = nullptr;
    std::vector<std::string> choices;           // CHOICE: label per value
    std::vector<int> values;                    // CHOICE: the values (or 0..n-1)
    int lo = 0, hi = 0;                         // NUMBER
    int id = 0;                                 // BUTTON
    int maxlen = 40;                            // TEXT
};

static Item key(const std::string& l, const char* v) { Item i{Item::KEY, l}; i.var = v; return i; }
static Item choice(const std::string& l, const char* v, std::vector<std::string> c, std::vector<int> vals = {}) {
    Item i{Item::CHOICE, l};
    i.var = v;
    i.choices = c;
    if (vals.empty()) for (int k = 0; k < (int)c.size(); k++) vals.push_back(k);
    i.values = vals;
    return i;
}
static Item number(const std::string& l, const char* v, int lo, int hi) { Item i{Item::NUMBER, l}; i.var = v; i.lo = lo; i.hi = hi; return i; }
static Item textv(const std::string& l, const char* v, int maxlen) { Item i{Item::TEXT, l}; i.var = v; i.maxlen = maxlen; return i; }
static Item button(const std::string& l, int id) { Item i{Item::BUTTON, l}; i.id = id; return i; }
static Item header(const std::string& l) { return Item{Item::HEADER, l}; }
static Item gap() { return Item{Item::GAP, ""}; }

static const std::vector<std::string> YESNO = {"No", "Yes"};
static const std::vector<std::string> BUTTONS = {"(none)", "Left button", "Right button", "Middle button"};
static const std::vector<int> BUTTON_VALS = {-1, 0, 1, 2};

static std::string value_of(const Item& it) {
    switch (it.kind) {
    case Item::KEY: return key_name(geti(it.var));
    case Item::CHOICE: {
        int v = geti(it.var);
        for (size_t k = 0; k < it.values.size(); k++) if (it.values[k] == v) return it.choices[k];
        return std::to_string(v);
    }
    case Item::NUMBER: return std::to_string(geti(it.var));
    case Item::TEXT: return gets(it.var);
    default: return "";
    }
}

static bool selectable(const Item& it) { return it.kind != Item::HEADER && it.kind != Item::GAP; }

// Messages shown under a menu for a moment.
static std::string note;
static int note_color = C_YELLOW;
static uint32_t note_until;
static void say(const std::string& s, int color = C_YELLOW) { note = s; note_color = color; note_until = SDL_GetTicks() + 4000; }

// Two keys in the game can't be the same: the one that had it gets this
// one's old key, as swapping two wires. The groups of keys checked
// together: what's pressed in play (movement, actions, weapons, the map...).
static const char* const PLAY_KEYS[] = {
    "key_up", "key_down", "key_left", "key_right", "key_strafeleft", "key_straferight", "key_speed",
    "key_strafe", "key_fire", "key_use", "key_prevweapon", "key_nextweapon", "key_weapon1", "key_weapon2",
    "key_weapon3", "key_weapon4", "key_weapon5", "key_weapon6", "key_weapon7", "key_weapon8",
    "key_map_toggle", "key_fullscreen", "key_pause", "key_multi_msg",
};
static const char* const MOUSE_BUTTONS[] = {
    "mouseb_fire", "mouseb_strafe", "mouseb_forward", "mouseb_backward", "mouseb_use",
    "mouseb_strafeleft", "mouseb_straferight", "mouseb_prevweapon", "mouseb_nextweapon",
};

static std::string label_of(const char* var);

static void assign_key(const char* var, int sc) {
    int old = geti(var);
    for (const char* other : PLAY_KEYS) {
        if (!strcmp(other, var) || geti(other) != sc || !sc) continue;
        seti(other, old);
        say(label_of(other) + " now uses " + key_name(old) + " (it had " + key_name(sc) + ")", C_LCYAN);
    }
    seti(var, sc);
}

static void assign_button(const char* var, int b) {
    int old = geti(var);
    if (b >= 0)
        for (const char* other : MOUSE_BUTTONS) {
            if (!strcmp(other, var) || geti(other) != b) continue;
            seti(other, old);
            say(label_of(other) + (old < 0 ? " is now off" : " moved to the button this one had"), C_LCYAN);
        }
    seti(var, b);
}

// ---------------------------------------------------------------- the screens
enum { B_KEYS = 1, B_MOUSE, B_VIDEO, B_SOUND, B_CHAT, B_DEFAULTS, B_RUN, B_SAVE, B_QUIT, B_NETWORK, B_BACK };

static std::vector<Item> keys_menu() {
    return {
        header("Moving"),
        key("Move forward", "key_up"), key("Move backward", "key_down"),
        key("Turn left", "key_left"), key("Turn right", "key_right"),
        key("Strafe left", "key_strafeleft"), key("Strafe right", "key_straferight"),
        key("Run (hold)", "key_speed"), key("Strafe (hold)", "key_strafe"),
        header("Doing"),
        key("Fire", "key_fire"), key("Use / open", "key_use"),
        key("Previous weapon", "key_prevweapon"), key("Next weapon", "key_nextweapon"),
        key("Weapon 1: fist", "key_weapon1"), key("Weapon 2: pistol", "key_weapon2"),
        key("Weapon 3: shotgun", "key_weapon3"), key("Weapon 4: chaingun", "key_weapon4"),
        key("Weapon 5: rocket launcher", "key_weapon5"), key("Weapon 6: plasma rifle", "key_weapon6"),
        key("Weapon 7: BFG 9000", "key_weapon7"), key("Weapon 8: chainsaw", "key_weapon8"),
        header("Other"),
        key("Automap", "key_map_toggle"), key("Fullscreen", "key_fullscreen"), key("Pause", "key_pause"),
        key("Send a message (net game)", "key_multi_msg"),
        key("Quick save", "key_menu_qsave"), key("Quick load", "key_menu_qload"),
        key("Brightness", "key_menu_gamma"), key("View another player (co-op)", "key_spy"),
        key("Quit", "key_menu_quit"),
        gap(), button("Back to the main menu", B_BACK),
    };
}

static std::vector<Item> mouse_menu() {
    return {
        number("Sensitivity (0-9)", "mouse_sensitivity", 0, 9),
        choice("Free look (mouse aims)", "mouse_freelook", {"Off: mouse moves, autoaim", "On: mouse looks, no autoaim"}),
        choice("Invert looking", "mouse_invert", {"No: push forward to look up", "Yes: pull back to look up"}),
        header("Buttons"),
        choice("Fire", "mouseb_fire", BUTTONS, BUTTON_VALS),
        choice("Strafe (hold)", "mouseb_strafe", BUTTONS, BUTTON_VALS),
        choice("Move forward", "mouseb_forward", BUTTONS, BUTTON_VALS),
        choice("Move backward", "mouseb_backward", BUTTONS, BUTTON_VALS),
        choice("Use / open", "mouseb_use", BUTTONS, BUTTON_VALS),
        choice("Double-click to use", "dclick_use", YESNO),
        gap(), button("Back to the main menu", B_BACK),
    };
}

static std::vector<Item> video_menu() {
    return {
        choice("Fullscreen", "fullscreen", {"No: a window", "Yes"}),
        choice("Widescreen (16:9)", "screen_widescreen", {"No: 4:3, as in 1993", "Yes: more at the sides"}),
        choice("Resolution", "screen_scale", {"320x200 (original)", "640x400", "960x600"}, {1, 2, 3}),
        choice("Screen size", "screenblocks",
               {"3 (smallest)", "4", "5", "6", "7", "8", "9", "10: full view, status bar", "11: no status bar"},
               {3, 4, 5, 6, 7, 8, 9, 10, 11}),
        choice("Detail", "detaillevel", {"High", "Low (blocky, faster)"}),
        choice("Messages", "show_messages", {"Off", "On"}),
        choice("ENDOOM screen on exit", "show_endoom", YESNO),
        gap(), button("Back to the main menu", B_BACK),
    };
}

static std::vector<Item> sound_menu() {
    return {
        number("Sound effects volume (0-15)", "sfx_volume", 0, 15),
        number("Music volume (0-15)", "music_volume", 0, 15),
        number("Sounds at once (1-16)", "snd_channels", 1, 16),
        gap(), button("Back to the main menu", B_BACK),
    };
}

static std::vector<Item> chat_menu() {
    std::vector<Item> v;
    v.push_back(header("Alt+0 .. Alt+9 while typing a message"));
    for (int i = 0; i < 10; i++) {
        static char names[10][12];
        snprintf(names[i], sizeof names[i], "chatmacro%d", i);
        v.push_back(textv("Macro " + std::to_string(i), names[i], 40));
    }
    v.push_back(gap());
    v.push_back(button("Back to the main menu", B_BACK));
    return v;
}

static bool have_tty;
static std::vector<Item> main_menu() {
    std::vector<Item> v = {
        button("Keyboard", B_KEYS), button("Mouse", B_MOUSE), button("Video", B_VIDEO),
        button("Sound", B_SOUND), button("Chat macros", B_CHAT),
    };
    if (have_tty) v.push_back(button("Network game (ipxsetup)", B_NETWORK));
    v.push_back(gap());
    v.push_back(button("Restore the default settings", B_DEFAULTS));
    v.push_back(gap());
    v.push_back(button("Save settings and run DOOM", B_RUN));
    v.push_back(button("Save settings and exit", B_SAVE));
    v.push_back(button("Exit without saving", B_QUIT));
    return v;
}

static std::string label_of(const char* var) {
    for (auto* f : {keys_menu, mouse_menu})
        for (auto& it : f())
            if (it.var && !strcmp(it.var, var)) return it.label;
    return var;
}

// ---------------------------------------------------------------- input
static bool fullscreen_win;
static void toggle_window() {
    fullscreen_win = !fullscreen_win;
    SDL_SetWindowFullscreen(win, fullscreen_win ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

enum { K_NONE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_ENTER, K_ESC, K_BS, K_DEL, K_QUIT, K_CHAR };
struct Key { int k; char c; SDL_Keysym sym; };

static Key wait_key(int timeout_ms = -1) {
    SDL_Event e;
    uint32_t t0 = SDL_GetTicks();
    for (;;) {
        int left = timeout_ms < 0 ? 200 : timeout_ms - (int)(SDL_GetTicks() - t0);
        if (timeout_ms >= 0 && left <= 0) return {K_NONE, 0, {}};
        if (!SDL_WaitEventTimeout(&e, left < 200 ? left : 200)) {
            if (timeout_ms < 0) return {K_NONE, 0, {}};             // redraw (the note's timer)
            continue;
        }
        if (e.type == SDL_QUIT) return {K_QUIT, 0, {}};
        if (e.type == SDL_WINDOWEVENT) return {K_NONE, 0, {}};
        if (e.type == SDL_TEXTINPUT) {
            unsigned char c = (unsigned char)e.text.text[0];
            if (c >= 32 && c < 127) return {K_CHAR, (char)c, {}};
            continue;
        }
        if (e.type != SDL_KEYDOWN) continue;
        const SDL_Keysym& s = e.key.keysym;
        if (s.scancode == SDL_SCANCODE_RETURN && (s.mod & KMOD_ALT)) { toggle_window(); continue; }
        Key k{K_NONE, 0, s};
        switch (s.scancode) {
        case SDL_SCANCODE_UP: case SDL_SCANCODE_KP_8: k.k = K_UP; break;
        case SDL_SCANCODE_DOWN: case SDL_SCANCODE_KP_2: k.k = K_DOWN; break;
        case SDL_SCANCODE_LEFT: case SDL_SCANCODE_KP_4: k.k = K_LEFT; break;
        case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_KP_6: k.k = K_RIGHT; break;
        case SDL_SCANCODE_HOME: k.k = K_HOME; break;
        case SDL_SCANCODE_END: k.k = K_END; break;
        case SDL_SCANCODE_PAGEUP: k.k = K_PGUP; break;
        case SDL_SCANCODE_PAGEDOWN: k.k = K_PGDN; break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: k.k = K_ENTER; break;
        case SDL_SCANCODE_ESCAPE: k.k = K_ESC; break;
        case SDL_SCANCODE_BACKSPACE: k.k = K_BS; break;
        case SDL_SCANCODE_DELETE: k.k = K_DEL; break;
        case SDL_SCANCODE_TAB: k.k = (s.mod & KMOD_SHIFT) ? K_UP : K_DOWN; break;
        default: continue;
        }
        return k;
    }
}

// The key for an action: a box, then any key (Esc: never mind).
static void capture_key(const Item& it) {
    std::string prompt = "Press the key for " + it.label;
    for (;;) {
        int bw = (int)prompt.size() + 8 < 50 ? 50 : (int)prompt.size() + 8;
        int bx = (W - bw) / 2;
        box(9, bx, 7, bw, "Keyboard", C_WHITE, C_RED);
        center(11, prompt, C_WHITE, C_RED);
        center(12, "now: " + key_name(geti(it.var)), C_YELLOW, C_RED);
        center(13, "Esc: leave it as it is", C_LGRAY, C_RED);
        present();
        SDL_Event e;
        if (!SDL_WaitEvent(&e)) return;
        if (e.type == SDL_QUIT) { SDL_PushEvent(&e); return; }
        if (e.type != SDL_KEYDOWN || e.key.repeat) continue;
        int sc = scan_of(e.key.keysym);
        if (sc == -1) return;
        if (sc == -2) { prompt = "The game can't use that key. Another one:"; continue; }
        if (sc == 0) { prompt = "That key isn't one the game knows. Another one:"; continue; }
        assign_key(it.var, sc);
        return;
    }
}

// A line of text, typed in place.
static void edit_text(const Item& it, int y, int x, int w) {
    std::string s = gets(it.var);
    SDL_StartTextInput();
    for (;;) {
        std::string shown = s.size() > (size_t)(w - 1) ? s.substr(s.size() - (w - 1)) : s;
        fill(y, x, 1, w, ' ', C_WHITE, C_BLACK);
        text(y, x, shown + "_", C_WHITE, C_BLACK);
        fill(H - 1, 0, 1, W, ' ', C_BLACK, C_LGRAY);
        text(H - 1, 1, "Type, then Enter to keep it   Esc: as it was", C_BLACK, C_LGRAY);
        present();
        Key k = wait_key(-1);
        if (k.k == K_QUIT) { SDL_Event q; q.type = SDL_QUIT; SDL_PushEvent(&q); break; }
        if (k.k == K_ENTER) { sets(it.var, s); break; }
        if (k.k == K_ESC) break;
        if (k.k == K_BS && !s.empty()) s.pop_back();
        if (k.k == K_CHAR && (int)s.size() < it.maxlen && k.c != '"') s += k.c;
    }
    SDL_StopTextInput();
}

static bool confirm(const std::string& q) {
    int bw = (int)q.size() + 8 < 44 ? 44 : (int)q.size() + 8;
    box(9, (W - bw) / 2, 6, bw, "", C_WHITE, C_RED);
    center(11, q, C_WHITE, C_RED);
    center(12, "Y / N", C_YELLOW, C_RED);
    present();
    for (;;) {
        SDL_Event e;
        if (!SDL_WaitEvent(&e)) return false;
        if (e.type == SDL_QUIT) return true;
        if (e.type != SDL_KEYDOWN) continue;
        SDL_Keycode c = e.key.keysym.sym;
        if (c == SDLK_y || c == SDLK_RETURN) return true;
        if (c == SDLK_n || c == SDLK_ESCAPE) return false;
    }
}

// A menu: returns the BUTTON id chosen, or B_BACK (Esc), or B_QUIT.
static int run_menu(const std::string& title, std::vector<Item> (*make)(), int* remember, int label_w, int width) {
    int sel = *remember;
    int top_row = 0;
    for (;;) {
        std::vector<Item> items = make();
        int n = (int)items.size();
        if (sel >= n) sel = n - 1;
        if (sel < 0) sel = 0;
        while (!selectable(items[sel])) sel = (sel + 1) % n;

        bool is_main = make == main_menu;
        backdrop(is_main ? "Up/Down: choose   Enter: go   Esc: exit"
                         : "Up/Down: choose   Left/Right: change   Enter: set   Del: none   Esc: back");
        int visible = n < H - 8 ? n : H - 8;
        if (sel < top_row) top_row = sel;
        if (sel >= top_row + visible) top_row = sel - visible + 1;
        int bh = visible + 4;
        int by = (H - bh) / 2;
        if (by < 2) by = 2;
        int bx = (W - width) / 2;
        box(by, bx, bh, width, title);
        if (top_row > 0) put(by + 1, bx + width - 3, G_UP, C_YELLOW, C_BLUE);
        if (top_row + visible < n) put(by + bh - 2, bx + width - 3, G_DOWN, C_YELLOW, C_BLUE);
        for (int r = 0; r < visible; r++) {
            const Item& it = items[top_row + r];
            int y = by + 2 + r;
            bool on = top_row + r == sel;
            int fg = on ? C_BLACK : C_WHITE, bg = on ? C_LGRAY : C_BLUE;
            switch (it.kind) {
            case Item::GAP: break;
            case Item::HEADER:
                rule(y, bx, width, C_WHITE, C_BLUE);
                text(y, bx + 3, " " + it.label + " ", C_LCYAN, C_BLUE);
                break;
            case Item::BUTTON:
                if (is_main) {
                    fill(y, bx + 3, 1, width - 6, ' ', fg, bg);
                    center(y, it.label, on ? C_BLACK : C_YELLOW, bg);
                } else {
                    fill(y, bx + 3, 1, width - 6, ' ', fg, bg);
                    text(y, bx + 4, it.label, on ? C_BLACK : C_YELLOW, bg);
                }
                break;
            default: {
                fill(y, bx + 2, 1, width - 4, ' ', fg, bg);
                text(y, bx + 3, it.label, fg, bg);
                std::string v = value_of(it);
                int vx = bx + 3 + label_w, vw = width - 6 - label_w;
                if ((int)v.size() > vw) v = v.substr(0, vw);
                int vfg = on ? C_BLACK : (it.kind == Item::KEY && geti(it.var) == 0 ? C_DGRAY : C_LCYAN);
                if (on && it.kind != Item::KEY && it.kind != Item::TEXT) {
                    put(y, vx - 2, G_LEFT, C_BLACK, bg);
                    put(y, vx + (int)v.size() + 1, G_RIGHT, C_BLACK, bg);
                }
                text(y, vx, v, vfg, bg);
            }
            }
        }
        if (!note.empty() && SDL_GetTicks() < note_until) {
            int nw = (int)note.size() + 4;
            if (nw > W - 2) nw = W - 2;
            int ny = by + bh + 1;                      // under the box, or on the help line
            if (ny > H - 2) { ny = H - 1; nw = W; }
            fill(ny, (W - nw) / 2, 1, nw, ' ', C_WHITE, C_BLACK);
            text(ny, (W - nw) / 2 + 2, note.substr(0, nw - 4), note_color, C_BLACK);
        }
        if (is_main && changed()) text(H - 1, W - 22, "settings changed", C_RED, C_LGRAY);
        present();

        Key k = wait_key();
        Item& it = items[sel];
        auto move = [&](int d) { for (int i = 0; i < n; i++) { sel = (sel + d + n) % n; if (selectable(items[sel])) break; } };
        auto step = [&](int d) {
            if (it.kind == Item::CHOICE) {
                int v = geti(it.var), at = 0;
                for (size_t q = 0; q < it.values.size(); q++) if (it.values[q] == v) at = (int)q;
                int m = (int)it.values.size();
                int nv = it.values[(at + d + m) % m];
                bool is_button = false;
                for (const char* b : MOUSE_BUTTONS) if (!strcmp(b, it.var)) is_button = true;
                if (is_button) assign_button(it.var, nv); else seti(it.var, nv);
            } else if (it.kind == Item::NUMBER) {
                int v = geti(it.var) + d;
                if (v < it.lo) v = it.lo;
                if (v > it.hi) v = it.hi;
                seti(it.var, v);
            }
        };
        switch (k.k) {
        case K_QUIT: *remember = sel; return B_QUIT;
        case K_ESC: *remember = sel; return B_BACK;
        case K_UP: move(-1); break;
        case K_DOWN: move(1); break;
        case K_PGUP: for (int i = 0; i < visible - 1; i++) move(-1); break;
        case K_PGDN: for (int i = 0; i < visible - 1; i++) move(1); break;
        case K_HOME: sel = 0; while (!selectable(items[sel])) sel++; break;
        case K_END: sel = n - 1; while (!selectable(items[sel])) sel--; break;
        case K_LEFT: step(-1); break;
        case K_RIGHT: step(1); break;
        case K_DEL:
            if (it.kind == Item::KEY) seti(it.var, 0);
            else if (it.kind == Item::CHOICE) {
                for (const char* b : MOUSE_BUTTONS) if (!strcmp(b, it.var)) seti(it.var, -1);
            }
            break;
        case K_ENTER:
            if (it.kind == Item::BUTTON) { *remember = sel; return it.id; }
            if (it.kind == Item::KEY) capture_key(it);
            else if (it.kind == Item::TEXT) {
                int y = by + 2 + (sel - top_row);
                edit_text(it, y, bx + 3 + label_w, width - 6 - label_w);
            } else step(1);
            break;
        }
    }
}

// ================================================================ main
static std::string self_dir() {
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) return "./";
    self[n] = 0;
    std::string d(self);
    return d.substr(0, d.rfind('/') + 1);
}

int main(int argc, char** argv) {
    have_tty = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    load_config();

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "setup: %s\n", SDL_GetError()); return 1; }
    // The VGA's 640x400 shown at 4:3, the biggest whole size that fits.
    int ww = 640, wh = 480;
    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0)
        for (int k = 3; k >= 2; k--)
            if (640 * k <= dm.w * 9 / 10 && 480 * k <= dm.h * 9 / 10) { ww = 640 * k; wh = 480 * k; break; }
    if (ww == 640 && SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w >= 1100 && dm.h >= 900) { ww = 960; wh = 720; }
    win = SDL_CreateWindow("DOOM Setup", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { fprintf(stderr, "setup: can't open a window: %s\n", SDL_GetError()); return 1; }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SDL_RenderSetLogicalSize(ren, 640, 480);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 640, 400);
    SDL_StopTextInput();

    int sel_main = 0, sel_keys = 1, sel_mouse = 0, sel_video = 0, sel_sound = 0, sel_chat = 1;
    int action = 0;
    for (;;) {
        int b = run_menu("DOOM SETUP", main_menu, &sel_main, 0, 44);
        if (b == B_KEYS) { if (run_menu("Keyboard", keys_menu, &sel_keys, 30, 64) == B_QUIT) b = B_QUIT; }
        else if (b == B_MOUSE) { if (run_menu("Mouse", mouse_menu, &sel_mouse, 26, 68) == B_QUIT) b = B_QUIT; }
        else if (b == B_VIDEO) { if (run_menu("Video", video_menu, &sel_video, 24, 64) == B_QUIT) b = B_QUIT; }
        else if (b == B_SOUND) { if (run_menu("Sound", sound_menu, &sel_sound, 30, 50) == B_QUIT) b = B_QUIT; }
        else if (b == B_CHAT) { if (run_menu("Chat macros", chat_menu, &sel_chat, 10, 60) == B_QUIT) b = B_QUIT; }
        else if (b == B_DEFAULTS) {
            if (confirm("Put every setting back to the default?")) {
                for (int i = 0; i < NSET; i++) values[i] = SETTINGS[i].def;
                say("Defaults restored. Save to keep them.", C_LCYAN);
            }
            continue;
        }
        if (b == B_RUN || b == B_SAVE || b == B_NETWORK) {
            if (!save_config()) { say("Couldn't write default.cfg here: " + std::string(strerror(errno)), C_LRED); continue; }
            action = b;
            break;
        }
        if (b == B_QUIT || (b == B_BACK)) {
            if (changed() && b != B_QUIT) {
                if (!confirm("Exit without saving your changes?")) continue;
            }
            break;
        }
    }
    SDL_Quit();

    if (action == B_RUN || action == B_NETWORK) {
        std::string prog = self_dir() + (action == B_RUN ? "doom" : "ipxsetup");
        std::vector<char*> a;
        a.push_back((char*)prog.c_str());
        for (int i = 1; i < argc; i++) a.push_back(argv[i]);
        a.push_back(nullptr);
        execv(prog.c_str(), a.data());
        fprintf(stderr, "setup: can't start %s: %s\n", prog.c_str(), strerror(errno));
        return 1;
    }
    if (action == B_SAVE) printf("Settings saved in default.cfg and dgextra.cfg.\n");
    return 0;
}
