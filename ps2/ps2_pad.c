// PS2 controller input via libpad (ps2sdk's pad driver -- the same one
// ps2_menu.c uses, not a hand-rolled USB/HID stack). Polled once per frame by
// the video backend, which passes a callback to receive Doom key press/release
// events. Kept out of SDL so it works with the gsKit backend too (that one
// never brings up SDL video or its event loop).
//
// DualShock mapping:
//   D-pad        move / turn      (arrows)
//   Cross (X)    fire
//   Circle       use (doors/switches)
//   Square       run              (hold)
//   L1 / R1      strafe left/right
//   Triangle     enter            (confirm menus)
//   Start        escape           (open/close menu)
//   Select       tab              (automap)

#include <tamtypes.h>
#include <loadfile.h>   // SifLoadModule
#include <libpad.h>

#include "doomkeys.h"

static char g_padBuf[256] __attribute__((aligned(64)));
static int  g_inited = 0;
static u16  g_prev   = 0xFFFF;   // button state, active-low (1 == released)

void PS2Pad_Init(void)
{
    if (g_inited)
        return;

    // SIO2 + pad managers (harmless if already loaded by something else).
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:PADMAN", 0, NULL);

    padInit(0);
    padPortOpen(0, 0, g_padBuf);
    g_inited = 1;
}

static const struct { u16 mask; unsigned char key; } g_map[] = {
    { PAD_UP,       KEY_UPARROW    },
    { PAD_DOWN,     KEY_DOWNARROW  },
    { PAD_LEFT,     KEY_LEFTARROW  },
    { PAD_RIGHT,    KEY_RIGHTARROW },
    { PAD_CROSS,    KEY_FIRE       },
    { PAD_CIRCLE,   KEY_USE        },
    { PAD_SQUARE,   KEY_RSHIFT     },
    { PAD_TRIANGLE, KEY_ENTER      },
    { PAD_L1,       KEY_STRAFE_L   },
    { PAD_R1,       KEY_STRAFE_R   },
    { PAD_START,    KEY_ESCAPE     },
    { PAD_SELECT,   KEY_TAB        },
};

// Poll the pad and emit a Doom key event for every button that changed since
// last poll. `emit(pressed, doomkey)`: pressed != 0 on press, 0 on release.
void PS2Pad_Poll(void (*emit)(int pressed, unsigned char doomkey))
{
    struct padButtonStatus btn;
    int s, i;
    u16 now, changed;

    PS2Pad_Init();   // lazy, one-time

    s = padGetState(0, 0);
    if (s != PAD_STATE_STABLE && s != PAD_STATE_FINDCTP1)
        return;      // not ready yet (no controller / still detecting)

    if (padRead(0, 0, &btn) == 0)
        return;

    now     = btn.btns;          // active-low: a 0 bit means that button is down
    changed = g_prev ^ now;

    for (i = 0; i < (int) (sizeof(g_map) / sizeof(g_map[0])); ++i)
        if (changed & g_map[i].mask)
            emit((now & g_map[i].mask) == 0, g_map[i].key);

    g_prev = now;
}
