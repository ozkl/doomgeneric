// Native PS2 sound module for doomgeneric.
//
// Replaces the SDL2 audio backend, which cannot sustain continuous output on
// PS2 (its threaded feed underruns no matter the rate/buffer). Instead we do
// what RetroArch's PS2 audio driver and the old PS2 Doom ports do: mix Doom's
// channels ourselves and push PCM straight to audsrv with a blocking
// audsrv_play_audio() from a dedicated thread. The blocking call self-paces to
// real time and decouples audio from Doom's (currently ~8 fps) render loop.
//
// Defines DG_sound_module (and a no-op DG_music_module, since PS2 has no MIDI).

#include <stdio.h>
#include <string.h>
#include <kernel.h>            // ee threads
#include <audsrv.h>
#include <ps2_audio_driver.h>  // init_audio_driver (our ps2_audio_driver.c)

#include "i_sound.h"
#include "i_system.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"
#include "deh_str.h"
#include "doomtype.h"

#ifndef MAX_VOLUME
#define MAX_VOLUME 0x3fff
#endif

#define OUT_RATE     22050     // output rate (Doom sfx are 11025; this is ample)
#define NUM_CHANNELS 16        // Doom mixing channels
#define MIX_SAMPLES  1024      // samples per audsrv chunk (~46ms @ 22050)

// Mix balance: sfx at 1/2 (>>1) so the boosted music sits on top without the
// gunshots drowning it. 0 = full sfx, higher = quieter.
#define SFX_VERIFY_SHIFT 1

// The OPL (DBOPL) output sits well below full scale, so boost the music when
// mixing it in. The final clamp catches the occasional loud peak.
#define MUSIC_GAIN 2

// A decoded sound effect (8-bit unsigned PCM from the WAD lump).
typedef struct
{
    const byte  *data;
    unsigned int len;
    int          rate;
} cached_sfx_t;

// A live mixing channel.
typedef struct
{
    const byte  *data;     // source samples (8-bit unsigned)
    unsigned int len;      // source length in samples
    unsigned int pos;      // 16.16 fixed-point source position
    unsigned int step;     // 16.16 source-samples per output-sample
    int          vol;      // 0..256 pre-scaled volume
    volatile int active;
} channel_t;

static channel_t channels[NUM_CHANNELS];
static boolean   use_sfx_prefix;
static volatile int sound_running = 0;

// Diagnostics surfaced on the boot screen (see doomgeneric_ps2.c).
volatile int g_mixer_chunks = 0;   // audsrv_play_audio calls so far
int g_snd_running = 0;             // mixer thread started
int g_snd_fmt_ret = -99;           // audsrv_set_format() return
int g_snd_tid     = -99;           // mixer thread id

// Audio gate: the mixer stays silent (and the music clock frozen) until the
// graphics come up, so audio and video start together. Opened in EnsureSdl().
volatile int g_audio_gate = 0;

// Config-bound globals that used to live in i_sdlsound.c (m_config.c still
// references them). Unused here, but must exist for the link.
int   use_libsamplerate  = 0;
float libsamplerate_scale = 0.65f;

static unsigned char mixer_stack[16 * 1024] __attribute__((aligned(16)));
static int mixer_tid = -1;

// ---- sfx loading -------------------------------------------------------

static void GetSfxName(sfxinfo_t *sfx, char *buf, size_t sz)
{
    if (sfx->link != NULL)
        sfx = sfx->link;

    if (use_sfx_prefix)
        M_snprintf(buf, sz, "ds%s", DEH_String(sfx->name));
    else
        M_snprintf(buf, sz, "%s", DEH_String(sfx->name));
}

// Decode (once) a sfx lump in DMX format and cache it on the sfxinfo.
static cached_sfx_t *GetCachedSfx(sfxinfo_t *sfx)
{
    cached_sfx_t *c;
    byte *data;
    int   lumpnum;
    unsigned int lumplen, len;
    int   rate;

    if (sfx->driver_data != NULL)
        return (cached_sfx_t *) sfx->driver_data;

    lumpnum = sfx->lumpnum;
    if (lumpnum < 0)
        return NULL;

    data    = W_CacheLumpNum(lumpnum, PU_STATIC);   // kept resident (zero-copy WAD)
    lumplen = W_LumpLength(lumpnum);

    // DMX header: 0x03 0x00, 16-bit rate, 32-bit length.
    if (lumplen < 8 || data[0] != 0x03 || data[1] != 0x00)
        return NULL;

    rate = (data[3] << 8) | data[2];
    len  = (data[7] << 24) | (data[6] << 16) | (data[5] << 8) | data[4];

    if (len > lumplen - 8 || len <= 48)
        return NULL;

    // DMX pads with 16 leading + 16 trailing bytes; samples follow the
    // 8-byte header.
    c = Z_Malloc(sizeof(cached_sfx_t), PU_STATIC, NULL);
    c->data = data + 8 + 16;
    c->len  = len - 32;
    c->rate = rate;

    sfx->driver_data = c;
    return c;
}

// ---- the mixer thread --------------------------------------------------

extern void OPL_PS2_Render(short *stereo, int nframes);   // opl_ps2.c

static int MixerThread(void *arg)
{
    static short out[MIX_SAMPLES];          // mono 16-bit output
    static short opl_buf[MIX_SAMPLES * 2];  // OPL music, stereo
    int acc[MIX_SAMPLES];
    int i, c;

    (void) arg;

    while (sound_running)
    {
        // Until EnsureSdl() opens the gate (graphics up), feed silence and
        // don't advance the music clock -- so audio starts in sync with video
        // rather than playing during the boot console and restarting.
        if (!g_audio_gate)
        {
            memset(out, 0, sizeof(out));
            audsrv_wait_audio(sizeof(out));
            audsrv_play_audio((char *) out, sizeof(out));
            g_mixer_chunks++;
            continue;
        }

        memset(acc, 0, sizeof(acc));

        for (c = 0; c < NUM_CHANNELS; ++c)
        {
            channel_t *ch = &channels[c];
            unsigned int pos, step;

            if (!ch->active)
                continue;

            pos  = ch->pos;
            step = ch->step;

            for (i = 0; i < MIX_SAMPLES; ++i)
            {
                unsigned int idx = pos >> 16;
                int s;

                if (idx >= ch->len)
                {
                    ch->active = 0;
                    break;
                }

                s = (int) ch->data[idx] - 128;   // 8-bit unsigned -> signed
                acc[i] += (s * ch->vol) >> SFX_VERIFY_SHIFT;   // s*vol, attenuated
                pos += step;
            }

            ch->pos = pos;
        }

        // Mix OPL music (stereo) into the accumulator, downmixed to mono and
        // boosted (MUSIC_GAIN) since the FM synth output is well below full scale.
        OPL_PS2_Render(opl_buf, MIX_SAMPLES);
        for (i = 0; i < MIX_SAMPLES; ++i)
        {
            int m = (opl_buf[2 * i] + opl_buf[2 * i + 1]) >> 1;   // L+R -> mono
            acc[i] += m * MUSIC_GAIN;
        }

        for (i = 0; i < MIX_SAMPLES; ++i)
        {
            int v = acc[i];
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            out[i] = (short) v;
        }

        // audsrv_play_audio() does NOT block -- it drops the chunk if the ring
        // buffer is full. audsrv_wait_audio() blocks until there's room, which
        // paces the mixer to real time and stops samples being dropped.
        audsrv_wait_audio(sizeof(out));
        audsrv_play_audio((char *) out, sizeof(out));
        g_mixer_chunks++;
    }

    ExitDeleteThread();
    return 0;
}

// ---- sound_module_t hooks ---------------------------------------------

static void SetChannelVol(channel_t *ch, int vol, int sep)
{
    (void) sep;   // mono output: ignore stereo separation
    if (vol < 0)   vol = 0;
    if (vol > 127) vol = 127;
    ch->vol = (vol * 256) / 127;   // -> [0,256]
}

static boolean I_PS2_InitSound(boolean _use_sfx_prefix)
{
    struct audsrv_fmt_t fmt;
    ee_thread_t th;
    void *gp;

    use_sfx_prefix = _use_sfx_prefix;
    memset(channels, 0, sizeof(channels));

    // Load LIBSD + AUDSRV and audsrv_init() (our ps2_audio_driver.c).
    if (init_audio_driver() != AUDIO_INIT_STATUS_OK)
    {
        printf("snd: init_audio_driver() failed - no sound\n");
        return false;
    }

    fmt.bits     = 16;
    fmt.freq     = OUT_RATE;
    fmt.channels = 1;
    g_snd_fmt_ret = audsrv_set_format(&fmt);
    if (g_snd_fmt_ret != 0)
    {
        printf("snd: audsrv_set_format() failed (%d) - no sound\n", g_snd_fmt_ret);
        return false;
    }
    audsrv_set_volume(MAX_VOLUME);

    // The mixer must out-rank the game/render thread so it gets scheduled to
    // mix the instant audsrv needs the next chunk -- otherwise short sfx
    // (gunshots, enemy deaths) are missed during render-heavy frames. It
    // blocks on audsrv_wait_audio(), so it runs in short bursts and doesn't
    // slow the game. Lower the main thread, run the mixer above it.
    ChangeThreadPriority(GetThreadId(), 0x40);

    sound_running = 1;
    __asm__ volatile ("move %0, $28" : "=r"(gp));   // current $gp
    memset(&th, 0, sizeof(th));
    th.func             = (void *) MixerThread;
    th.stack            = mixer_stack;
    th.stack_size       = sizeof(mixer_stack);
    th.gp_reg           = gp;
    th.initial_priority = 0x20;
    mixer_tid = CreateThread(&th);
    g_snd_tid = mixer_tid;
    if (mixer_tid < 0)
    {
        printf("snd: CreateThread() failed - no sound\n");
        sound_running = 0;
        return false;
    }
    StartThread(mixer_tid, NULL);
    g_snd_running = 1;

    printf("snd: native audsrv mixer running (%d Hz mono, %d ch)\n",
           OUT_RATE, NUM_CHANNELS);
    return true;
}

static void I_PS2_ShutdownSound(void)
{
    sound_running = 0;
}

static int I_PS2_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char buf[9];
    GetSfxName(sfx, buf, sizeof(buf));
    return W_GetNumForName(buf);
}

static void I_PS2_Update(void)
{
    // The mixer thread does all the work; nothing to do per frame.
}

static void I_PS2_UpdateSoundParams(int channel, int vol, int sep)
{
    if (channel < 0 || channel >= NUM_CHANNELS)
        return;
    SetChannelVol(&channels[channel], vol, sep);
}

static int I_PS2_StartSound(sfxinfo_t *sfx, int channel, int vol, int sep)
{
    cached_sfx_t *c;
    channel_t *ch;

    if (channel < 0 || channel >= NUM_CHANNELS)
        return -1;

    c = GetCachedSfx(sfx);
    if (c == NULL)
        return -1;

    ch = &channels[channel];
    ch->active = 0;                 // stop whatever was playing
    ch->data   = c->data;
    ch->len    = c->len;
    ch->pos    = 0;
    ch->step   = ((unsigned int) c->rate << 16) / OUT_RATE;
    SetChannelVol(ch, vol, sep);
    ch->active = 1;                 // arm last (mixer reads this)

    return channel;
}

static void I_PS2_StopSound(int channel)
{
    if (channel < 0 || channel >= NUM_CHANNELS)
        return;
    channels[channel].active = 0;
}

static boolean I_PS2_SoundIsPlaying(int channel)
{
    if (channel < 0 || channel >= NUM_CHANNELS)
        return false;
    return channels[channel].active;
}

static void I_PS2_CacheSounds(sfxinfo_t *sounds, int num_sounds)
{
    // Lazy: sounds are decoded on first play in StartSound.
    (void) sounds;
    (void) num_sounds;
}

static snddevice_t sound_ps2_devices[] =
{
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module =
{
    sound_ps2_devices,
    arrlen(sound_ps2_devices),
    I_PS2_InitSound,
    I_PS2_ShutdownSound,
    I_PS2_GetSfxLumpNum,
    I_PS2_Update,
    I_PS2_UpdateSoundParams,
    I_PS2_StartSound,
    I_PS2_StopSound,
    I_PS2_SoundIsPlaying,
    I_PS2_CacheSounds,
};

// ---- no-op music module (no MIDI synth on PS2) -------------------------

static boolean I_PS2_InitMusic(void)       { return true; }
static void    I_PS2_ShutdownMusic(void)   { }
static void    I_PS2_SetMusicVol(int v)    { (void) v; }
static void    I_PS2_PauseMusic(void)      { }
static void    I_PS2_ResumeMusic(void)     { }
static void   *I_PS2_RegisterSong(void *d, int l) { (void) d; (void) l; return NULL; }
static void    I_PS2_UnRegisterSong(void *h){ (void) h; }
static void    I_PS2_PlaySong(void *h, boolean loop) { (void) h; (void) loop; }
static void    I_PS2_StopSong(void)        { }
static boolean I_PS2_MusicIsPlaying(void)  { return false; }
static void    I_PS2_PollMusic(void)       { }

music_module_t DG_music_module =
{
    NULL,
    0,
    I_PS2_InitMusic,
    I_PS2_ShutdownMusic,
    I_PS2_SetMusicVol,
    I_PS2_PauseMusic,
    I_PS2_ResumeMusic,
    I_PS2_RegisterSong,
    I_PS2_UnRegisterSong,
    I_PS2_PlaySong,
    I_PS2_StopSong,
    I_PS2_MusicIsPlaying,
    I_PS2_PollMusic,
};
