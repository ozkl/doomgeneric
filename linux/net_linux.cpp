// The game's side of networking: picks the driver from the command line and
// connects it to d_net.c. See net_linux.h.
//
//   -netfd N        started by ipxsetup / sersetup: the driver is that
//                   process, at the other end of socket N
//   -ipx..., -serial ...   the driver built in (net_ipx.cpp, net_serial.cpp)
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "net_linux.h"

extern "C" void I_Error(const char* error, ...);

static doomcom_t dc;
static enum { NONE, IPX, SERIAL, NETFD } driver;
static bool running;
static int netfd = -1;

static doomcom_t* net_init() { return driver == NONE ? nullptr : &dc; }

// ---------------------------------------------------------------- -netfd
// ipxsetup / sersetup is the driver, as on DOS; each command is a message.
static void netfd_cmd() {
    if (dc.command == CMD_SEND) {
        uint8_t m[sizeof(netfd_msg) + NET_DATA_MAX];
        int n = dc.datalength < 0 ? 0 : dc.datalength > NET_DATA_MAX ? NET_DATA_MAX : dc.datalength;
        netfd_msg h = {dc.remotenode, (int16_t)n};
        memcpy(m, &h, sizeof h);
        memcpy(m + sizeof h, &dc.data, n);
        if (send(netfd, m, sizeof h + n, MSG_NOSIGNAL) < 0 && (errno == EPIPE || errno == ECONNRESET)) {
            running = false;
            I_Error("The network driver (ipxsetup / sersetup) has gone away");
        }
    } else if (dc.command == CMD_GET) {
        dc.remotenode = -1;
        uint8_t m[sizeof(netfd_msg) + NET_DATA_MAX];
        ssize_t r = recv(netfd, m, sizeof m, MSG_DONTWAIT);
        if (r == 0) {
            running = false;
            I_Error("The network driver (ipxsetup / sersetup) has gone away");
        }
        if (r < (ssize_t)sizeof(netfd_msg)) return;
        netfd_msg h;
        memcpy(&h, m, sizeof h);
        int n = (int)r - (int)sizeof h;
        if (h.datalength < n) n = h.datalength < 0 ? 0 : h.datalength;
        dc.remotenode = h.remotenode;
        dc.datalength = (int16_t)n;
        memcpy(&dc.data, m + sizeof h, n);
    }
}

static bool netfd_setup(const char* v) {
    char* end;
    long fd = strtol(v, &end, 10);
    if (*end || fd < 0) { fprintf(stderr, "Network: -netfd %s isn't a file descriptor\n", v); return false; }
    netfd = (int)fd;
    // The driver's doomcom_t comes first.
    struct pollfd p = {netfd, POLLIN, 0};
    if (poll(&p, 1, 10000) <= 0) { fprintf(stderr, "Network: nothing from the driver on -netfd %d\n", netfd); return false; }
    ssize_t r = recv(netfd, &dc, sizeof dc, 0);
    if (r != (ssize_t)sizeof dc || dc.id != DOOMCOM_ID) {
        fprintf(stderr, "Network: -netfd %d isn't an ipxsetup / sersetup connection\n", netfd);
        return false;
    }
    printf("Network: driver on -netfd %d, %d node(s), player %d of %d\n", netfd, dc.numnodes,
           dc.consoleplayer + 1, dc.numplayers);
    return true;
}

// ---------------------------------------------------------------- picking one
static void net_cmd() {
    if (driver == IPX) ipx_cmd(&dc);
    else if (driver == SERIAL) ser_cmd(&dc);
    else if (driver == NETFD) netfd_cmd();
}

bool net_setup(int argc, char** argv) {
    // Started by ipxsetup / sersetup: the -ipx... options it was given (and
    // passed on, as IPXSETUP passed its own to DOOM.EXE) are its business.
    if (const char* v = net_arg_value(argc, argv, "-netfd")) {
        if (!netfd_setup(v)) return false;
        driver = NETFD;
    } else {
        bool ipx = ipx_wanted(argc, argv), ser = ser_wanted(argc, argv);
        if (!ipx && !ser) return true;
        if (ipx && ser) { fprintf(stderr, "Network: IPX or serial, not both\n"); return false; }

        net_doomcom_defaults(&dc);
        net_catch_ctrl_c(true);
        bool ok = ipx ? ipx_setup(argc, argv, &dc) : ser_setup(argc, argv, &dc);
        net_catch_ctrl_c(false);
        if (!ok) {
            if (net_aborted()) printf("\nNetwork game synchronization aborted.\n");
            if (ipx) ipx_shutdown(); else ser_shutdown();
            return false;
        }
        net_doomcom_options(argc, argv, &dc);
        driver = ipx ? IPX : SERIAL;
    }
    running = true;
    I_VanillaNetInit = net_init;
    I_VanillaNetCmd = net_cmd;
    return true;
}

void net_shutdown() {
    if (!running) return;
    running = false;
    if (driver == IPX) ipx_shutdown();
    else if (driver == SERIAL) ser_shutdown();
    else if (driver == NETFD) {
        // Let the driver pass on Doom's goodbye before it sees us go.
        shutdown(netfd, SHUT_WR);
    }
}
