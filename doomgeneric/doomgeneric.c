#include <stdio.h>

#include "m_argv.h"

#include "doomgeneric.h"
#include "i_video.h"

pixel_t* DG_ScreenBuffer = NULL;

void M_FindResponseFile(void);
void D_DoomMain (void);


void doomgeneric_Create(int argc, char **argv)
{
	// save arguments
    myargc = argc;
    myargv = argv;

	M_FindResponseFile();

	// Big enough for any screen size (i_video.h: up to MAXWIDTH x MAXHEIGHT
	// at 8 bits, or the platform's DOOMGENERIC_RESX x RESY at 32).
	{
		size_t n = (size_t)DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4;
		if (n < (size_t)MAXWIDTH * MAXHEIGHT)
			n = (size_t)MAXWIDTH * MAXHEIGHT;
		DG_ScreenBuffer = malloc(n);
	}

	DG_Init();

	D_DoomMain ();
}

