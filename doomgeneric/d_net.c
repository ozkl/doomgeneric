//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	DOOM Network game communication and protocol,
//	all OS independend parts.
//

#include <stdlib.h>

#include "doomfeatures.h"

#include "d_main.h"
#include "m_argv.h"
#include "m_menu.h"
#include "m_misc.h"
#include "i_system.h"
#include "i_timer.h"
#include "i_video.h"
#include "g_game.h"
#include "doomdef.h"
#include "doomstat.h"
#include "w_checksum.h"
#include "w_wad.h"

#include "deh_main.h"

#include "d_loop.h"
#include "d_doomcom.h"

ticcmd_t *netcmds;

static void VanillaCheckNetGame(void);     // below: IPXSETUP / SERSETUP games
static doomcom_t *vanilla_doomcom(void);

// Called when a player leaves the game

static void PlayerQuitGame(player_t *player)
{
    static char exitmsg[80];
    unsigned int player_num;

    player_num = player - players;

    // Do this the same way as Vanilla Doom does, to allow dehacked
    // replacements of this message

    M_StringCopy(exitmsg, DEH_String("Player 1 left the game"),
                 sizeof(exitmsg));

    exitmsg[7] += player_num;

    playeringame[player_num] = false;
    players[consoleplayer].message = exitmsg;

    // TODO: check if it is sensible to do this:

    if (demorecording) 
    {
        G_CheckDemoStatus ();
    }
}

static void RunTic(ticcmd_t *cmds, boolean *ingame)
{
    extern boolean advancedemo;
    unsigned int i;

    // Check for player quits.

    for (i = 0; i < MAXPLAYERS; ++i)
    {
        if (!demoplayback && playeringame[i] && !ingame[i])
        {
            PlayerQuitGame(&players[i]);
        }
    }

    netcmds = cmds;

    // check that there are players in the game.  if not, we cannot
    // run a tic.

    if (advancedemo)
        D_DoAdvanceDemo ();

    G_Ticker ();
}

static loop_interface_t doom_loop_interface = {
    D_ProcessEvents,
    G_BuildTiccmd,
    RunTic,
    M_Ticker
};


// Load game settings from the specified structure and
// set global variables.

static void LoadGameSettings(net_gamesettings_t *settings)
{
    unsigned int i;

    deathmatch = settings->deathmatch;
    startepisode = settings->episode;
    startmap = settings->map;
    startskill = settings->skill;
    startloadgame = settings->loadgame;
    lowres_turn = settings->lowres_turn;
    nomonsters = settings->nomonsters;
    fastparm = settings->fast_monsters;
    respawnparm = settings->respawn_monsters;
    timelimit = settings->timelimit;
    consoleplayer = settings->consoleplayer;

    if (lowres_turn)
    {
        printf("NOTE: Turning resolution is reduced; this is probably "
               "because there is a client recording a Vanilla demo.\n");
    }

    for (i = 0; i < MAXPLAYERS; ++i)
    {
        playeringame[i] = i < settings->num_players;
    }
}

// Save the game settings from global variables to the specified
// game settings structure.

static void SaveGameSettings(net_gamesettings_t *settings)
{
    // Fill in game settings structure with appropriate parameters
    // for the new game

    settings->deathmatch = deathmatch;
    settings->episode = startepisode;
    settings->map = startmap;
    settings->skill = startskill;
    settings->loadgame = startloadgame;
    settings->gameversion = gameversion;
    settings->nomonsters = nomonsters;
    settings->fast_monsters = fastparm;
    settings->respawn_monsters = respawnparm;
    settings->timelimit = timelimit;

    settings->lowres_turn = M_CheckParm("-record") > 0
                         && M_CheckParm("-longtics") == 0;
}

static void InitConnectData(net_connect_data_t *connect_data)
{
    connect_data->max_players = MAXPLAYERS;
    connect_data->drone = false;

    //!
    // @category net
    //
    // Run as the left screen in three screen mode.
    //

    if (M_CheckParm("-left") > 0)
    {
        viewangleoffset = ANG90;
        connect_data->drone = true;
    }

    //! 
    // @category net
    //
    // Run as the right screen in three screen mode.
    //

    if (M_CheckParm("-right") > 0)
    {
        viewangleoffset = ANG270;
        connect_data->drone = true;
    }

    //
    // Connect data
    //

    // Game type fields:

    connect_data->gamemode = gamemode;
    connect_data->gamemission = gamemission;

    // Are we recording a demo? Possibly set lowres turn mode

    connect_data->lowres_turn = M_CheckParm("-record") > 0
                             && M_CheckParm("-longtics") == 0;

    // Read checksums of our WAD directory and dehacked information

    W_Checksum(connect_data->wad_sha1sum);

#if ORIGCODE
    DEH_Checksum(connect_data->deh_sha1sum);
#endif

    // Are we playing with the Freedoom IWAD?

    connect_data->is_freedoom = W_CheckNumForName("FREEDOOM") >= 0;
}

void D_ConnectNetGame(void)
{
    net_connect_data_t connect_data;

    InitConnectData(&connect_data);
    netgame = D_InitNetGame(&connect_data);

    //!
    // @arg <address>
    // @category net
    //
    // Play a net game through an original DOS network driver (IPXSETUP,
    // SERSETUP), with this port or the original DOOM.EXE on the other
    // machines. The driver adds this itself when it starts the game.
    //

    if (vanilla_doomcom() != NULL)
    {
        netgame = true;
    }

    //!
    // @category net
    //
    // Start the game playing as though in a netgame with a single
    // player.  This can also be used to play back single player netgame
    // demos.
    //

    if (M_CheckParm("-solo-net") > 0)
    {
        netgame = true;
    }
}

//
// D_CheckNetGame
// Works out player numbers among the net participants
//
void D_CheckNetGame (void)
{
    net_gamesettings_t settings;

    if (netgame)
    {
        autostart = true;
    }

    D_RegisterLoopCallbacks(&doom_loop_interface);

    if (D_VanillaNet())
    {
        VanillaCheckNetGame();
        return;
    }

    SaveGameSettings(&settings);
    D_StartNetGame(&settings, NULL);
    LoadGameSettings(&settings);

    DEH_printf("startskill %i  deathmatch: %i  startmap: %i  startepisode: %i\n",
               startskill, deathmatch, startmap, startepisode);

    DEH_printf("player %i of %i (%i nodes)\n",
               consoleplayer+1, settings.num_players, settings.num_players);

    // Show players here; the server might have specified a time limit

    if (timelimit > 0 && deathmatch)
    {
        // Gross hack to work like Vanilla:

        if (timelimit == 20 && M_CheckParm("-avg"))
        {
            DEH_printf("Austin Virtual Gaming: Levels will end "
                           "after 20 minutes\n");
        }
        else
        {
            DEH_printf("Levels will end after %d minute", timelimit);
            if (timelimit > 1)
                printf("s");
            printf(".\n");
        }
    }
}



// ================================================================
// Vanilla network games: the original DOS drivers (IPXSETUP, SERSETUP).
//
// Started from a driver with -net <address>, the game speaks DOOM 1.9's own
// protocol through the driver's doomcom_t (d_doomcom.h) instead of
// Chocolate Doom's client/server one, so it can play with the original
// DOOM.EXE (same IWAD, same version). This is vanilla's d_net.c: lockstep
// ticcmds, each node sending its own to every other, resends on request,
// and the key player (player 1) sending the game settings at the start.
// ================================================================

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "doomkeys.h"

#define NCMD_EXIT       0x80000000
#define NCMD_RETRANSMIT 0x40000000
#define NCMD_SETUP      0x20000000
#define NCMD_KILL       0x10000000      // kill game
#define NCMD_CHECKSUM   0x0fffffff

#define RESENDCOUNT     10
#define PL_DRONE        0x80            // bit flag in doomdata->player

doomcom_t *(*I_VanillaNetInit)(void);
void (*I_VanillaNetCmd)(void);

static doomcom_t  *doomcom;             // the platform's copy of the driver's
static doomdata_t *netbuffer;           // points inside doomcom

static ticcmd_t localcmds[BACKUPTICS];
static ticcmd_t vnetcmds[MAXPLAYERS][BACKUPTICS];
static boolean  vingame[NET_MAXPLAYERS];    // false once a player has left
static int      nettics[MAXNETNODES];
static boolean  nodeingame[MAXNETNODES];    // set false as nodes leave game
static boolean  remoteresend[MAXNETNODES];  // set when local needs tics
static int      resendto[MAXNETNODES];      // set when remote needs tics
static int      resendcount[MAXNETNODES];
static int      nodeforplayer[MAXPLAYERS];

static int      vmaketic;
static int      vskiptics;
static int      gametime;

static boolean    reboundpacket;
static doomdata_t reboundstore;
static uint8_t    reboundext[sizeof(((doomcom_t *)0)->dataext)];
static int        reboundlength;

static boolean  nodeisport[MAXNETNODES];    // seen PORT_MARKER from it
static boolean  packetext;                  // the packet in netbuffer has look_ext_t

int D_VanillaNet(void)
{
    return doomcom != NULL;
}

// The driver's doomcom_t, if the platform has one (-net); asked once.
static doomcom_t *vanilla_doomcom(void)
{
    static boolean asked;

    if (!asked)
    {
        asked = true;
        if (I_VanillaNetInit != NULL && I_VanillaNetCmd != NULL)
        {
            doomcom = I_VanillaNetInit();
            if (doomcom != NULL && doomcom->id != DOOMCOM_ID)
                I_Error("Doomcom buffer invalid!");
        }
    }
    return doomcom;
}

// Every other node in the game runs this port: free look may go on.
int D_NetFreeLook(void)
{
    int i;

    if (!doomcom)
        return false;
    for (i = 1; i < doomcom->numnodes; i++)
        if (nodeingame[i] && !nodeisport[i])
            return false;
    return true;
}

// The free-look extension: right after the packet's numtics ticcmds, which
// is where it goes on the wire (it may run on into doomcom->dataext).
static look_ext_t *PacketExt(void)
{
    return (look_ext_t *) &netbuffer->cmds[netbuffer->numtics];
}

// Vanilla's wire ticcmd <-> ours.
static void PackCmd(vanilla_ticcmd_t *w, const ticcmd_t *c)
{
    w->forwardmove = c->forwardmove;
    w->sidemove = c->sidemove;
    w->angleturn = c->angleturn;
    w->consistancy = c->consistancy;
    w->chatchar = c->chatchar;
    w->buttons = c->buttons;
}

static void UnpackCmd(ticcmd_t *c, const vanilla_ticcmd_t *w)
{
    memset(c, 0, sizeof(*c));
    c->forwardmove = w->forwardmove;
    c->sidemove = w->sidemove;
    c->angleturn = w->angleturn;
    c->consistancy = w->consistancy;
    c->chatchar = w->chatchar;
    c->buttons = w->buttons;
}

static int NetbufferSize(void)
{
    return (int) offsetof(doomdata_t, cmds) + netbuffer->numtics * (int) sizeof(vanilla_ticcmd_t);
}

// With the free-look extension (packets between copies of this port).
static int NetbufferExtSize(void)
{
    return NetbufferSize() + netbuffer->numtics * (int) sizeof(look_ext_t);
}

// As DOS vanilla (the Linux port returned 0 here): a weighted sum of the
// packet's words after the checksum. size: NetbufferSize(), or
// NetbufferExtSize() for a packet with the extension (it follows the
// vanilla part directly in doomcom_t).
static unsigned NetbufferChecksum(int size)
{
    unsigned c = 0x1234567;
    const uint8_t *p = &netbuffer->retransmitfrom;
    int i, l;

    l = (size - (int) offsetof(doomdata_t, retransmitfrom)) / 4;
    for (i = 0; i < l; i++)
    {
        uint32_t w = p[i*4] | p[i*4+1] << 8 | p[i*4+2] << 16 | (uint32_t) p[i*4+3] << 24;
        c += w * (i + 1);
    }

    return c & NCMD_CHECKSUM;
}

static int ExpandTics(int low)
{
    int delta = low - (vmaketic & 0xff);

    if (delta >= -64 && delta <= 64)
        return (vmaketic & ~0xff) + low;
    if (delta > 64)
        return (vmaketic & ~0xff) - 256 + low;
    if (delta < -64)
        return (vmaketic & ~0xff) + 256 + low;

    I_Error("ExpandTics: strange value %i at maketic %i", low, vmaketic);
    return 0;
}

// ext: the packet has the free-look extension (filled in by the caller);
// only ever for this node or a node known to run this port.
static void HSendPacket(int node, unsigned flags, boolean ext)
{
    int size = ext && netbuffer->numtics ? NetbufferExtSize() : NetbufferSize();

    netbuffer->checksum = NetbufferChecksum(size) | flags;

    if (!node)
    {
        reboundstore = *netbuffer;
        memcpy(reboundext, doomcom->dataext, sizeof(reboundext));
        reboundlength = size;
        reboundpacket = true;
        return;
    }

    if (demoplayback)
        return;

    doomcom->command = CMD_SEND;
    doomcom->remotenode = node;
    doomcom->datalength = size;
    I_VanillaNetCmd();
}

// Returns false if no packet is waiting.
static boolean HGetPacket(void)
{
    if (reboundpacket)
    {
        *netbuffer = reboundstore;
        memcpy(doomcom->dataext, reboundext, sizeof(reboundext));
        packetext = reboundlength != NetbufferSize();
        doomcom->remotenode = 0;
        reboundpacket = false;
        return true;
    }

    if (demoplayback)
        return false;

    doomcom->command = CMD_GET;
    I_VanillaNetCmd();

    if (doomcom->remotenode == -1)
        return false;
    if (doomcom->remotenode < 0 || doomcom->remotenode >= MAXNETNODES)
        return false;
    if (netbuffer->numtics > VANILLA_BACKUPTICS)
        return false;

    // A vanilla packet, or one with the free-look extension.
    if (doomcom->datalength == NetbufferSize())
        packetext = false;
    else if (netbuffer->numtics && doomcom->datalength == NetbufferExtSize())
        packetext = true;
    else
        return false;

    if (NetbufferChecksum(doomcom->datalength) != (netbuffer->checksum & NCMD_CHECKSUM))
        return false;

    return true;
}

static void GetPackets(void)
{
    int netconsole, netnode;
    int realstart, realend;

    while (HGetPacket())
    {
        if (netbuffer->checksum & NCMD_SETUP)
            continue;           // extra setup packet

        netconsole = netbuffer->player & ~PL_DRONE;
        netnode = doomcom->remotenode;
        if (netconsole >= MAXPLAYERS)
            continue;

        // to save bytes, only the low byte of tic numbers are sent
        realstart = ExpandTics(netbuffer->starttic);
        realend = realstart + netbuffer->numtics;

        // check for exiting the game
        if (netbuffer->checksum & NCMD_EXIT)
        {
            if (!nodeingame[netnode])
                continue;
            nodeingame[netnode] = false;
            // RunTic (above) shows "Player N left the game".
            vingame[netconsole] = false;
            continue;
        }

        // check for a remote game kill
        if (netbuffer->checksum & NCMD_KILL)
            I_Error("Killed by network driver");

        nodeforplayer[netconsole] = netnode;

        // Another copy of this port (see PORT_MARKER).
        if (packetext || (!(netbuffer->checksum & NCMD_RETRANSMIT)
                          && netbuffer->retransmitfrom == PORT_MARKER))
            nodeisport[netnode] = true;

        // check for retransmit request
        if (resendcount[netnode] <= 0
         && (netbuffer->checksum & NCMD_RETRANSMIT))
        {
            resendto[netnode] = ExpandTics(netbuffer->retransmitfrom);
            resendcount[netnode] = RESENDCOUNT;
        }
        else
            resendcount[netnode]--;

        // check for out of order / duplicated packet
        if (realend == nettics[netnode])
            continue;
        if (realend < nettics[netnode])
            continue;

        // check for a missed packet
        if (realstart > nettics[netnode])
        {
            // stop processing until the other system resends the missed tics
            remoteresend[netnode] = true;
            continue;
        }

        // update command store from the packet
        {
            int i = nettics[netnode] - realstart;

            remoteresend[netnode] = false;
            while (nettics[netnode] < realend)
            {
                ticcmd_t *dest = &vnetcmds[netconsole][nettics[netnode] % BACKUPTICS];

                UnpackCmd(dest, &netbuffer->cmds[i]);
                if (packetext)
                {
                    dest->lookdelta = PacketExt()[i].lookdelta;
                    dest->lookflags = PacketExt()[i].lookflags;
                }
                i++;
                nettics[netnode]++;
            }
        }
    }
}

// Builds ticcmds for the console player, sends out a packet.
void D_VanillaNetUpdate(void)
{
    int nowtime, newtics;
    int i, j, realstart, gameticdiv;
    boolean ext;

    // check time
    nowtime = I_GetTime() / ticdup;
    newtics = nowtime - gametime;
    gametime = nowtime;

    if (newtics <= 0)           // nothing new to update
        goto listen;

    if (vskiptics <= newtics)
    {
        newtics -= vskiptics;
        vskiptics = 0;
    }
    else
    {
        vskiptics -= newtics;
        newtics = 0;
    }

    netbuffer->player = consoleplayer;

    // build new ticcmds for console player
    gameticdiv = gametic / ticdup;
    for (i = 0; i < newtics; i++)
    {
        I_StartTic();
        D_ProcessEvents();
        M_Ticker();             // the menu runs whatever the game's doing
        if (vmaketic - gameticdiv >= VANILLA_BACKUPTICS/2 - 1)
            break;              // can't hold any more

        G_BuildTiccmd(&localcmds[vmaketic % BACKUPTICS], vmaketic);
        vmaketic++;
    }

    if (singletics)
        return;                 // singletic update is syncronous

    // send the packet to the other nodes
    for (i = 0; i < doomcom->numnodes; i++)
    {
        if (!nodeingame[i])
            continue;

        netbuffer->starttic = realstart = resendto[i];
        netbuffer->numtics = vmaketic - realstart;
        if (netbuffer->numtics > VANILLA_BACKUPTICS)
            I_Error("NetUpdate: netbuffer->numtics > BACKUPTICS");

        resendto[i] = vmaketic - doomcom->extratics;

        // Ourselves and other copies of this port get the look extension.
        ext = i == 0 || nodeisport[i];
        for (j = 0; j < netbuffer->numtics; j++)
        {
            const ticcmd_t *c = &localcmds[(realstart+j) % BACKUPTICS];

            PackCmd(&netbuffer->cmds[j], c);
            if (ext)
            {
                PacketExt()[j].lookdelta = c->lookdelta;
                PacketExt()[j].lookflags = c->lookflags;
                PacketExt()[j].reserved = 0;
            }
            else if (c->lookflags)
                I_Error("NetUpdate: free look to a node without it");
        }

        if (remoteresend[i])
        {
            netbuffer->retransmitfrom = nettics[i];
            HSendPacket(i, NCMD_RETRANSMIT, ext);
        }
        else
        {
            netbuffer->retransmitfrom = PORT_MARKER;    // vanilla ignores it here
            HSendPacket(i, 0, ext);
        }
    }

    // listen for other packets
listen:
    GetPackets();
}

// Escape aborts while waiting for the other players.
static void CheckAbort(void)
{
    event_t *ev;
    int stoptic;

    stoptic = I_GetTime() + 2;
    while (I_GetTime() < stoptic)
    {
        I_StartTic();
        I_Sleep(1);
    }

    I_StartTic();
    while ((ev = D_PopEvent()) != NULL)
    {
        if (ev->type == ev_keydown && ev->data1 == KEY_ESCAPE)
            I_Error("Network game synchronization aborted.");
    }
}

// The key player (node 0's console player 0) sends the settings; the
// others wait for them.
static void ArbitrateNetStart(void)
{
    boolean gotinfo[MAXNETNODES];
    int i, version = G_VanillaVersionCode();

    autostart = true;
    memset(gotinfo, 0, sizeof(gotinfo));

    if (doomcom->consoleplayer)
    {
        // listen for setup info from key player
        printf("listening for network start info...\n");
        while (1)
        {
            CheckAbort();
            if (!HGetPacket())
                continue;
            if (netbuffer->checksum & NCMD_SETUP)
            {
                if (netbuffer->player != version)
                    I_Error("Different DOOM versions cannot play a net game! "
                            "(%i here, %i there)", version, netbuffer->player);
                startskill = netbuffer->retransmitfrom & 15;
                deathmatch = (netbuffer->retransmitfrom & 0xc0) >> 6;
                nomonsters = (netbuffer->retransmitfrom & 0x20) > 0;
                respawnparm = (netbuffer->retransmitfrom & 0x10) > 0;
                startmap = netbuffer->starttic & 0x3f;
                startepisode = netbuffer->starttic >> 6;
                return;
            }
        }
    }
    else
    {
        // key player, send the setup info
        printf("sending network start info...\n");
        do
        {
            CheckAbort();
            for (i = 0; i < doomcom->numnodes; i++)
            {
                netbuffer->retransmitfrom = startskill;
                if (deathmatch)
                    netbuffer->retransmitfrom |= (deathmatch << 6);
                if (nomonsters)
                    netbuffer->retransmitfrom |= 0x20;
                if (respawnparm)
                    netbuffer->retransmitfrom |= 0x10;
                netbuffer->starttic = startepisode * 64 + startmap;
                netbuffer->player = version;
                netbuffer->numtics = 0;
                HSendPacket(i, NCMD_SETUP, false);
            }

            for (i = 10; i && HGetPacket(); --i)
            {
                if ((netbuffer->player & 0x7f) < MAXNETNODES)
                    gotinfo[netbuffer->player & 0x7f] = true;
            }

            for (i = 1; i < doomcom->numnodes; i++)
                if (!gotinfo[i])
                    break;
        } while (i < doomcom->numnodes);
    }
}

// D_CheckNetGame's vanilla half: who's who, and the settings from the key
// player.
static void VanillaCheckNetGame(void)
{
    int i;

    for (i = 0; i < MAXNETNODES; i++)
    {
        nodeingame[i] = false;
        nettics[i] = 0;
        remoteresend[i] = false;
        resendto[i] = 0;
    }

    netbuffer = &doomcom->data;
    consoleplayer = displayplayer = doomcom->consoleplayer;
    if (consoleplayer < 0 || consoleplayer >= MAXPLAYERS
     || doomcom->numplayers < 1 || doomcom->numplayers > MAXPLAYERS
     || doomcom->numnodes < 1 || doomcom->numnodes > MAXNETNODES)
        I_Error("Bad network driver settings: player %i of %i, %i nodes",
                doomcom->consoleplayer + 1, doomcom->numplayers, doomcom->numnodes);
    if (doomcom->drone)
        I_Error("Drone (three-screen) mode isn't supported");

    // Vanilla's sync: free look, demos and the like stay as DOOM.EXE has them.
    netgame = true;
    ArbitrateNetStart();

    DEH_printf("startskill %i  deathmatch: %i  startmap: %i  startepisode: %i\n",
               startskill, deathmatch, startmap, startepisode);

    // read values out of doomcom
    ticdup = doomcom->ticdup < 1 ? 1 : doomcom->ticdup;

    for (i = 0; i < MAXPLAYERS; i++)
    {
        playeringame[i] = i < doomcom->numplayers;
        vingame[i] = playeringame[i];
    }
    for (i = 0; i < doomcom->numnodes; i++)
        nodeingame[i] = true;

    DEH_printf("player %i of %i (%i nodes)\n",
               consoleplayer+1, doomcom->numplayers, doomcom->numnodes);
}

// Called before quitting to leave a net game without hanging the others.
void D_VanillaQuitNetGame(void)
{
    int i, j;

    if (!doomcom || !usergame || consoleplayer == -1 || demoplayback)
        return;

    // send a bunch of packets for security
    netbuffer->player = consoleplayer;
    netbuffer->numtics = 0;
    for (i = 0; i < 4; i++)
    {
        for (j = 1; j < doomcom->numnodes; j++)
            if (nodeingame[j])
                HSendPacket(j, NCMD_EXIT, false);
        I_Sleep(15);
    }
}

static int LowTic(void)
{
    int i, lowtic = INT_MAX;

    for (i = 0; i < doomcom->numnodes; i++)
        if (nodeingame[i] && nettics[i] < lowtic)
            lowtic = nettics[i];
    return lowtic;
}

void D_VanillaTryRunTics(void)
{
    static int oldentertics, frameon, frameskip[4], oldnettics;
    int i, lowtic, entertic, realtics, availabletics, counts;

    // get real tics
    entertic = I_GetTime() / ticdup;
    realtics = entertic - oldentertics;
    oldentertics = entertic;

    // get available tics
    D_VanillaNetUpdate();

    lowtic = LowTic();
    availabletics = lowtic - gametic/ticdup;

    // decide how many tics to run
    if (realtics < availabletics-1)
        counts = realtics+1;
    else if (realtics < availabletics)
        counts = realtics;
    else
        counts = availabletics;

    if (counts < 1)
        counts = 1;

    frameon++;

    if (!demoplayback)
    {
        // ideally nettics[0] should be 1 - 3 tics above lowtic
        // if we are consistantly slower, speed up time
        for (i = 0; i < MAXPLAYERS; i++)
            if (playeringame[i])
                break;
        if (consoleplayer == i)
        {
            // the key player does not adapt
        }
        else if (i < MAXPLAYERS)
        {
            if (nettics[0] <= nettics[nodeforplayer[i]])
                gametime--;
            frameskip[frameon & 3] = oldnettics > nettics[nodeforplayer[i]];
            oldnettics = nettics[0];
            if (frameskip[0] && frameskip[1] && frameskip[2] && frameskip[3])
                vskiptics = 1;
        }
    }

    // wait for new tics if needed
    while (lowtic < gametic/ticdup + counts)
    {
        D_VanillaNetUpdate();
        lowtic = LowTic();

        if (lowtic < gametic/ticdup)
            I_Error("TryRunTics: lowtic < gametic");

        // don't stay in here forever -- give the menu and screen a chance
        if (I_GetTime()/ticdup - entertic >= 1)
            return;

        I_Sleep(1);
    }

    // run the count * ticdup tics
    while (counts--)
    {
        for (i = 0; i < ticdup; i++)
        {
            ticcmd_t cmds[NET_MAXPLAYERS];
            int p, buf = (gametic/ticdup) % BACKUPTICS;

            if (gametic/ticdup > lowtic)
                I_Error("gametic>lowtic");

            memset(cmds, 0, sizeof(cmds));
            for (p = 0; p < MAXPLAYERS; p++)
                cmds[p] = vnetcmds[p][buf];
            // Our own commands: rebound through node 0 into vnetcmds.
            RunTic(cmds, vingame);
            gametic++;

            // modify command for duplicated tics
            if (i != ticdup-1)
            {
                for (p = 0; p < MAXPLAYERS; p++)
                {
                    ticcmd_t *cmd = &vnetcmds[p][buf];
                    cmd->chatchar = 0;
                    if (cmd->buttons & BT_SPECIAL)
                        cmd->buttons = 0;
                }
            }
        }
        D_VanillaNetUpdate();   // check for new console commands
    }
}
