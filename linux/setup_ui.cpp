// ipxsetup's screens. See setup_ui.h.
//
// The menu stands in for what DOS players went through SETUP.EXE or DM.EXE
// for: which game, how many players, deathmatch or cooperative, the map and
// skill, and here how the IPX packets travel. The choices are kept in
// ipxsetup.cfg (in the current directory, by default.cfg). The box is
// IPXSETUP.EXE's "DOOM Network Play / Looking for N players... / Press ESC
// to abort".
//
// Plain ANSI: the terminal's alternate screen, blue as the DOS screens.
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <langinfo.h>
#include <locale.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <string>
#include <vector>
#include "net_linux.h"
#include "setup_ui.h"

bool ui_available(int argc, char** argv) {
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) && !net_arg(argc, argv, "-text")
        && getenv("TERM") && strcmp(getenv("TERM"), "dumb") != 0;
}

// ================================================================ terminal
static FILE* tty;                       // our own handle on the screen (stdout may be kept)
// A UTF-8 terminal: double-lined boxes and arrows. Otherwise the VT100 line
// drawing set (single lines) and ASCII.
static bool utf8;
struct Glyphs { const char *tl, *tr, *bl, *br, *h, *v, *lt, *rt, *dot, *l, *r, *ell; };
static const Glyphs G_UTF8 = {"╔", "╗", "╚", "╝", "═", "║", "╡", "╞", "•", "◄ ", " ►", "…"};
static const Glyphs G_VT100 = {"\033(0l\033(B", "\033(0k\033(B", "\033(0m\033(B", "\033(0j\033(B",
                               "\033(0q\033(B", "\033(0x\033(B", "\033(0u\033(B", "\033(0t\033(B",
                               "*", "< ", " >", "~"};
static const Glyphs* g = &G_VT100;
static struct termios tio_saved;
static bool tio_changed, on_alt;

static void tty_restore() {
    if (on_alt && tty) { fputs("\033[0m\033[?25h\033[?1049l", tty); fflush(tty); on_alt = false; }
    if (tio_changed) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &tio_saved); tio_changed = false; }
}

static void on_fatal(int sig) {
    tty_restore();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void tty_start() {
    if (!tty) {
        setlocale(LC_CTYPE, "");
        utf8 = !strcasecmp(nl_langinfo(CODESET), "UTF-8");
        g = utf8 ? &G_UTF8 : &G_VT100;
        tty = fdopen(dup(STDOUT_FILENO), "w");
        atexit(tty_restore);
        signal(SIGTERM, on_fatal); signal(SIGHUP, on_fatal);
        signal(SIGSEGV, on_fatal); signal(SIGABRT, on_fatal);
    }
    if (!tio_changed && tcgetattr(STDIN_FILENO, &tio_saved) == 0) {
        struct termios t = tio_saved;
        // Keys one at a time, unechoed; Ctrl+C a key too (Esc's twin), so
        // the screen is always put back.
        t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
        t.c_iflag &= ~(IXON | ICRNL);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
        tio_changed = true;
    }
    if (!on_alt) { fputs("\033[?1049h\033[?25l", tty); on_alt = true; }
}

static void screen_size(int* w, int* h) {
    struct winsize ws;
    *w = 80; *h = 25;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) { *w = ws.ws_col; *h = ws.ws_row; }
}

enum { K_NONE = 0, K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_ENTER, K_ESC, K_BS, K_DEL, K_TAB, K_QUIT };

static int read_byte(int ms) {
    struct pollfd p = {STDIN_FILENO, POLLIN, 0};
    if (poll(&p, 1, ms) <= 0) return -1;
    unsigned char c;
    return read(STDIN_FILENO, &c, 1) == 1 ? c : -1;
}

static int read_key(int ms) {
    int c = read_byte(ms);
    if (c < 0) return K_NONE;
    if (c == 27) {
        int d = read_byte(25);
        if (d < 0) return K_ESC;
        if (d != '[' && d != 'O') return K_ESC;
        char seq[16];
        int n = 0, e;
        while ((e = read_byte(25)) >= 0 && n < 15) {
            seq[n++] = (char)e;
            if (e >= 0x40 && e <= 0x7E) break;
        }
        seq[n] = 0;
        if (!n) return K_ESC;
        switch (seq[n - 1]) {
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        case 'H': return K_HOME;
        case 'F': return K_END;
        case 'Z': return K_UP;                      // Shift+Tab
        case '~':
            switch (atoi(seq)) {
            case 1: case 7: return K_HOME;
            case 4: case 8: return K_END;
            case 3: return K_DEL;
            case 5: return K_PGUP;
            case 6: return K_PGDN;
            }
        }
        return K_NONE;
    }
    if (c == '\r' || c == '\n') return K_ENTER;
    if (c == 127 || c == 8) return K_BS;
    if (c == 9) return K_TAB;
    if (c == 3) return K_QUIT;                      // Ctrl+C
    return c >= 32 && c < 127 ? c : K_NONE;
}

// ---------------------------------------------------------------- drawing
// Colours as the DOS screens: white on blue, the bar cyan, values yellow.
static std::string out;
static void at(int row, int col) { char b[24]; snprintf(b, sizeof b, "\033[%d;%dH", row, col); out += b; }
static void color(int fg, int bg, bool bright = false) {
    char b[24];
    snprintf(b, sizeof b, "\033[0;%s%d;%dm", bright ? "1;" : "", fg, bg);
    out += b;
}
enum { BLACK = 30, RED, GREEN, YELLOW, BLUE, MAGENTA, CYAN, WHITE };
#define BG(c) ((c) + 10)

// UTF-8 is one column per character here (the box characters are).
static int cols(const std::string& s) {
    int n = 0;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = s[i];
        if (c == 27) { i += 2; continue; }        // ESC ( 0 / ESC ( B
        if ((c & 0xC0) != 0x80) n++;
    }
    return n;
}
static std::string fit(const std::string& s, int w) {
    if (w <= 0) return "";
    if (cols(s) <= w) return s + std::string(w - cols(s), ' ');
    std::string r;
    int n = 0;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = s[i];
        if ((c & 0xC0) != 0x80) { if (n == w - 1) break; n++; }
        r += s[i];
    }
    return r + g->ell;
}
static std::string rep(const char* s, int n) { std::string r; while (n-- > 0) r += s; return r; }

static void flush_out() { fputs(out.c_str(), tty); fflush(tty); out.clear(); }

// A double-lined box, title in the top edge, `foot` in the bottom one.
static void box(int top, int left, int h, int w, const std::string& title, const std::string& foot) {
    color(WHITE, BG(BLUE), true);
    at(top, left);
    int tl = cols(title) + 2, l1 = (w - 2 - tl) / 2;
    std::string H = g->h;
    out += g->tl + rep(H.c_str(), l1) + (title.empty() ? H + H : g->lt + title + g->rt) + rep(H.c_str(), w - 2 - l1 - tl) + g->tr;
    for (int r = 1; r < h - 1; r++) {
        at(top + r, left);
        out += g->v + std::string(w - 2, ' ') + g->v;
    }
    at(top + h - 1, left);
    int fl = cols(foot) + 2, l2 = (w - 2 - fl) / 2;
    out += g->bl + rep(H.c_str(), l2) + (foot.empty() ? H + H : g->lt + foot + g->rt) + rep(H.c_str(), w - 2 - l2 - fl) + g->br;
}

static void background(int sw, int sh, const std::string& title) {
    color(WHITE, BG(BLUE));
    out += "\033[2J";
    at(1, 1);
    color(BLACK, BG(CYAN));
    std::string t = " " + title;
    out += fit(t, sw);
    (void)sh;
}

static void help_bar(int sw, int sh, const std::string& keys) {
    at(sh, 1);
    color(BLACK, BG(CYAN));
    out += fit(" " + keys, sw);
}

// ================================================================ the games
struct Iwad {
    std::string path, label;
    int episodes = 0;       // E1M1.. games: how many; 0: MAP01.. games
    int maps = 0;           // MAP games: the highest MAPxx
};

// Which maps a WAD has, from its directory.
static bool wad_scan(const std::string& path, Iwad* w) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t h[12];
    bool ok = fread(h, 1, 12, f) == 12 && !memcmp(h, "IWAD", 4);
    if (ok) {
        uint32_t n = h[4] | h[5] << 8 | h[6] << 16 | (uint32_t)h[7] << 24;
        uint32_t ofs = h[8] | h[9] << 8 | h[10] << 16 | (uint32_t)h[11] << 24;
        if (n > 100000 || fseek(f, ofs, SEEK_SET) != 0) ok = false;
        for (uint32_t i = 0; ok && i < n; i++) {
            uint8_t e[16];
            if (fread(e, 1, 16, f) != 16) break;
            char name[9];
            memcpy(name, e + 8, 8);
            name[8] = 0;
            if (name[0] == 'E' && name[2] == 'M' && name[4] == 0 && name[1] >= '1' && name[1] <= '9' && name[3] >= '1' && name[3] <= '9') {
                if (name[1] - '0' > w->episodes) w->episodes = name[1] - '0';
            } else if (!strncmp(name, "MAP", 3) && name[3] >= '0' && name[3] <= '9' && name[4] >= '0' && name[4] <= '9' && !name[5]) {
                int m = (name[3] - '0') * 10 + name[4] - '0';
                if (m > w->maps) w->maps = m;
            }
        }
    }
    fclose(f);
    if (w->maps) w->episodes = 0;
    return ok && (w->episodes || w->maps);
}

// Where the game looks (linux_platform.cpp's find_iwad), the same names.
static std::vector<Iwad> find_iwads() {
    static const struct { const char* file; const char* label; } known[] = {
        {"doom2.wad", "Doom II"}, {"plutonia.wad", "Final Doom: Plutonia"}, {"tnt.wad", "Final Doom: TNT"},
        {"doom.wad", "Doom"}, {"doom1.wad", "Doom (shareware)"}, {"freedoom2.wad", "Freedoom: Phase 2"},
        {"freedoom1.wad", "Freedoom: Phase 1"}, {"chex.wad", "Chex Quest"}, {"hacx.wad", "HacX"},
    };
    std::vector<std::string> dirs = {"."};
    if (const char* e = getenv("DOOMWADDIR")) dirs.push_back(e);
    if (const char* e = getenv("DOOMWADPATH")) {
        std::string p = e;
        size_t a = 0;
        while (a <= p.size()) {
            size_t b = p.find(':', a);
            if (b == std::string::npos) b = p.size();
            if (b > a) dirs.push_back(p.substr(a, b - a));
            a = b + 1;
        }
    }
    if (const char* h = getenv("HOME")) dirs.push_back(std::string(h) + "/.local/share/games/doom");
    dirs.push_back("/usr/local/share/games/doom");
    dirs.push_back("/usr/share/games/doom");

    std::vector<Iwad> found;
    for (auto& k : known) {
        for (auto& d : dirs) {
            bool got = false;
            for (int upper = 0; upper < 2 && !got; upper++) {
                std::string f = k.file;
                if (upper) for (auto& c : f) c = (char)toupper((unsigned char)c);
                std::string path = (d == "." ? f : d + "/" + f);
                struct stat st;
                if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
                Iwad w;
                w.path = path;
                w.label = k.label;
                if (!wad_scan(path, &w)) continue;
                if (!strcmp(k.file, "doom.wad")) w.label = w.episodes >= 4 ? "The Ultimate Doom" : "Doom (registered)";
                found.push_back(w);
                got = true;
            }
            if (got) break;                         // the first of each name, as the game
        }
    }
    return found;
}

// ================================================================ settings
static const char* const SKILLS[] = {"I'm too young to die", "Hey, not too rough", "Hurt me plenty",
                                     "Ultra-Violence", "Nightmare!"};
static const char* const MODES[] = {"Cooperative", "Deathmatch", "Deathmatch 2.0 (items respawn)"};
static const char* const CONNS[] = {"This LAN (Linux players, no server)", "Host a server (DOSBox can join)",
                                    "Join a server", "Real IPX on Ethernet (DOS PCs)"};
static const int TIMERS[] = {0, 5, 10, 15, 20, 30, 60};

struct Settings {
    std::string iwad;                   // path
    int players = 2, mode = 1, episode = 1, map = 1, skill = 3;
    int monsters = 1, respawn = 0, fast = 0, timer = 0;
    int conn = 0;
    std::string port = "21300", server, iface, extra;
};

static const char* CFG = "ipxsetup.cfg";

static void load(Settings* s) {
    FILE* f = fopen(CFG, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char* nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        char* v = strchr(line, ' ');
        if (!v) continue;
        *v++ = 0;
        while (*v == ' ') v++;
        std::string k = line;
        if (k == "iwad") s->iwad = v;
        else if (k == "players") s->players = atoi(v);
        else if (k == "mode") s->mode = atoi(v);
        else if (k == "episode") s->episode = atoi(v);
        else if (k == "map") s->map = atoi(v);
        else if (k == "skill") s->skill = atoi(v);
        else if (k == "monsters") s->monsters = atoi(v);
        else if (k == "respawn") s->respawn = atoi(v);
        else if (k == "fast") s->fast = atoi(v);
        else if (k == "timer") s->timer = atoi(v);
        else if (k == "connection") s->conn = atoi(v);
        else if (k == "port") s->port = v;
        else if (k == "server") s->server = v;
        else if (k == "interface") s->iface = v;
        else if (k == "options") s->extra = v;
    }
    fclose(f);
    if (s->players < 2 || s->players > 4) s->players = 2;
    if (s->mode < 0 || s->mode > 2) s->mode = 1;
    if (s->skill < 1 || s->skill > 5) s->skill = 3;
    if (s->conn < 0 || s->conn > 3) s->conn = 0;
    if (s->timer < 0) s->timer = 0;
}

static void save(const Settings& s) {
    FILE* f = fopen(CFG, "w");
    if (!f) return;
    fprintf(f, "iwad %s\nplayers %d\nmode %d\nepisode %d\nmap %d\nskill %d\nmonsters %d\nrespawn %d\nfast %d\n"
               "timer %d\nconnection %d\nport %s\nserver %s\ninterface %s\noptions %s\n",
            s.iwad.c_str(), s.players, s.mode, s.episode, s.map, s.skill, s.monsters, s.respawn, s.fast, s.timer,
            s.conn, s.port.c_str(), s.server.c_str(), s.iface.c_str(), s.extra.c_str());
    fclose(f);
}

// This machine's IPv4 addresses (for "others connect to"), and its first
// Ethernet-ish interface.
static std::vector<std::string> my_addresses(std::string* first_iface) {
    std::vector<std::string> r;
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) != 0) return r;
    for (ifaddrs* i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        char a[32];
        inet_ntop(AF_INET, &((sockaddr_in*)i->ifa_addr)->sin_addr, a, sizeof a);
        r.push_back(a);
        if (first_iface && first_iface->empty()) *first_iface = i->ifa_name;
    }
    freeifaddrs(ifs);
    return r;
}

// ================================================================ the menu
enum RowId { R_GAME, R_PLAYERS, R_MODE, R_MAP, R_SKILL, R_MONSTERS, R_RESPAWN, R_FAST, R_TIMER,
             R_CONN, R_PORT, R_SERVER, R_IFACE, R_INFO, R_EXTRA, R_START, R_GAP };

struct Row { RowId id; std::string label, value; bool text; };

static std::string map_name(const Iwad* w, int e, int m) {
    char b[16];
    if (w && w->maps) snprintf(b, sizeof b, "MAP%02d", m);
    else snprintf(b, sizeof b, "E%dM%d", e, m);
    return b;
}

static std::vector<Row> rows(const Settings& s, const Iwad* w, const std::vector<std::string>& addrs) {
    std::vector<Row> r;
    char b[64];
    r.push_back({R_GAME, "Game", w ? w->label : "no IWAD found", false});
    snprintf(b, sizeof b, "%d", s.players);
    r.push_back({R_PLAYERS, "Players", b, false});
    r.push_back({R_MODE, "Game type", MODES[s.mode], false});
    r.push_back({R_MAP, "Start on", map_name(w, s.episode, s.map), false});
    r.push_back({R_SKILL, "Skill", SKILLS[s.skill - 1], false});
    r.push_back({R_MONSTERS, "Monsters", s.monsters ? "Yes" : "No", false});
    r.push_back({R_RESPAWN, "Monsters respawn", s.respawn ? "Yes" : "No", false});
    r.push_back({R_FAST, "Fast monsters", s.fast ? "Yes" : "No", false});
    if (s.mode) {
        if (s.timer) snprintf(b, sizeof b, "%d minutes", s.timer); else snprintf(b, sizeof b, "None");
        r.push_back({R_TIMER, "Time limit", b, false});
    }
    r.push_back({R_GAP, "", "", false});
    r.push_back({R_CONN, "Connection", CONNS[s.conn], false});
    if (s.conn == 1) {
        r.push_back({R_PORT, "Server UDP port", s.port, true});
        std::string a;
        for (auto& x : addrs) { if (!a.empty()) a += ", "; a += x; }
        if (a.empty()) a = "this machine's address";
        r.push_back({R_INFO, "", "DOSBox: ipxnet connect " + a + " " + s.port, false});
    } else if (s.conn == 2) {
        r.push_back({R_SERVER, "Server address", s.server.empty() ? "" : s.server, true});
        r.push_back({R_INFO, "", "host or host:port (port 213 if not given)", false});
    } else if (s.conn == 3) {
        r.push_back({R_IFACE, "Network interface", s.iface, true});
        r.push_back({R_INFO, "", "needs: sudo setcap cap_net_raw+ep doom ipxsetup", false});
    }
    r.push_back({R_EXTRA, "More options", s.extra, true});
    r.push_back({R_GAP, "", "", false});
    r.push_back({R_START, "", "Start the game", false});
    return r;
}

static bool selectable(RowId id) { return id != R_GAP && id != R_INFO; }

static void clamp_map(Settings* s, const Iwad* w) {
    if (!w) return;
    if (w->maps) { s->episode = 1; if (s->map < 1 || s->map > w->maps) s->map = 1; }
    else {
        if (s->episode < 1 || s->episode > w->episodes) s->episode = 1;
        if (s->map < 1 || s->map > 9) s->map = 1;
    }
}

static void change(Settings* s, RowId id, int d, const std::vector<Iwad>& iwads, int* wi) {
    auto cyc = [&](int v, int lo, int hi) { return v + d > hi ? lo : v + d < lo ? hi : v + d; };
    const Iwad* w = *wi >= 0 ? &iwads[*wi] : nullptr;
    switch (id) {
    case R_GAME:
        if (iwads.empty()) break;
        *wi = cyc(*wi, 0, (int)iwads.size() - 1);
        s->iwad = iwads[*wi].path;
        clamp_map(s, &iwads[*wi]);
        break;
    case R_PLAYERS: s->players = cyc(s->players, 2, 4); break;
    case R_MODE: s->mode = cyc(s->mode, 0, 2); break;
    case R_MAP:
        if (!w) break;
        if (w->maps) s->map = cyc(s->map, 1, w->maps);
        else {
            int i = cyc((s->episode - 1) * 9 + s->map - 1, 0, w->episodes * 9 - 1);
            s->episode = i / 9 + 1; s->map = i % 9 + 1;
        }
        break;
    case R_SKILL: s->skill = cyc(s->skill, 1, 5); break;
    case R_MONSTERS: s->monsters ^= 1; break;
    case R_RESPAWN: s->respawn ^= 1; break;
    case R_FAST: s->fast ^= 1; break;
    case R_TIMER: {
        int n = sizeof TIMERS / sizeof TIMERS[0], i = 0;
        while (i < n && TIMERS[i] != s->timer) i++;
        if (i == n) i = 0;
        s->timer = TIMERS[cyc(i, 0, n - 1)];
        break;
    }
    case R_CONN: s->conn = cyc(s->conn, 0, 3); break;
    default: break;
    }
}

static std::string* text_of(Settings* s, RowId id) {
    switch (id) {
    case R_PORT: return &s->port;
    case R_SERVER: return &s->server;
    case R_IFACE: return &s->iface;
    case R_EXTRA: return &s->extra;
    default: return nullptr;
    }
}

static void split_words(const std::string& s, std::vector<std::string>* out) {
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') i++;
        size_t j = i;
        while (j < s.size() && s[j] != ' ') j++;
        if (j > i) out->push_back(s.substr(i, j - i));
        i = j;
    }
}

bool ui_menu(std::vector<std::string>& args, const std::string& first_note) {
    std::vector<Iwad> iwads = find_iwads();
    Settings s;
    std::string iface0;
    std::vector<std::string> addrs = my_addresses(&iface0);
    load(&s);
    if (s.iface.empty()) s.iface = iface0.empty() ? "eth0" : iface0;
    int wi = -1;
    for (size_t i = 0; i < iwads.size(); i++) if (iwads[i].path == s.iwad) wi = (int)i;
    if (wi < 0 && !iwads.empty()) { wi = 0; s.iwad = iwads[0].path; }
    if (wi >= 0) clamp_map(&s, &iwads[wi]);

    std::string note = first_note;
    int sel = 0;                        // index into rows
    bool editing = false;
    std::string edit_buf;
    tty_start();

    for (;;) {
        const Iwad* w = wi >= 0 ? &iwads[wi] : nullptr;
        std::vector<Row> r = rows(s, w, addrs);
        // Keep the selection on the same row id when rows come and go.
        if (sel >= (int)r.size()) sel = (int)r.size() - 1;
        while (!selectable(r[sel].id)) sel = (sel + 1) % (int)r.size();

        int sw, sh;
        screen_size(&sw, &sh);
        background(sw, sh, "DOOM Network Setup                                        IPXSETUP for Linux");
        int bw = sw - 4 < 70 ? sw - 4 : 70;
        int bh = (int)r.size() + 4;
        int top = (sh - bh) / 2 + 1, left = (sw - bw) / 2 + 1;
        if (top < 3) top = 3;
        box(top, left, bh, bw, " DOOM Network Play ", "");
        int lw = 19, vw = bw - 4 - lw - 2;
        for (size_t i = 0; i < r.size(); i++) {
            int row = top + 2 + (int)i;
            const Row& x = r[i];
            bool on = (int)i == sel;
            at(row, left + 2);
            if (x.id == R_START) {
                std::string b = on ? std::string(">> ") + x.value + " <<" : "   " + x.value + "   ";
                int pad = (bw - 4 - cols(b)) / 2;
                color(WHITE, BG(BLUE), true);
                out += std::string(pad, ' ');
                if (on) color(BLACK, BG(CYAN)); else color(YELLOW, BG(BLUE), true);
                out += b;
                continue;
            }
            if (x.id == R_INFO) {
                color(CYAN, BG(BLUE));
                out += fit(std::string(lw + 2, ' ') + x.value, bw - 4);
                continue;
            }
            if (x.id == R_GAP) continue;
            std::string val = x.value;
            if (on && editing) val = edit_buf;
            if (on) color(BLACK, BG(CYAN)); else color(WHITE, BG(BLUE), true);
            out += " " + fit(x.label, lw) + " ";
            if (on && editing) {
                color(WHITE, BG(BLACK), true);
                std::string v = val;
                if (cols(v) > vw - 2) v = v.substr(v.size() - (vw - 2));
                out += fit(v + "_", vw);
            } else {
                if (on) color(BLACK, BG(CYAN)); else color(YELLOW, BG(BLUE), true);
                std::string shown = val;
                if (!x.text && on) shown = g->l + val + g->r;
                if (x.text && val.empty()) shown = on ? "(Enter to type)" : "";
                out += fit(shown, vw);
            }
        }
        // What went wrong, or what to do.
        if (!note.empty()) {
            at(top + bh, left);
            color(YELLOW, BG(RED), true);
            out += fit(" " + note, bw);
        }
        help_bar(sw, sh, editing ? "Type, then Enter to keep it   Esc: as it was"
                                 : "Up/Down: choose   Left/Right: change   Enter: type / start   Esc: quit");
        flush_out();

        int k = read_key(1000);
        if (k == K_NONE) continue;
        RowId id = r[sel].id;
        if (editing) {
            std::string* t = text_of(&s, id);
            if (k == K_ENTER) { if (t) *t = edit_buf; editing = false; save(s); }
            else if (k == K_ESC || k == K_QUIT) editing = false;
            else if (k == K_BS) { if (!edit_buf.empty()) edit_buf.pop_back(); }
            else if (k >= 32 && k < 127 && edit_buf.size() < 200) edit_buf += (char)k;
            continue;
        }
        note.clear();
        auto move = [&](int d) {
            int n = (int)r.size();
            for (int i = 0; i < n; i++) { sel = (sel + d + n) % n; if (selectable(r[sel].id)) break; }
        };
        switch (k) {
        case K_UP: move(-1); break;
        case K_DOWN: case K_TAB: move(1); break;
        case K_HOME: sel = 0; break;
        case K_END: sel = (int)r.size() - 1; break;
        case K_LEFT: change(&s, id, -1, iwads, &wi); save(s); break;
        case K_RIGHT: case ' ': change(&s, id, 1, iwads, &wi); save(s); break;
        case K_ESC: case K_QUIT: case 'q': case 'Q':
            tty_restore();
            return false;
        case K_ENTER:
            if (text_of(&s, id)) { editing = true; edit_buf = *text_of(&s, id); break; }
            if (id != R_START) { change(&s, id, 1, iwads, &wi); save(s); break; }
            // Start: the command line IPXSETUP would have been given.
            if (!w) { note = "No IWAD found: put doom.wad / doom2.wad here or in /usr/share/games/doom"; break; }
            if (s.conn == 2 && s.server.empty()) { note = "Type the server's address first"; break; }
            if (s.conn == 1 && (atoi(s.port.c_str()) <= 0 || atoi(s.port.c_str()) > 65535)) { note = "The port is a number, 1 to 65535"; break; }
            save(s);
            args.clear();
            args.push_back("ipxsetup");
            args.push_back("-nodes"); args.push_back(std::to_string(s.players));
            args.push_back("-iwad"); args.push_back(w->path);
            if (s.mode == 1) args.push_back("-deathmatch");
            if (s.mode == 2) args.push_back("-altdeath");
            args.push_back("-warp");
            if (!w->maps) args.push_back(std::to_string(s.episode));
            args.push_back(std::to_string(s.map));
            args.push_back("-skill"); args.push_back(std::to_string(s.skill));
            if (!s.monsters) args.push_back("-nomonsters");
            if (s.respawn) args.push_back("-respawn");
            if (s.fast) args.push_back("-fast");
            if (s.mode && s.timer) { args.push_back("-timer"); args.push_back(std::to_string(s.timer)); }
            if (s.conn == 0) args.push_back("-ipx");
            if (s.conn == 1) { args.push_back("-ipxserver"); args.push_back(s.port); }
            if (s.conn == 2) { args.push_back("-ipxnet"); args.push_back(s.server); }
            if (s.conn == 3) { args.push_back("-ipxeth"); args.push_back(s.iface); }
            split_words(s.extra, &args);
            return true;
        }
    }
}

// ================================================================ the box
static FILE* kept;                      // what the driver prints, meanwhile
static char* kept_buf;
static size_t kept_len;
static FILE *saved_out, *saved_err;
static int box_wanted, box_found = -1;
static std::string box_how;
static uint32_t box_drawn_at;
static int box_drawn_found = -2;

static uint32_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// The last thing the driver said.
static std::string last_line() {
    fflush(kept);
    std::string s(kept_buf ? kept_buf : "", kept_len);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    size_t nl = s.find_last_of('\n');
    std::string l = nl == std::string::npos ? s : s.substr(nl + 1);
    while (!l.empty() && l.back() == '.') l.pop_back();     // "Looking for a node....."
    return l;
}

static void draw_box(const char* status, int status_color) {
    int sw, sh;
    screen_size(&sw, &sh);
    background(sw, sh, "DOOM Network Play                                         IPXSETUP for Linux");
    int bw = 52;
    if (bw > sw - 2) bw = sw - 2;
    int bh = box_wanted + 10;
    int top = (sh - bh) / 2 + 1, left = (sw - bw) / 2 + 1;
    if (top < 3) top = 3;
    box(top, left, bh, bw, "DOOM Network Play", "Press ESC to abort");
    int row = top + 2;
    char b[96];
    snprintf(b, sizeof b, "Looking for %d players...", box_wanted);
    at(row++, left + 3);
    color(WHITE, BG(BLUE), true);
    out += b;
    row++;
    for (int i = 0; i < box_wanted; i++) {
        at(row++, left + 5);
        bool got = i < box_found;
        color(got ? WHITE : CYAN, BG(BLUE), got);
        snprintf(b, sizeof b, "(%s) Player %d%s", got ? g->dot : "-", i + 1, i == 0 ? "  (this machine)" : "");
        out += b;
    }
    row++;
    at(row++, left + 3);
    color(status_color, BG(BLUE), true);
    out += fit(status, bw - 6);
    at(row++, left + 3);
    color(CYAN, BG(BLUE));
    out += fit(box_how, bw - 6);
    at(row++, left + 3);
    color(WHITE, BG(BLUE));
    out += fit(last_line(), bw - 6);
    help_bar(sw, sh, "Esc: give up");
    flush_out();
}

static void box_tick(int found, int wanted) {
    (void)wanted;
    for (;;) {
        int k = read_key(0);
        if (k == K_NONE) break;
        if (k == K_ESC || k == K_QUIT) net_abort();
    }
    uint32_t t = now_ms();
    if (found == box_drawn_found && t - box_drawn_at < 300) return;
    box_found = found;
    box_drawn_found = found;
    box_drawn_at = t;
    static const char* spin[] = {"Searching", "Searching.", "Searching..", "Searching..."};
    draw_box(found < 0 ? "Connecting..." : spin[(t / 400) % 4], YELLOW);
}

void ui_search_begin(int wanted, const std::string& how) {
    tty_start();
    box_wanted = wanted < 1 ? 1 : wanted > 8 ? 8 : wanted;
    box_found = -1;
    box_drawn_found = -2;
    box_how = how;
    fflush(stdout);
    fflush(stderr);
    saved_out = stdout;
    saved_err = stderr;
    kept = open_memstream(&kept_buf, &kept_len);
    if (kept) { stdout = kept; stderr = kept; }
    net_unabort();
    net_ui_tick = box_tick;
    box_tick(-1, wanted);
}

std::string ui_search_end(bool ok, int console, int players) {
    net_ui_tick = nullptr;
    std::string log;
    if (kept) {
        fflush(kept);
        log.assign(kept_buf ? kept_buf : "", kept_len);
        stdout = saved_out;
        stderr = saved_err;
        fclose(kept);
        free(kept_buf);
        kept = nullptr; kept_buf = nullptr; kept_len = 0;
    }
    if (ok) {
        box_found = box_wanted;
        char b[64];
        snprintf(b, sizeof b, "Found them all! You are player %d of %d", console + 1, players);
        box_drawn_found = -2;
        draw_box(b, GREEN);
        usleep(900 * 1000);
    }
    tty_restore();
    // The plain story, as IPXSETUP printed it.
    fputs(log.c_str(), stdout);
    fflush(stdout);
    if (ok) return log;
    // For the menu: the last couple of lines that say something.
    std::vector<std::string> lines;
    size_t a = 0;
    while (a < log.size()) {
        size_t b = log.find('\n', a);
        if (b == std::string::npos) b = log.size();
        std::string l = log.substr(a, b - a);
        while (!l.empty() && (l.back() == '.' || l.back() == ' ' || l.back() == '\r')) l.pop_back();
        if (!l.empty() && l.find("-----") == std::string::npos) lines.push_back(l);
        a = b + 1;
    }
    std::string tail = net_aborted() ? "Network game synchronization aborted." : lines.empty() ? "It didn't work." : lines.back();
    return tail;
}
