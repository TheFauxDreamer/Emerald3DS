// Audio, from m4a to NDSP.
//
// The file rp2350/m4a_mix.c is the mixer interface of the port, compiled into
// the game archive. It gives signed 16-bit samples with DirectSound and the
// four PSG channels already summed. Rp2350MixFrameStereo16() gives interleaved,
// panned L,R samples, and Rp2350MixFrame16() gives a downmix. NDSP plays PCM16
// directly, so the samples reach the DSP with no conversion.
//
// The mixer runs once for each game frame and can stall (a load, a save flush),
// but the DSP uses samples at a constant rate. A ring buffer between them
// absorbs that jitter, like the GBA's DirectSound DMA double buffer.
//
// Thread priority is important here; see fix_thread_priority() below. With the
// wrong priority, this file looks correct and gives silence on a console.
//
// CTR_AUDIO_STEREO selects the stereo path or the older mono path. It is a
// bisect switch for tests, not a preference.

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "../bridge.h"
#include "trace.h"

// The m4a engine makes this many samples for each VBlank at
// SOUND_MODE_FREQ_13379 (gPcmSamplesPerVBlankTable[3] in src/m4a_tables.c;
// m4a.c uses freq - 1, and SOUND_MODE_FREQ_13379 is freq 4).
#define SAMPLES_PER_FRAME 224

// The 3DS panel paces the game, not the GBA's 59.7275 Hz. Thus the game makes
// SAMPLES_PER_FRAME once for each 3DS refresh. The playback rate must match
// that. Otherwise the ring drifts, and the drift causes a dropout at regular
// times.
//
// The panel is 59.8261 Hz, not 60. A rate of 60.0 asks for 39 samples a second
// more than the game makes, which empties the ring and clicks about every six
// seconds.
//
// The rate is 224 x 59.8261 = 13401 Hz. That is also closer to the GBA's 13379,
// so the pitch error is 0.16%.
#define REFRESH_HZ  59.8261f
#define SAMPLE_RATE (SAMPLES_PER_FRAME * REFRESH_HZ)   // 13401 Hz

// Enough wave buffers to stay queued through two slow frames, but few enough to
// keep the latency small. The worst case is 4 x 224 samples at 13401 Hz, 67 ms.
#define NUM_WAVEBUFS 4
#define BLOCK_SAMPLES SAMPLES_PER_FRAME

// NDSP is always stereo. The STEREO A/B switch puts a downmix on both sides and
// does not change the channel format.
//
// A change of ndspChnSetFormat on a running channel means stop, clear the
// queued wave buffers and restart. A switch that finds faults must not cause
// one. Both sides with (L + R) / 2 give the same samples as the mono format.
//
// Everything below uses AUDIO_CHANNELS, not a literal 2. A literal caused a
// 0x1C00-byte overrun here once, when the channel count changed.
#define AUDIO_CHANNELS 2

// In sample frames: the unit of sRingHead and sRingTail, of ring_fill() and of
// BLOCK_SAMPLES. The ring holds interleaved stereo, so the array holds twice
// this many int16 values.
//
// Keep this distinction. An array sized in samples, with the index [frame * 2 +
// channel], wrote 0x1C00 bytes past its end. On a real ARM11 that caused a data
// abort at address 0 a few seconds after boot.
#define RING_FRAMES (SAMPLES_PER_FRAME * 16)

// The libctru library makes the NDSP service thread at this priority (ndspInit,
// in libctru/source/ndsp/ndsp.c). The value is fixed there.
#define NDSP_THREAD_PRIO 0x18

// The main thread's priority after NDSP starts. Every .3dsx under hbmenu runs
// at priority 0x30, which is how NDSP is usually tested. It gives this order:
//
//     0x18  NDSP service thread   (ndsp.c)
//     0x1A  GSP event thread      (gspgpu.c)   signals VBlank / P3D / PPF
//     0x30  this port's main thread
//     0x31  APT event handler     (apt.c)
#define MAIN_THREAD_PRIO 0x30

// The time before the one-line audio health report.
#define HEALTH_REPORT_FRAME 600   // about 10 seconds

static ndspWaveBuf sWaveBuf[NUM_WAVEBUFS];

// PCM16 from the mixer to the DSP. The PSG channels are made at 16 bits, and a
// console mixes them with DirectSound in analog, not on an 8-bit grid. PCM16
// keeps that precision.
static int16_t    *sBlock[NUM_WAVEBUFS];   // linearAlloc'd, visible to the DSP

static int16_t  sRing[RING_FRAMES * AUDIO_CHANNELS];
static uint32_t sRingHead, sRingTail;      // free-running; head - tail = fill

static int sReady;

// The counters for the health report. Each one shows a different cause of
// silence. A console has no debugger and no console output, so this log line is
// the only evidence.
static uint32_t sFrames;
static uint32_t sUnderruns;   // a buffer was free, but the ring was short
static uint32_t sStalled;     // nothing was free: the DSP does not consume
static uint32_t sZeroMix;     // the game-side mixer made no samples
static uint32_t sDropped;     // the ring was full; samples dropped
static uint32_t sQueued;      // wave buffers given to the DSP

// This measurement splits the problem. All counters above can look normal while
// all samples are zero. A return of 224 from Rp2350MixFrame means only that the
// sound engine started, not that anything plays. A peak of 0 means that the
// silence comes from before this file, and no DSP setting can help.
static uint32_t sPeak;        // largest |sample| seen
static uint32_t sNonZero;     // the number of samples that were not silence

// Where a repeating click occurs.
//
// A click is a jump between two samples. Its position in the frame tells the
// cause. At index 0, the fault is at the frame boundary: the buffer handoff,
// the render window or the engine tick. Spread through the frame, the fault is
// in the audio itself. Nothing else in this report can tell the two apart.
//
// The threshold is 32 DirectSound LSBs. DirectSound is 8-bit, so a normal step
// is a multiple of 256. A real waveform at 13.4 kHz does not jump an eighth of
// full scale between two samples.
#define JUMP_THRESHOLD 8192

static int32_t  sPrevSample;   // kept between frames, so index 0 is measured
static uint32_t sJumpCount;    // jumps over the threshold
static uint32_t sJumpAtFrameStart;  // of those, the number at index 0
static uint32_t sJumpMax;      // the largest jump
static uint32_t sJumpMaxPos;   // and its position in the frame

static uint32_t ring_fill(void) { return sRingHead - sRingTail; }

// The address of one frame. Scale the frame index by the channel count only
// here, not at each use. Scaling at each use caused the array to have half the
// needed size.
static int16_t *ring_slot(uint32_t frame)
{
    return &sRing[(frame % RING_FRAMES) * AUDIO_CHANNELS];
}

// Keep the DSP thread able to preempt the game loop.
//
// The 3DS scheduler uses strict priority, with no round-robin between different
// priorities. A lower-priority thread runs only while all higher ones are
// blocked. The libctru library runs the NDSP work on its own thread at 0x18.
// SystemModeExt Legacy keeps that thread and this one on core 0, and on an Old
// 3DS nothing of ours leaves core 0 at all. A main thread above it would starve
// the DSP whenever the rasterizer has work, which is always. Under hbmenu, a
// .3dsx gets main priority 0x30, below NDSP.
//
// The file 3ds/emerald3ds.rsf asks for main priority 0x10, which would be above
// NDSP. On hardware, the main thread was already at 0x18 or below, and this
// function changed nothing. Thus the log below always reports the priority.
//
// The guard stays. It costs one comparison, and the exheader still asks for the
// wrong value.
static void fix_thread_priority(void)
{
    s32 before = 0, after = 0;
    Result rc;

    svcGetThreadPriority(&before, CUR_THREAD_HANDLE);
    after = before;

    if (before < NDSP_THREAD_PRIO) {
        rc = svcSetThreadPriority(CUR_THREAD_HANDLE, MAIN_THREAD_PRIO);
        if (R_FAILED(rc)) {
            CtrLog("emerald3ds: main thread priority 0x%02lX outranks NDSP's "
                   "0x%02X and could not be lowered (rc=0x%08lX)\n",
                   (unsigned long)before, NDSP_THREAD_PRIO, (unsigned long)rc);
            return;
        }
        svcGetThreadPriority(&after, CUR_THREAD_HANDLE);
    }

    // Always log. A missing line proves nothing. If it logged only when it
    // acted, "no inversion" and "the code did not run" would look the same.
    CtrLog("emerald3ds: main thread priority 0x%02lX (NDSP 0x%02X)%s\n",
           (unsigned long)after, NDSP_THREAD_PRIO,
           after != before ? " [lowered]" : "");
}

void CtrAudioInit(void)
{
    Result rc = ndspInit();
    if (R_FAILED(rc)) {
        // Usually a missing DSP firmware dump. The libctru library loads the
        // DSP component from sdmc:/3ds/dspfirm.cdc, and ndspInit() fails if
        // that file is not there. The game works without sound, so this is a
        // warning, but it must be visible. Otherwise it looks like "the port
        // has no sound".
        //
        // In a debug build, it goes to sdmc:/3ds/emerald3ds/log.txt and to the
        // emulator's debug output (3ds/host/log.c). A release build writes no
        // file. On a console, the Limitations section of README.md answers
        // this.
        CtrLog("emerald3ds: audio disabled - ndspInit failed (rc=0x%08lX). "
               "Missing sdmc:/3ds/dspfirm.cdc? Dump it with DSP1.\n",
               (unsigned long)rc);
        return;
    }

    // STEREO, not MONO. A mono channel played into a stereo output with an
    // equal front-left and front-right mix reaches both speakers. That is the
    // usual NDSP setup. NDSP_OUTPUT_MONO changes the DSP's downmix path for no
    // benefit.
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);

    // Both values are already the libctru defaults (ndspInitMaster sets the
    // master volume to 1.0, and ndspChnReset sets mix[0] and mix[1] to 1.0).
    // Set them anyway, because a future libctru can change a default.
    ndspSetMasterVol(1.0f);
    {
        float mix[12] = { 0 };
        mix[0] = 1.0f;   // front left
        mix[1] = 1.0f;   // front right
        ndspChnSetMix(0, mix);
    }

    // LINEAR, not NONE. The DSP runs at 32728 Hz and the mixer gives 13401 Hz,
    // so the DSP always resamples by about 2.44x. NDSP_INTERP_NONE is a
    // zero-order hold, which adds grit to the music. Linear interpolation costs
    // the DSP nothing and removes most of it.
    //
    // Not POLYPHASE: libctru changes it to NONE when the rate ratio is below
    // 1.0 (ndspiUpdateChn). Here the ratio is always below 1.0.
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, SAMPLE_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);

    for (int i = 0; i < NUM_WAVEBUFS; i++) {
        sBlock[i] = linearAlloc(BLOCK_SAMPLES * AUDIO_CHANNELS * sizeof(int16_t));
        if (sBlock[i] == NULL) {
            CtrLog("emerald3ds: audio disabled - linearAlloc(%d) failed\n",
                   (int)(BLOCK_SAMPLES * AUDIO_CHANNELS * sizeof(int16_t)));
            return;
        }
        memset(sBlock[i], 0, BLOCK_SAMPLES * AUDIO_CHANNELS * sizeof(int16_t));
        memset(&sWaveBuf[i], 0, sizeof(sWaveBuf[i]));
        sWaveBuf[i].data_vaddr = sBlock[i];
        sWaveBuf[i].nsamples   = BLOCK_SAMPLES;   // sample frames, not bytes
        sWaveBuf[i].status     = NDSP_WBUF_DONE;  // free for the first fill
    }

    // Only now that there is a DSP thread to yield to.
    fix_thread_priority();

    sReady = 1;
    CtrLog("emerald3ds: audio ready (%d Hz, stereo PCM16)\n", (int)SAMPLE_RATE);
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

// One line, once, which tells which failure occurred. Without it, "no sound"
// and "sound, in a silent part of the game" look the same on a console.
//
// One underrun for each frame is the normal state. The mixer makes one buffer
// of samples for each frame, so the second free wave buffer in the refill loop
// always finds an empty ring. Thus both counters below count only a frame where
// nothing was queued. Only that case loses audio.
static void health_report(void)
{
    // In the same order as M4A_DBG_* in rp2350/m4a_mix.c. Each entry is what a
    // clear bit means, and the first clear bit is the answer. The text makes
    // the log a diagnosis, not a number to decode.
    static const char *const kChainFaults[] = {
        "m4aSoundInit never published gSoundInfo (SOUND_INFO_PTR is wrong)",
        "MPlayOpen never ran: the music player chain is empty",
        "MPlayExtender never ran: no CGB hook",
        "the BGM player was never opened",
        "no song has been started",
        "the started song has no tracks",
        "no track is running (nothing is playing right now)",
    };

    uint32_t ident = 0, bgmStatus = 0, zeroRet = 0, active = 0, flags = 0;
    uint32_t chType = 0, chStatus = 0, chEnv = 0, chFreq = 0, chNonZero = 0;
    uint32_t dsPeak = 0, psgPeak = 0, cryPeak = 0, clipped = 0, dsWrap = 0;
    uint8_t  masterVol = 0, maxChans = 0;
    int32_t  spvb = 0;
    const char *verdict;
    unsigned i;

    Rp2350AudioDebug(&ident, &spvb, &bgmStatus, &zeroRet);
    Rp2350MixerDebug(&masterVol, &maxChans, &active, &flags);
    Rp2350ChannelDebug(&chType, &chStatus, &chEnv, &chFreq, &chNonZero);
    Rp2350AudioPeaks(&dsPeak, &psgPeak, &cryPeak, &clipped);
    { extern volatile uint32_t gM4aDbgDsWrap; dsWrap = gM4aDbgDsWrap; }

    if (sPeak == 0) {
        // Name the first broken link. This runs only when all samples are zero,
        // so the chain is the explanation.
        verdict = "samples all zero, and every engine stage looks set";
        for (i = 0; i < sizeof(kChainFaults) / sizeof(kChainFaults[0]); i++) {
            if (!(flags & (1u << i))) {
                verdict = kChainFaults[i];
                break;
            }
        }
        if (masterVol == 0)
            verdict = "masterVolume is 0: every channel mixes to nothing";
        else if (maxChans == 0)
            verdict = "maxChans is 0: the mixer loop never runs";
        else if (active == 0)
            verdict = "no channel is live: nothing asked to be played";
        // The data above is for the full engine. Below is the live channel, the
        // last place where the silence can be.
        else if (chType & 0x30)
            verdict = "the live channel is compressed/reverse: check cry= "
                      "in the peaks line, not this one";
        else if ((chEnv & 0xFFFF) == 0)
            verdict = "channel envelope volume is 0: real samples multiplied "
                      "by nothing (volume chain)";
        else if (chNonZero == 0)
            verdict = "the wave data under currentPointer is silence: "
                      "bad sample pointer, or silent .bin assets";
        else
            verdict = "live channel has volume AND non-silent samples, "
                      "yet the mix is zero: the fault is inside MixChannel";
    }
    else if (sStalled > sFrames / 4)
        verdict = "the DSP is not draining buffers (NDSP thread starved?)";
    else if (sUnderruns > sFrames / 4)
        verdict = "the game is not keeping up with the DSP";
    else
        verdict = "audible samples are reaching the DSP";

    CtrLog("emerald3ds: audio after %lu frames - queued %lu, stalled %lu, "
           "underruns %lu, silent-mix %lu, dropped %lu, peak %lu, nonzero %lu\n",
           (unsigned long)sFrames, (unsigned long)sQueued,
           (unsigned long)sStalled, (unsigned long)sUnderruns,
           (unsigned long)sZeroMix, (unsigned long)sDropped,
           (unsigned long)sPeak, (unsigned long)sNonZero);

    // The engine's own view, which the host cannot find. A value of bgm 0 means
    // that no song plays, which fully explains a peak of 0. A value of
    // masterVol 0 or chans 0 means that the mixer cannot make any sound.
    CtrLog("emerald3ds: m4a ident=%08lX spvb=%ld bgm=%08lX zero-returns=%lu "
           "masterVol=%u maxChans=%u active=%lu chain=%02lX\n",
           (unsigned long)ident, (long)spvb, (unsigned long)bgmStatus,
           (unsigned long)zeroRet, (unsigned)masterVol, (unsigned)maxChans,
           (unsigned long)active, (unsigned long)flags);
    CtrLog("emerald3ds: chan type=%02lX status=%02lX env=%06lX freq=%lu "
           "sampleNonZero=%lu/64\n",
           (unsigned long)chType, (unsigned long)chStatus, (unsigned long)chEnv,
           (unsigned long)chFreq, (unsigned long)chNonZero);
    // Split by subsystem: DirectSound, the PSG synthesizer, and the compressed
    // and reverse path each report their own peak. Thus each part of the sound
    // work can be checked from the log.
    CtrLog("emerald3ds: mix peaks - directSound=%lu psg=%lu cry=%lu clipped=%lu\n",
           (unsigned long)dsPeak, (unsigned long)psgPeak,
           (unsigned long)cryPeak, (unsigned long)clipped);

    // The jumps= value is the number of jumps. The atFrameStart= value is how
    // many were on the first sample of a frame. Near jumps means that the click
    // is at the frame boundary. Near 0 means that it is in the audio.
    CtrLog("emerald3ds: discontinuities - jumps=%lu atFrameStart=%lu "
           "biggest=%lu at sample %lu of %d, dsWrap=%lu\n",
           (unsigned long)sJumpCount, (unsigned long)sJumpAtFrameStart,
           (unsigned long)sJumpMax, (unsigned long)sJumpMaxPos,
           SAMPLES_PER_FRAME, (unsigned long)dsWrap);
    CtrLog("emerald3ds: audio verdict - %s\n", verdict);
}

// Run the mixer for one game frame and fill the DSP queue. Rp2350PresentFrame()
// calls this on the main thread.
void CtrAudioFrame(void)
{
    int16_t mixed[SAMPLES_PER_FRAME * AUDIO_CHANNELS];
    int n, queuedThisFrame = 0;

    if (!sReady)
        return;

    sFrames++;

    // 1. Produce. Mix directly into the ring. Drop the frame if the ring is
    // full (the DSP is behind, so the game runs fast).
    n = Rp2350MixFrameStereo16(mixed, SAMPLES_PER_FRAME);

    // The STEREO A/B switch, applied here and not in the mixer. It tests if the
    // difference between the two sides sounds wrong. A downmix here answers
    // that without a change to how the sides are made.
    if (!Ctr3dsGetAudioDbg(CTR_AUDIO_DBG_STEREO)) {
        for (int i = 0; i < n; i++) {
            int16_t mono = (int16_t)(((int32_t)mixed[i * 2]
                                    + (int32_t)mixed[i * 2 + 1]) / 2);
            mixed[i * 2] = mono;
            mixed[i * 2 + 1] = mono;
        }
    }
    if (n <= 0)
        sZeroMix++;

    for (int i = 0; i < n; i++) {
        // Measure here, before anything else can be at fault, and over both
        // sides, so a hard-panned line counts. Widen first: -32768 negates to
        // itself in int16.
        for (int c = 0; c < AUDIO_CHANNELS; c++) {
            int mag = mixed[i * AUDIO_CHANNELS + c] < 0
                          ? -(int)mixed[i * AUDIO_CHANNELS + c]
                          : (int)mixed[i * AUDIO_CHANNELS + c];
            if (mag > 0) {
                sNonZero++;
                if ((uint32_t)mag > sPeak)
                    sPeak = (uint32_t)mag;
            }
        }

        // Measure the left channel only. Both channels have the same jump, and
        // one channel costs one subtract for each frame.
        {
            int32_t cur = mixed[i * AUDIO_CHANNELS];
            int32_t d = cur - sPrevSample;
            uint32_t mag = (uint32_t)(d < 0 ? -d : d);

            if (mag > JUMP_THRESHOLD) {
                sJumpCount++;
                if (i == 0)
                    sJumpAtFrameStart++;
                if (mag > sJumpMax) {
                    sJumpMax = mag;
                    sJumpMaxPos = (uint32_t)i;
                }
            }
            sPrevSample = cur;
        }

        if (ring_fill() >= RING_FRAMES) {
            sDropped++;
            break;
        }

        {
            int16_t *slot = ring_slot(sRingHead);

            for (int c = 0; c < AUDIO_CHANNELS; c++)
                slot[c] = mixed[i * AUDIO_CHANNELS + c];
        }
        sRingHead++;
    }

    // 2. Consume. Refill every finished wave buffer that the ring can cover.
    for (int i = 0; i < NUM_WAVEBUFS; i++) {
        if (sWaveBuf[i].status != NDSP_WBUF_FREE &&
            sWaveBuf[i].status != NDSP_WBUF_DONE)
            continue;

        // Do not count an underrun here. With one buffer for each frame, the
        // next free wave buffer always finds an empty ring. Only a frame that
        // queued nothing lost audio, and that is counted once, below.
        if (ring_fill() < BLOCK_SAMPLES)
            break;

        for (int s = 0; s < BLOCK_SAMPLES; s++) {
            // The mixer already gives PCM16 in the DSP's layout, so this is a
            // copy.
            const int16_t *slot = ring_slot(sRingTail);

            for (int c = 0; c < AUDIO_CHANNELS; c++)
                sBlock[i][s * AUDIO_CHANNELS + c] = slot[c];
            sRingTail++;
        }

        // The DSP reads this memory directly and does not see the ARM11 cache.
        DSP_FlushDataCache(sBlock[i], BLOCK_SAMPLES * AUDIO_CHANNELS * sizeof(int16_t));
        ndspChnWaveBufAdd(0, &sWaveBuf[i]);
        sQueued++;
        queuedThisFrame++;
    }

    // A frame that gave the DSP nothing, split by cause. `stalled` means every
    // buffer was still QUEUED or PLAYING, so the DSP did not return one: a
    // starved NDSP thread looks like this. `underruns` means a buffer was free
    // and there was nothing to put in it. The two need opposite fixes.
    if (queuedThisFrame == 0) {
        if (ring_fill() >= BLOCK_SAMPLES)
            sStalled++;
        else
            sUnderruns++;
    }

    if (sFrames == HEALTH_REPORT_FRAME)
        health_report();
}

uint32_t CtrAudioUnderruns(void) { return sUnderruns; }
