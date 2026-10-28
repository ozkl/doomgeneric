// The network drivers of the Linux port. On DOS, IPXSETUP.EXE or
// SERSETUP.EXE finds the other players, then starts DOOM.EXE with
// "-net <address of its doomcom_t>", and Doom calls the driver's interrupt
// to send or fetch a packet. Here the same drivers (the same packets, byte
// for byte, so a Linux machine plays DOS machines running IPXSETUP/SERSETUP
// and the original DOOM.EXE) run one of two ways:
//
//   built in        ./doom -ipx ...: the game finds the players itself
//   as on DOS       ./ipxsetup ...: finds the players, starts ./doom with
//                   "-netfd N" (a socket to it) and stays as its driver
//
// Either way d_net.c gets a doomcom_t through I_VanillaNetInit /
// I_VanillaNetCmd, as in the DOS build.
#pragma once
#include <stddef.h>
#include <stdint.h>
extern "C" {
#include "d_doomcom.h"
}

// The game (net_linux.cpp), from main() before the game starts. True: carry
// on (a net game was set up, or none was asked for); false: it failed or
// was given up (said why).
bool net_setup(int argc, char** argv);
void net_shutdown();

// The two drivers (net_ipx.cpp, net_serial.cpp). setup fills in the
// doomcom_t (numnodes, consoleplayer, numplayers...), cmd carries out
// dc->command, shutdown lets go of the network or line. fds: what to wait
// on for incoming packets (up to 4; the count).
bool ipx_wanted(int argc, char** argv);
bool ipx_setup(int argc, char** argv, doomcom_t* dc);
void ipx_cmd(doomcom_t* dc);
void ipx_shutdown();
int ipx_fds(int* fds);

bool ser_wanted(int argc, char** argv);
bool ser_setup(int argc, char** argv, doomcom_t* dc);
void ser_cmd(doomcom_t* dc);
void ser_shutdown();
int ser_fds(int* fds);

// Shared (net_common.cpp).
int net_arg(int argc, char** argv, const char* name);          // index, or 0
const char* net_arg_value(int argc, char** argv, const char* name);   // the word after, or null
bool net_aborted();                                             // Ctrl+C while catching it
void net_catch_ctrl_c(bool on);
void net_abort();                                               // as Ctrl+C (Esc in ipxsetup's box)
void net_unabort();

// ipxsetup's box: called often while a driver waits for the other players
// (found: nodes found so far, this one included; -1 before the search
// starts). It may call net_abort().
extern void (*net_ui_tick)(int found, int wanted);
void net_tick(int found, int wanted);
// After the driver's name in its banner: ", built in" in the game, "" in
// ipxsetup / sersetup.
extern const char* net_driver_note;
void net_doomcom_defaults(doomcom_t* dc);
void net_doomcom_options(int argc, char** argv, doomcom_t* dc); // -dup, -extratic

// Bytes in doomcom_t from data on: the vanilla packet and the free-look
// extension after it.
#define NET_DATA_MAX ((int)(sizeof(doomcom_t) - offsetof(doomcom_t, data)))

// ipxsetup / sersetup <-> the game, over a SOCK_SEQPACKET socket pair (the
// game's end is "-netfd N"). First the driver sends the whole doomcom_t
// (who is who, the game settings). Then each message is one packet: this
// header and datalength bytes of data; from the game, to send to
// remotenode; from the driver, one that came from remotenode.
struct netfd_msg {
    int16_t remotenode;
    int16_t datalength;
};
