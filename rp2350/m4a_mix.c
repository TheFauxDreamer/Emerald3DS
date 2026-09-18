// The own interface of the m4a port: all the code around the engine that the
// original does not have.
//
// It holds three things:
// - The frame entry points that the host audio drivers call (Rp2350MixFrame*).
//   A GBA sends pcmBuffer to the DAC FIFOs by DMA. There is no DMA here, so the
//   code must empty the buffer once each frame.
// - The PSG sum. A GBA mixes its four CGB voices with DirectSound in hardware.
//   Thus pcmBuffer holds only the DirectSound half, and rp2350/psg.c makes the
//   other half, which this file adds. Because the sum is here, MixChannel and
//   MixAllChannels stay byte-identical to the original.
// - Telemetry and the A/B switches. A fault in this mixer is about how it
//   sounds, and nothing else in the system can measure that.
//
// None of it depends on the engine below it, on purpose. See rp2350/m4a_port.h.

#include "global.h"
#include "gba/m4a_internal.h"
#include "psg.h"
#include "m4a_port.h"

// The PSG render chunk, in sample frames. It is small on purpose. These buffers
// are stereo and on the stack, and the mono wrappers nest one in another. Thus
// a large chunk costs four times its size.
//
// The SDK stack of the RP2350 port is 2 KB by default, and the frame hook calls
// the mono path. A 256-frame chunk would use almost all of that stack. At 64
// frames, the deepest path uses 512 bytes.
//
// The cost of chunks is that the PSG registers are read again for each chunk.
// In one frame, that gives the same result. The only side effect is that the
// NRx4 trigger bit is used, and that occurs on the first chunk, as it must.
#define PSG_CHUNK 64
// The peak of each subsystem, so each half of the mixer can show if it has
// sound. A silent PSG with a good DirectSound is a different problem from two
// silent halves.
volatile u32 gM4aDbgDsPeak;    // largest |sample| of the DirectSound mix
volatile u32 gM4aDbgPsgPeak;   // the same, of the PSG synthesizer
volatile u32 gM4aDbgCryPeak;   // the same, of SpecialSample
volatile u32 gM4aDbgClipped;   // samples the final clamp had to catch
volatile u32 gM4aDbgDsWrap;   // s8 accumulator overflows in MixChannel

// Audio A/B switches, which the EXTRA tab sets.
//
// With both halves of this mixer on, the ear cannot tell them apart, and no
// counter in the log can measure "sounds wrong". These switches let a listener
// mute one half at a time on the console. That is the only instrument for a
// fault in quality, not in the connections.
//
// They are on by default, so a usual boot uses the real mixer.
volatile u8 gM4aPsgOn = 1;
volatile u8 gM4aReverbOn = 1;
volatile u8 gM4aDsOn = 1;

void Rp2350SetAudioDebug(int psgOn, int reverbOn, int dsOn)
{
    gM4aPsgOn = (u8)(psgOn ? 1 : 0);
    gM4aReverbOn = (u8)(reverbOn ? 1 : 0);
    gM4aDsOn = (u8)(dsOn ? 1 : 0);
}
// Which window of pcmBuffer the engine rendered into on this frame.
//
// This is a copy of the logic in SoundMain in src/m4a_1.s, because the two must
// agree exactly, and only one of them is replaceable:
//
//     ldrb r4, [r0, o_SoundInfo_pcmDmaCounter]
//     subs r7, r4, 1
//     bls  SoundMain_5                  @ counter <= 1 -> window 0
//     ldrb r1, [r0, o_SoundInfo_pcmDmaPeriod]
//     subs r1, r7                       @ window = period - (counter - 1)
//
// The m4aSoundVSync function counts the counter down from period to 1, and then
// loads it again. Thus the window goes 1, 2, and so on to period-1, then 0, and
// then starts again. Because this uses that counter, and not its own counter,
// the original assembly can go below this file with no change.
s32 Rp2350MixWindowOffset(void)
{
    struct SoundInfo *si = SOUND_INFO_PTR;
    s32 counter = si->pcmDmaCounter;
    s32 period = si->pcmDmaPeriod;
    s32 window;

    if (period < 1)
        return 0;

    window = (counter <= 1) ? 0 : (period - counter + 1);

    // The engine owns the counter byte. A value outside [1, period] means that
    // it is not initialized yet, and the original also uses window 0 in that
    // case.
    if (window < 0 || window >= period)
        window = 0;

    return window * si->pcmSamplesPerVBlank;
}

extern struct SoundInfo gSoundInfo;
extern struct MusicPlayerInfo gMPlayInfo_BGM;

// Debug snapshot for the audio telemetry line (game_main.c).
volatile u32 gM4aDbgIdent;
volatile s32 gM4aDbgSpvb;
volatile u32 gM4aDbgBgmStatus;
volatile u32 gM4aDbgZeroRet;   // frames where Rp2350MixFrame returned 0


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

// A second snapshot, for the audio health report of the 3DS port
// (3ds/host/audio.c). It is separate from Rp2350AudioDebug, because
// rp2350/hw/game_main.c shares that signature.
//
// It exists because all host-side counters can look correct while all samples
// are zero. When Rp2350MixFrame returns a full 224, that only means that the
// engine is initialized. When the mix is silent, the question is which link of
// the chain below is missing. From outside m4a, they all look the same.
//
// The flags follow that chain in order, so the lowest clear bit is the failure.
// The sound info must be published, the player chain must be open, a song must
// start, and the song must have tracks. Only then can a channel turn on.
#define M4A_DBG_SOUNDINFO_PUBLISHED  (1u << 0)  // SOUND_INFO_PTR == &gSoundInfo
#define M4A_DBG_MPLAY_CHAIN          (1u << 1)  // MPlayOpen set MPlayMainHead
#define M4A_DBG_CGB_HOOK             (1u << 2)  // MPlayExtender set CgbSound
#define M4A_DBG_BGM_OPEN             (1u << 3)  // BGM player ident == ID_NUMBER
#define M4A_DBG_BGM_SONG             (1u << 4)  // a song header is loaded
#define M4A_DBG_BGM_TRACKS           (1u << 5)  // that song has tracks
#define M4A_DBG_BGM_PLAYING          (1u << 6)  // a track is running

void Rp2350MixerDebug(u8 *masterVolume, u8 *maxChans, u32 *activeChans,
                      u32 *engineFlags)
{
    struct SoundInfo *si = SOUND_INFO_PTR;
    u32 active = 0;
    u32 flags = 0;
    s32 c;

    // GBA memory is zero before m4aSoundInit, so this can really be NULL.
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

// The last thing that the host side cannot see: what the live channels contain.
// This reports the first channel with SOUND_CHANNEL_SF_ON. That is sufficient,
// because a fault that silences one channel silences all of them.
//
// When the engine looks correct (chain=7F, active=4) but each output sample is
// zero, only three causes are possible. These fields show which one:
//
//   envVol == 0        the envelope or the volume chain failed, so real
//                      samples are multiplied by zero
//   sampleNonZero == 0 the wave data at currentPointer is silence, or
//                      the pointer is not where the sample is
//   type & 0x30        the channel is compressed or plays in reverse, so
//                      it goes through MixChannelSpecial, not the plain
//                      sample loops
//
// The cost is low enough for each mix: it walks 5 channels and 64 bytes at
// most.
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

    // The extent of the wave is the bound. Thus a stale pointer cannot walk
    // past the end of the sample and read the data after it.
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

    // Packed, not three parameters: a single hex value shows all three, and
    // they are only important together.
    if (type)        *type = chan->type;
    if (statusFlags) *statusFlags = chan->statusFlags;
    if (envVol)      *envVol = ((u32)chan->envelopeVolume << 16)
                             | ((u32)chan->envelopeVolumeRight << 8)
                             |  (u32)chan->envelopeVolumeLeft;
    if (frequency)   *frequency = chan->frequency;
    if (sampleNonZero) *sampleNonZero = nonZero;
}

// Shared prologue: publish the debug snapshot, refuse to run before the engine
// is ready, and clamp to the frame that the engine rendered. Returns 0 when
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

// Render the frames [base, base+cnt) as interleaved left and right pairs. The
// `out` buffer holds 2*cnt samples.
//
// The GBA adds its two DirectSound channels to the four PSG generators in
// hardware. The pcmBuffer holds only the DirectSound half, so this function
// adds the PSG half, not MixAllChannels. Thus MixChannel and MixAllChannels
// stay byte-identical to the original engine, and the additions of the port
// stay in the interface of the port.
//
// Both halves are really stereo. The m4a engine renders DirectSound into two
// separate buffers, and pans each note across them (ChnVolSetAsm). The PSG pans
// each of its four channels through NR51. Thus a mono mix here would lose the
// panning that the music uses.
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

        // Muted after the render, never by a skipped render. The PSG keeps its
        // own phase, envelope and length state. A skip would freeze all three,
        // and the mute would change the timing of the sound when it comes back
        // on.
        if (!gM4aPsgOn)
        {
            for (i = 0; i < part * 2; i++)
                psg[i] = 0;
        }

        for (i = 0; i < part; i++)
        {
            // Read in all cases, and discarded when muted, never skipped. The
            // mute must only remove this half from the output. A change to how
            // the code walks the buffer would test two things at once.
            s32 dsL = gM4aDsOn ? bufL[base + done + i] : 0;
            s32 dsR = gM4aDsOn ? bufR[base + done + i] : 0;
            s32 l, r;
            u32 mag;

            // Full scale, not halved.
            //
            // In theory, this can overflow. The value dsL << 8 goes up to 32512
            // of the 32767 available, and the PSG can add 30720 more. Thus the
            // worst case is 63232. In practice, the peaks are much lower: a log
            // of the Birch intro gave a peak of 8524, a quarter of full scale,
            // with clipped=0.
            //
            // A half gain for headroom would cost 6 dB on a console with quiet
            // speakers. The clamp and the counter below stay, because they
            // catch a song that drives both halves at full level.
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

            // This measurement decides if this mix needs a lower gain. A
            // clipped= value above zero in the log shows that it does. A zero
            // shows that it does not.
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

// Interleaved stereo PCM16. This is the preferred entry point, because only it
// keeps the stereo mix of the music.
int Rp2350MixFrameStereo16(s16 *out, int n)
{
    n = mix_begin(n);
    if (n <= 0)
        return 0;

    // The window that SoundMain rendered into, not the start of the buffer.
    mix_stereo_range(out, Rp2350MixWindowOffset(), n);
    return n;
}

// Mono PCM16, for outputs that have no second channel.
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

// Rp2350MixFrame: the original 8-bit mono interface. It puts the PCM mix of
// this frame into a mono int8 buffer for the I2S ring, and overrides the weak
// silence stub in rp2350/hw/audio.c. It stays because the I2S ring of
// rp2350/hw/audio.c uses this signature. It must lose the second channel and
// the extra precision. The engine filled gSoundInfo.pcmBuffer (right half, then
// left half) in VBlankIntr and m4aSoundMain on this frame.
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
