// The m4a port's own seam: everything around the engine that the reference does
// not have.
//
// Three things live here and nowhere else:
//
//  - The frame entry points the host audio drivers call (Rp2350MixFrame*).
//    A GBA streams pcmBuffer to the DAC FIFOs by DMA; there is no DMA here, so
//    the buffer has to be drained explicitly once a frame.
//  - The PSG sum. A GBA mixes its four CGB voices with DirectSound in HARDWARE,
//    so pcmBuffer holds only the DirectSound half and the other half has to be
//    synthesised (rp2350/psg.c) and added here. Doing it at this seam is what
//    keeps MixChannel and MixAllChannels byte-identical to the reference.
//  - Telemetry and the A/B switches, which exist because a fault in this mixer
//    is about how it SOUNDS, and nothing else in the system can measure that.
//
// None of it depends on which engine is underneath, which is the point: see
// rp2350/m4a_port.h.

#include "global.h"
#include "gba/m4a_internal.h"
#include "psg.h"
#include "m4a_port.h"

//
// Kept small deliberately. These buffers are stereo and on the stack, and the
// mono wrappers nest one inside another, so a large chunk costs four times what
// it looks like. The RP2350 port's SDK stack defaults to 2 KB and calls the
// mono path from the frame hook, which a 256-frame chunk would come close to
// exhausting. At 64 frames the deepest path uses 512 bytes.
//
// The cost of chunking is re-reading the PSG registers once per chunk, which is
// idempotent within a frame: the only side effect is consuming the NRx4 trigger
// bit, and that happens on the first chunk exactly as it should.
#define PSG_CHUNK 64
// Per-subsystem peaks, so "is there sound" can be answered for each half of the
// mixer separately instead of for the sum. A silent PSG with a healthy
// DirectSound reads very differently from both being silent.
volatile u32 gM4aDbgDsPeak;    // largest |sample| out of the DirectSound mix
volatile u32 gM4aDbgPsgPeak;   // ... out of the PSG synthesiser
volatile u32 gM4aDbgCryPeak;   // ... out of the compressed/reverse path
volatile u32 gM4aDbgClipped;   // samples the final clamp had to catch

// Audio A/B switches, driven from the EXTRA tab.
//
// The two halves of this mixer cannot be told apart by ear while both are
// playing, and no counter in the log can measure "sounds wrong". These let a
// listener silence one half at a time on the console itself, which is the only
// instrument available for a fault that is about quality rather than plumbing.
//
// Default on, so a normal boot is the real mixer.
volatile u8 gM4aPsgOn = 1;
volatile u8 gM4aReverbOn = 1;
volatile u8 gM4aDsOn = 1;

void Rp2350SetAudioDebug(int psgOn, int reverbOn, int dsOn)
{
    gM4aPsgOn = (u8)(psgOn ? 1 : 0);
    gM4aReverbOn = (u8)(reverbOn ? 1 : 0);
    gM4aDsOn = (u8)(dsOn ? 1 : 0);
}
// ----------------------------------------------------------------------------
// Rp2350MixFrame: drain this frame's PCM mix into a mono int8 buffer for the
// I2S ring (strong override of the weak silence stub in rp2350/hw/audio.c). The
// engine has already filled gSoundInfo.pcmBuffer (right half then left half)
// during VBlankIntr -> m4aSoundMain this frame.
// ----------------------------------------------------------------------------
extern struct SoundInfo gSoundInfo;
extern struct MusicPlayerInfo gMPlayInfo_BGM;

// Debug snapshot for the audio telemetry line (game_main.c).
volatile u32 gM4aDbgIdent;
volatile s32 gM4aDbgSpvb;
volatile u32 gM4aDbgBgmStatus;
volatile u32 gM4aDbgZeroRet;   // count of frames Rp2350MixFrame returned 0


void Rp2350AudioPeaks(u32 *dsPeak, u32 *psgPeak, u32 *cryPeak, u32 *clipped)
{
    if (dsPeak)  *dsPeak = gM4aDbgDsPeak;
    if (psgPeak) *psgPeak = gM4aDbgPsgPeak;
    if (cryPeak) *cryPeak = gM4aDbgCryPeak;
    if (clipped) *clipped = gM4aDbgClipped;
}

void Rp2350AudioDebug(u32 *ident, s32 *spvb, u32 *bgmStatus, u32 *zeroRet)
{
    if (ident)     *ident = gM4aDbgIdent;
    if (spvb)      *spvb = gM4aDbgSpvb;
    if (bgmStatus) *bgmStatus = gM4aDbgBgmStatus;
    if (zeroRet)   *zeroRet = gM4aDbgZeroRet;
}

// A second snapshot, for the 3DS port's audio health report (3ds/host/audio.c).
// Kept separate from Rp2350AudioDebug rather than folded into it because that
// signature is shared with rp2350/hw/game_main.c.
//
// This exists because every host-side counter can read perfectly healthy while
// the samples flowing through are all zero: Rp2350MixFrame returning a full 224
// only means the engine is INITIALISED. When the mix is silent, the question is
// which link of the chain below never got made, and from outside m4a they are
// indistinguishable.
//
// The flags walk that chain in order, so the LOWEST clear bit is the failure:
// the sound info has to be published, the player chain has to be opened, a song
// has to be started, and it has to have tracks before a single channel can ever
// turn on.
#define M4A_DBG_SOUNDINFO_PUBLISHED  (1u << 0)  // SOUND_INFO_PTR == &gSoundInfo
#define M4A_DBG_MPLAY_CHAIN          (1u << 1)  // MPlayOpen ran: MPlayMainHead set
#define M4A_DBG_CGB_HOOK             (1u << 2)  // MPlayExtender ran: CgbSound set
#define M4A_DBG_BGM_OPEN             (1u << 3)  // BGM player ident == ID_NUMBER
#define M4A_DBG_BGM_SONG             (1u << 4)  // a song header is loaded
#define M4A_DBG_BGM_TRACKS           (1u << 5)  // that song has tracks
#define M4A_DBG_BGM_PLAYING          (1u << 6)  // status says a track is running

void Rp2350MixerDebug(u8 *masterVolume, u8 *maxChans, u32 *activeChans,
                      u32 *engineFlags)
{
    struct SoundInfo *si = SOUND_INFO_PTR;
    u32 active = 0;
    u32 flags = 0;
    s32 c;

    // Zeroed GBA memory before m4aSoundInit, so this really can be NULL.
    if (si != NULL)
    {
        if (si == &gSoundInfo)      flags |= M4A_DBG_SOUNDINFO_PUBLISHED;
        if (si->MPlayMainHead)      flags |= M4A_DBG_MPLAY_CHAIN;
        if (si->CgbSound)           flags |= M4A_DBG_CGB_HOOK;

        for (c = 0; c < si->maxChans && c < MAX_DIRECTSOUND_CHANNELS; c++)
            if (si->chans[c].statusFlags & SOUND_CHANNEL_SF_ON)
                active++;
    }

    if (gMPlayInfo_BGM.ident == ID_NUMBER)  flags |= M4A_DBG_BGM_OPEN;
    if (gMPlayInfo_BGM.songHeader != NULL)  flags |= M4A_DBG_BGM_SONG;
    if (gMPlayInfo_BGM.trackCount != 0)     flags |= M4A_DBG_BGM_TRACKS;
    if (gMPlayInfo_BGM.status & MUSICPLAYER_STATUS_TRACK)
        flags |= M4A_DBG_BGM_PLAYING;

    if (masterVolume) *masterVolume = (si != NULL) ? si->masterVolume : 0;
    if (maxChans)     *maxChans     = (si != NULL) ? si->maxChans : 0;
    if (activeChans)  *activeChans  = active;
    if (engineFlags)  *engineFlags  = flags;
}

// The last thing the host side cannot see: what the live channels actually
// contain. Reported for the FIRST channel with SOUND_CHANNEL_SF_ON, which is
// enough, because a fault that silences one silences all of them.
//
// With the engine measuring healthy (chain=7F, active=4) and every output
// sample still zero, exactly three things can be responsible, and these fields
// separate them:
//
//   envVol == 0        the envelope or the volume chain collapsed, so real
//                      samples are being multiplied by nothing
//   sampleNonZero == 0 the wave data under currentPointer really is silence,
//                      or the pointer is not where the sample is
//   type & 0x30        the channel is compressed or reverse-playback, so it
//                      runs through MixChannelSpecial rather than the plain
//                      sample loops
//
// Cheap enough to run every mix: it walks at most 5 channels and 64 bytes.
void Rp2350ChannelDebug(u32 *type, u32 *statusFlags, u32 *envVol,
                        u32 *frequency, u32 *sampleNonZero)
{
    struct SoundInfo *si = SOUND_INFO_PTR;
    struct SoundChannel *chan = NULL;
    u32 nonZero = 0;
    s32 c;

    if (si != NULL)
    {
        for (c = 0; c < si->maxChans && c < MAX_DIRECTSOUND_CHANNELS; c++)
        {
            if (si->chans[c].statusFlags & SOUND_CHANNEL_SF_ON)
            {
                chan = &si->chans[c];
                break;
            }
        }
    }

    if (chan == NULL)
    {
        if (type)          *type = 0;
        if (statusFlags)   *statusFlags = 0;
        if (envVol)        *envVol = 0;
        if (frequency)     *frequency = 0;
        if (sampleNonZero) *sampleNonZero = 0;
        return;
    }

    // Bounded by the wave's own extent, so a stale pointer cannot walk off the
    // end of the sample and read whatever data follows it.
    if (chan->wav != NULL && chan->currentPointer != NULL)
    {
        const s8 *start = chan->wav->data;
        const s8 *end   = start + chan->wav->size;
        const s8 *p     = chan->currentPointer;

        if (p >= start && p < end)
        {
            s32 i;
            for (i = 0; i < 64 && (p + i) < end; i++)
                if (p[i] != 0)
                    nonZero++;
        }
    }

    // Packed rather than three parameters: it is read as hex in one glance,
    // and all three are only ever interesting together.
    if (type)        *type = chan->type;
    if (statusFlags) *statusFlags = chan->statusFlags;
    if (envVol)      *envVol = ((u32)chan->envelopeVolume << 16)
                             | ((u32)chan->envelopeVolumeRight << 8)
                             |  (u32)chan->envelopeVolumeLeft;
    if (frequency)   *frequency = chan->frequency;
    if (sampleNonZero) *sampleNonZero = nonZero;
}

// Shared prologue: publish the debug snapshot, refuse to run before the engine
// is up, and clamp to the frame the engine actually rendered. Returns 0 when
// there is nothing to mix.
static int mix_begin(int n)
{
    s32 avail;

    gM4aDbgIdent = gSoundInfo.ident;
    gM4aDbgSpvb = gSoundInfo.pcmSamplesPerVBlank;
    gM4aDbgBgmStatus = gMPlayInfo_BGM.status;

    avail = gSoundInfo.pcmSamplesPerVBlank;
    if (avail <= 0 || gSoundInfo.ident != ID_NUMBER)
    {
        gM4aDbgZeroRet++;
        return 0;
    }

    return (n > avail) ? avail : n;
}

// Render frames [base, base+cnt) as interleaved left/right pairs. `out` holds
// 2*cnt samples.
//
// The GBA sums its two DirectSound channels with the four PSG generators in
// hardware. pcmBuffer holds only the DirectSound half, so the PSG half is added
// here rather than inside MixAllChannels: that keeps MixChannel and
// MixAllChannels byte-identical to the reference engine, and confines the
// port's own additions to the port's own seam.
//
// Both halves are genuinely stereo. m4a renders DirectSound into two separate
// buffers and pans every note across them (ChnVolSetAsm), and the PSG pans each
// of its four channels through NR51, so collapsing to mono here would throw
// away panning the music was written with.
static void mix_stereo_range(s16 *out, int base, int cnt)
{
    const s8 *bufR = gSoundInfo.pcmBuffer;
    const s8 *bufL = gSoundInfo.pcmBuffer + PCM_DMA_BUF_SIZE;
    int done = 0;

    while (done < cnt)
    {
        s16 psg[PSG_CHUNK * 2];
        int part = cnt - done;
        int i;

        if (part > PSG_CHUNK)
            part = PSG_CHUNK;

        PsgRender(psg, part, gSoundInfo.pcmFreq);

        // Silenced AFTER rendering, never by skipping the render. The PSG
        // carries its own phase, envelope and length state, so skipping would
        // freeze all three and make the mute change the timing of what comes
        // back when it is switched on again.
        if (!gM4aPsgOn)
        {
            for (i = 0; i < part * 2; i++)
                psg[i] = 0;
        }

        for (i = 0; i < part; i++)
        {
            // Read regardless and discarded when muted, never skipped: the
            // point of the mute is to remove this half from the OUTPUT, and
            // anything that also changed how the buffer is walked would be
            // testing two things at once.
            s32 dsL = gM4aDsOn ? bufL[base + done + i] : 0;
            s32 dsR = gM4aDsOn ? bufR[base + done + i] : 0;
            s32 l, r;
            u32 mag;

            // Full scale, NOT halved.
            //
            // On paper this can overflow: dsL << 8 reaches 32512 of the 32767
            // available and the PSG can add another 30720 on top, so the worst
            // case is 63232. That worst case does not occur. A 600-frame log
            // from the Birch intro measured directSound=68 of 127 and psg=5138
            // of 30720, for a final peak of 8524 -- a quarter of full scale --
            // with clipped=0.
            //
            // Halving it "for headroom" therefore bought nothing and cost 6 dB
            // on a console whose speakers are quiet to begin with. The clamp
            // and the counter below stay: they are what turned that from an
            // argument into a measurement, and they are what will catch it if
            // some song really does drive both halves at once.
            l = ((s32)dsL << 8) + psg[i * 2];
            r = ((s32)dsR << 8) + psg[i * 2 + 1];

            mag = (u32)(dsL < 0 ? -dsL : dsL);
            if (mag > gM4aDbgDsPeak)
                gM4aDbgDsPeak = mag;
            mag = (u32)(dsR < 0 ? -dsR : dsR);
            if (mag > gM4aDbgDsPeak)
                gM4aDbgDsPeak = mag;

            mag = (u32)(psg[i * 2] < 0 ? -(s32)psg[i * 2] : (s32)psg[i * 2]);
            if (mag > gM4aDbgPsgPeak)
                gM4aDbgPsgPeak = mag;

            // The measurement that decides whether this mix needs scaling
            // down. A non-zero clipped= in the log is the evidence that it
            // does; a zero one is why it currently does not.
            if (l > 32767)       { l = 32767;  gM4aDbgClipped++; }
            else if (l < -32768) { l = -32768; gM4aDbgClipped++; }
            if (r > 32767)       { r = 32767;  gM4aDbgClipped++; }
            else if (r < -32768) { r = -32768; gM4aDbgClipped++; }

            out[(done + i) * 2] = (s16)l;
            out[(done + i) * 2 + 1] = (s16)r;
        }

        done += part;
    }
}

// Interleaved stereo PCM16. The preferred entry point: it is the only one that
// preserves what the music was mixed with.
int Rp2350MixFrameStereo16(s16 *out, int n)
{
    n = mix_begin(n);
    if (n <= 0)
        return 0;

    // The window SoundMain just rendered into, not the start of the buffer.
    mix_stereo_range(out, Rp2350MixWindowOffset(), n);
    return n;
}

// Mono PCM16, for outputs that have nowhere to put a second channel.
int Rp2350MixFrame16(s16 *out, int n)
{
    int done;

    n = mix_begin(n);
    if (n <= 0)
        return 0;

    for (done = 0; done < n; )
    {
        s16 st[PSG_CHUNK * 2];
        int cnt = n - done;
        int i;

        if (cnt > PSG_CHUNK)
            cnt = PSG_CHUNK;

        mix_stereo_range(st, Rp2350MixWindowOffset() + done, cnt);

        for (i = 0; i < cnt; i++)
            out[done + i] = (s16)(((s32)st[i * 2] + (s32)st[i * 2 + 1]) >> 1);

        done += cnt;
    }

    return n;
}

// The original 8-bit mono seam, kept because rp2350/hw/audio.c's I2S ring is
// built on this signature. Necessarily throws away both the second channel and
// the extra precision.
int Rp2350MixFrame(s8 *out, int n)
{
    int done;

    n = mix_begin(n);
    if (n <= 0)
        return 0;

    for (done = 0; done < n; )
    {
        s16 st[PSG_CHUNK * 2];
        int cnt = n - done;
        int i;

        if (cnt > PSG_CHUNK)
            cnt = PSG_CHUNK;

        mix_stereo_range(st, Rp2350MixWindowOffset() + done, cnt);

        for (i = 0; i < cnt; i++)
        {
            s32 v = ((s32)st[i * 2] + (s32)st[i * 2 + 1]) >> 1;

            out[done + i] = (s8)(v >> 8);
        }

        done += cnt;
    }

    return n;
}
