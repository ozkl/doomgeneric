#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

#include "doomgeneric.h"

#define WIDTH  DOOMGENERIC_RESX
#define HEIGHT DOOMGENERIC_RESY

extern pixel_t* DG_ScreenBuffer;

// ============================
// Helper functions
// ============================
void save_bmp(const char* filename)
{
    FILE* f = fopen(filename, "wb");
    if (!f)
    {
        printf("Failed to open file\n");
        return;
    }

    int row_padded = (WIDTH * 3 + 3) & (~3);
    int filesize = 54 + row_padded * HEIGHT;

    // =========================
    // FILE HEADER (14 bytes)
    // =========================
    unsigned char bmpfileheader[14] = {
        'B','M',

        filesize & 0xFF,
        (filesize >> 8) & 0xFF,
        (filesize >> 16) & 0xFF,
        (filesize >> 24) & 0xFF,

        0,0,0,0,
        54,0,0,0
    };

    // =========================
    // INFO HEADER (40 bytes)
    // =========================
    unsigned char bmpinfoheader[40] = {
        40,0,0,0,

        WIDTH & 0xFF,
        (WIDTH >> 8) & 0xFF,
        (WIDTH >> 16) & 0xFF,
        (WIDTH >> 24) & 0xFF,

        HEIGHT & 0xFF,
        (HEIGHT >> 8) & 0xFF,
        (HEIGHT >> 16) & 0xFF,
        (HEIGHT >> 24) & 0xFF,

        1,0,        // planes
        24,0,       // bpp

        0,0,0,0,    // compression
        0,0,0,0,    // image size (можно 0 для BI_RGB)

        0,0,0,0,    // x pixels per meter
        0,0,0,0,    // y pixels per meter

        0,0,0,0,    // colors used
        0,0,0,0     // important colors
    };

    fwrite(bmpfileheader, 1, 14, f);
    fwrite(bmpinfoheader, 1, 40, f);

    // =========================
    // PIXELS (bottom-up)
    // =========================
    unsigned char padding[3] = {0, 0, 0};

    for (int y = HEIGHT - 1; y >= 0; y--)
    {
        for (int x = 0; x < WIDTH; x++)
        {
            uint32_t p = DG_ScreenBuffer[y * WIDTH + x];

            unsigned char r = (p >> 16) & 0xFF;
            unsigned char g = (p >> 8) & 0xFF;
            unsigned char b = p & 0xFF;

            unsigned char pixel[3] = { b, g, r }; // BGR
            fwrite(pixel, 1, 3, f);
        }

        // padding
        fwrite(padding, 1, row_padded - WIDTH * 3, f);
    }

    fclose(f);

    printf("BMP saved: %s\n", filename);
}

// ============================
// DOOM callbacks
// ============================
void DG_Init(void)
{
    // do nothing
    printf("DG_Init executed...\n");
}

void DG_SetWindowTitle(const char *title)
{
    // ignore in headless mode
}

void DG_DrawFrame(void)
{
    static int frame = 0;
    frame++;

    if (frame == 500)
    {
        save_bmp("/www/doom.bmp");
        printf("frame saved\n");
    }
}

void DG_SleepMs(uint32_t ms)
{
    usleep(ms * 1000);
}

uint32_t DG_GetTicksMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    // no keyboard interaction yet - demo mode
    return 0;
}

// ============================
// entry point
// ============================

extern void doomgeneric_Tick(void);

int main(int argc, char **argv)
{
    printf("Starting Doom (headless)...\n");

    doomgeneric_Create(argc, argv);

    while (1)
    {
        doomgeneric_Tick();
        DG_SleepMs(1);
    }

    return 0;
}
