// ipxsetup and sersetup for Linux: the DOS drivers' way of starting a net
// game. Built twice from this file (sersetup with -DSETUP_SERIAL).
//
//   ./ipxsetup -nodes 3 -deathmatch -warp 1 1
//   ./sersetup -com2 -19200
//
// As IPXSETUP.EXE / SERSETUP.EXE: finds the other players (the same packets,
// so DOS machines running the originals can be among them), then starts the
// game with every option it was given plus "-netfd N", and stays as its
// network driver until the game ends: the game sends and fetches packets
// through socket N where DOOM.EXE called the driver's interrupt.
//
// ipxsetup in a terminal draws IPXSETUP.EXE's box while it looks for the
// players; run with no options it starts with a setup menu (the game,
// players, map, skill, connection: setup_ui.cpp) and comes back to it when
// the game ends. -text: plain lines, as sersetup.
//
// The game: -exec PROGRAM, else the doom (or baremetaldoom) next to this
// program, else baremetaldoom or doom on the PATH.
//
// ipxsetup carries IPX as the game's built-in driver does: -ipx (the
// default: UDP broadcast on the LAN), -ipxnet HOST[:PORT], -ipxserver
// [PORT], -ipxeth IFACE. sersetup: -com1 .. -com4 (/dev/ttyS0..3, COM1 the
// default, as SERSETUP), or -serial DEVICE | HOST:PORT | listen:PORT.
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>
#include <vector>
#include "net_linux.h"
#ifndef SETUP_SERIAL
#include "setup_ui.h"
#endif

#ifdef SETUP_SERIAL
#define NAME "sersetup"
static bool (*const drv_setup)(int, char**, doomcom_t*) = ser_setup;
static void (*const drv_cmd)(doomcom_t*) = ser_cmd;
static void (*const drv_shutdown)() = ser_shutdown;
static int (*const drv_fds)(int*) = ser_fds;
#else
#define NAME "ipxsetup"
static bool (*const drv_setup)(int, char**, doomcom_t*) = ipx_setup;
static void (*const drv_cmd)(doomcom_t*) = ipx_cmd;
static void (*const drv_shutdown)() = ipx_shutdown;
static int (*const drv_fds)(int*) = ipx_fds;
#endif

static bool is_exe(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(p.c_str(), X_OK) == 0;
}

// The game to start, as IPXSETUP ran DOOM2.EXE if there was one, else DOOM.EXE.
static std::string find_game(int argc, char** argv) {
    if (const char* e = net_arg_value(argc, argv, "-exec")) return e;
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n > 0) {
        self[n] = 0;
        std::string dir(self);
        dir.erase(dir.rfind('/') + 1);
        for (const char* g : {"doom", "baremetaldoom"})
            if (is_exe(dir + g)) return dir + g;
    }
    if (const char* path = getenv("PATH")) {
        std::string p = path;
        for (const char* g : {"baremetaldoom", "doom"}) {
            size_t a = 0;
            while (a <= p.size()) {
                size_t b = p.find(':', a);
                if (b == std::string::npos) b = p.size();
                std::string f = (b > a ? p.substr(a, b - a) : std::string(".")) + "/" + g;
                if (is_exe(f)) return f;
                a = b + 1;
            }
        }
    }
    return "";
}

// While the game runs: its packets out through the driver, the driver's in
// to it. Returns when the game closes its end.
static void serve(int gamefd, doomcom_t* dc) {
    uint8_t m[sizeof(netfd_msg) + NET_DATA_MAX];
    for (;;) {
        struct pollfd p[5];
        int n = 0;
        p[n++] = {gamefd, POLLIN, 0};
        int fds[4];
        int k = drv_fds(fds);
        for (int i = 0; i < k && n < 5; i++) p[n++] = {fds[i], POLLIN, 0};
        // A short timeout all the same: serial output waiting for room, the
        // built-in IPX server.
        poll(p, n, 5);

        // From the game: packets to send.
        for (;;) {
            ssize_t r = recv(gamefd, m, sizeof m, MSG_DONTWAIT);
            if (r == 0) return;                         // the game is over
            if (r < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
                return;
            }
            if (r < (ssize_t)sizeof(netfd_msg)) continue;
            netfd_msg h;
            memcpy(&h, m, sizeof h);
            int len = (int)r - (int)sizeof h;
            if (h.datalength >= 0 && h.datalength < len) len = h.datalength;
            dc->command = CMD_SEND;
            dc->remotenode = h.remotenode;
            dc->datalength = (int16_t)len;
            memcpy(&dc->data, m + sizeof h, len);
            drv_cmd(dc);
        }

        // To the game: what came in. If it can't take them (stopped, say),
        // they're lost, as when the IPX driver ran out of receive buffers.
        for (;;) {
            dc->command = CMD_GET;
            drv_cmd(dc);
            if (dc->remotenode == -1) break;
            int len = dc->datalength < 0 ? 0 : dc->datalength > NET_DATA_MAX ? NET_DATA_MAX : dc->datalength;
            netfd_msg h = {dc->remotenode, (int16_t)len};
            memcpy(m, &h, sizeof h);
            memcpy(m + sizeof h, &dc->data, len);
            send(gamefd, m, sizeof h + len, MSG_DONTWAIT | MSG_NOSIGNAL);
        }
    }
}

// How the IPX packets go, for the box.
#ifndef SETUP_SERIAL
static std::string how(int argc, char** argv) {
    if (const char* v = net_arg_value(argc, argv, "-ipxnet")) return std::string("Joining the IPX server at ") + v;
    if (net_arg(argc, argv, "-ipxserver")) {
        const char* v = net_arg_value(argc, argv, "-ipxserver");
        return std::string("Hosting an IPX server, UDP port ") + (v && v[0] >= '0' && v[0] <= '9' ? v : "213");
    }
    if (const char* v = net_arg_value(argc, argv, "-ipxeth")) return std::string("IPX on Ethernet, ") + v;
    return "IPX over UDP broadcast on this LAN";
}
#endif

// One net game: find the players, run the game, serve it. 0 when it ran;
// with the menu, `why` says what went wrong.
static int run(int argc, char** argv, bool ui, std::string* why) {
    // The options the driver reads. sersetup's -comN become -serial
    // /dev/ttyS<N-1>; ipxsetup with no way given uses the LAN (-ipx).
    std::vector<char*> args(argv, argv + argc);
    static char dev[32];
#ifdef SETUP_SERIAL
    (void)ui;
    if (!net_arg(argc, argv, "-serial")) {
        int com = 1;
        for (int i = 2; i <= 4; i++) {
            char o[8];
            snprintf(o, sizeof o, "-com%d", i);
            if (net_arg(argc, argv, o)) com = i;
        }
        snprintf(dev, sizeof dev, "/dev/ttyS%d", com - 1);
        args.push_back((char*)"-serial");
        args.push_back(dev);
    }
#else
    (void)dev;
    if (!ipx_wanted(argc, argv)) args.push_back((char*)"-ipx");
#endif
    args.push_back(nullptr);
    int dargc = (int)args.size() - 1;
    char** dargv = args.data();

    std::string game = find_game(argc, argv);
    if (game.empty()) {
        fprintf(stderr, NAME ": can't find the game (doom, next to " NAME "); -exec PROGRAM\n");
        if (why) *why = "Can't find the game (doom) next to ipxsetup";
        return 1;
    }

    doomcom_t dc;
    net_doomcom_defaults(&dc);
    net_unabort();
    bool ok;
#ifndef SETUP_SERIAL
    if (ui) {
        int nodes = 2;
        if (const char* v = net_arg_value(dargc, dargv, "-nodes")) nodes = atoi(v);
        ui_search_begin(nodes, how(dargc, dargv));
        ok = drv_setup(dargc, dargv, &dc);
        std::string tail = ui_search_end(ok, dc.consoleplayer, dc.numplayers);
        if (!ok && why) *why = tail;
    } else
#endif
    {
        net_catch_ctrl_c(true);
        ok = drv_setup(dargc, dargv, &dc);
        net_catch_ctrl_c(false);
    }
    if (!ok) {
        if (net_aborted()) printf("\nNetwork game synchronization aborted.\n");
        drv_shutdown();
        return 1;
    }
    net_doomcom_options(dargc, dargv, &dc);

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) { perror(NAME ": socketpair"); drv_shutdown(); return 1; }
    int bufsz = 256 * 1024;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof bufsz);
    // The game's options: everything we were given, as IPXSETUP passed its
    // command line to DOOM.EXE, and our end of the socket.
    char fdarg[16];
    snprintf(fdarg, sizeof fdarg, "%d", sv[1]);
    std::vector<char*> gargs;
    gargs.push_back((char*)game.c_str());
    for (int i = 1; i < argc; i++) gargs.push_back(argv[i]);
    gargs.push_back((char*)"-netfd");
    gargs.push_back(fdarg);
    gargs.push_back(nullptr);

    printf("Starting %s\n", game.c_str());
    // Ctrl+C in the terminal goes to the game too; it ends, we follow.
    struct sigaction ign, old_int;
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigaction(SIGINT, &ign, &old_int);
    pid_t pid = fork();
    if (pid < 0) { perror(NAME ": fork"); drv_shutdown(); sigaction(SIGINT, &old_int, nullptr); return 1; }
    if (pid == 0) {
        close(sv[0]);
        signal(SIGINT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        execv(gargs[0], gargs.data());
        fprintf(stderr, NAME ": can't start %s: %s\n", gargs[0], strerror(errno));
        _exit(127);
    }
    close(sv[1]);
    send(sv[0], &dc, sizeof dc, MSG_NOSIGNAL);

    serve(sv[0], &dc);
    close(sv[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    drv_shutdown();
    sigaction(SIGINT, &old_int, nullptr);
    printf("Returned from DOOM\n");
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    net_driver_note = "";

#ifndef SETUP_SERIAL
    bool ui = ui_available(argc, argv);
    // Nothing that sets up the game (or -menu): the setup menu, and back to
    // it after each game, as DM.EXE. Other options (-nosound, -file x.wad...)
    // go to the game as well.
    static const char* const setup_opts[] = {"-nodes", "-ipx", "-ipxnet", "-ipxserver", "-ipxeth", "-warp",
                                             "-deathmatch", "-altdeath", "-skill", "-episode"};
    bool menu = net_arg(argc, argv, "-menu");
    if (!menu) {
        menu = true;
        for (const char* o : setup_opts) if (net_arg(argc, argv, o)) menu = false;
    }
    if (ui && menu) {
        std::string why;
        for (;;) {
            std::vector<std::string> a;
            if (!ui_menu(a, why)) return 0;
            for (int i = 1; i < argc; i++)
                if (strcasecmp(argv[i], "-menu")) a.push_back(argv[i]);
            std::vector<char*> av;
            for (auto& x : a) av.push_back((char*)x.c_str());
            av.push_back(nullptr);
            why.clear();
            int r = run((int)a.size(), av.data(), true, &why);
            if (r == 0) why.clear();
            else if (why.empty()) why = "The game ended with an error (see the terminal after quitting)";
        }
    }
    return run(argc, argv, ui, nullptr);
#else
    return run(argc, argv, false, nullptr);
#endif
}
