//
// PS2 OPL driver: software FM synthesis (DBOPL) rendered on demand.
//
// This replaces opl_sdl.c. Instead of being driven by an SDL audio callback,
// it exposes OPL_PS2_Render(), which the native audsrv mixer (i_audsrvsound.c)
// calls each chunk to pull `nframes` of stereo music. The render loop mirrors
// opl_sdl.c: synthesize up to the next scheduled MIDI event, fire due events
// (which write OPL registers / schedule the next event), repeat.
//
// SDL mutexes are replaced by PS2 semaphores: q_sema guards the event queue,
// cb_sema is the "callback lock" (OPL_Lock/Unlock) the game thread holds while
// (re)arranging playback so the mixer thread won't fire events meanwhile.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>          // ee semaphores

#include "dbopl.h"
#include "opl.h"
#include "opl_internal.h"
#include "opl_queue.h"

#ifndef OPL_SECOND
#define OPL_SECOND ((uint64_t) 1000000)
#endif

typedef struct
{
    unsigned int rate;
    unsigned int enabled;
    unsigned int value;
    uint64_t expire_time;
} opl_timer_t;

static opl_callback_queue_t *callback_queue;
static uint64_t current_time;
static int      opl_ps2_paused;
static uint64_t pause_offset;

#define OPL_PS2_BLOCK 1024            // max frames rendered per DBOPL call

static Chip        opl_chip;
static int         dbopl_tables_ready = 0;
static int32_t     dbopl_mix[OPL_PS2_BLOCK * 2];   // DBOPL emits 32-bit samples
static int         opl_opl3mode;
static int         register_num = 0;
static unsigned int mixing_freq;

static opl_timer_t timer1 = { 12500, 0, 0, 0 };
static opl_timer_t timer2 = { 3125,  0, 0, 0 };

static int q_sema  = -1;   // protects callback_queue
static int cb_sema = -1;   // held while a callback runs / game thread edits

static void SemaTake(int s) { if (s >= 0) WaitSema(s); }
static void SemaGive(int s) { if (s >= 0) SignalSema(s); }

// Advance time by `us` microseconds, firing any callbacks whose time arrived.
static void AdvanceTimeUs(uint64_t us)
{
    opl_callback_t callback;
    void *callback_data;

    SemaTake(q_sema);

    current_time += us;

    if (opl_ps2_paused)
        pause_offset += us;

    while (!OPL_Queue_IsEmpty(callback_queue)
        && current_time >= OPL_Queue_Peek(callback_queue) + pause_offset)
    {
        if (!OPL_Queue_Pop(callback_queue, &callback, &callback_data))
            break;

        // Release the queue lock while invoking (the callback may schedule
        // new events), but hold cb_sema so the game thread's OPL_Lock blocks.
        SemaGive(q_sema);
        SemaTake(cb_sema);
        callback(callback_data);
        SemaGive(cb_sema);
        SemaTake(q_sema);
    }

    SemaGive(q_sema);
}

static void AdvanceTime(unsigned int nsamples)
{
    AdvanceTimeUs(((uint64_t) nsamples * OPL_SECOND) / mixing_freq);
}

// Advance the OPL clock synchronously (no rendering). OPL_Delay() uses this so
// chip detection works -- detection runs before the mixer thread exists, so
// nothing else would move time forward.
void OPL_PS2_AdvanceUs(uint64_t us)
{
    AdvanceTimeUs(us);
}

// Render `n` frames of OPL audio as interleaved int16 stereo. DBOPL emits
// 32-bit samples -- mono in OPL2 mode (the Doom default; doubled to stereo
// here), stereo in OPL3 mode -- which we clamp and downcast to int16.
static void RenderBlock(int16_t *out, unsigned int n)
{
    unsigned int i;
    int32_t v;

    if (opl_opl3mode)
    {
        Chip__GenerateBlock3(&opl_chip, n, dbopl_mix);
        for (i = 0; i < n; ++i)
        {
            v = dbopl_mix[i * 2];
            if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
            out[i * 2] = (int16_t) v;
            v = dbopl_mix[i * 2 + 1];
            if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
            out[i * 2 + 1] = (int16_t) v;
        }
    }
    else
    {
        Chip__GenerateBlock2(&opl_chip, n, dbopl_mix);
        for (i = 0; i < n; ++i)
        {
            v = dbopl_mix[i];
            if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
            out[i * 2] = out[i * 2 + 1] = (int16_t) v;
        }
    }
}

// Pull `nframes` of stereo (interleaved L,R int16) OPL music. Called by the
// audsrv mixer thread.
void OPL_PS2_Render(int16_t *stereo_out, int nframes)
{
    int filled = 0;

    // No OPL, or no event scheduled (= no song playing) -> silence cheaply,
    // skipping the expensive FM synthesis when there's nothing to play.
    if (q_sema < 0 || OPL_Queue_IsEmpty(callback_queue))
    {
        memset(stereo_out, 0, (size_t) nframes * 2 * sizeof(int16_t));
        return;
    }

    while (filled < nframes)
    {
        uint64_t ns;

        SemaTake(q_sema);
        if (opl_ps2_paused || OPL_Queue_IsEmpty(callback_queue))
        {
            ns = nframes - filled;
        }
        else
        {
            uint64_t next = OPL_Queue_Peek(callback_queue) + pause_offset;
            ns = (next - current_time) * mixing_freq;
            ns = (ns + OPL_SECOND - 1) / OPL_SECOND;
            if (ns > (uint64_t)(nframes - filled))
                ns = nframes - filled;
            if (ns == 0)
                ns = 1;        // always make progress
        }
        SemaGive(q_sema);

        if (ns > OPL_PS2_BLOCK)
            ns = OPL_PS2_BLOCK;

        RenderBlock(stereo_out + filled * 2, (unsigned int) ns);
        filled += ns;
        AdvanceTime(ns);
    }
}

// ---- opl_driver_t implementation --------------------------------------

static int OPL_PS2_Init(unsigned int port_base)
{
    ee_sema_t s;

    (void) port_base;

    mixing_freq     = opl_sample_rate;   // set via OPL_SetSampleRate()
    opl_ps2_paused  = 0;
    pause_offset    = 0;
    current_time    = 0;
    opl_opl3mode    = 0;

    callback_queue = OPL_Queue_Create();
    if (!dbopl_tables_ready)        // global wave/volume tables, computed once
    {
        DBOPL_InitTables();
        dbopl_tables_ready = 1;
    }
    Chip__Chip(&opl_chip);
    Chip__Setup(&opl_chip, mixing_freq);

    memset(&s, 0, sizeof(s));
    s.init_count = 1; s.max_count = 1;
    q_sema = CreateSema(&s);
    memset(&s, 0, sizeof(s));
    s.init_count = 1; s.max_count = 1;
    cb_sema = CreateSema(&s);

    return 1;
}

static void OPL_PS2_Shutdown(void)
{
    if (callback_queue) { OPL_Queue_Destroy(callback_queue); callback_queue = NULL; }
    if (q_sema  >= 0) { DeleteSema(q_sema);  q_sema  = -1; }
    if (cb_sema >= 0) { DeleteSema(cb_sema); cb_sema = -1; }
}

static unsigned int OPL_PS2_PortRead(opl_port_t port)
{
    unsigned int result = 0;

    if (port == OPL_REGISTER_PORT_OPL3)
        return 0xff;

    if (timer1.enabled && current_time > timer1.expire_time)
    {
        result |= 0x80;
        result |= 0x40;
    }
    if (timer2.enabled && current_time > timer2.expire_time)
    {
        result |= 0x80;
        result |= 0x20;
    }
    return result;
}

static void OPLTimer_CalculateEndTime(opl_timer_t *timer)
{
    int tics;
    if (timer->enabled)
    {
        tics = 0x100 - timer->value;
        timer->expire_time = current_time
                           + ((uint64_t) tics * OPL_SECOND) / timer->rate;
    }
}

static void WriteRegister(unsigned int reg_num, unsigned int value)
{
    switch (reg_num)
    {
        case OPL_REG_TIMER1:
            timer1.value = value;
            OPLTimer_CalculateEndTime(&timer1);
            break;
        case OPL_REG_TIMER2:
            timer2.value = value;
            OPLTimer_CalculateEndTime(&timer2);
            break;
        case OPL_REG_TIMER_CTRL:
            if (value & 0x80)
            {
                timer1.enabled = 0;
                timer2.enabled = 0;
            }
            else
            {
                if ((value & 0x40) == 0)
                {
                    timer1.enabled = (value & 0x01) != 0;
                    OPLTimer_CalculateEndTime(&timer1);
                }
                if ((value & 0x20) == 0)
                {
                    timer2.enabled = (value & 0x02) != 0;
                    OPLTimer_CalculateEndTime(&timer2);
                }
            }
            break;
        case OPL_REG_NEW:
            opl_opl3mode = value & 0x01;
            /* fall through */
        default:
            Chip__WriteReg(&opl_chip, reg_num, value & 0xff);
            break;
    }
}

static void OPL_PS2_PortWrite(opl_port_t port, unsigned int value)
{
    if (port == OPL_REGISTER_PORT)
        register_num = value;
    else if (port == OPL_REGISTER_PORT_OPL3)
        register_num = value | 0x100;
    else if (port == OPL_DATA_PORT)
        WriteRegister(register_num, value);
}

static void OPL_PS2_SetCallback(uint64_t us, opl_callback_t callback, void *data)
{
    SemaTake(q_sema);
    OPL_Queue_Push(callback_queue, callback, data,
                   current_time - pause_offset + us);
    SemaGive(q_sema);
}

static void OPL_PS2_ClearCallbacks(void)
{
    SemaTake(q_sema);
    OPL_Queue_Clear(callback_queue);
    SemaGive(q_sema);
}

static void OPL_PS2_Lock(void)   { SemaTake(cb_sema); }
static void OPL_PS2_Unlock(void) { SemaGive(cb_sema); }
static void OPL_PS2_SetPaused(int paused) { opl_ps2_paused = paused; }

static void OPL_PS2_AdjustCallbacks(float factor)
{
    SemaTake(q_sema);
    OPL_Queue_AdjustCallbacks(callback_queue, current_time, factor);
    SemaGive(q_sema);
}

opl_driver_t opl_ps2_driver =
{
    "PS2",
    OPL_PS2_Init,
    OPL_PS2_Shutdown,
    OPL_PS2_PortRead,
    OPL_PS2_PortWrite,
    OPL_PS2_SetCallback,
    OPL_PS2_ClearCallbacks,
    OPL_PS2_Lock,
    OPL_PS2_Unlock,
    OPL_PS2_SetPaused,
    OPL_PS2_AdjustCallbacks,
};
