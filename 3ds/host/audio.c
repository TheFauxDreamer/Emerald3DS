// m4a -> NDSP audio.
//
// rp2350/m4a_1.c is the C port of the GBA's MP2K mixer and is compiled into the
// game archive; it hands us signed 8-bit mono samples through Rp2350MixFrame().
// NDSP plays PCM8 natively, so no format conversion is needed -- the samples go
// to the DSP exactly as the mixer produced them.
//
// The mixer runs once per game frame and can stall (a load spike, a save
// flush), while the DSP consumes at a constant rate. A ring between them
// absorbs that jitter, the same job the GBA's DirectSound DMA double-buffer
// did for the VBlank mixer.
//
// The one thing that is NOT obvious here is thread priority; see
// fix_thread_priority() below. It is why this file could look completely
// correct and still produce silence on a console.

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "../bridge.h"
#include "trace.h"

// m4a produces this many samples per VBlank at Emerald's SOUND_MODE_FREQ_13379
// (gPcmSamplesPerVBlankTable index 4, m4a_internal.h).
#define SAMPLES_PER_FRAME 224

// The game is paced by the 3DS panel, not by the GBA's 59.7275 Hz, so it
// produces SAMPLES_PER_FRAME once per 3DS refresh. The playback rate has to
// match THAT, or the ring drifts one way at a fixed rate and the drift alone
// guarantees a dropout on a timer.
//
// The panel is 59.8261 Hz, not 60: the old 60.0 here asked the DSP for 39
// samples a second more than the game produces, which drains the ring and
// clicks roughly every six seconds even when nothing else is wrong.
//
// 224 x 59.8261 = 13401 Hz, which is also closer to the GBA's true 13379 than
// the old value was, so the pitch error drops from 0.46% to 0.16%.
#define REFRESH_HZ  59.8261f
#define SAMPLE_RATE (SAMPLES_PER_FRAME * REFRESH_HZ)   // 13401 Hz

// Enough wave buffers to stay queued through a couple of slow frames without
// the DSP running dry, but short enough to keep audio latency imperceptible
// (4 x 224 samples @ 13401 Hz = 67 ms worst case).
#define NUM_WAVEBUFS 4
#define BLOCK_SAMPLES SAMPLES_PER_FRAME

#define RING_SAMPLES (SAMPLES_PER_FRAME * 16)

// libctru creates the NDSP service thread at this priority (ndspInit,
// libctru/source/ndsp/ndsp.c). Hardcoded there, so it is a constant we have to
// live with rather than something we can ask for.
#define NDSP_THREAD_PRIO 0x18

// What the main thread is lowered to once NDSP is up. 0x30 is the priority
// every .3dsx gets under hbmenu, which is the configuration NDSP is actually
// tested in, and it reproduces that ordering exactly:
//
//     0x18  NDSP service thread   (ndsp.c)
//     0x1A  GSP event thread      (gspgpu.c)   signals VBlank / P3D / PPF
//     0x30  us
//     0x31  APT event handler     (apt.c)
#define MAIN_THREAD_PRIO 0x30

// How long to run before writing the one-line audio health report.
#define HEALTH_REPORT_FRAME 600   // ~10 seconds

static ndspWaveBuf sWaveBuf[NUM_WAVEBUFS];
static int8_t     *sBlock[NUM_WAVEBUFS];   // linearAlloc'd, DSP-visible

static int8_t   sRing[RING_SAMPLES];
static uint32_t sRingHead, sRingTail;      // free-running; head - tail = fill

static int sReady;

// Counters behind the health report. Each one distinguishes a different way for
// this to end up silent, which is the whole point of keeping them apart: on a
// console there is no debugger and no console output, so the log line they
// produce is the only evidence available.
static uint32_t sFrames;
static uint32_t sUnderruns;   // a buffer was free but the ring was short
static uint32_t sStalled;     // NOTHING was free: the DSP is not consuming
static uint32_t sZeroMix;     // the game-side mixer produced no samples
static uint32_t sDropped;     // ring full, samples discarded
static uint32_t sQueued;      // wave buffers handed to the DSP

static uint32_t ring_fill(void) { return sRingHead - sRingTail; }

// Why audio can be silent on a console and fine in an emulator.
//
// The 3DS scheduler is strict priority with no round-robin between different
// priorities: a lower-priority thread runs only while every higher-priority
// one is blocked. NDSP does its work on its own thread, created at 0x18.
//
// This port ships as a CIA, and 3ds/emerald3ds.rsf sets the main thread to
// priority 0x10 -- NUMERICALLY LOWER, so HIGHER priority than the DSP thread.
// AffinityMask is 1 and SystemModeExt is Legacy, so both threads are confined
// to core 0 and cannot simply run side by side. The result is a game loop that
// software-rasterises a whole GBA frame while outranking the thread whose job
// is to keep the DSP fed, and NDSP gets only the slack left over.
//
// GSP's event thread (0x1A) is outranked today too, but that one recovers by
// itself: the main thread blocks waiting on the very events it signals, which
// forces the yield. Nothing in the frame loop ever waits on audio, so NDSP has
// no equivalent rescue.
//
// An emulator hides this completely: the frame's work costs almost nothing in
// emulated time, so the main thread blocks early and often and the DSP thread
// always gets scheduled. Real silicon has no such slack.
//
// A .3dsx under hbmenu gets main priority 0x30, i.e. BELOW NDSP, which is the
// arrangement every working NDSP homebrew actually runs in. Restore it.
//
// Done here rather than in the exheader because it should only happen when
// there is a DSP thread to yield to: with audio disabled there is nothing on
// this core to give priority to, and the game may as well keep it.
static void fix_thread_priority(void)
{
    s32 before = 0, after = 0;
    Result rc;

    svcGetThreadPriority(&before, CUR_THREAD_HANDLE);

    if (before < NDSP_THREAD_PRIO) {
        rc = svcSetThreadPriority(CUR_THREAD_HANDLE, MAIN_THREAD_PRIO);
        svcGetThreadPriority(&after, CUR_THREAD_HANDLE);

        if (R_FAILED(rc)) {
            // Not fatal: it means audio may stutter or stay silent under load,
            // which is exactly what the health report below will then say.
            CtrLog("emerald3ds: could not lower main thread priority "
                   "(0x%02lX, rc=0x%08lX) - audio may starve\n",
                   (unsigned long)before, (unsigned long)rc);
            return;
        }

        CtrLog("emerald3ds: main thread priority 0x%02lX -> 0x%02lX "
               "(below NDSP's 0x%02X)\n",
               (unsigned long)before, (unsigned long)after, NDSP_THREAD_PRIO);
    }
}

void CtrAudioInit(void)
{
    Result rc = ndspInit();
    if (R_FAILED(rc)) {
        // Almost always a missing DSP firmware dump: libctru loads the DSP
        // component from sdmc:/3ds/dspfirm.cdc and ndspInit() fails outright if
        // that file is absent. The game is perfectly playable silent, so this
        // is a warning -- but it must be a VISIBLE one, or it presents as
        // "the port has no sound".
        //
        // This goes to sdmc:/3ds/emerald3ds/log.txt as well as to the
        // emulator's debug output (3ds/host/log.c), because on a console the
        // debug output goes nowhere and this message is the whole answer.
        CtrLog("emerald3ds: audio disabled - ndspInit failed (rc=0x%08lX). "
               "Missing sdmc:/3ds/dspfirm.cdc? Dump it with DSP1.\n",
               (unsigned long)rc);
        return;
    }

    ndspSetOutputMode(NDSP_OUTPUT_MONO);
    ndspChnSetInterp(0, NDSP_INTERP_NONE);      // no resampling: rate matches
    ndspChnSetRate(0, (float)SAMPLE_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM8);

    for (int i = 0; i < NUM_WAVEBUFS; i++) {
        sBlock[i] = linearAlloc(BLOCK_SAMPLES);
        if (sBlock[i] == NULL) {
            CtrLog("emerald3ds: audio disabled - linearAlloc(%d) failed\n",
                   BLOCK_SAMPLES);
            return;
        }
        memset(sBlock[i], 0, BLOCK_SAMPLES);
        memset(&sWaveBuf[i], 0, sizeof(sWaveBuf[i]));
        sWaveBuf[i].data_vaddr = sBlock[i];
        sWaveBuf[i].nsamples   = BLOCK_SAMPLES;
        sWaveBuf[i].status     = NDSP_WBUF_DONE;   // free for the first fill
    }

    // Only now that there is a DSP thread worth yielding to.
    fix_thread_priority();

    sReady = 1;
    CtrLog("emerald3ds: audio ready (%d Hz, mono PCM8)\n", (int)SAMPLE_RATE);
}

void CtrAudioExit(void)
{
    if (!sReady)
        return;

    ndspChnWaveBufClear(0);
    for (int i = 0; i < NUM_WAVEBUFS; i++)
        linearFree(sBlock[i]);
    ndspExit();
    sReady = 0;
}

// One line, once, naming which of the ways this can fail actually happened.
// Without it "no sound" is indistinguishable from "sound, but you were in a
// silent room of the game", and every cause below looks identical on a console.
static void health_report(void)
{
    const char *verdict;

    if (sZeroMix >= sFrames)
        verdict = "the game produced no samples at all";
    else if (sStalled > sFrames / 4)
        verdict = "the DSP is not draining buffers (NDSP thread starved?)";
    else if (sUnderruns > sFrames / 4)
        verdict = "the game is not keeping up with the DSP";
    else
        verdict = "healthy";

    CtrLog("emerald3ds: audio after %lu frames - queued %lu, stalled %lu, "
           "underruns %lu, silent-mix %lu, dropped %lu: %s\n",
           (unsigned long)sFrames, (unsigned long)sQueued,
           (unsigned long)sStalled, (unsigned long)sUnderruns,
           (unsigned long)sZeroMix, (unsigned long)sDropped, verdict);
}

// Run the mixer for one game frame and top up the DSP queue. Called from
// Rp2350PresentFrame() on the main thread.
void CtrAudioFrame(void)
{
    int8_t mixed[SAMPLES_PER_FRAME];
    int n, queuedThisFrame = 0;

    if (!sReady)
        return;

    sFrames++;

    // 1. Produce. Mix straight into the ring; drop the frame if the ring is
    //    full (the DSP is behind, which means we are running fast).
    n = Rp2350MixFrame(mixed, SAMPLES_PER_FRAME);
    if (n <= 0)
        sZeroMix++;

    for (int i = 0; i < n; i++) {
        if (ring_fill() >= RING_SAMPLES) {
            sDropped++;
            break;
        }
        sRing[sRingHead % RING_SAMPLES] = mixed[i];
        sRingHead++;
    }

    // 2. Consume. Refill every finished wave buffer that the ring can cover.
    for (int i = 0; i < NUM_WAVEBUFS; i++) {
        if (sWaveBuf[i].status != NDSP_WBUF_FREE &&
            sWaveBuf[i].status != NDSP_WBUF_DONE)
            continue;

        if (ring_fill() < BLOCK_SAMPLES) {
            sUnderruns++;
            break;
        }

        for (int s = 0; s < BLOCK_SAMPLES; s++) {
            sBlock[i][s] = sRing[sRingTail % RING_SAMPLES];
            sRingTail++;
        }

        // The DSP reads this memory directly and does not see the ARM11 cache.
        DSP_FlushDataCache(sBlock[i], BLOCK_SAMPLES);
        ndspChnWaveBufAdd(0, &sWaveBuf[i]);
        sQueued++;
        queuedThisFrame++;
    }

    // Every buffer still QUEUED or PLAYING. Separate from an underrun on
    // purpose: an underrun means WE were short, this means the DSP side has not
    // returned a single buffer, which is what a starved NDSP thread looks like
    // from here and is not a shortage of samples at all.
    if (queuedThisFrame == 0 && ring_fill() >= BLOCK_SAMPLES)
        sStalled++;

    if (sFrames == HEALTH_REPORT_FRAME)
        health_report();
}

uint32_t CtrAudioUnderruns(void) { return sUnderruns; }
