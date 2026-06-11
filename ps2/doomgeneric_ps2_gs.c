// doomgeneric PS2 backend — native gsKit video (PSMT8 + CLUT).
//
// Alternative to doomgeneric_ps2.c (the SDL2 video backend). Built when the
// Makefile is invoked with GSKIT_VIDEO=1 (which also defines CMAP256, so
// doomgeneric keeps Doom's framebuffer 8-bit indexed instead of expanding it
// to 32-bit RGB on the EE).
//
// Doom's 320x200 8-bit framebuffer is uploaded as a GS PSMT8 texture and the
// 256-colour palette as a CT32 CLUT; the GS does the palette expansion AND the
// scale-to-640x448 in hardware (bilinear), so the EE no longer expands pixels
// or pushes 256 KB/frame -- only 64 KB of indices. Audio (audsrv) and the
// entry point / timer / input stay on SDL exactly as before; gsKit owns only
// the GS.

#include "doomkeys.h"
#include "m_argv.h"
#include "doomgeneric.h"

#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <stdbool.h>

#include <SDL.h>            // libSDL2main entry + SDL_GetTicks/SDL_Delay (+ input later)
#include <gsKit.h>
#include <dmaKit.h>

// Doom's palette, exposed by i_video.c under CMAP256 (b/g/r/a bitfields).
struct color { uint32_t b:8, g:8, r:8, a:8; };
extern struct color colors[256];

// ---- input (unchanged from the SDL backend; SDL keeps input) -----------

#define KEYQUEUE_SIZE 16
static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int s_KeyQueueWriteIndex = 0;
static unsigned int s_KeyQueueReadIndex = 0;

static unsigned char convertToDoomKey(unsigned int key)
{
  switch (key)
    {
    case SDLK_RETURN:   key = KEY_ENTER;      break;
    case SDLK_ESCAPE:   key = KEY_ESCAPE;     break;
    case SDLK_LEFT:     key = KEY_LEFTARROW;  break;
    case SDLK_RIGHT:    key = KEY_RIGHTARROW; break;
    case SDLK_UP:       key = KEY_UPARROW;    break;
    case SDLK_DOWN:     key = KEY_DOWNARROW;  break;
    case SDLK_LCTRL:
    case SDLK_RCTRL:    key = KEY_FIRE;       break;
    case SDLK_SPACE:    key = KEY_USE;        break;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:   key = KEY_RSHIFT;     break;
    case SDLK_LALT:
    case SDLK_RALT:     key = KEY_LALT;       break;
    case SDLK_F2:  key = KEY_F2;  break;
    case SDLK_F3:  key = KEY_F3;  break;
    case SDLK_F4:  key = KEY_F4;  break;
    case SDLK_F5:  key = KEY_F5;  break;
    case SDLK_F6:  key = KEY_F6;  break;
    case SDLK_F7:  key = KEY_F7;  break;
    case SDLK_F8:  key = KEY_F8;  break;
    case SDLK_F9:  key = KEY_F9;  break;
    case SDLK_F10: key = KEY_F10; break;
    case SDLK_F11: key = KEY_F11; break;
    case SDLK_EQUALS:
    case SDLK_PLUS:  key = KEY_EQUALS; break;
    case SDLK_MINUS: key = KEY_MINUS;  break;
    default:         key = tolower(key); break;
    }
  return key;
}

static void addKeyToQueue(int pressed, unsigned int keyCode)
{
  unsigned char key = convertToDoomKey(keyCode);
  unsigned short keyData = (pressed << 8) | key;
  s_KeyQueue[s_KeyQueueWriteIndex] = keyData;
  s_KeyQueueWriteIndex++;
  s_KeyQueueWriteIndex %= KEYQUEUE_SIZE;
}

static void handleKeyInput(void)
{
  SDL_Event e;
  while (SDL_PollEvent(&e)){
    if (e.type == SDL_QUIT){ atexit(SDL_Quit); exit(1); }
    if (e.type == SDL_KEYDOWN)      addKeyToQueue(1, e.key.keysym.sym);
    else if (e.type == SDL_KEYUP)   addKeyToQueue(0, e.key.keysym.sym);
  }
}

// Controller input (ps2_pad.c, libpad). Buttons are already mapped to Doom
// keys, so push them straight into the queue (no SDL-keysym conversion).
extern void PS2Pad_Poll(void (*emit)(int pressed, unsigned char doomkey));
static void pushDoomKey(int pressed, unsigned char doomKey)
{
  s_KeyQueue[s_KeyQueueWriteIndex] = (unsigned short)((pressed << 8) | doomKey);
  s_KeyQueueWriteIndex = (s_KeyQueueWriteIndex + 1) % KEYQUEUE_SIZE;
}

// ---- boot console + audio (ps2_bootscr.c / ps2_audio.c) ----------------

extern void BootScr_Begin(void);
extern void BootScr_End(void);
extern void PS2Audio_Init(void);

// PS2 NTSC display size; the 320x200 texture is GS-scaled up to fill it.
#ifndef DG_DISPLAY_W
#define DG_DISPLAY_W 640
#endif
#ifndef DG_DISPLAY_H
#define DG_DISPLAY_H 448
#endif
#ifndef BOOT_LOG_HOLD_MS
#define BOOT_LOG_HOLD_MS 10000
#endif

// ---- gsKit video -------------------------------------------------------

static GSGLOBAL  *gsGlobal = NULL;
static GSTEXTURE  tex;                 // 320x200 PSMT8 + CT32 CLUT
static int        gs_ready = 0;

// Bring up gsKit lazily on the first frame, so the libdebug boot log stays on
// the GS through Doom's startup, then we take the GS over (mirrors EnsureSdl).
static void EnsureGs(void)
{
  if (gs_ready)
    return;

  SDL_InitSubSystem(SDL_INIT_TIMER);

  printf("\n[ boot log shown for %d s, then starting DOOM ... ]\n",
         BOOT_LOG_HOLD_MS / 1000);
  printf(">>> build %s %s  (gsKit PSMT8 video, %dx%d) <<<\n",
         __DATE__, __TIME__, DOOMGENERIC_RESX, DOOMGENERIC_RESY);
  {
    extern int g_snd_running, g_snd_fmt_ret, g_snd_tid;
    extern volatile int g_mixer_chunks;
    printf("AUDIO: run=%d fmt=%d tid=%d chunks=%d\n",
           g_snd_running, g_snd_fmt_ret, g_snd_tid, g_mixer_chunks);
  }
  SDL_Delay(BOOT_LOG_HOLD_MS);

  // Don't let Doom fast-forward the held seconds (see i_timer.c).
  { extern void I_ResetBaseTime(void); I_ResetBaseTime(); }

  // Hand the GS over from the libdebug text console to gsKit.
  BootScr_End();

  gsGlobal = gsKit_init_global();
#ifdef GS_OUTPUT_480P
  // 480p progressive (DTV, 31 kHz): sharp + no interlace flicker. Needs a
  // component/YPbPr cable on real hardware (works directly in PCSX2). 640x480
  // CT24 double-buffered is ~2.5 MB, leaving plenty of the 4 MB VRAM for the tex.
  gsGlobal->Mode      = GS_MODE_DTV_480P;
  gsGlobal->Interlace = GS_NONINTERLACED;
  gsGlobal->Field     = GS_FRAME;
  gsGlobal->Width     = 640;
  gsGlobal->Height    = 480;
#else
  // NTSC 640x448 interlaced: works on any TV including composite.
  gsGlobal->Mode      = GS_MODE_NTSC;
  gsGlobal->Interlace = GS_INTERLACED;
  gsGlobal->Field     = GS_FIELD;
  gsGlobal->Width     = DG_DISPLAY_W;
  gsGlobal->Height    = DG_DISPLAY_H;
#endif
  gsGlobal->PSM           = GS_PSM_CT24;
  gsGlobal->PSMZ          = GS_PSMZ_16S;
  gsGlobal->ZBuffering    = GS_SETTING_OFF;
  gsGlobal->DoubleBuffering = GS_SETTING_ON;

  dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
              D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
  dmaKit_chan_init(DMA_CHANNEL_GIF);

  gsKit_init_screen(gsGlobal);
  gsKit_mode_switch(gsGlobal, GS_ONESHOT);   // queue + flip every frame
  gsKit_TexManager_init(gsGlobal);

  // 320x200 8-bit indexed texture + 256-entry CT32 CLUT. Mem is a 128-aligned
  // staging buffer we copy the (malloc'd, unaligned) framebuffer into each
  // frame; the GS DMA wants aligned source data.
  tex.Width    = DOOMGENERIC_RESX;
  tex.Height   = DOOMGENERIC_RESY;
  tex.PSM      = GS_PSM_T8;
  tex.ClutPSM  = GS_PSM_CT32;
  tex.Filter   = GS_FILTER_LINEAR;           // bilinear smoothing on upscale
  tex.Delayed  = 0;
  tex.Vram     = 0;
  tex.VramClut = 0;
  tex.Mem  = memalign(128, gsKit_texture_size_ee(tex.Width, tex.Height, tex.PSM));
  tex.Clut = memalign(128, gsKit_texture_size_ee(256, 1, tex.ClutPSM));

  gs_ready = 1;

  // GS now shows the game: open the audio gate so music/sfx start in sync.
  { extern volatile int g_audio_gate; g_audio_gate = 1; }
}

void DG_Init(void)
{
  // gsKit is brought up lazily in EnsureGs() on the first frame, so the
  // on-screen boot log stays visible during startup.
}

void DG_DrawFrame(void)
{
  unsigned int *clut;
  int i;

  EnsureGs();

  // Rebuild the CLUT from Doom's palette every frame (cheap, 256 entries) so
  // palette effects -- damage red, item pickup, radsuit -- show. GS RGBA32 is
  // R|G<<8|B<<16|A<<24; force A=0x80 (= 1.0) for opaque.
  //
  // The GS stores a 256-entry CT32 CLUT swizzled (CSM1): palette-index bits 3
  // and 4 are swapped in VRAM. gsKit doesn't fix this for us, so we write each
  // colour to the swizzled slot -- otherwise grays/browns tint green (reds and
  // the indices where bit3==bit4 happen to land right, which masks it).
  clut = (unsigned int *) tex.Clut;
  for (i = 0; i < 256; ++i)
  {
    int j = (i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
    clut[j] = (unsigned int) colors[i].r
            | ((unsigned int) colors[i].g << 8)
            | ((unsigned int) colors[i].b << 16)
            | (0x80u << 24);
  }

  // Copy the 8-bit indices into the aligned upload buffer.
  memcpy(tex.Mem, DG_ScreenBuffer,
         (size_t) DOOMGENERIC_RESX * DOOMGENERIC_RESY);

  gsKit_clear(gsGlobal, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x00, 0x00));

  gsKit_TexManager_invalidate(gsGlobal, &tex);   // force re-upload (changed)
  gsKit_TexManager_bind(gsGlobal, &tex);

  gsKit_prim_sprite_texture(gsGlobal, &tex,
                            0.0f, 0.0f,                          // screen x1,y1
                            0.0f, 0.0f,                          // tex   u1,v1
                            (float) gsGlobal->Width, (float) gsGlobal->Height, // x2,y2 (fills the active mode)
                            (float) DOOMGENERIC_RESX, (float) DOOMGENERIC_RESY, // u2,v2
                            0,                                   // z
                            GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00)); // 1.0 modulate

  gsKit_queue_exec(gsGlobal);
  gsKit_sync_flip(gsGlobal);
  gsKit_TexManager_nextFrame(gsGlobal);

  handleKeyInput();
  PS2Pad_Poll(pushDoomKey);
}

void DG_SleepMs(uint32_t ms)        { SDL_Delay(ms); }
uint32_t DG_GetTicksMs(void)        { return SDL_GetTicks(); }
void DG_SetWindowTitle(const char *title) { (void) title; }

int DG_GetKey(int *pressed, unsigned char *doomKey)
{
  if (s_KeyQueueReadIndex == s_KeyQueueWriteIndex)
    return 0;

  unsigned short keyData = s_KeyQueue[s_KeyQueueReadIndex];
  s_KeyQueueReadIndex++;
  s_KeyQueueReadIndex %= KEYQUEUE_SIZE;
  *pressed = keyData >> 8;
  *doomKey = keyData & 0xFF;
  return 1;
}

int main(int argc, char **argv)
{
  // On-screen GS text console first; unbuffer stdout so every boot message is
  // drawn immediately (PS2 stdout is fully buffered and the game never returns).
  BootScr_Begin();
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);

  printf("\n");
  printf("===========================================================\n");
  printf(" doomgeneric for PlayStation 2  (native gsKit video)\n");
  printf("===========================================================\n");

  PS2Audio_Init();

  doomgeneric_Create(argc, argv);

  for (;;)
    doomgeneric_Tick();

  return 0;
}
