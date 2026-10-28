// Shared by the game (net_linux.cpp) and the separate ipxsetup / sersetup
// programs (setup_main.cpp): the command line, and Ctrl+C while looking for
// the other players.
#include <signal.h>
#include <string.h>
#include <strings.h>
#include "net_linux.h"

int net_arg(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; i++) if (!strcasecmp(argv[i], name)) return i;
    return 0;
}

const char* net_arg_value(int argc, char** argv, const char* name) {
    int i = net_arg(argc, argv, name);
    return i && i + 1 < argc ? argv[i + 1] : nullptr;
}

static volatile sig_atomic_t aborted;
static void on_int(int) { aborted = 1; }
bool net_aborted() { return aborted != 0; }
void net_abort() { aborted = 1; }
void net_unabort() { aborted = 0; }

void (*net_ui_tick)(int found, int wanted);
const char* net_driver_note = ", built in";

void net_tick(int found, int wanted) {
    if (net_ui_tick) net_ui_tick(found, wanted);
}

static struct sigaction old_int;
void net_catch_ctrl_c(bool on) {
    if (on) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_int;             // no SA_RESTART: a wait comes back at once
        sigaction(SIGINT, &sa, &old_int);
    } else {
        sigaction(SIGINT, &old_int, nullptr);
    }
}

void net_doomcom_defaults(doomcom_t* dc) {
    memset(dc, 0, sizeof *dc);
    dc->id = DOOMCOM_ID;
    dc->intnum = 0x66;                      // no interrupt here; what IPXSETUP would print
    dc->ticdup = 1;
    dc->episode = 1;
    dc->map = 1;
    dc->skill = 2;
    dc->savegame = -1;
}

void net_doomcom_options(int argc, char** argv, doomcom_t* dc) {
    // As the drivers' own -dup / -extratic would (the Linux i_net.c read them).
    if (const char* v = net_arg_value(argc, argv, "-dup")) {
        int d = v[0] - '0';
        if (d >= 1 && d <= 9) dc->ticdup = (short)d;
    }
    if (net_arg(argc, argv, "-extratic")) dc->extratics = 1;
}
