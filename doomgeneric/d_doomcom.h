//
// Copyright(C) 1993-1996 Id Software, Inc.
//
// The original DOS network driver interface (IPXSETUP.EXE, SERSETUP.EXE):
// the driver starts DOOM with "-net <address>", the linear address of
// a doomcom_t in its own memory, and DOOM executes doomcom->intnum to have
// it send or fetch a packet. These are vanilla's layouts byte for byte
// (little-endian), so this port and the original DOOM.EXE can play together.
//

#ifndef __D_DOOMCOM__
#define __D_DOOMCOM__

#include <stdint.h>

#define DOOMCOM_ID          0x12345678
#define MAXNETNODES         8
#define VANILLA_BACKUPTICS  12          // ticcmds per packet, at most

#define CMD_SEND            1
#define CMD_GET             2

// A ticcmd as vanilla sends it: 8 bytes.
typedef struct
{
    int8_t      forwardmove;
    int8_t      sidemove;
    int16_t     angleturn;
    int16_t     consistancy;
    uint8_t     chatchar;
    uint8_t     buttons;
} vanilla_ticcmd_t;

typedef struct
{
    uint32_t            checksum;       // high bits: NCMD_* flags
    uint8_t             retransmitfrom; // only valid if NCMD_RETRANSMIT
    uint8_t             starttic;
    uint8_t             player;
    uint8_t             numtics;
    vanilla_ticcmd_t    cmds[VANILLA_BACKUPTICS];
} doomdata_t;

typedef struct doomcom_s
{
    int32_t     id;                     // DOOMCOM_ID
    int16_t     intnum;                 // the driver's interrupt
    int16_t     command;                // CMD_SEND or CMD_GET
    int16_t     remotenode;             // dest for send, set by get (-1 = no packet)
    int16_t     datalength;             // bytes in data to send / read
    int16_t     numnodes;               // console is always node 0
    int16_t     ticdup;                 // 1 = no duplication, 2-5 = dup for slow nets
    int16_t     extratics;              // 1 = send a backup tic in every packet
    int16_t     deathmatch;
    int16_t     savegame;
    int16_t     episode;
    int16_t     map;
    int16_t     skill;
    int16_t     consoleplayer;          // 0-3
    int16_t     numplayers;             // 1-4
    int16_t     angleoffset;            // 1 = left, 0 = center, -1 = right
    int16_t     drone;
    doomdata_t  data;

    // Not vanilla: room for the free-look extension (below) right after
    // the vanilla packet. The drivers' own buffers hold 512 bytes.
    uint8_t     dataext[VANILLA_BACKUPTICS * 4];
} doomcom_t;

// Compile-time checks that the layouts are vanilla's.
typedef char doomcom_check_ticcmd[sizeof(vanilla_ticcmd_t) == 8 ? 1 : -1];
typedef char doomcom_check_data[sizeof(doomdata_t) == 104 ? 1 : -1];
typedef char doomcom_check_doomcom[sizeof(doomcom_t) == 140 + VANILLA_BACKUPTICS * 4 ? 1 : -1];

// Free look between copies of this port. Each of our packets that isn't a
// resend request carries PORT_MARKER in retransmitfrom (which vanilla only
// reads in resend requests). Once a machine has seen it from every other
// node, it sends them the free-look extension: after the numtics vanilla
// ticcmds, one look_ext_t per ticcmd, the checksum then covering the
// whole packet. A packet to the original DOOM.EXE never has it.
#define PORT_MARKER         0xF1

typedef struct
{
    int16_t     lookdelta;
    uint8_t     lookflags;
    uint8_t     reserved;
} look_ext_t;

// The platform's side, set before doomgeneric_Create when it has a driver
// (the DOS build, from -net): NetInit returns its copy of the driver's
// doomcom_t (or NULL), NetCmd carries out doomcom->command through the
// driver, copying the packet across.
extern doomcom_t *(*I_VanillaNetInit)(void);
extern void (*I_VanillaNetCmd)(void);

// d_net.c: true when this is a vanilla-driver net game.
int D_VanillaNet(void);
// True when every other machine in the game runs this port, so this one
// may send free-look ticcmds (P_FreeLook).
int D_NetFreeLook(void);
void D_VanillaNetUpdate(void);
void D_VanillaTryRunTics(void);
void D_VanillaQuitNetGame(void);

#endif
