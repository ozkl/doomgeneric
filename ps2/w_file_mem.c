//
// In-memory WAD I/O backend.
//
// Serves the IWAD straight out of a byte array linked into the
// executable (see doom1_wad.c, generated from DOOM1.WAD by bin2c).
// Because wad_file_t.mapped is set, W_CacheLumpNum reads lumps as
// direct pointers into the array -- zero copy, and no filesystem.
//

#include <stdio.h>
#include <string.h>

#include "w_file.h"
#include "z_zone.h"

// Only built when the IWAD is embedded (make EMBED_WAD=1).
#ifdef EMBED_WAD

// The embedded shareware IWAD (from doom1_wad.c).
extern unsigned char doom1_wad[];
extern unsigned int  size_doom1_wad;

extern wad_file_class_t mem_wad_file;

static wad_file_t *W_Mem_OpenFile(char *path)
{
    wad_file_t *result;

    // We only know how to serve the one embedded IWAD.
    (void) path;

    printf("W_OpenFile: '%s' served from memory: %u bytes, "
           "mapped @ %p (zero-copy)\n",
           path, size_doom1_wad, (void *) doom1_wad);

    result = Z_Malloc(sizeof(wad_file_t), PU_STATIC, 0);
    result->file_class = &mem_wad_file;
    result->mapped = doom1_wad;          // enables zero-copy lump access
    result->length = size_doom1_wad;

    return result;
}

static void W_Mem_CloseFile(wad_file_t *wad)
{
    Z_Free(wad);
}

static size_t W_Mem_Read(wad_file_t *wad, unsigned int offset,
                         void *buffer, size_t buffer_len)
{
    (void) wad;

    if (offset >= size_doom1_wad)
    {
        return 0;
    }

    if (offset + buffer_len > size_doom1_wad)
    {
        buffer_len = size_doom1_wad - offset;
    }

    memcpy(buffer, doom1_wad + offset, buffer_len);

    return buffer_len;
}

wad_file_class_t mem_wad_file =
{
    W_Mem_OpenFile,
    W_Mem_CloseFile,
    W_Mem_Read,
};

#endif /* EMBED_WAD */
