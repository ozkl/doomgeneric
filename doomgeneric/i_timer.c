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
//      Timer functions.
//

#include "i_timer.h"
#include "doomtype.h"

#include "doomgeneric.h"

#include <stdarg.h>

//#include <sys/time.h>
//#include <unistd.h>


//
// I_GetTime
// returns time in 1/35th second tics
//

static uint32_t basetime = 0;


int I_GetTicks(void)
{
	return DG_GetTicksMs();
}

int  I_GetTime (void)
{
    uint32_t ticks;

    ticks = I_GetTicks();

    if (basetime == 0)
        basetime = ticks;

    ticks -= basetime;

    return (ticks * TICRATE) / 1000;
}


//
// Same as I_GetTime, but returns time in milliseconds
//

int I_GetTimeMS(void)
{
    uint32_t ticks;

    ticks = I_GetTicks();

    if (basetime == 0)
        basetime = ticks;

    return ticks - basetime;
}

// Sleep for a specified number of ms

void I_Sleep(int ms)
{
    //SDL_Delay(ms);
    //usleep (ms * 1000);

	DG_SleepMs(ms);
}

void I_WaitVBL(int count)
{
    //I_Sleep((count * 1000) / 70);
}


void I_InitTimer(void)
{
    // initialize timer

    //SDL_Init(SDL_INIT_TIMER);
}

#ifdef __PS2__
// Re-base the game clock so the next I_GetTime() starts from "now". Called
// after the on-screen boot-log hold (doomgeneric_ps2.c), otherwise Doom would
// see the seconds spent showing the log as elapsed game time and fast-forward
// ("catch up") that many tics in a burst -- which desyncs the demo and thrashes
// the music right as the graphics come up.
void I_ResetBaseTime(void)
{
    basetime = 0;
}
#endif

