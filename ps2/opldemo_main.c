//
// Standalone PS2 OPL music demo -- no Doom, no SDL video.
//
// Purpose: isolate the OPL FM music path (Nuked OPL3 synthesis + Doom's
// i_oplmusic MIDI->OPL player) from the rest of Doom, to find out whether the
// synthesis alone runs in real time on the EE. It loads an embedded MIDI,
// plays it through the OPL synth, and streams the result to the SPU2 via
// audsrv from a single thread -- printing a synthesis-load meter so we can see
// how much EE time the FM synth actually costs.
//
// libSDL2main provides the PS2 entry point (IOP reset + filesystem init) and
// calls this main(); we only use SDL for its millisecond timer. The on-screen
// libdebug text console (ps2_bootscr.c) shows the log -- we never hand the GS
// to SDL, so it stays up for the whole run.
//

#include <SDL.h>                // libSDL2main entry; SDL_GetTicks
#include <stdio.h>
#include <audsrv.h>
#include <ps2_audio_driver.h>   // init_audio_driver (ps2_audio_driver.c)

#include "i_sound.h"            // music_module_t

extern void BootScr_Begin(void);     // ps2_bootscr.c
extern void PS2Audio_Init(void);     // ps2_audio.c (sbv patches + iop heap)

extern music_module_t music_opl_module;          // i_oplmusic.c
extern void OPL_PS2_Render(short *stereo, int n); // opl_ps2.c
extern int  snd_samplerate;                       // opldemo_stubs.c

extern unsigned char demo_song[];                 // demo_song.c (embedded MIDI)
extern unsigned int  demo_song_len;

#define RATE        22050      // synth + output rate (same as the game)
#define FRAMES      1024       // frames per audsrv chunk (~46 ms)
#define MAX_VOLUME  0x3fff

int main(int argc, char **argv)
{
    static short buf[FRAMES * 2];      // interleaved stereo L,R
    struct audsrv_fmt_t fmt;
    void *song;
    unsigned long chunks = 0;
    Uint32 win_synth_ms = 0, win_chunks = 0, t_last;
    int r;

    (void) argc; (void) argv;

    BootScr_Begin();
    setvbuf(stdout, NULL, _IONBF, 0);
    SDL_InitSubSystem(SDL_INIT_TIMER);

    printf("\n=== PS2 OPL music demo (Nuked OPL3, no Doom) ===\n");
    printf(">>> build %s %s <<<\n", __DATE__, __TIME__);

    // SPU2 bring-up: sbv patches + iop heap (ps2_audio.c), then load
    // LIBSD + AUDSRV and audsrv_init() (ps2_audio_driver.c).
    PS2Audio_Init();
    if (init_audio_driver() != AUDIO_INIT_STATUS_OK)
    {
        printf("audio: init_audio_driver FAILED\n");
        for (;;) {}
    }
    fmt.bits = 16; fmt.freq = RATE; fmt.channels = 2;
    r = audsrv_set_format(&fmt);
    printf("audio: set_format=%d  (%d Hz 16-bit stereo)\n", r, RATE);
    audsrv_set_volume(MAX_VOLUME);

    // Bring up the OPL music player (detects the software chip, loads the
    // embedded GENMIDI instrument bank via the W_CacheLumpName stub).
    snd_samplerate = RATE;
    if (!music_opl_module.Init())
    {
        printf("music: OPL Init FAILED (no chip detected)\n");
        for (;;) {}
    }
    printf("music: OPL init OK (chip detected)\n");

    song = music_opl_module.RegisterSong(demo_song, (int) demo_song_len);
    printf("music: RegisterSong(%u bytes) = %p\n", demo_song_len, song);
    if (song == NULL)
    {
        printf("music: failed to load MIDI\n");
        for (;;) {}
    }
    music_opl_module.SetMusicVolume(127);
    music_opl_module.PlaySong(song, 1);     // loop forever
    printf("music: playing... streaming to SPU2\n");

    // Render + feed loop. audsrv_wait_audio() blocks until the SPU2 ring has
    // room, pacing us to real time. We time how long the synth takes per chunk
    // vs how much audio that chunk represents -> the FM synthesis load.
    t_last = SDL_GetTicks();
    for (;;)
    {
        Uint32 a = SDL_GetTicks();
        OPL_PS2_Render(buf, FRAMES);
        win_synth_ms += SDL_GetTicks() - a;

        audsrv_wait_audio(sizeof(buf));
        audsrv_play_audio((char *) buf, sizeof(buf));

        ++chunks;
        ++win_chunks;

        if (win_chunks >= 43)            // ~2 s of audio
        {
            Uint32 now      = SDL_GetTicks();
            Uint32 wall     = now - t_last;
            Uint32 audio_ms = win_chunks * FRAMES * 1000 / RATE;
            int    load     = (int) (win_synth_ms * 100 / (audio_ms ? audio_ms : 1));

            printf("music: t=%lus  synth %lums per %lums audio = %d%% load%s\n",
                   chunks * FRAMES / RATE,
                   (unsigned long) win_synth_ms, (unsigned long) audio_ms, load,
                   wall > audio_ms + audio_ms / 8 ? "   <-- CAN'T KEEP UP" : "");

            win_synth_ms = 0;
            win_chunks   = 0;
            t_last       = now;
        }
    }
    return 0;
}
