// PS2 boot text console.
//
// Brings up the GS debug text framebuffer (libdebug) and redirects the EE
// stdout/stderr to it, so Doom's normal boot log is visible on screen
// before SDL takes over the GS for the game's graphical framebuffer.
//
// ps2sdk's default printf goes to the SIO serial port, which emulators don't
// show -- hence this on-screen route.

#include <stdio.h>
#include <string.h>
#include <debug.h>      // init_scr, scr_printf

static int g_scr_active = 0;

// Start the on-screen text console.
void BootScr_Begin(void)
{
    init_scr();
    g_scr_active = 1;
}

// Stop drawing boot text (called right before SDL grabs the GS).
void BootScr_End(void)
{
    g_scr_active = 0;
}

// Case-insensitive substring test.
static int ci_contains(const char *hay, size_t n, const char *needle)
{
    size_t nl = strlen(needle), i, j;
    if (nl == 0 || nl > n)
        return 0;
    for (i = 0; i + nl <= n; ++i)
    {
        for (j = 0; j < nl; ++j)
        {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (j == nl) return 1;
    }
    return 0;
}

// Only the lines we actually want during boot reach the (slow) GS console;
// the rest of Doom's startup spam is dropped -- which also speeds boot.
static int want_line(const char *p, size_t n)
{
    static const char *keys[] = {
        "audio", "snd", "audsrv", "libsd", "iwad",
        "build", "game:", "error", ">>>", "===",
        "opl", "music", "adlib", "genmidi", "midi", "dude", NULL
    };
    int k;
    for (k = 0; keys[k] != NULL; ++k)
        if (ci_contains(p, n, keys[k]))
            return 1;
    return 0;
}

// Override the newlib EE syscall so stdout/stderr land on the debug screen
// while the boot console is active. scr_printf wants a NUL-terminated string,
// so copy the write payload out in bounded chunks.
int _write(int fd, const void *buf, size_t nbyte)
{
    if ((fd == 1 || fd == 2) && g_scr_active && want_line((const char *) buf, nbyte))
    {
        const char *p = (const char *) buf;
        char tmp[120];
        size_t i = 0;

        while (i < nbyte)
        {
            size_t n = nbyte - i;
            if (n > sizeof(tmp) - 1)
                n = sizeof(tmp) - 1;
            memcpy(tmp, p + i, n);
            tmp[n] = '\0';
            scr_printf("%s", tmp);
            i += n;
        }
    }

    // We have no filesystem; other fds are no-ops.
    return (int) nbyte;
}
