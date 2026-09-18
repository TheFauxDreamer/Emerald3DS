// A C version of src/m4a_1.s, the assembly core of the GBA "m4a" (MP2K) sound
// engine: the song command interpreter and the PCM mixer.
//
// The pokeemerald tree keeps the high-level engine in C (src/m4a.c), but the
// hot core is hand-written ARM and Thumb assembly. This file is a portable C
// version of each symbol that the assembly exports. Thus the real m4a.c engine
// can run on a target that cannot execute that assembly.
//
// The reason for this file: the Cortex-M33 of the RP2350 can execute only
// Thumb-2, not ARMv4T ARM-mode code. That reason does not apply to all targets
// (see rp2350/m4a_port.h). A target that can assemble the original does not use
// this file.
//
// The mixer renders into gSoundInfo.pcmBuffer as the GBA DirectSound path does:
// signed 8-bit, two PCM_DMA_BUF_SIZE halves, first right, then left. The file
// m4a_mix.c handles the buffer after that.
//
// Differences from the assembly:
// - The assembly mixes 4 samples in each 32-bit word with a rotate and
//   accumulate method, and discards the carries between bytes. This file adds
//   each output sample as a plain signed 8-bit add that wraps. That is the
//   usual correct C model of the MP2K mixer. It is bit-identical, except for
//   the rare case of a hard overflow carry.
// - This file ignores maxLines (the render deadline for each scanline), and
//   always mixes each active channel fully. There is no VCOUNT race to lose.
// - Compressed (TONEDATA_TYPE_CMP) and reverse (TONEDATA_TYPE_REV) samples go
//   through MixChannelSpecial. It decodes the BDPCM block format, and does not
//   copy the pointer arithmetic of the original.

#include "global.h"
#include "gba/m4a_internal.h"
#include "m4a_port.h"

// Flags not exported to the C header (only in constants/m4a_constants.inc).
#define SOUND_CHANNEL_SF_SPECIAL 0x20
#define TONEDATA_TYPE_REV        0x10
#define TONEDATA_TYPE_CMP        0x20
#define WAVE_DATA_FLAG_LOOP      0xC0

// The PSG render chunk, in sample frames. See the note on PSG_CHUNK in
// m4a_mix.c.
#define PSG_CHUNK 64

// SOUND_INFO_PTR is a macro (gba/defines.h) for the SoundInfo pointer slot.
extern const u8 gClockTable[];
extern const s8 gDeltaEncodingTable[];
extern void *const gMPlayJumpTableTemplate[];
extern void ClearChain(void *x);
extern void Clear64byte(void *x);
extern void TrkVolPitSet(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track);
extern void FadeOutBody(struct MusicPlayerInfo *mplayInfo);
extern u32 MidiKeyToFreq(struct WaveData *wav, u8 key, u8 fineAdjust);

// The WaveData "flags" byte (loop bits) lives at byte offset 3 -- the high byte
// of the u16 `status` field in the C struct. Read it positionally to match asm.
static inline u8 WaveFlags(const struct WaveData *wav)
{
    return ((const u8 *)wav)[3];
}

// ----------------------------------------------------------------------------
// umul3232H32: high 32 bits of a 32x32 unsigned multiply. Used by MidiKeyToFreq.
// ----------------------------------------------------------------------------
u32 umul3232H32(u32 multiplier, u32 multiplicand)
{
    return (u32)(((u64)multiplier * (u64)multiplicand) >> 32);
}

// ----------------------------------------------------------------------------
// RealClearChain: unlink a SoundChannel from its track's active-channel list.
// ----------------------------------------------------------------------------
void RealClearChain(void *x)
{
    struct SoundChannel *chan = x;
    struct MusicPlayerTrack *track = chan->track;

    if (track == NULL)
        return;

    struct SoundChannel *next = chan->nextChannelPointer;
    struct SoundChannel *prev = chan->prevChannelPointer;

    if (prev == NULL)
        track->chan = next;
    else
        prev->nextChannelPointer = next;

    if (next != NULL)
        next->prevChannelPointer = prev;

    chan->track = NULL;
}

// ----------------------------------------------------------------------------
// SoundMainBTM: zero a 64-byte block (used to clear a SoundChannel/CgbChannel).
// ----------------------------------------------------------------------------
// SoundMainBTM is a vestigial entry at the tail of gMPlayJumpTableTemplate
// (only indices 0-29 are reachable as commands). Provide the header signature;
// it is never invoked on RP2350.
void SoundMainBTM(void)
{
}

// One accumulated sample, into both halves of the PCM buffer.
//
// The add wraps at s8, and does not saturate, as the original does. The
// original mixes four samples in each 32-bit word, and discards the carries
// between bytes. Thus five channels with a sum past +-127 go to the opposite
// sign. The waveform makes a full-scale step that no peak counter can see,
// because the result stays in +-127.
//
// This counts the wraps here, because all three mixer loops come through this
// place. The question is how often it occurs in this game, and only a count can
// answer it.
static inline void MixAccum(s8 *bufR, s8 *bufL, s32 i, s32 addR, s32 addL)
{
    s32 sumR = bufR[i] + addR;
    s32 sumL = bufL[i] + addL;

    if (sumR > 127 || sumR < -128 || sumL > 127 || sumL < -128)
        gM4aDbgDsWrap++;

    bufR[i] = (s8)sumR;
    bufL[i] = (s8)sumL;
}

// ----------------------------------------------------------------------------
// Compressed (BDPCM) and reverse-playback sample generation.
//
// A compressed wave stores 64 samples in each 33-byte block: one verbatim
// sample, then 32 bytes of 4-bit deltas. Each delta is an index into
// gDeltaEncodingTable, low nibble before high. The high nibble of the first
// packed byte is not used, because 1 base and 63 deltas fill the block.
//
// The original (SoundMainRAM_Unk1/Unk2 in src/m4a_1.s) changes
// chan->currentPointer into a sample index, and keeps the decoded block in
// chan->xpi. This does the same work by index, which suits the format. Its
// cache key is the wave and the block. Thus two channels that read different
// compressed samples cannot see the buffer of the other.
// ----------------------------------------------------------------------------

static const struct WaveData *sDecodedWav;
static u32 sDecodedBlock;
static s8  sDecodeBuf[64];

static s32 BdpcmSample(const struct WaveData *wav, u32 index)
{
    u32 block = index >> 6;

    if (wav != sDecodedWav || block != sDecodedBlock || sDecodedWav == NULL)
    {
        const u8 *p = (const u8 *)wav->data + block * 0x21;
        s32 acc = (s8)*p++;
        s32 k = 0;
        u8 packed;

        sDecodeBuf[k++] = (s8)acc;

        // Only the low nibble of the first packed byte has a delta.
        packed = *p++;
        acc = (s8)(acc + gDeltaEncodingTable[packed & 0xF]);
        sDecodeBuf[k++] = (s8)acc;

        while (k < 64)
        {
            packed = *p++;
            acc = (s8)(acc + gDeltaEncodingTable[packed >> 4]);
            sDecodeBuf[k++] = (s8)acc;
            acc = (s8)(acc + gDeltaEncodingTable[packed & 0xF]);
            sDecodeBuf[k++] = (s8)acc;
        }

        sDecodedWav = wav;
        sDecodedBlock = block;
    }

    return sDecodeBuf[index & 63];
}

// One sample by index, with compression and playback direction. The index is
// clamped, because the interpolator reads index+1, which is past the end on the
// last sample of a wave that does not loop.
static s32 SpecialSample(const struct SoundChannel *chan,
                         const struct WaveData *wav, s32 index)
{
    s32 size = (s32)wav->size;

    if (index < 0)
        index = 0;
    else if (index >= size)
        index = size - 1;

    if (chan->type & TONEDATA_TYPE_REV)
        index = size - 1 - index;

    if (wav->type != 0)
        return BdpcmSample(wav, (u32)index);

    return wav->data[index];
}

// The compressed and reverse version of the plain sample loops below. It has
// the same interpolation and the same loop handling, and only the fetch is
// different. Thus it also works for a fixed-rate channel: an inc of one whole
// sample keeps the interpolation weight at zero.
static void MixChannelSpecial(struct SoundInfo *si, struct SoundChannel *chan,
                              s8 *bufR, s8 *bufL, s32 n, s32 envR, s32 envL)
{
    struct WaveData *wav = chan->wav;
    u32 fw = chan->fw;
    u32 inc;
    s32 count = chan->count;
    s32 size = (s32)wav->size;
    s32 index = size - count;
    s32 loopLength = 0;
    s32 base, delta;

    if (wav == NULL || size <= 0)
    {
        chan->statusFlags = 0;
        return;
    }

    inc = (chan->type & TONEDATA_TYPE_FIX) ? (1u << 23)
                                           : (u32)si->divFreq * chan->frequency;

    if (chan->statusFlags & SOUND_CHANNEL_SF_LOOP)
        loopLength = size - (s32)wav->loopStart;

    base = SpecialSample(chan, wav, index);
    delta = SpecialSample(chan, wav, index + 1) - base;

    for (s32 i = 0; i < n; i++)
    {
        s32 interp = base + (s32)(((s64)(s32)fw * delta) >> 23);
        u32 adv;
        u32 mag;

        MixAccum(bufR, bufL, i, (envR * interp) >> 8, (envL * interp) >> 8);

        mag = (u32)(interp < 0 ? -interp : interp);
        if (mag > gM4aDbgCryPeak)
            gM4aDbgCryPeak = mag;

        fw += inc;
        adv = fw >> 23;
        if (adv != 0)
        {
            fw &= ~0x3F800000u;
            count -= adv;
            if (count <= 0)
            {
                if (loopLength <= 0)
                {
                    chan->statusFlags = 0;
                    chan->count = 0;
                    chan->fw = fw;
                    return;
                }
                do { count += loopLength; } while (count <= 0);
            }
            index = size - count;
            base = SpecialSample(chan, wav, index);
            delta = SpecialSample(chan, wav, index + 1) - base;
        }
    }

    chan->fw = fw;
    chan->count = count;
    // Kept in step for all code that reads it, the telemetry also, but this
    // path uses an index and does not walk the pointer.
    chan->currentPointer = wav->data + (size - count);
}

// ----------------------------------------------------------------------------
// Mix one direct-sound channel into the two PCM half-buffers.
// bufR = dma + 0 (right), bufL = dma + PCM_DMA_BUF_SIZE (left). `n` output
// samples this frame. Returns nothing; updates channel state in place.
// ----------------------------------------------------------------------------
static void MixChannel(struct SoundInfo *si, struct SoundChannel *chan, s8 *dma, s32 n)
{
    s8 *bufR = dma;
    s8 *bufL = dma + PCM_DMA_BUF_SIZE;

    struct WaveData *wav = chan->wav;
    u8 flags = chan->statusFlags;

    if (!(flags & SOUND_CHANNEL_SF_ON))
        return;

    s32 env = chan->envelopeVolume;

    // ---- envelope state machine ----
    if (flags & SOUND_CHANNEL_SF_START)
    {
        if (flags & SOUND_CHANNEL_SF_STOP)
        {
            chan->statusFlags = 0;
            return;
        }
        // Begin a fresh note.
        flags = SOUND_CHANNEL_SF_ENV_ATTACK;
        chan->statusFlags = flags;
        chan->currentPointer = wav->data + chan->count;
        chan->count = wav->size - chan->count;
        env = 0;
        chan->envelopeVolume = 0;
        chan->fw = 0;
        if (WaveFlags(wav) & WAVE_DATA_FLAG_LOOP)
        {
            flags |= SOUND_CHANNEL_SF_LOOP;
            chan->statusFlags = flags;
        }
        goto env_attack;   // a starting note takes one attack step immediately
    }

    if (flags & SOUND_CHANNEL_SF_IEC)
    {
        // Pseudo-echo tail: count down its length, then stop.
        u8 pel = chan->pseudoEchoLength - 1;
        chan->pseudoEchoLength = pel;
        if (pel > 0)
            goto apply_env;
        chan->statusFlags = 0;
        return;
    }

    if (flags & SOUND_CHANNEL_SF_STOP)
    {
        // Release phase.
        env = (env * chan->release) >> 8;
        if (env > chan->pseudoEchoVolume)
            goto apply_env;
        goto pseudo_echo;
    }

    switch (flags & SOUND_CHANNEL_SF_ENV)
    {
    case SOUND_CHANNEL_SF_ENV_DECAY:
        env = (env * chan->decay) >> 8;
        if (env > chan->sustain)
            goto apply_env;
        env = chan->sustain;
        if (env == 0)
            goto pseudo_echo;
        flags -= 1;   // DECAY -> SUSTAIN
        chan->statusFlags = flags;
        goto apply_env;

    case SOUND_CHANNEL_SF_ENV_ATTACK:
    env_attack:
        env += chan->attack;
        if (env >= 0xFF)
        {
            env = 0xFF;
            flags -= 1;   // ATTACK -> DECAY
            chan->statusFlags = flags;
        }
        goto apply_env;

    default:   // SUSTAIN / RELEASE: hold
        goto apply_env;
    }

pseudo_echo:
    env = chan->pseudoEchoVolume;
    if (env == 0)
    {
        chan->statusFlags = 0;
        return;
    }
    flags |= SOUND_CHANNEL_SF_IEC;
    chan->statusFlags = flags;
    // fall through to apply_env

apply_env:
    chan->envelopeVolume = env;
    // Scale by master volume, then split into per-side envelope volumes.
    env = ((si->masterVolume + 1) * env) >> 4;
    chan->envelopeVolumeRight = (chan->rightVolume * env) >> 8;
    chan->envelopeVolumeLeft  = (chan->leftVolume  * env) >> 8;

    // ---- loop region ----
    s8 *loopStart = NULL;
    s32 loopLength = 0;
    if (flags & SOUND_CHANNEL_SF_LOOP)
    {
        loopStart = wav->data + wav->loopStart;
        loopLength = wav->size - wav->loopStart;
    }

    // ---- sample generation ----
    s32 envR = chan->envelopeVolumeRight;
    s32 envL = chan->envelopeVolumeLeft;
    s32 count = chan->count;
    s8 *src = chan->currentPointer;

    if (chan->type & (TONEDATA_TYPE_CMP | TONEDATA_TYPE_REV))
    {
        MixChannelSpecial(si, chan, bufR, bufL, n, envR, envL);
        return;
    }

    if (chan->type & TONEDATA_TYPE_FIX)
    {
        // Fixed-rate (no resampling): one source sample per output sample.
        for (s32 i = 0; i < n; i++)
        {
            s32 s = *src++;
            MixAccum(bufR, bufL, i, (envR * s) >> 8, (envL * s) >> 8);
            if (--count == 0)
            {
                if (loopLength != 0)
                {
                    src = loopStart;
                    count = loopLength;
                }
                else
                {
                    chan->statusFlags = 0;
                    chan->count = 0;
                    chan->currentPointer = src;
                    return;
                }
            }
        }
        chan->count = count;
        chan->currentPointer = src;
        return;
    }

    // Resampled (pitched) playback with linear interpolation. fw is a fixed-
    // point phase accumulator; bit 23 == one whole source sample.
    u32 fw = chan->fw;
    u32 inc = (u32)si->divFreq * chan->frequency;
    s32 base = *src++;          // src now points at the sample after `base`
    s32 delta = *src - base;

    for (s32 i = 0; i < n; i++)
    {
        s32 interp = base + (s32)(((s64)(s32)fw * delta) >> 23);
        MixAccum(bufR, bufL, i, (envR * interp) >> 8, (envL * interp) >> 8);

        fw += inc;
        u32 adv = fw >> 23;
        if (adv != 0)
        {
            fw &= ~0x3F800000u;
            count -= adv;
            if (count <= 0)
            {
                // loopLength <= 0 means no usable loop (or corrupt wave): stop.
                // The <=0 guard also prevents an infinite wrap spin if a bad
                // frequency drove `count` hugely negative in one step.
                if (loopLength <= 0)
                {
                    chan->statusFlags = 0;
                    chan->count = 0;
                    chan->currentPointer = src;
                    chan->fw = fw;
                    return;
                }
                do { count += loopLength; } while (count <= 0);
                src = loopStart + (loopLength - count);
            }
            else
            {
                src += (adv - 1);
            }
            base = *src++;
            delta = *src - base;
        }
    }

    chan->fw = fw;
    chan->count = count;
    chan->currentPointer = src;
}

// ----------------------------------------------------------------------------
// SoundMainRAM: clear the window of this frame in both PCM halves, then mix
// each active channel into it. Restores the SoundInfo lock (ident) on exit.
// ----------------------------------------------------------------------------
// The dma argument is the window of pcmBuffer that this frame writes. The tap
// argument is the window that the reverb takes its delayed tap from. See
// SoundMain.
static void MixAllChannels(struct SoundInfo *si, s8 *dma, const s8 *tap, s32 n)
{
    // The frame starts from the reverb feedback, not from silence, when a
    // reverb depth is set.
    //
    // The m4aSoundInit function sets no reverb, but MPlayStart calls
    // m4aSoundMode(songHeader->reverb) for each song whose header sets
    // SOUND_MODE_REVERB_SET (src/m4a.c:734). Of the 529 songs in Emerald, 479
    // are built with -R50 (sound/songs/midi/midi.cfg). Thus almost all of the
    // music uses reverb 50.
    //
    // This pass is the one of the original (SoundMainRAM_Reverb, src/m4a_1.s).
    // It sets each sample to the scaled sum of four taps: left and right at two
    // different positions in the DMA ring. The ipatix/agbplay player does the
    // same thing as:
    //
    //   (rbuf[pos].l + rbuf[pos].r + rbuf[pos2].l + rbuf[pos2].r)
    //       * intensity / 4
    //
    // It uses a delay line of (rate / fps) * numAgbBuffers samples, which is
    // several frames, not one.
    //
    // Two taps at different delays make a reverb. One tap at 16.7 ms with
    // feedback makes a comb filter, with a notch every 60 Hz. That gives all
    // sounds a metal color, changes the balance between the instruments, and
    // joins notes together.
    s32 reverb = gM4aReverbOn ? si->reverb : 0;

    if (reverb == 0)
    {
        for (s32 i = 0; i < n; i++)
        {
            dma[i] = 0;
            dma[i + PCM_DMA_BUF_SIZE] = 0;
        }
    }
    else
    {
        for (s32 i = 0; i < n; i++)
        {
            s32 r = dma[i];
            s32 l = dma[i + PCM_DMA_BUF_SIZE];
            s32 r2 = tap[i];
            s32 l2 = tap[i + PCM_DMA_BUF_SIZE];
            s32 v = ((r + l + r2 + l2) * reverb) >> 9;

            // The rounding fix of the original, kept verbatim.
            if (v & 0x80)
                v += 1;

            dma[i] = (s8)v;
            dma[i + PCM_DMA_BUF_SIZE] = (s8)v;
        }
    }

    s32 maxChans = si->maxChans;
    for (s32 c = 0; c < maxChans; c++)
        MixChannel(si, &si->chans[c], dma, n);

    si->ident = ID_NUMBER;
}

// ----------------------------------------------------------------------------
// SoundMain: per-frame entry. Runs the player chain + CGB, then the PCM mixer.
// ----------------------------------------------------------------------------
void SoundMain(void)
{
    struct SoundInfo *si = SOUND_INFO_PTR;

    if (si->ident != ID_NUMBER)
        return;
    si->ident++;

    if (si->MPlayMainHead != NULL)
        si->MPlayMainHead(si->musicPlayerHead);

    si->CgbSound();

    // Render into a rotating window of pcmBuffer, as the DMA double buffer of
    // the GBA does. There is no DMA here, so nothing reads the rotation. It
    // exists so that the reverb has a delay line with real history, not one
    // frame of its own output.
    //
    // The pcmDmaPeriod value is PCM_DMA_BUF_SIZE / pcmSamplesPerVBlank, which
    // is 1584 / 224 = 7. Thus the delayed tap is about 100 ms back, which makes
    // a reverb. A tap from the same window would be 16.7 ms back, which makes a
    // comb filter.
    //
    // The window comes from Rp2350MixWindowOffset(), not from a counter in this
    // file. The mixer interface must read the same window that this wrote, and
    // only one of the two is replaceable. See the copy of the assembly there.
    {
        s32 n = si->pcmSamplesPerVBlank;
        s32 period = si->pcmDmaPeriod;
        s32 cur = Rp2350MixWindowOffset();
        s32 old;

        if (period < 1)
            period = 1;

        // The next window in the ring holds the oldest data. That is the
        // position that `addne r7, r5, r8` in the original selects. It wraps to
        // window 0 at the same point as its `cmp r4, 0x2` special case.
        old = cur + n;
        if (old >= period * n)
            old = 0;

        MixAllChannels(si, si->pcmBuffer + cur, si->pcmBuffer + old, n);
    }
}

// ----------------------------------------------------------------------------
// m4aSoundVSync: on the GBA this re-arms the DirectSound DMA FIFOs each VCount.
// RP2350 reads pcmBuffer directly (no FIFO DMA), so this is a no-op.
// ----------------------------------------------------------------------------
void m4aSoundVSync(void)
{
    struct SoundInfo *si = SOUND_INFO_PTR;
    s32 counter;

    if (si == NULL || si->ident - ID_NUMBER > 1)
        return;

    // The counter half of m4aSoundVSync in the original, and only that half.
    //
    // The rest of it arms the two DirectSound DMA channels again, which has no
    // meaning with no DMA engine: the mixer interface reads pcmBuffer directly.
    // The counter has a meaning, because it sets the window that SoundMain
    // renders into (see Rp2350MixWindowOffset). Without it, the window stays at
    // 0, and the reverb has no delay line.
    //
    // It counts down to 1 and loads again, as `subs r1, 1 / bgt / reload` does.
    // A counter that starts at 0 goes negative on the first tick and loads
    // again. That is also how the original recovers from a counter that is not
    // initialized.
    counter = (s32)si->pcmDmaCounter - 1;
    if (counter <= 0)
        counter = si->pcmDmaPeriod;

    si->pcmDmaCounter = (u8)counter;
}

// ----------------------------------------------------------------------------
// ChnVolSetAsm: derive a channel's per-side hardware volume from velocity, pan
// (rhythmPan) and the track's per-side volume.
// ----------------------------------------------------------------------------
void ChnVolSetAsm(struct SoundChannel *chan, struct MusicPlayerTrack *track)
{
    u32 velocity = chan->velocity;
    s32 pan = (s8)chan->rhythmPan;

    s32 r = (0x80 + pan) * velocity;
    r = (track->volMR * r) >> 14;
    if (r > 0xFF)
        r = 0xFF;
    chan->rightVolume = r;

    s32 l = (0x7F - pan) * velocity;
    l = (track->volML * l) >> 14;
    if (l > 0xFF)
        l = 0xFF;
    chan->leftVolume = l;
}

// ----------------------------------------------------------------------------
// TrackStop: silence and detach every channel owned by a track.
// ----------------------------------------------------------------------------
void TrackStop(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    if (!(track->flags & MPT_FLG_EXIST))
        return;

    struct SoundChannel *chan = track->chan;
    while (chan != NULL)
    {
        if (chan->statusFlags != 0)
        {
            if (chan->type & TONEDATA_TYPE_CGB)
            {
                struct SoundInfo *si = SOUND_INFO_PTR;
                si->CgbOscOff(chan->type & TONEDATA_TYPE_CGB);
            }
            chan->statusFlags = 0;
        }
        chan->track = NULL;
        chan = chan->nextChannelPointer;
    }
    track->chan = NULL;
}

// ----------------------------------------------------------------------------
// MPlayJumpTableCopy: copy the command jump-table template into RAM. On the GBA
// this also guards against BIOS-ROM reads; on RP2350 it is a plain copy.
// ----------------------------------------------------------------------------
void MPlayJumpTableCopy(MPlayFunc *mplayJumpTable)
{
    for (int i = 0; i < 36; i++)
        mplayJumpTable[i] = (MPlayFunc)gMPlayJumpTableTemplate[i];
}

// ----------------------------------------------------------------------------
// Track-command helpers. `track->cmdPtr` walks the song byte-stream.
// ----------------------------------------------------------------------------
static inline u8 ReadByte(struct MusicPlayerTrack *track)
{
    return *track->cmdPtr++;
}

void ply_fine(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    struct SoundChannel *chan = track->chan;
    while (chan != NULL)
    {
        if (chan->statusFlags & SOUND_CHANNEL_SF_ON)
            chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
        RealClearChain(chan);
        chan = chan->nextChannelPointer;
    }
    track->flags = 0;
}

void ply_goto(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    u8 *p = track->cmdPtr;
    u32 addr = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
    track->cmdPtr = (u8 *)(uintptr_t)addr;
}

void ply_patt(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u8 level = track->patternLevel;
    if (level < 3)
    {
        track->patternStack[level] = track->cmdPtr + 4;
        track->patternLevel = level + 1;
        ply_goto(mplayInfo, track);
        return;
    }
    ply_fine(mplayInfo, track);
}

void ply_pend(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    u8 level = track->patternLevel;
    if (level != 0)
    {
        level--;
        track->patternLevel = level;
        track->cmdPtr = track->patternStack[level];
    }
}

void ply_rept(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    if (track->cmdPtr[0] == 0)
    {
        track->cmdPtr += 1;
        ply_goto(mplayInfo, track);
        return;
    }
    u8 n = track->repN + 1;
    track->repN = n;
    u8 target = track->cmdPtr[0];
    if (n < target)
    {
        track->cmdPtr += 1;   // ld_r3_tp_adr_i consumed the count byte
        ply_goto(mplayInfo, track);
        return;
    }
    track->repN = 0;
    track->cmdPtr += 5;
}

void ply_prio(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->priority = ReadByte(track);
}

void ply_tempo(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u32 t = ReadByte(track) << 1;
    mplayInfo->tempoD = t;
    mplayInfo->tempoI = (t * mplayInfo->tempoU) >> 8;
}

void ply_keysh(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->keyShift = ReadByte(track);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_voice(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    u8 voice = ReadByte(track);
    struct ToneData *tone = mplayInfo->tone + voice;
    // Copy the 12-byte ToneData (type/key/length/pan, wav, attack/decay/...).
    track->tone = *tone;
}

void ply_vol(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->vol = ReadByte(track);
    track->flags |= MPT_FLG_VOLCHG;
}

void ply_pan(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->pan = ReadByte(track) - C_V;
    track->flags |= MPT_FLG_VOLCHG;
}

void ply_bend(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->bend = ReadByte(track) - C_V;
    track->flags |= MPT_FLG_PITCHG;
}

void ply_bendr(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->bendRange = ReadByte(track);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_lfodl(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->lfoDelay = ReadByte(track);
}

static void clear_modM(struct MusicPlayerTrack *track)
{
    track->modM = 0;
    track->lfoSpeedC = 0;
    if (track->modT == 0)
        track->flags |= MPT_FLG_PITCHG;
    else
        track->flags |= MPT_FLG_VOLCHG;
}

void ply_modt(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    u8 v = ReadByte(track);
    if (track->modT != v)
    {
        track->modT = v;
        track->flags |= (MPT_FLG_VOLCHG | MPT_FLG_PITCHG);
    }
}

void ply_tune(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->tune = ReadByte(track) - C_V;
    track->flags |= MPT_FLG_PITCHG;
}

void ply_port(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    // GBA writes a CGB sound register indexed by the first arg; we have no such
    // register file, so we consume both bytes and drop the write.
    (void)ReadByte(track);
    (void)ReadByte(track);
}

void ply_lfos(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->lfoSpeed = ReadByte(track);
    if (track->lfoSpeed == 0)
        clear_modM(track);
}

void ply_mod(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    track->mod = ReadByte(track);
    if (track->mod == 0)
        clear_modM(track);
}

void ply_endtie(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    (void)mplayInfo;
    u8 key;
    if (track->cmdPtr[0] < 0x80)
    {
        key = track->cmdPtr[0];
        track->key = key;
        track->cmdPtr += 1;
    }
    else
    {
        key = track->key;
    }

    struct SoundChannel *chan = track->chan;
    while (chan != NULL)
    {
        u8 sf = chan->statusFlags;
        if ((sf & (SOUND_CHANNEL_SF_START | SOUND_CHANNEL_SF_ENV)) &&
            !(sf & SOUND_CHANNEL_SF_STOP) &&
            chan->midiKey == key)
        {
            chan->statusFlags = sf | SOUND_CHANNEL_SF_STOP;
            return;
        }
        chan = chan->nextChannelPointer;
    }
}

// ----------------------------------------------------------------------------
// ply_note: trigger a note. Allocates/steals a SoundChannel, sets up its
// envelope/wave/frequency, and links it onto the track. (CGB tone types are
// routed through the SoundInfo callbacks, which are DummyFunc on RP2350.)
// ----------------------------------------------------------------------------
void ply_note(u32 note_cmd, struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track)
{
    struct SoundInfo *si = SOUND_INFO_PTR;

    track->gateTime = gClockTable[note_cmd];

    // key / velocity / gate-extension args (each optional, < 0x80).
    u8 *p = track->cmdPtr;
    if (p[0] < 0x80)
    {
        track->key = p[0];
        p++;
        if (p[0] < 0x80)
        {
            track->velocity = p[0];
            p++;
            if (p[0] < 0x80)
            {
                track->gateTime += p[0];
                p++;
            }
        }
        track->cmdPtr = p;
    }

    s32 rhythmPan = 0;
    struct ToneData *tone = &track->tone;
    u8 type = track->tone.type;

    if (type & (TONEDATA_TYPE_RHY | TONEDATA_TYPE_SPL))
    {
        u8 key = track->key;
        u8 idx;
        if (type & TONEDATA_TYPE_SPL)
        {
            // The key-split table pointer aliases the tone's `attack` field
            // (o_..._keySplitTable == o_..._attack in m4a_constants.inc).
            const u8 *keySplitTable = *(const u8 *const *)&track->tone.attack;
            idx = keySplitTable[key];
        }
        else
        {
            idx = key;
        }

        tone = (struct ToneData *)(uintptr_t)track->tone.wav + idx;

        if (tone->type & (TONEDATA_TYPE_SPL | TONEDATA_TYPE_RHY))
            return;   // malformed -- bail (matches asm _081DDCEA)

        if (type & TONEDATA_TYPE_RHY)
        {
            u8 ps = tone->pan_sweep;
            if (ps & 0x80)
                rhythmPan = (ps - TONEDATA_P_S_PAN) << 1;
        }
    }

    u8 keyForFreq = (type & (TONEDATA_TYPE_RHY | TONEDATA_TYPE_SPL)) ? tone->key : track->key;

    s32 priority = mplayInfo->priority + track->priority;
    if (priority > 0xFF)
        priority = 0xFF;

    u32 cgbType = tone->type & TONEDATA_TYPE_CGB;
    struct SoundChannel *chan = NULL;

    if (cgbType != 0)
    {
        // A CGB voice gets exactly one channel, with its type (1..4) as the
        // index. The DirectSound path below searches a pool, but the GBA has
        // one square-1, one square-2, one wave and one noise generator. Thus
        // there is nothing to search for.
        //
        // The structs CgbChannel and SoundChannel overlay each other on
        // purpose. The fields statusFlags, priority, track, frequency and
        // wav/wavePointer are at the same offsets, and both structs are 0x40
        // bytes. Thus the original drives both kinds through one code path from
        // here on. The cast does the same. The build already uses
        // -fno-strict-aliasing.
        struct CgbChannel *cgb = si->cgbChans;

        if (cgb == NULL)
            return;   // MPlayExtender never ran, so there are no CGB channels

        chan = (struct SoundChannel *)(cgb + (cgbType - 1));

        // Steal rules, from ply_note in src/m4a_1.s. A free channel, or one
        // that is already in release, is taken. If not, this note must have a
        // higher priority than the one that plays. At equal priority, the track
        // address decides, so a later track cannot stop an earlier one.
        {
            u8 sf = chan->statusFlags;

            if ((sf & SOUND_CHANNEL_SF_ON) && !(sf & SOUND_CHANNEL_SF_STOP))
            {
                if (chan->priority > priority)
                    return;
                if (chan->priority == priority
                 && (uintptr_t)chan->track < (uintptr_t)track)
                    return;
            }
        }
        goto chan_found;
    }

    // Find a channel to (re)use: a free one wins immediately; otherwise steal
    // the lowest-priority releasing channel, else the lowest-priority active
    // channel below this note's priority (ties broken by higher track address).
    // Mirrors the m4a_1.s voice-allocation search exactly.
    {
        s32 bestPriority = priority;
        uintptr_t bestTrack = (uintptr_t)track;
        s32 have = 0;
        s32 maxChans = si->maxChans;
        for (s32 i = 0; i < maxChans; i++)
        {
            struct SoundChannel *ch = &si->chans[i];
            u8 sf = ch->statusFlags;
            if (!(sf & SOUND_CHANNEL_SF_ON))
            {
                chan = ch;   // free channel -- take it immediately
                goto chan_found;
            }
            if (sf & SOUND_CHANNEL_SF_STOP)
            {
                if (have == 0)
                {
                    have = 1;
                    bestPriority = ch->priority;
                    bestTrack = (uintptr_t)ch->track;
                    chan = ch;
                    continue;
                }
            }
            else if (have != 0)
            {
                continue;   // an active note can't be stolen once a release exists
            }

            if (ch->priority < bestPriority)
            {
                bestPriority = ch->priority;
                bestTrack = (uintptr_t)ch->track;
                chan = ch;
            }
            else if (ch->priority == bestPriority && (uintptr_t)ch->track >= bestTrack)
            {
                bestTrack = (uintptr_t)ch->track;
                chan = ch;
            }
        }
        if (chan == NULL)
            return;
    }
chan_found:;

    // Link the channel onto the head of the track's list.
    ClearChain(chan);
    chan->prevChannelPointer = NULL;
    chan->nextChannelPointer = track->chan;
    if (track->chan != NULL)
        track->chan->prevChannelPointer = chan;
    track->chan = chan;
    chan->track = track;

    track->lfoDelayC = track->lfoDelay;
    if (track->lfoDelay != 0)
        clear_modM(track);
    TrkVolPitSet(mplayInfo, track);

    // The original does this with one 32-bit store (ply_note, m4a_1.s). It
    // copies gateTime, key, velocity and runningStatus of MusicPlayerTrack at
    // 0x04..0x07 to gateTime, midiKey, velocity and priority of SoundChannel at
    // 0x10..0x13. Two instructions later, it overwrites priority with the
    // calculated value. Thus these fields must all be copied.
    //
    // The chan->velocity field is necessary for sound. ChnVolSetAsm multiplies
    // by it, so a velocity of 0 makes rightVolume and leftVolume 0 for each
    // note.
    //
    // The chan->midiKey field is necessary too. The ply_endtie command finds
    // the channel to release by it, so with a key of 0, tied notes never
    // release.
    //
    // The two other packed stores in the same routine are
    // attack/decay/sustain/release and pseudoEchoVolume/pseudoEchoLength. The
    // code below already unpacks them.
    chan->gateTime = track->gateTime;
    chan->midiKey  = track->key;
    chan->velocity = track->velocity;
    chan->priority = priority;
    chan->key = keyForFreq;
    chan->rhythmPan = rhythmPan;
    chan->type = tone->type;
    chan->wav = tone->wav;
    chan->attack = tone->attack;
    chan->decay = tone->decay;
    chan->sustain = tone->sustain;
    chan->release = tone->release;
    // pseudoEchoVolume + pseudoEchoLength come from the track (one halfword).
    chan->pseudoEchoVolume = track->pseudoEchoVolume;
    chan->pseudoEchoLength = track->pseudoEchoLength;
    ChnVolSetAsm(chan, track);

    s32 midiKey = chan->key + (s8)track->keyM;
    if (midiKey < 0)
        midiKey = 0;

    if (cgbType != 0)
    {
        // The CGB end of ply_note. It has no `count`, on purpose: a PSG note
        // has no sample that can end. It plays until the length counter or the
        // envelope stops it.
        struct CgbChannel *cgb = (struct CgbChannel *)chan;
        u8 ps = tone->pan_sweep;

        cgb->length = tone->length;

        // The pan_sweep field is one byte with two jobs. With bit 7 set, it
        // holds a PAN value, so there is no sweep. A sweep field of all zeros
        // also means no sweep. In both cases, the value is 8, the inert value
        // of the original.
        cgb->sweep = (!(ps & 0x80) && (ps & 0x70)) ? ps : 8;

        // Through the SoundInfo hook, not a direct call to MidiKeyToCgbFreq, as
        // the original does. MPlayExtender installs the hook, so a build
        // without CGB support cannot get here.
        chan->frequency = si->MidiKeyToCgbFreq(cgbType, midiKey, track->pitM);
    }
    else
    {
        chan->count = track->unk_3C;
        chan->frequency = MidiKeyToFreq(chan->wav, midiKey, track->pitM);
    }
    chan->statusFlags = SOUND_CHANNEL_SF_START;
    track->flags &= 0xF0;
}

// ----------------------------------------------------------------------------
// MPlayMain: the per-frame song interpreter. Advances every track of one music
// player by one tick: processes wait countdowns, dispatches commands, applies
// per-channel gate/LFO/volume/pitch. Mirrors src/m4a_1.s MPlayMain exactly.
// ----------------------------------------------------------------------------
void MPlayMain(struct MusicPlayerInfo *mplayInfo)
{
    if (mplayInfo->ident != ID_NUMBER)
        return;
    mplayInfo->ident++;

    // Chain to the next player in the list, if any.
    if (mplayInfo->MPlayMainNext != NULL)
        mplayInfo->MPlayMainNext(mplayInfo->musicPlayerNext);

    struct SoundInfo *si = SOUND_INFO_PTR;

    if ((s32)mplayInfo->status < 0)   // paused
        goto done;

    FadeOutBody(mplayInfo);
    if ((s32)mplayInfo->status < 0)
        goto done;

    u32 tempoAcc = mplayInfo->tempoC + mplayInfo->tempoI;

    while (tempoAcc >= 150)
    {
        tempoAcc -= 150;

        u8 trackCount = mplayInfo->trackCount;
        struct MusicPlayerTrack *track = mplayInfo->tracks;
        u32 trackBit = 1;
        u32 anyExist = 0;

        for (; trackCount != 0; trackCount--, track++, trackBit <<= 1)
        {
            if (!(track->flags & MPT_FLG_EXIST))
                continue;

            anyExist |= trackBit;

            // Gate-time countdown on this track's active channels.
            struct SoundChannel *chan = track->chan;
            while (chan != NULL)
            {
                if (chan->statusFlags & SOUND_CHANNEL_SF_ON)
                {
                    if (chan->gateTime != 0 && --chan->gateTime == 0)
                        chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
                }
                else
                {
                    ClearChain(chan);
                }
                chan = chan->nextChannelPointer;
            }

            // First servicing of a freshly started track: reset its state.
            if (track->flags & MPT_FLG_START)
            {
                Clear64byte(track);
                track->flags = MPT_FLG_EXIST;
                track->bendRange = 2;
                track->volX = 0x40;
                track->lfoSpeed = 0x16;
                track->tone.type = 1;
            }

            // Process commands until the track hits a wait.
            if (track->wait == 0)
            {
                for (;;)
                {
                    u8 cmd = track->cmdPtr[0];
                    if (cmd < 0x80)
                    {
                        cmd = track->runningStatus;   // running status
                    }
                    else
                    {
                        track->cmdPtr++;
                        if (cmd >= 0xBD)
                            track->runningStatus = cmd;
                    }

                    if (cmd >= 0xCF)
                    {
                        si->plynote(cmd - 0xCF, mplayInfo, track);
                    }
                    else if (cmd > 0xB0)
                    {
                        mplayInfo->cmd = cmd - 0xB1;
                        MPlayFunc fn = (MPlayFunc)si->MPlayJumpTable[cmd - 0xB1];
                        fn(mplayInfo, track);
                        if (track->flags == 0)
                            break;
                    }
                    else
                    {
                        track->wait = gClockTable[cmd - 0x80];
                    }

                    if (track->wait != 0)
                        break;
                }
            }

            // Wait countdown + LFO/modulation update.
            if (track->wait != 0)
            {
                track->wait--;
                if (track->lfoSpeed != 0 && track->mod != 0)
                {
                    if (track->lfoDelayC != 0)
                    {
                        track->lfoDelayC--;
                    }
                    else
                    {
                        u8 sc = track->lfoSpeedC + track->lfoSpeed;
                        track->lfoSpeedC = sc;
                        // Triangle LFO: rising while (sc - 0x40) < 0, else falling.
                        s32 x;
                        if ((s8)(sc - 0x40) < 0)
                            x = (s8)sc;
                        else
                            x = 0x80 - sc;
                        s8 newModM = (s8)(((s32)track->mod * x) >> 6);
                        if ((u8)track->modM != (u8)newModM)
                        {
                            track->modM = newModM;
                            if (track->modT == 0)
                                track->flags |= MPT_FLG_PITCHG;
                            else
                                track->flags |= MPT_FLG_VOLCHG;
                        }
                    }
                }
            }
        }

        mplayInfo->clock++;

        if (anyExist == 0)
        {
            mplayInfo->status = MUSICPLAYER_STATUS_PAUSE;
            goto done;
        }
        mplayInfo->status = anyExist;
    }

    mplayInfo->tempoC = tempoAcc;

    // Apply queued volume/pitch changes to every channel.
    {
        u8 trackCount = mplayInfo->trackCount;
        struct MusicPlayerTrack *track = mplayInfo->tracks;
        for (; trackCount != 0; trackCount--, track++)
        {
            if (!(track->flags & MPT_FLG_EXIST))
                continue;
            if (!(track->flags & (MPT_FLG_VOLCHG | MPT_FLG_PITCHG)))
                continue;

            TrkVolPitSet(mplayInfo, track);

            struct SoundChannel *chan = track->chan;
            while (chan != NULL)
            {
                if (!(chan->statusFlags & SOUND_CHANNEL_SF_ON))
                {
                    ClearChain(chan);
                    chan = chan->nextChannelPointer;
                    continue;
                }

                u32 cgbType = chan->type & TONEDATA_TYPE_CGB;

                if (track->flags & MPT_FLG_VOLCHG)
                {
                    ChnVolSetAsm(chan, track);
                    // CGB volume-modify bit not relevant on RP2350.
                }

                if (track->flags & MPT_FLG_PITCHG)
                {
                    s32 midiKey = chan->key + (s8)track->keyM;
                    if (midiKey < 0)
                        midiKey = 0;
                    if (cgbType == 0)
                        chan->frequency = MidiKeyToFreq(chan->wav, midiKey, track->pitM);
                }
                chan = chan->nextChannelPointer;
            }

            track->flags &= 0xF0;
        }
    }

done:
    mplayInfo->ident = ID_NUMBER;
}

