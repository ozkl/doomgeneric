// PS2 IWAD selection.
//
// Prefers an IWAD on the host filesystem (hostfs) next to the ELF, showing a
// controller menu if several are present; otherwise falls back to the
// shareware IWAD embedded in the executable (EMBED_WAD builds only).
//
// Called from the one PS2 hook in the upstream d_main.c (guarded by __PS2__).

#include <stdio.h>

// Controller menu (ps2_menu.c).
extern int PS2_SelectMenu(const char *title, char **items, int count);

char *PS2_GetIWAD(void)
{
#ifdef EMBED_WAD
    static char  embedded_iwad[] = "doom1.wad";
#endif
    static char *host_iwads[] = {
        "host:DOOM.WAD", "host:DOOM2.WAD", "host:DOOM1.WAD",
        "host:PLUTONIA.WAD", "host:TNT.WAD",
        "host:FREEDOOM1.WAD", "host:FREEDOOM2.WAD", NULL
    };
    char *found[16];
    int   nfound = 0;
    int   i;
    FILE *f;

    // Probe hostfs, reporting each attempt so a missing/mismapped host: is
    // obvious on the boot screen.
    printf("IWAD: scanning hostfs...\n");
    for (i = 0; host_iwads[i] != NULL && nfound < 16; ++i)
    {
        f = fopen(host_iwads[i], "rb");
        printf("  %-22s %s\n", host_iwads[i], f ? "[found]" : "-");
        if (f != NULL)
        {
            fclose(f);
            found[nfound++] = host_iwads[i];
        }
    }

    if (nfound == 1)
    {
        printf("IWAD: using %s\n", found[0]);
        return found[0];
    }

    if (nfound > 1)
    {
        int sel = PS2_SelectMenu("Select IWAD", found, nfound);
        printf("IWAD: selected %s\n", found[sel]);
        return found[sel];
    }

    // Nothing on hostfs.
#ifdef EMBED_WAD
    printf("IWAD: none on hostfs; using embedded shareware WAD\n");
    return embedded_iwad;
#else
    printf("\n");
    printf("  *** No IWAD found ***\n");
    printf("  Put a DOOM WAD (e.g. DOOM1.WAD) next to doomgeneric.elf.\n");
    printf("  In PCSX2: enable Host Filesystem; host: maps to the\n");
    printf("  folder that contains the ELF you launched.\n");
    printf("  (halted - nothing to run without a WAD)\n");
    for (;;) { }   // halt visibly instead of exiting to the PS2 BIOS
#endif
}
