// PS2 audio prerequisite.
//
// SDL's PS2 audio driver loads and initialises the 'audsrv' IOP module
// ITSELF, via libps2_drivers' init_audio_driver() (called on
// SDL_Init(SDL_INIT_AUDIO)). That loads audsrv from an EE memory buffer with
// SifExecModuleBuffer + audsrv_init().
//
// A retail kernel blocks loading modules from EE buffers unless the SBV "load
// module buffer" patch is applied, and neither libSDL2main nor
// init_audio_driver applies it -- so audsrv load returns -200 and there is no
// sound. All we need to do is enable buffer module loading here, BEFORE Doom's
// S_Init opens audio.
//
// IMPORTANT: do NOT load audsrv or call audsrv_init() here. Doing so in
// addition to init_audio_driver double-loads / double-inits audsrv, which
// fails to reserve the audio channel and produces silence.

#include <stdio.h>
#include <iopheap.h>    // SifInitIopHeap
#include <loadfile.h>   // SifLoadFileInit
#include <sbv_patches.h>// sbv_patch_enable_lmb

void PS2Audio_Init(void)
{
    // NOTE: libSDL2main already did SifInitRpc(); re-doing it can break hostfs.
    SifLoadFileInit();
    SifInitIopHeap();
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();

    printf("audio: enabled IRX-from-buffer loading (audsrv handled by SDL)\n");
}
