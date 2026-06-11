// Vendored PS2 audio driver bring-up.
//
// SDL's PS2 audio backend calls init_audio_driver()/deinit_audio_driver()
// (normally provided by libps2_drivers) when the audio subsystem starts.
// We provide our OWN versions here so we control -- and can see -- exactly
// how the audsrv sound stack is loaded. The linker uses these over the
// prebuilt library copies.
//
// It loads LIBSD.IRX + AUDSRV.IRX (embedded) and calls audsrv_init() ONCE
// (guarded), after enabling IRX-from-buffer loading on the retail kernel.

#include <stdio.h>
#include <loadfile.h>       // SifExecModuleBuffer, SifLoadFileInit
#include <iopheap.h>        // SifInitIopHeap
#include <sbv_patches.h>    // sbv_patch_enable_lmb
#include <audsrv.h>         // audsrv_init, audsrv_quit
#include <ps2_audio_driver.h>  // enum AUDIO_INIT_STATUS

// Embedded IRX modules (bin2c: libsd_irx.c, audsrv_irx.c).
extern unsigned char libsd_irx[];   extern unsigned int size_libsd_irx;
extern unsigned char audsrv_irx[];  extern unsigned int size_audsrv_irx;

static int g_audio_loaded = 0;

enum AUDIO_INIT_STATUS init_audio_driver(void)
{
    int ret = 0;

    // SDL can call this more than once; only do the work once.
    if (g_audio_loaded)
        return AUDIO_INIT_STATUS_OK;

    // Allow loading IRX modules from EE memory buffers (retail kernel blocks
    // it otherwise -> SifExecModuleBuffer returns -200).
    SifLoadFileInit();
    SifInitIopHeap();
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();

    if (SifExecModuleBuffer(libsd_irx, size_libsd_irx, 0, NULL, &ret) < 0)
    {
        printf("audio: LIBSD.IRX load FAILED\n");
        return AUDIO_INIT_STATUS_LIBSD_IRX_ERROR;
    }
    printf("audio: LIBSD.IRX loaded (start ret=%d)\n", ret);

    if (SifExecModuleBuffer(audsrv_irx, size_audsrv_irx, 0, NULL, &ret) < 0)
    {
        printf("audio: AUDSRV.IRX load FAILED\n");
        return AUDIO_INIT_STATUS_AUDSRV_IRX_ERROR;
    }
    printf("audio: AUDSRV.IRX loaded (start ret=%d)\n", ret);

    if (audsrv_init() != 0)
    {
        printf("audio: audsrv_init() FAILED\n");
        return AUDIO_INIT_STATUS_EEAUDSRV_ERROR;
    }
    printf("audio: audsrv_init() OK -- sound stack ready\n");

    g_audio_loaded = 1;
    return AUDIO_INIT_STATUS_OK;
}

void deinit_audio_driver(void)
{
    if (g_audio_loaded)
    {
        audsrv_quit();
        g_audio_loaded = 0;
    }
}
