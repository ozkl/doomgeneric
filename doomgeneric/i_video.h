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
//	System specific interface stuff.
//


#ifndef __I_VIDEO__
#define __I_VIDEO__

#include "doomtype.h"

// Screen width and height: set once at startup (I_SetScreenDimensions),
// from screen_scale (1-3) and screen_widescreen in default.cfg or -hires N
// and -widescreen / -nowidescreen. 320x200 by default.
//
// Everything drawn as a patch (menus, status bar, HUD, intermission...)
// stays in ORIGWIDTH x ORIGHEIGHT coordinates; the 2D code scales it by
// HIRES and centers it, WIDESCREENDELTA (in those coordinates) from the
// left. The 3D view uses the whole width: a 16:9 screen shows more at
// the sides. Pixels keep Doom's shape (1.2 times taller than wide), so a
// widescreen 426x200 picture is 16:9 when shown at 426x240.

#define ORIGWIDTH   320
#define ORIGHEIGHT  200
#define WIDEWIDTH   426                 // 16:9 at Doom's pixel shape
#define MAXHIRES    3
#define MAXWIDTH    (WIDEWIDTH * MAXHIRES)
#define MAXHEIGHT   (ORIGHEIGHT * MAXHIRES)

extern int SCREENWIDTH;                 // in pixels
extern int SCREENHEIGHT;
extern int HIRES;                       // pixels per ORIGWIDTH unit: 1, 2 or 3
extern int WIDESCREENDELTA;             // 4:3 area's left edge, in ORIGWIDTH units
extern int NONWIDEWIDTH;                // the 4:3 part's width, in pixels

extern int screen_scale;                // config: 1-3
extern int screen_widescreen;           // config: 0 or 1

void I_SetScreenDimensions(void);

// Screen width used for "squash" scale functions

#define SCREENWIDTH_4_3 256

// Screen height used for "stretch" scale functions.

#define SCREENHEIGHT_4_3 240

#define MAX_MOUSE_BUTTONS 8

typedef struct
{
    // Screen width and height

    int width;
    int height;

    // Initialisation function to call when using this mode.
    // Called with a pointer to the Doom palette.
    //
    // If NULL, no init function is called.

    void (*InitMode)(byte *palette);
    
    // Function to call to draw the screen from the source buffer.
    // Return true if draw was successful.

    boolean (*DrawScreen)(int x1, int y1, int x2, int y2);

    // If true, this is a "poor quality" mode.  The autoadjust
    // code should always attempt to use a different mode to this
    // mode in fullscreen.
    //
    // Some notes about what "poor quality" means in this context:
    //
    // The aspect ratio correction works by scaling up to the larger
    // screen size and then drawing pixels on the edges between the
    // "virtual" pixels so that an authentic blocky look-and-feel is
    // achieved.
    //
    // For a mode like 640x480, you can imagine the grid of the
    // "original" pixels spaced out, with extra "blurry" pixels added
    // in the space between them to fill it out. However, when you're
    // running at a resolution like 320x240, this is not the case. In
    // the small screen case, every single pixel has to be a blurry
    // interpolation of two pixels from the original image.
    //
    // If you run in 320x240 and put your face up close to the screen
    // you can see this: it's particularly visible in the small yellow
    // status bar numbers for example. Overall it still looks "okay"
    // but there's an obvious - albeit small - deterioration in
    // quality.
    //
    // Once you get to 640x480, all the original pixels are there at
    // least once and it's okay (the higher the resolution, the more
    // accurate it is). When I first wrote the code I was expecting
    // that even higher resolutions would be needed before it would
    // look acceptable, but it turned out to be okay even at 640x480.

    boolean poor_quality;
} screen_mode_t;

typedef boolean (*grabmouse_callback_t)(void);

// Called by D_DoomMain,
// determines the hardware configuration
// and sets up the video mode
void I_InitGraphics (void);

void I_GraphicsCheckCommandLine(void);

void I_ShutdownGraphics(void);

// Takes full 8 bit values.
void I_SetPalette (byte* palette);
int I_GetPaletteIndex(int r, int g, int b);

void I_UpdateNoBlit (void);
void I_FinishUpdate (void);

void I_ReadScreen (byte* scr);

void I_BeginRead (void);

void I_SetWindowTitle(char *title);

void I_CheckIsScreensaver(void);
void I_SetGrabMouseCallback(grabmouse_callback_t func);

void I_DisplayFPSDots(boolean dots_on);
void I_BindVideoVariables(void);

void I_InitWindowTitle(void);
void I_InitWindowIcon(void);

// Called before processing any tics in a frame (just after displaying a frame).
// Time consuming syncronous operations are performed here (joystick reading).

void I_StartFrame (void);

// Called before processing each tic in a frame.
// Quick syncronous operations are performed here.

void I_StartTic (void);

// Enable the loading disk image displayed when reading from disk.

void I_EnableLoadingDisk(void);

void I_EndRead (void);

struct color {
    uint32_t b:8;
    uint32_t g:8;
    uint32_t r:8;
    uint32_t a:8;
};


extern char *video_driver;
extern boolean screenvisible;

extern float mouse_acceleration;
extern int mouse_threshold;
extern int vanilla_keyboard_mapping;
extern boolean screensaver_mode;
extern int usegamma;
extern byte *I_VideoBuffer;

extern int screen_width;
extern int screen_height;
extern int screen_bpp;
extern int fullscreen;
extern int aspect_ratio_correct;

extern int show_diskicon;
extern int diskicon_readbytes;

#ifdef CMAP256

extern boolean palette_changed;
extern struct color colors[256];

#endif  // CMAP256

#endif
