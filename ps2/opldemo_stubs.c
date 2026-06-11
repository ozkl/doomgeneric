//
// Minimal Doom-subsystem stubs for the standalone OPL music demo.
//
// The demo reuses Doom's OPL music player (i_oplmusic.c), MIDI parser
// (midifile.c), MUS converter (mus2mid.c) and memio, but NOT the rest of Doom
// (WAD, zone allocator, m_misc, dehacked). These few externals are all those
// units reference; the file-I/O ones are linked but never hit on the MIDI path
// (MIDI_LoadStream / fmemopen are used instead).
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// The GENMIDI instrument bank, extracted from shareware DOOM1.WAD and embedded
// (demo_genmidi.c). i_oplmusic asks for it by lump name "genmidi".
extern unsigned char demo_genmidi[];
extern unsigned int  demo_genmidi_len;

// Output/synthesis sample rate. i_oplmusic reads this (normally from i_sound.c).
int snd_samplerate = 22050;

// ---- WAD: serve only GENMIDI from the embedded bank --------------------

void *W_CacheLumpName(const char *name, int tag)
{
    (void) tag;
    if (name != NULL && strcasecmp(name, "genmidi") == 0)
        return demo_genmidi;
    return NULL;
}

void W_ReleaseLumpName(const char *name)
{
    (void) name;
}

// ---- dehacked: identity ------------------------------------------------

char *DEH_String(const char *s)
{
    return (char *) s;
}

// ---- zone allocator -> plain malloc ------------------------------------

void  Z_Init(void) { }
void *Z_Malloc(int size, int tag, void *user) { (void) tag; (void) user; return malloc((size_t) size); }
void  Z_Free(void *ptr) { free(ptr); }

// ---- m_misc / file helpers (linked, unused on the MIDI path) -----------

void *I_Realloc(void *ptr, size_t size)
{
    void *p = realloc(ptr, size);
    if (p == NULL && size != 0)
    {
        printf("I_Realloc: failed on %u bytes\n", (unsigned) size);
        abort();
    }
    return p;
}

int M_snprintf(char *buf, size_t buf_len, const char *s, ...)
{
    va_list args;
    int result;
    if (buf_len < 1)
        return 0;
    va_start(args, s);
    result = vsnprintf(buf, buf_len, s, args);
    va_end(args);
    if (result < 0 || (size_t) result >= buf_len)
        result = (int) buf_len - 1;
    return result;
}

int M_StringConcat(char *dest, const char *src, size_t dest_size)
{
    size_t offset = strlen(dest);
    if (offset > dest_size)
        offset = dest_size;
    M_snprintf(dest + offset, dest_size - offset, "%s", src);
    return (strlen(dest) - offset) == strlen(src);
}

FILE *M_fopen(const char *filename, const char *mode) { return fopen(filename, mode); }
int   M_WriteFile(const char *name, const void *src, size_t len) { (void) name; (void) src; (void) len; return 0; }
int   M_ReadFile(const char *name, unsigned char **buffer) { (void) name; (void) buffer; return -1; }
