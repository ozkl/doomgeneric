// Minimal SDL2 video smoke test for PS2.
// Cycles the whole screen red -> green -> blue every half second.
// If you see colors: SDL2 video works on this setup.
// If black: read the EE Console log (enable it in PCSX2) for where it bailed.

#include <SDL.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("smoke: start\n");

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("smoke: SDL_Init(VIDEO) FAILED: %s\n", SDL_GetError());
        return 1;
    }
    printf("smoke: SDL_Init ok\n");

    int nvd = SDL_GetNumVideoDrivers();
    printf("smoke: %d video drivers\n", nvd);
    for (int i = 0; i < nvd; i++)
        printf("  video[%d]=%s\n", i, SDL_GetVideoDriver(i));

    int nrd = SDL_GetNumRenderDrivers();
    printf("smoke: %d render drivers\n", nrd);
    for (int i = 0; i < nrd; i++) {
        SDL_RendererInfo ri;
        if (SDL_GetRenderDriverInfo(i, &ri) == 0)
            printf("  render[%d]=%s\n", i, ri.name);
    }

    SDL_Window *w = SDL_CreateWindow("smoke",
                                     SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                     640, 448, SDL_WINDOW_SHOWN);
    printf("smoke: window=%p (%s)\n", (void*)w, w ? "ok" : SDL_GetError());

    SDL_Renderer *r = SDL_CreateRenderer(w, -1, SDL_RENDERER_ACCELERATED);
    if (!r) {
        printf("smoke: ACCELERATED renderer failed: %s\n", SDL_GetError());
        r = SDL_CreateRenderer(w, -1, 0);
    }
    printf("smoke: renderer=%p (%s)\n", (void*)r, r ? "ok" : SDL_GetError());
    if (!r) { printf("smoke: no renderer, giving up\n"); for(;;); }

    printf("smoke: entering color loop\n");
    int c = 0;
    for (;;) {
        Uint8 R = (c == 0) ? 255 : 0;
        Uint8 G = (c == 1) ? 255 : 0;
        Uint8 B = (c == 2) ? 255 : 0;
        SDL_SetRenderDrawColor(r, R, G, B, 255);
        SDL_RenderClear(r);
        SDL_RenderPresent(r);
        c = (c + 1) % 3;
        SDL_Delay(500);
    }
    return 0;
}
