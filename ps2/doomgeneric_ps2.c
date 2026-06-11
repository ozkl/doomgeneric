//doomgeneric for cross-platform development library 'Simple DirectMedia Layer'

#include "doomkeys.h"
#include "m_argv.h"
#include "doomgeneric.h"

#include <stdio.h>
#include <unistd.h>

#include <stdbool.h>
#include <SDL.h>

SDL_Window* window = NULL;
SDL_Renderer* renderer = NULL;
SDL_Texture* texture;

#define KEYQUEUE_SIZE 16

static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int s_KeyQueueWriteIndex = 0;
static unsigned int s_KeyQueueReadIndex = 0;

static unsigned char convertToDoomKey(unsigned int key){
  switch (key)
    {
    case SDLK_RETURN:
      key = KEY_ENTER;
      break;
    case SDLK_ESCAPE:
      key = KEY_ESCAPE;
      break;
    case SDLK_LEFT:
      key = KEY_LEFTARROW;
      break;
    case SDLK_RIGHT:
      key = KEY_RIGHTARROW;
      break;
    case SDLK_UP:
      key = KEY_UPARROW;
      break;
    case SDLK_DOWN:
      key = KEY_DOWNARROW;
      break;
    case SDLK_LCTRL:
    case SDLK_RCTRL:
      key = KEY_FIRE;
      break;
    case SDLK_SPACE:
      key = KEY_USE;
      break;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
      key = KEY_RSHIFT;
      break;
    case SDLK_LALT:
    case SDLK_RALT:
      key = KEY_LALT;
      break;
    case SDLK_F2:
      key = KEY_F2;
      break;
    case SDLK_F3:
      key = KEY_F3;
      break;
    case SDLK_F4:
      key = KEY_F4;
      break;
    case SDLK_F5:
      key = KEY_F5;
      break;
    case SDLK_F6:
      key = KEY_F6;
      break;
    case SDLK_F7:
      key = KEY_F7;
      break;
    case SDLK_F8:
      key = KEY_F8;
      break;
    case SDLK_F9:
      key = KEY_F9;
      break;
    case SDLK_F10:
      key = KEY_F10;
      break;
    case SDLK_F11:
      key = KEY_F11;
      break;
    case SDLK_EQUALS:
    case SDLK_PLUS:
      key = KEY_EQUALS;
      break;
    case SDLK_MINUS:
      key = KEY_MINUS;
      break;
    default:
      key = tolower(key);
      break;
    }

  return key;
}

static void addKeyToQueue(int pressed, unsigned int keyCode){
  unsigned char key = convertToDoomKey(keyCode);

  unsigned short keyData = (pressed << 8) | key;

  s_KeyQueue[s_KeyQueueWriteIndex] = keyData;
  s_KeyQueueWriteIndex++;
  s_KeyQueueWriteIndex %= KEYQUEUE_SIZE;
}
static void handleKeyInput(){
  SDL_Event e;
  while (SDL_PollEvent(&e)){
    if (e.type == SDL_QUIT){
      puts("Quit requested");
      atexit(SDL_Quit);
      exit(1);
    }
    if (e.type == SDL_KEYDOWN) {
      //KeySym sym = XKeycodeToKeysym(s_Display, e.xkey.keycode, 0);
      //printf("KeyPress:%d sym:%d\n", e.xkey.keycode, sym);
      addKeyToQueue(1, e.key.keysym.sym);
    } else if (e.type == SDL_KEYUP) {
      //KeySym sym = XKeycodeToKeysym(s_Display, e.xkey.keycode, 0);
      //printf("KeyRelease:%d sym:%d\n", e.xkey.keycode, sym);
      addKeyToQueue(0, e.key.keysym.sym);
    }
  }
}

// Controller input (ps2_pad.c, libpad). Buttons are already Doom keys, so push
// them straight into the queue (no SDL-keysym conversion).
extern void PS2Pad_Poll(void (*emit)(int pressed, unsigned char doomkey));
static void pushDoomKey(int pressed, unsigned char doomKey)
{
  s_KeyQueue[s_KeyQueueWriteIndex] = (unsigned short)((pressed << 8) | doomKey);
  s_KeyQueueWriteIndex = (s_KeyQueueWriteIndex + 1) % KEYQUEUE_SIZE;
}


// Boot text console (ps2_bootscr.c): on-screen boot log before SDL.
extern void BootScr_Begin(void);
extern void BootScr_End(void);

// PS2 audio bring-up (ps2_audio.c): loads audsrv.irx + audsrv_init().
extern void PS2Audio_Init(void);

static int sdl_ready = 0;

// Bring up SDL video the first time we actually need to draw a frame. This
// keeps the libdebug boot-text screen visible for the whole Doom startup
// (W_Init, R_Init, etc.) and only switches to the game's framebuffer here.
// How long to hold the boot log on screen before switching to the game.
#ifndef BOOT_LOG_HOLD_MS
#define BOOT_LOG_HOLD_MS 10000
#endif

// PS2 NTSC display size. The window is created at this size and the small
// 320x200 texture is scaled up to fill it (the GS does the scaling for free),
// so the cheap low-res render still covers the whole screen.
#ifndef DG_DISPLAY_W
#define DG_DISPLAY_W 640
#endif
#ifndef DG_DISPLAY_H
#define DG_DISPLAY_H 448
#endif

static void EnsureSdl(void)
{
  if (sdl_ready)
    return;

  // Keep the boot text on screen long enough to read. The timer subsystem
  // doesn't touch the GS, so the libdebug console stays up during the wait.
  printf("\n[ boot log shown for %d s, then starting DOOM ... ]\n",
         BOOT_LOG_HOLD_MS / 1000);
  // Build marker (last line) so the running build is verifiable on screen.
  printf(">>> build %s %s  (audio+OPL music, %dx%d) <<<\n",
         __DATE__, __TIME__, DOOMGENERIC_RESX, DOOMGENERIC_RESY);
  // Audio diagnostics: chunks>0 means the mixer thread is feeding audsrv.
  {
    extern int g_snd_running, g_snd_fmt_ret, g_snd_tid;
    extern volatile int g_mixer_chunks;
    printf("AUDIO: run=%d fmt=%d tid=%d chunks=%d\n",
           g_snd_running, g_snd_fmt_ret, g_snd_tid, g_mixer_chunks);
  }
  SDL_InitSubSystem(SDL_INIT_TIMER);
  SDL_Delay(BOOT_LOG_HOLD_MS);

  // We just blocked real time for BOOT_LOG_HOLD_MS while showing the log. Re-
  // base Doom's clock so it doesn't treat those seconds as elapsed game time
  // and fast-forward a burst of tics (which desyncs the demo and thrashes the
  // music as the GS switches to the game).
  {
    extern void I_ResetBaseTime(void);
    I_ResetBaseTime();
  }

  // Hand the GS over from the boot text console to SDL.
  BootScr_End();

  // The stock backend never initialised SDL; required before any
  // window/renderer call (and on PS2 brings up the GS video driver).
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
  {
    printf("SDL_Init(VIDEO) failed: %s\n", SDL_GetError());
    return;
  }

  window = SDL_CreateWindow("DOOM",
                            SDL_WINDOWPOS_UNDEFINED,
                            SDL_WINDOWPOS_UNDEFINED,
                            DG_DISPLAY_W,
                            DG_DISPLAY_H,
                            SDL_WINDOW_SHOWN
                            );

  // Setup renderer
  renderer =  SDL_CreateRenderer( window, -1, SDL_RENDERER_ACCELERATED);
  // Clear winow
  SDL_RenderClear( renderer );
  // Render the rect to the screen
  SDL_RenderPresent(renderer);

  // STREAMING: we hand it a fresh CPU-side framebuffer every frame via
  // SDL_UpdateTexture (TARGET would be for render-to-texture).
  texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, DOOMGENERIC_RESX, DOOMGENERIC_RESY);

  sdl_ready = 1;

  // GS now shows the game. Open the audio gate so the mixer (which has been
  // feeding silence) starts music + sfx in sync with the first rendered frame.
  {
    extern volatile int g_audio_gate;
    g_audio_gate = 1;
  }
}

void DG_Init(){
  // SDL is brought up lazily in EnsureSdl() on the first frame so the
  // on-screen boot log (started in main) stays visible during startup.
}

void DG_DrawFrame()
{
  EnsureSdl();

  SDL_UpdateTexture(texture, NULL, DG_ScreenBuffer, DOOMGENERIC_RESX*sizeof(uint32_t));

  SDL_RenderClear(renderer);
  SDL_RenderCopy(renderer, texture, NULL, NULL);
  SDL_RenderPresent(renderer);

  handleKeyInput();
  PS2Pad_Poll(pushDoomKey);
}

void DG_SleepMs(uint32_t ms)
{
  SDL_Delay(ms);
}

uint32_t DG_GetTicksMs()
{
  return SDL_GetTicks();
}

int DG_GetKey(int* pressed, unsigned char* doomKey)
{
  if (s_KeyQueueReadIndex == s_KeyQueueWriteIndex){
    //key queue is empty
    return 0;
  }else{
    unsigned short keyData = s_KeyQueue[s_KeyQueueReadIndex];
    s_KeyQueueReadIndex++;
    s_KeyQueueReadIndex %= KEYQUEUE_SIZE;

    *pressed = keyData >> 8;
    *doomKey = keyData & 0xFF;

    return 1;
  }

  return 0;
}

void DG_SetWindowTitle(const char * title)
{
  if (window != NULL){
    SDL_SetWindowTitle(window, title);
  }
}

int main(int argc, char **argv)
{
    // Bring up the on-screen GS text console first, and unbuffer stdout so
    // every boot message is drawn immediately (PS2 stdout is fully buffered
    // and the game never returns).
    BootScr_Begin();
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("\n");
    printf("===========================================================\n");
    printf(" doomgeneric for PlayStation 2  (SDL2 backend)\n");
    printf(" IWAD embedded in executable - no filesystem\n");
    printf("===========================================================\n");

    // Bring up the SPU2 (load audsrv.irx + audsrv_init) before Doom's
    // S_Init opens the SDL audio device.
    PS2Audio_Init();

    doomgeneric_Create(argc, argv);

    for (int i = 0; ; i++)
    {
        doomgeneric_Tick();
    }


    return 0;
}