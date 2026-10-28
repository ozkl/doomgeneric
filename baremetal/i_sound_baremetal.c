// Doom's sound on bare metal. Effects: the WAD's DMX samples (8-bit
// unsigned, 11025 Hz mostly) go to the interrupt-driven mixer (mixer.cpp).
// Music: MUS lumps become MIDI (mus2mid) and play on SBPRO's General MIDI
// synth over two emulated OPL3s (music.cpp). Both reach the sound card
// from the timer interrupt.
#include <string.h>
#include <stdio.h>
#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "mixer.hpp"
#include "music.h"
#include "memio.h"
#include "mus2mid.h"

int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;
char* timidity_cfg_path = "";
char* gus_patch_path = "";
int gus_ram_kb = 1024;
void I_InitTimidityConfig(void) {}

static boolean use_prefix;

// A sound lump, checked and parsed once: the samples and their rate.
typedef struct { const uint8_t* data; uint32_t len, rate; } sample_t;

static boolean Init(boolean use_sfx_prefix)
{
    // On even with no card: one can be picked later in the Sound menu.
    use_prefix = use_sfx_prefix;
    printf("I_InitSound: sound effects through the mixer%s\n", mixer_enabled() ? "" : " (no card yet)");
    return true;
}

static void Shutdown(void)
{
    for (int c = 0; c < MIXER_CHANNELS; c++) mixer_stop(c);
}

static int GetSfxLumpNum(sfxinfo_t* sfx)
{
    char name[16];
    if (sfx->link) sfx = sfx->link;
    snprintf(name, sizeof name, use_prefix ? "ds%s" : "%s", sfx->name);
    return W_CheckNumForName(name);
}

static sample_t* Load(sfxinfo_t* sfx)
{
    if (sfx->link) sfx = sfx->link;
    if (sfx->driver_data) return sfx->driver_data;
    if (sfx->lumpnum < 0) sfx->lumpnum = GetSfxLumpNum(sfx);
    if (sfx->lumpnum < 0) return NULL;
    // PU_STATIC: the mixer reads it from the timer interrupt, so the
    // zone must never move or free it.
    const uint8_t* d = W_CacheLumpNum(sfx->lumpnum, PU_STATIC);
    uint32_t size = W_LumpLength(sfx->lumpnum);
    // DMX: format 3, rate, sample count; the samples are padded by 16
    // bytes at each end.
    if (size < 8 || d[0] != 3 || d[1] != 0) return NULL;
    uint32_t rate = d[2] | d[3] << 8;
    uint32_t len = d[4] | d[5] << 8 | (uint32_t)d[6] << 16 | (uint32_t)d[7] << 24;
    if (len > size - 8) len = size - 8;
    sample_t* s = Z_Malloc(sizeof *s, PU_STATIC, NULL);
    if (len > 32) { s->data = d + 8 + 16; s->len = len - 32; }
    else { s->data = d + 8; s->len = len; }
    s->rate = rate ? rate : 11025;
    sfx->driver_data = s;
    return s;
}

static void UpdateSoundParams(int channel, int vol, int sep) { mixer_set(channel, vol, sep); }

static int StartSound(sfxinfo_t* sfx, int channel, int vol, int sep)
{
    if (channel < 0 || channel >= MIXER_CHANNELS) return -1;
    sample_t* s = Load(sfx);
    if (!s) return -1;
    mixer_start(channel, s->data, s->len, s->rate, vol, sep);
    return channel;
}

static void StopSound(int channel) { mixer_stop(channel); }
static boolean SoundIsPlaying(int channel) { return mixer_playing(channel) ? true : false; }
static void Update(void) {}
static void CacheSounds(sfxinfo_t* sounds, int num)
{
    for (int i = 0; i < num; i++) Load(&sounds[i]);
}

static snddevice_t devices[] = { SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER,
                                 SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32 };

sound_module_t DG_sound_module = {
    devices, sizeof devices / sizeof devices[0],
    Init, Shutdown, GetSfxLumpNum, Update, UpdateSoundParams,
    StartSound, StopSound, SoundIsPlaying, CacheSounds,
};

// ---------------------------------------------------------------- music
static int music_vol = 100;

static boolean MusInit(void) { return true; }
static void MusShutdown(void) { music_stop(); }
static void MusVolume(int v) { music_vol = v; music_volume(v); }
static void MusPause(void) { music_pause(1); }
static void MusResume(void) { music_pause(0); }

// The lump as MIDI: as is if it already is, else through mus2mid.
static void* MusRegister(void* data, int len)
{
    if (len > 4 && !memcmp(data, "MThd", 4))
        return music_load(data, (uint32_t)len);
    MEMFILE* in = mem_fopen_read(data, len);
    MEMFILE* out = mem_fopen_write();
    void* song = NULL;
    if (!mus2mid(in, out)) {                    // false: converted
        void* buf; size_t blen;
        mem_get_buf(out, &buf, &blen);
        song = music_load(buf, (uint32_t)blen);
    } else {
        printf("Music: not a MUS or MIDI lump\n");
    }
    mem_fclose(in);
    mem_fclose(out);
    return song;
}
static void MusUnregister(void* h) { music_free(h); }
static void MusPlay(void* h, boolean loop) { music_volume(music_vol); music_play(h, loop); }
static void MusStop(void) { music_stop(); }
static boolean MusPlaying(void) { return music_playing() ? true : false; }

music_module_t DG_music_module = {
    devices, sizeof devices / sizeof devices[0],
    MusInit, MusShutdown, MusVolume, MusPause, MusResume,
    MusRegister, MusUnregister, MusPlay, MusStop, MusPlaying, NULL,
};
