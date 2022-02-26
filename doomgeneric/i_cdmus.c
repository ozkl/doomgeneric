//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 1993-2008 Raven Software
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
//
// SDL implementation of the Hexen CD interface.
//

#include <stdio.h>

#ifdef ORIGCODE
#include "SDL2/SDL.h"
#include "SDL2/SDL_cdrom.h"
#endif

#include "doomtype.h"

#include "i_cdmus.h"

#ifdef ORIGCODE
static SDL_CD *cd_handle = NULL;
static char *startup_error = NULL;
static const char *cd_name = NULL;
#endif

int cd_Error;

int I_CDMusInit(void)
{
#ifdef ORIGCODE
    int drive_num = 0;

    // The initialize function is re-invoked when the CD track play cheat
    // is used, so use the opportunity to call SDL_CDStatus() to update
    // the status of the drive.

    if (cd_handle == NULL)
    {
        if (SDL_Init(SDL_INIT_CDROM) < 0)
        {
            startup_error = "Failed to init CD subsystem.";
            cd_Error = 1;
            return -1;
        }

        // TODO: config variable to control CDROM to use.

        cd_handle = SDL_CDOpen(drive_num);

        if (cd_handle == NULL)
        {
            startup_error = "Failed to open CD-ROM drive.";
            cd_Error = 1;
            return -1;
        }

        cd_name = SDL_CDName(drive_num);
    }

    if (SDL_CDStatus(cd_handle) == CD_ERROR)
    {
        startup_error = "Failed to read CD status.";
        cd_Error = 1;
        return -1;
    }

    if (!CD_INDRIVE(cd_handle->status))
    {
        startup_error = "No CD in drive.";
        cd_Error = 1;
        return -1;
    }

    cd_Error = 0;
#endif
    return 0;
}

// We cannot print status messages inline during startup, they must
// be deferred until after I_CDMusInit has returned.

void I_CDMusPrintStartup(void)
{
#ifdef ORIGCODE
    if (cd_name != NULL)
    {
        printf("I_CDMusInit: Using CD-ROM drive: %s\n", cd_name);
    }

    if (startup_error != NULL)
    {
        fprintf(stderr, "I_CDMusInit: %s\n", startup_error);
    }
#endif
}

int I_CDMusPlay(int track)
{
#ifdef ORIGCODE
    int result;

    if (cd_handle == NULL)
    {
        cd_Error = 1;
        return -1;
    }

    // Play one track
    // Track is indexed from 1.

    result = SDL_CDPlayTracks(cd_handle, track - 1, 0, 1, 0);

    cd_Error = 0;
    return result;
#else
	return 0;
#endif
}

int I_CDMusStop(void)
{
#ifdef ORIGCODE
    int result;

    result = SDL_CDStop(cd_handle);

    cd_Error = 0;

    return result;
#else
	return 0;
#endif
}

int I_CDMusResume(void)
{
#ifdef ORIGCODE
    int result;

    result = SDL_CDResume(cd_handle);

    cd_Error = 0;

    return result;
#else
	return 0;
#endif
}

int I_CDMusSetVolume(int volume)
{
    /* Not supported yet */

    cd_Error = 0;

    return 0;
}

int I_CDMusFirstTrack(void)
{
#ifdef ORIGCODE
    int i;

    if (cd_handle == NULL)
    {
        cd_Error = 1;
        return -1;
    }

    // Find the first audio track.

    for (i=0; i<cd_handle->numtracks; ++i) 
    {
        if (cd_handle->track[i].type == SDL_AUDIO_TRACK)
        {
            cd_Error = 0;

            // Tracks are indexed from 1.
            return i + 1;
        }
    }

    // Don't know?
    cd_Error = 1;

    return -1;
#else
	return 0;
#endif
}

int I_CDMusLastTrack(void)
{
#ifdef ORIGCODE
    if (cd_handle == NULL)
    {
        cd_Error = 1;
        return -1;
    }

    cd_Error = 0;

    return cd_handle->numtracks;
#else
	return 0;
#endif
}

int I_CDMusTrackLength(int track_num)
{
#ifdef ORIGCODE
    SDL_CDtrack *track;

    if (cd_handle == NULL || track_num < 1 || track_num > cd_handle->numtracks)
    {
        cd_Error = 1;
        return -1;
    }

    // Track number is indexed from 1.

    track = &cd_handle->track[track_num - 1];

    // Round up to the next second

    cd_Error = 0;

    return (track->length + CD_FPS - 1) / CD_FPS;
#else
	return 0;
#endif
}

