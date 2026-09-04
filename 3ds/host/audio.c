// m4a -> NDSP audio.
//
// rp2350/m4a_1.c is the C port of the GBA's MP2K mixer and is compiled into the
// game archive; it hands us interleaved stereo 16-bit samples through
// Rp2350MixFrameStereo16(), which is DirectSound and the four PSG channels
// already summed and panned. NDSP plays stereo PCM16 natively, so the samples
// reach the DSP exactly as the mixer produced them, with no conversion, no
// requantisation and no downmix.
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

// PCM16 all the way from the mixer. Originally chosen while chasing the silence
// (every working NDSP homebrew feeds PCM16, and an emulator's HLE DSP is not the
// real DSP); it now also carries real precision, because the PSG channels are
// generated at 16-bit and a console mixes them with DirectSound in the analog
// domain rather than on an 8-bit grid.
static int16_t    *sBlock[NUM_WAVEBUFS];   // linearAlloc'd, DSP-visible, stereo

static int16_t  sRing[RING_SAMPLES];
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

// The measurement that actually splits the problem, and the one the first
// version of this report was missing. Every counter above can read perfectly
// healthy while the samples flowing through are all zero, because
// Rp2350MixFrame returning 224 only means the sound ENGINE is initialised, not
// that anything is playing. A peak of 0 means the silence is upstream of this
// file entirely and no amount of DSP configuration will help.
static uint32_t sPeak;        // largest |sample| seen
static uint32_t sNonZero;     // how many samples were not silence

static uint32_t ring_fill(void) { return sRingHead - sRingTail; }

// Keep the DSP thread able to preempt the game loop.
//
// The 3DS scheduler is strict priority with no round-robin between different
// priorities: a lower-priority thread runs only while every higher-priority one
// is blocked. libctru puts NDSP's work on its own thread at 0x18, and
// AffinityMask 1 with SystemModeExt Legacy confine both it and us to core 0, so
// a main thread that outranked it would starve the DSP whenever the software
// rasteriser had work to do -- and it always does. A .3dsx under hbmenu gets
// main priority 0x30, i.e. below NDSP, which is the arrangement NDSP is
// actually exercised in.
//
// 3ds/emerald3ds.rsf asks for main thread priority 0x10, which WOULD be that
// inversion. It measurably is not what the console hands out: on hardware this
// function found the main thread already at or below 0x18 and changed nothing,
// which is why the log below reports the priority unconditionally rather than
// only when it acts. That reading is what ruled the theory out; do not let the
// silence of an untaken branch look like a fix that worked.
//
// The guard stays because it costs one comparison and the exheader still asks
// for the wrong thing.
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

    // Unconditional. An absent line is not evidence of anything, and the first
    // version of this only logged when it acted, which made "the inversion is
    // not happening" indistinguishable from "the code never ran".
    CtrLog("emerald3ds: main thread priority 0x%02lX (NDSP 0x%02X)%s\n",
           (unsigned long)after, NDSP_THREAD_PRIO,
           after != before ? " [lowered]" : "");
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

    // STEREO, not the MONO this used to ask for. A mono channel played into a
    // stereo output with an even front-left/front-right mix reaches both
    // speakers and is the configuration NDSP is actually exercised in
    // everywhere else; NDSP_OUTPUT_MONO changes the DSP's whole downmix path
    // for no benefit here, and an emulator's HLE DSP is free to ignore the
    // distinction where real firmware does not.
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);

    // Both of these are already the libctru defaults (ndspInitMaster sets the
    // master volume to 1.0, ndspChnReset sets mix[0] and mix[1] to 1.0). Set
    // them anyway: "silent" is the symptom being chased, and a default is only
    // a default until some future libctru changes it.
    ndspSetMasterVol(1.0f);
    {
        float mix[12] = { 0 };
        mix[0] = 1.0f;   // front left
        mix[1] = 1.0f;   // front right
        ndspChnSetMix(0, mix);
    }

    // LINEAR, not NONE. The old comment here claimed "no resampling: rate
    // matches", which is not true of anything: the DSP runs at 32728 Hz and the
    // mixer feeds it 13401, so every sample is resampled up by about 2.44x no
    // matter what. NDSP_INTERP_NONE makes that a zero-order hold, whose imaging
    // is heard as grit on top of the music. Linear interpolation costs the DSP
    // nothing and removes most of it.
    //
    // Not POLYPHASE: libctru degrades it to NONE whenever the rate ratio is
    // below 1.0 (ndspiUpdateChn), which ours always is, so asking for it would
    // quietly land back on the zero-order hold.
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, SAMPLE_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);

    for (int i = 0; i < NUM_WAVEBUFS; i++) {
        sBlock[i] = linearAlloc(BLOCK_SAMPLES * 2 * sizeof(int16_t));
        if (sBlock[i] == NULL) {
            CtrLog("emerald3ds: audio disabled - linearAlloc(%d) failed\n",
                   (int)(BLOCK_SAMPLES * 2 * sizeof(int16_t)));
            return;
        }
        memset(sBlock[i], 0, BLOCK_SAMPLES * 2 * sizeof(int16_t));
        memset(&sWaveBuf[i], 0, sizeof(sWaveBuf[i]));
        sWaveBuf[i].data_vaddr = sBlock[i];
        sWaveBuf[i].nsamples   = BLOCK_SAMPLES;   // sample FRAMES, not bytes
        sWaveBuf[i].status     = NDSP_WBUF_DONE;  // free for the first fill
    }

    // Only now that there is a DSP thread worth yielding to.
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

// One line, once, naming which of the ways this can fail actually happened.
// Without it "no sound" is indistinguishable from "sound, but you were in a
// silent room of the game", and every cause below looks identical on a console.
//
// The first version of this report got its verdict wrong, and the wrongness is
// worth recording. It treated `underruns` as a fault, but one underrun per
// frame is the DESIGNED steady state: the mixer produces exactly one buffer's
// worth of samples per frame, so the second free wave buffer in the refill loop
// always finds an empty ring and always counts an underrun. It read
// "underruns == frames" and cried starvation about a pipeline that was working
// perfectly. Both counters below now only count a frame where NOTHING was
// queued, which is the only case that can actually cost you audio.
static void health_report(void)
{
    // Mirrors the M4A_DBG_* order in rp2350/m4a_1.c: each entry is what a CLEAR
    // bit means, and the first clear one is the answer. Kept as text here so the
    // log reads as a diagnosis rather than a number to decode by hand.
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
    uint32_t dsPeak = 0, psgPeak = 0, cryPeak = 0;
    uint8_t  masterVol = 0, maxChans = 0;
    int32_t  spvb = 0;
    const char *verdict;
    unsigned i;

    Rp2350AudioDebug(&ident, &spvb, &bgmStatus, &zeroRet);
    Rp2350MixerDebug(&masterVol, &maxChans, &active, &flags);
    Rp2350ChannelDebug(&chType, &chStatus, &chEnv, &chFreq, &chNonZero);
    Rp2350AudioPeaks(&dsPeak, &psgPeak, &cryPeak);

    if (sPeak == 0) {
        // Name the first broken link rather than just reporting silence. Only
        // reached when the samples really are all zero, so the chain walk is
        // the explanation and not a coincidence.
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
        // Everything above is engine-wide. Below is the live channel itself,
        // which is the only place left for the silence to be hiding.
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

    // The engine's own view, which the host side cannot infer. bgm 0 means no
    // song is playing, which is a complete and innocent explanation for a peak
    // of 0 and has nothing to do with the 3DS at all; masterVol 0 or chans 0
    // mean the mixer is structurally incapable of producing anything.
    CtrLog("emerald3ds: m4a ident=%08lX spvb=%ld bgm=%08lX zero-returns=%lu "
           "masterVol=%u maxChans=%u active=%lu chain=%02lX\n",
           (unsigned long)ident, (long)spvb, (unsigned long)bgmStatus,
           (unsigned long)zeroRet, (unsigned)masterVol, (unsigned)maxChans,
           (unsigned long)active, (unsigned long)flags);
    CtrLog("emerald3ds: chan type=%02lX status=%02lX env=%06lX freq=%lu "
           "sampleNonZero=%lu/64\n",
           (unsigned long)chType, (unsigned long)chStatus, (unsigned long)chEnv,
           (unsigned long)chFreq, (unsigned long)chNonZero);
    // Split by subsystem, so each stage of the sound work can be signed off
    // from the log rather than by ear: DirectSound, the PSG synthesiser, and
    // the compressed/reverse path each report their own peak.
    CtrLog("emerald3ds: mix peaks - directSound=%lu psg=%lu cry=%lu\n",
           (unsigned long)dsPeak, (unsigned long)psgPeak,
           (unsigned long)cryPeak);
    CtrLog("emerald3ds: audio verdict - %s\n", verdict);
}

// Run the mixer for one game frame and top up the DSP queue. Called from
// Rp2350PresentFrame() on the main thread.
void CtrAudioFrame(void)
{
    int16_t mixed[SAMPLES_PER_FRAME * 2];   // interleaved L,R
    int n, queuedThisFrame = 0;

    if (!sReady)
        return;

    sFrames++;

    // 1. Produce. Mix straight into the ring; drop the frame if the ring is
    //    full (the DSP is behind, which means we are running fast).
    n = Rp2350MixFrameStereo16(mixed, SAMPLES_PER_FRAME);
    if (n <= 0)
        sZeroMix++;

    for (int i = 0; i < n; i++) {
        // Measured on the way past, before anything else can be blamed, and
        // over both sides so a hard-panned line still registers. Widened first:
        // -32768 negates to itself in int16.
        for (int c = 0; c < 2; c++) {
            int mag = mixed[i * 2 + c] < 0 ? -(int)mixed[i * 2 + c]
                                           : (int)mixed[i * 2 + c];
            if (mag > 0) {
                sNonZero++;
                if ((uint32_t)mag > sPeak)
                    sPeak = (uint32_t)mag;
            }
        }

        if (ring_fill() >= RING_SAMPLES) {
            sDropped++;
            break;
        }
        sRing[(sRingHead % RING_SAMPLES) * 2] = mixed[i * 2];
        sRing[(sRingHead % RING_SAMPLES) * 2 + 1] = mixed[i * 2 + 1];
        sRingHead++;
    }

    // 2. Consume. Refill every finished wave buffer that the ring can cover.
    for (int i = 0; i < NUM_WAVEBUFS; i++) {
        if (sWaveBuf[i].status != NDSP_WBUF_FREE &&
            sWaveBuf[i].status != NDSP_WBUF_DONE)
            continue;

        // NOT counted as an underrun here. Producing one buffer per frame
        // means the next free wave buffer legitimately finds an empty ring
        // every single frame; only a frame that queued NOTHING has actually
        // lost audio, and that is counted once, below.
        if (ring_fill() < BLOCK_SAMPLES)
            break;

        for (int s = 0; s < BLOCK_SAMPLES; s++) {
            // Already interleaved stereo PCM16 from the mixer, so this is a
            // copy rather than a conversion.
            sBlock[i][s * 2] = sRing[(sRingTail % RING_SAMPLES) * 2];
            sBlock[i][s * 2 + 1] = sRing[(sRingTail % RING_SAMPLES) * 2 + 1];
            sRingTail++;
        }

        // The DSP reads this memory directly and does not see the ARM11 cache.
        DSP_FlushDataCache(sBlock[i], BLOCK_SAMPLES * 2 * sizeof(int16_t));
        ndspChnWaveBufAdd(0, &sWaveBuf[i]);
        sQueued++;
        queuedThisFrame++;
    }

    // A frame that handed the DSP nothing, split by whose fault it was. Held
    // apart on purpose: `stalled` means every buffer was still QUEUED or
    // PLAYING, so the DSP side has not returned one, which is what a starved
    // NDSP thread looks like from here. `underruns` means a buffer WAS free and
    // we had nothing to put in it. They call for opposite fixes.
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
