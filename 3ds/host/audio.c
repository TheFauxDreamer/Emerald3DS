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

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "../bridge.h"
#include "trace.h"

// m4a produces this many samples per VBlank at Emerald's SOUND_MODE_FREQ_13379
// (gPcmSamplesPerVBlankTable index 4, m4a_internal.h).
#define SAMPLES_PER_FRAME 224

// The game is paced to 60 Hz here, not the GBA's 59.73, so it produces
// SAMPLES_PER_FRAME 60 times a second. Matching the playback rate to that keeps
// the ring from drifting full or empty. (0.5% sharp of the true GBA rate.)
#define SAMPLE_RATE (SAMPLES_PER_FRAME * 60)   // 13440 Hz

// Enough wave buffers to stay queued through a couple of slow frames without
// the DSP running dry, but short enough to keep audio latency imperceptible
// (4 x 224 samples @ 13440 Hz = 66 ms worst case).
#define NUM_WAVEBUFS 4
#define BLOCK_SAMPLES SAMPLES_PER_FRAME

#define RING_SAMPLES (SAMPLES_PER_FRAME * 16)

static ndspWaveBuf sWaveBuf[NUM_WAVEBUFS];
static int8_t     *sBlock[NUM_WAVEBUFS];   // linearAlloc'd, DSP-visible

static int8_t   sRing[RING_SAMPLES];
static uint32_t sRingHead, sRingTail;      // free-running; head - tail = fill

static int sReady;
static uint32_t sUnderruns;

static uint32_t ring_fill(void) { return sRingHead - sRingTail; }

void CtrAudioInit(void)
{
    Result rc = ndspInit();
    if (R_FAILED(rc)) {
        // Almost always a missing DSP firmware dump: libctru loads the DSP
        // component from sdmc:/3ds/dspfirm.cdc and ndspInit() fails outright if
        // that file is absent. The game is perfectly playable silent, so this
        // is a warning -- but it must be a VISIBLE one, or it presents as
        // "the port has no sound".
        CtrLog("emerald3ds: audio disabled - ndspInit failed (rc=0x%08lX). "
               "Missing sdmc:/3ds/dspfirm.cdc?\n", (unsigned long)rc);
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

    sReady = 1;
    CtrLog("emerald3ds: audio ready (%d Hz, mono PCM8)\n", SAMPLE_RATE);
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

// Run the mixer for one game frame and top up the DSP queue. Called from
// Rp2350PresentFrame() on the main thread.
void CtrAudioFrame(void)
{
    if (!sReady)
        return;

    // 1. Produce. Mix straight into the ring; drop the frame if the ring is
    //    full (the DSP is behind, which means we are running fast).
    int8_t mixed[SAMPLES_PER_FRAME];
    int n = Rp2350MixFrame(mixed, SAMPLES_PER_FRAME);

    for (int i = 0; i < n; i++) {
        if (ring_fill() >= RING_SAMPLES)
            break;
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
    }
}

uint32_t CtrAudioUnderruns(void) { return sUnderruns; }
