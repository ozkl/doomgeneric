// Boot-time IWAD picker, drawn on the libdebug text screen and driven by
// the PS2 controller (libpad). Used before SDL takes over the GS.
//
// If a controller never becomes ready within a short budget, it falls back
// to the first item so it can never hang.

#include <stdio.h>
#include <tamtypes.h>
#include <debug.h>      // scr_clear, scr_printf, scr_setXY
#include <loadfile.h>   // SifLoadModule
#include <libpad.h>     // padInit, padPortOpen, padRead, PAD_*

static char g_padBuf[256] __attribute__((aligned(64)));
static int  g_pad_inited = 0;

static void busy_wait(volatile int n)
{
    while (n-- > 0)
        __asm__ volatile ("nop");
}

static void pad_init_once(void)
{
    if (g_pad_inited)
        return;

    // The controller needs the SIO2 + pad managers. These may already be
    // loaded; SifLoadModule just errors harmlessly if so.
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:PADMAN", 0, NULL);

    padInit(0);
    padPortOpen(0, 0, g_padBuf);
    g_pad_inited = 1;
}

// Wait (bounded) for the pad port to reach a readable state.
static int pad_wait_ready(void)
{
    int tries;

    for (tries = 0; tries < 250; tries++)
    {
        int s = padGetState(0, 0);
        if (s == PAD_STATE_STABLE || s == PAD_STATE_FINDCTP1)
            return 1;
        busy_wait(2000000);
    }
    return 0;
}

int PS2_SelectMenu(const char *title, char **items, int count)
{
    struct padButtonStatus btn;
    int sel = 0;
    u16 prev = 0xFFFF;   // active-low: all released
    int i;

    pad_init_once();

    if (!pad_wait_ready())
    {
        printf("menu: no controller; auto-selecting %s\n", items[0]);
        return 0;
    }

    for (;;)
    {
        // Redraw the menu.
        scr_clear();
        scr_setXY(2, 1);
        scr_printf("%s", title);
        for (i = 0; i < count; i++)
        {
            scr_setXY(2, 3 + i);
            scr_printf("%s %s", (i == sel) ? ">" : " ", items[i]);
        }
        scr_setXY(2, 4 + count + 1);
        scr_printf("Up/Down: move    Cross/Start: select");

        // Read controller (btns are active-low: 0 == pressed).
        if (padRead(0, 0, &btn) != 0)
        {
            u16 now     = btn.btns;
            u16 pressed = (prev & ~now);   // 0->? edge: bit was 1 (up), now 0 (down)
            prev = now;

            if (pressed & PAD_UP)
                sel = (sel - 1 + count) % count;
            if (pressed & PAD_DOWN)
                sel = (sel + 1) % count;
            if (pressed & (PAD_CROSS | PAD_START))
                break;
        }

        busy_wait(1500000);
    }

    scr_clear();
    return sel;
}
