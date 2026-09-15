// Software rendering of the four PSG (CGB) sound channels of the GBA. See
// psg.h.
//
// This is a full implementation at register level. It does not read the m4a
// channel structs, because CgbSound() does not set the volume on each frame. It
// programs the hardware envelope and sweep units: it writes a step time and
// direction into NRx2, and triggers again through NRx4. Then the units ramp by
// themselves between its 60 Hz updates. A synthesizer that ignored those units
// would get each attack and decay wrong. Thus this file implements them.
//
// The structure follows the hardware. A 512 Hz frame sequencer drives the
// length, envelope and sweep units. A Q16 phase accumulator, stepped once for
// each output sample, makes the waveform of each channel. This is not
// cycle-accurate, but the sequencer sets the audible behavior, and the
// accumulator is correct to one output sample at 13 kHz.
//
// This comes from the register descriptions in Pan Docs and GBATEK, not from an
// emulator. Thus it brings no license obligations into rp2350/.

#include "global.h"
#include "psg.h"

// ---------------------------------------------------------------- constants --

// Duty patterns, one bit for each step of the 8-step cycle: bit N is step N.
//   12.5% 00000001   25% 10000001   50% 10000111   75% 01111110
static const u8 kDutyTable[4] = { 0x80, 0x81, 0xE1, 0x7E };

// Noise divisor for each NR43 code. Code 0 is the "0.5" case, which is 8 here,
// because all of the table is multiplied by 16.
static const u8 kNoiseDivisor[8] = { 8, 16, 32, 48, 64, 80, 96, 112 };

// Wave channel output shift for NR32 bits 5-6: mute, 100%, 50%, 25%. Index 0 is
// silence, not a shift.
static const u8 kWaveShift[4] = { 4, 0, 1, 2 };

// PSG to DirectSound balance for SOUNDCNT_H bits 0-1: 25%, 50%, 100%, reserved.
// Each value is a numerator over 4.
static const u8 kPsgRatio[4] = { 1, 2, 4, 4 };

// The loudness of one PSG step in the output range. A channel swings by 7.5 in
// each direction after centering, so four channels at full level reach 30. At
// this gain, that is 23040, well inside a full-scale DirectSound note at 32512.
//
// This is the balance control. If the PSG is too loud for the sampled
// instruments, or too quiet, change this number. The value 512 keeps even the
// theoretical maximum (four channels at volume 15, both sides, master volume 7)
// at 30720, with no clipping.
#define PSG_GAIN 512

// The time constant of the output DC blocker, as a right shift on a Q8
// accumulator. A value of 9 puts the corner near 26 Hz at 13.4 kHz, similar to
// the output capacitor of the console. See the blocker for each side in
// PsgRender.
#define DC_SHIFT 9

// ------------------------------------------------------------------- state --

struct PsgPulse
{
    u32 phase;        // Q16 position within the 8-step duty cycle
    u32 step;         // Q16 duty steps per output sample
    u16 length;       // remaining ticks at 256 Hz
    u16 sweepShadow;  // ch1 only
    u8  volume;       // current envelope output, 0..15
    u8  envPeriod, envDir, envTimer;
    u8  sweepPeriod, sweepDir, sweepShift, sweepTimer, sweepOn;
    u8  duty;
    u8  lengthOn;
    u8  on;
};

struct PsgWave
{
    u32 phase;        // Q16 position within the 32 nibble samples
    u32 step;
    u16 length;       // remaining ticks at 256 Hz
    u8  shift;        // kWaveShift value, 4 means silent
    u8  lengthOn;
    u8  on;
};

struct PsgNoise
{
    u32 phase;        // Q16 LFSR clocks
    u32 step;
    u16 lfsr;
    u16 length;
    u8  volume;
    u8  envPeriod, envDir, envTimer;
    u8  width7;
    u8  lengthOn;
    u8  on;
};

static struct PsgPulse sPulse[2];
static struct PsgWave  sWave;
static struct PsgNoise sNoise;

static u32 sSeqPhase;     // Q16 accumulator of 512 Hz sequencer steps
static u8  sSeqStep;      // 0..7

// One DC-blocker accumulator for each side. With one shared accumulator, a
// channel panned hard left would move the baseline of the right output.
static s32 sDcAccL, sDcAccR;   // Q8 running means

void PsgReset(void)
{
    memset(sPulse, 0, sizeof(sPulse));
    memset(&sWave, 0, sizeof(sWave));
    memset(&sNoise, 0, sizeof(sNoise));
    sSeqPhase = 0;
    sSeqStep = 0;
    sDcAccL = 0;
    sDcAccR = 0;
    sNoise.lfsr = 0x7FFF;
}

// ------------------------------------------------------------- frequencies --
//
// The output frequency of a pulse channel is 131072 / (2048 - x) Hz, and its
// duty pattern has 8 steps. Thus the step rate is 1048576 / (2048 - x). The
// wave channel walks 32 samples at 2097152 / (2048 - x). Both use 64-bit math,
// because the numerator shifted up by 16 does not fit in 32 bits.

static u32 pulse_step(u16 freq, s32 rate)
{
    u32 period = 2048u - (freq & 0x7FF);

    return (u32)(((u64)1048576u << 16) / ((u64)period * (u32)rate));
}

static u32 wave_step(u16 freq, s32 rate)
{
    u32 period = 2048u - (freq & 0x7FF);

    return (u32)(((u64)2097152u << 16) / ((u64)period * (u32)rate));
}

static u32 noise_step(u8 nr43, s32 rate)
{
    u32 divisor = kNoiseDivisor[nr43 & 7];
    u32 shift = nr43 >> 4;

    // The documents say that shifts 14 and 15 make no output.
    if (shift >= 14)
        return 0;

    return (u32)(((u64)4194304u << 16) / ((u64)(divisor << shift) * (u32)rate));
}

// ---------------------------------------------------------- sequencer units --

static void pulse_clock_env(struct PsgPulse *p)
{
    if (p->envPeriod == 0)
        return;

    if (++p->envTimer < p->envPeriod)
        return;

    p->envTimer = 0;
    if (p->envDir)
    {
        if (p->volume < 15)
            p->volume++;
    }
    else
    {
        if (p->volume > 0)
            p->volume--;
    }
}

static void noise_clock_env(struct PsgNoise *nz)
{
    if (nz->envPeriod == 0)
        return;

    if (++nz->envTimer < nz->envPeriod)
        return;

    nz->envTimer = 0;
    if (nz->envDir)
    {
        if (nz->volume < 15)
            nz->volume++;
    }
    else
    {
        if (nz->volume > 0)
            nz->volume--;
    }
}

// Returns the new frequency, and clears `on` if it overflowed. That is how the
// hardware stops a rising sweep that goes past the top of the range.
static u16 sweep_next(struct PsgPulse *p)
{
    u32 delta = p->sweepShadow >> p->sweepShift;
    u32 next;

    if (p->sweepDir)
        next = p->sweepShadow - delta;
    else
        next = p->sweepShadow + delta;

    if (next > 2047)
    {
        p->on = 0;
        return p->sweepShadow;
    }

    return (u16)next;
}

// Returns TRUE when the shadow frequency changed, so the caller calculates the
// phase step again only then. An unconditional update would overwrite the pitch
// of each note without a sweep from a shadow register that was never loaded.
static bool8 pulse_clock_sweep(struct PsgPulse *p)
{
    if (!p->sweepOn || p->sweepPeriod == 0 || p->sweepShift == 0)
        return FALSE;

    if (++p->sweepTimer < p->sweepPeriod)
        return FALSE;

    p->sweepTimer = 0;
    p->sweepShadow = sweep_next(p);
    return TRUE;
}

// ------------------------------------------------------------ register read --
//
// Trigger detection by polling, not by interception of writes. Nothing here
// sees the store that CgbSound makes. Thus a set bit 7 in NRx4 means "this note
// just started", and this code then clears it, as the hardware does with that
// bit. Without the clear, each frame would restart each note.

static void pulse_sync(struct PsgPulse *p, vu8 *nrx1, vu8 *nrx2, vu8 *nrx3,
                       vu8 *nrx4, s32 rate)
{
    u8 r1 = *nrx1, r2 = *nrx2, r4 = *nrx4;
    u16 freq = (u16)(((r4 & 7) << 8) | *nrx3);

    p->duty = r1 >> 6;
    p->envPeriod = r2 & 7;
    p->envDir = (r2 >> 3) & 1;
    p->lengthOn = (r4 >> 6) & 1;
    p->step = pulse_step(freq, rate);

    if (r4 & 0x80)
    {
        // CgbSound triggers again on each frame where it changes the volume,
        // not only when a note starts. It writes the new envelope volume into
        // the top nibble of NRx2, and then sets bit 7 so that the hardware
        // latches it. Thus a trigger here means "use this volume". Only a
        // trigger on a channel that was off means "a note started".
        bool8 wasOff = !p->on;

        *nrx4 = (u8)(r4 & 0x7F);

        p->on = 1;
        p->volume = r2 >> 4;
        p->envTimer = 0;

        // No phase reset, on purpose. The hardware does not reset the duty step
        // on a trigger. A reset here would restart the waveform 60 times a
        // second, which sounds like a buzz, not a note.

        // The length comes from NRx1, as the hardware reads it, for all values
        // that the engine puts there. It reloads on a real note start, or when
        // the counter is at zero.
        if (wasOff || p->length == 0)
            p->length = (u16)(64 - (r1 & 0x3F));

        if (wasOff)
        {
            // The sweep starts from the frequency of the note start.
            p->sweepShadow = freq;
            p->sweepTimer = 0;
            p->sweepOn = (p->sweepPeriod != 0 || p->sweepShift != 0);
        }
    }

    // Volume 0 with a downward envelope turns the DAC off. That makes the
    // channel silent, not only quiet.
    if ((r2 & 0xF8) == 0)
        p->on = 0;
}

static void wave_sync(struct PsgWave *w, s32 rate)
{
    u8 r0 = REG_NR30, r2 = REG_NR32, r4 = REG_NR34;
    u16 freq = (u16)(((r4 & 7) << 8) | REG_NR33);

    w->shift = kWaveShift[(r2 >> 5) & 3];
    w->lengthOn = (r4 >> 6) & 1;
    w->step = wave_step(freq, rate);

    if (r4 & 0x80)
    {
        // Different from the pulse channels, CgbSound triggers channel 3 once
        // for each note (it clears its own bit 7 after). Thus this is a real
        // note start, and the wave position can reset, as on hardware.
        bool8 wasOff = !w->on;

        REG_NR34 = (u8)(r4 & 0x7F);

        w->on = 1;
        w->phase = 0;
        if (wasOff || w->length == 0)
            w->length = (u16)(256 - REG_NR31);
    }

    if (!(r0 & 0x80))
        w->on = 0;   // DAC off
}

static void noise_sync(struct PsgNoise *nz, s32 rate)
{
    u8 r2 = REG_NR42, r3 = REG_NR43, r4 = REG_NR44;

    nz->envPeriod = r2 & 7;
    nz->envDir = (r2 >> 3) & 1;
    nz->width7 = (r3 >> 3) & 1;
    nz->lengthOn = (r4 >> 6) & 1;
    nz->step = noise_step(r3, rate);

    if (r4 & 0x80)
    {
        // The same trigger on each frame as the pulse channels, so the LFSR
        // reloads only on a real note start. A new seed on each frame would
        // make the noise periodic at 60 Hz, which sounds like a rasp, not a
        // drum.
        bool8 wasOff = !nz->on;

        REG_NR44 = (u8)(r4 & 0x7F);

        nz->on = 1;
        nz->volume = r2 >> 4;
        nz->envTimer = 0;
        if (wasOff)
            nz->lfsr = 0x7FFF;
        if (wasOff || nz->length == 0)
            nz->length = (u16)(64 - (REG_NR41 & 0x3F));
    }

    if ((r2 & 0xF8) == 0)
        nz->on = 0;
}

// ----------------------------------------------------------------- generate --

static u8 pulse_output(struct PsgPulse *p)
{
    u8 step = (u8)((p->phase >> 16) & 7);

    if (!p->on)
        return 0;

    return ((kDutyTable[p->duty] >> step) & 1) ? p->volume : 0;
}

static u8 wave_output(struct PsgWave *w)
{
    u32 idx;
    const vu8 *ram = (const vu8 *)REG_ADDR_WAVE_RAM0;
    u8 sample;

    if (!w->on || w->shift >= 4)
        return 0;

    // The 32 nibble samples in the 16 bytes that CgbSound loaded, high nibble
    // first.
    idx = (w->phase >> 16) & 31;
    sample = ram[idx >> 1];
    sample = (idx & 1) ? (sample & 0xF) : (sample >> 4);

    return (u8)(sample >> w->shift);
}

static u8 noise_output(struct PsgNoise *nz)
{
    if (!nz->on)
        return 0;

    // Bit 0 low means output high, as the hardware inverts it.
    return (nz->lfsr & 1) ? 0 : nz->volume;
}

static void noise_advance(struct PsgNoise *nz, u32 clocks)
{
    while (clocks-- != 0)
    {
        u16 x = (u16)((nz->lfsr ^ (nz->lfsr >> 1)) & 1);

        nz->lfsr = (u16)((nz->lfsr >> 1) | (x << 14));
        if (nz->width7)
            nz->lfsr = (u16)((nz->lfsr & ~0x40u) | (x << 6));
    }
}

// ------------------------------------------------------------------- render --

void PsgRender(s16 *out, s32 n, s32 sampleRate)
{
    u8 nr50, nr51;
    s32 rightVol, leftVol, ratio;
    u32 seqStepInc;
    s32 i;

    if (out == NULL || n <= 0 || sampleRate <= 0)
        return;

    // Lazy init, so no caller has to remember a reset. PsgReset() stays public
    // for an explicit reset (a soft reset, a mode change).
    {
        static bool8 sInitialised = FALSE;

        if (!sInitialised)
        {
            sInitialised = TRUE;
            PsgReset();
        }
    }

    // Master switch. With the APU off, the hardware has no output.
    if (!(REG_SOUNDCNT_X & 0x80))
    {
        for (i = 0; i < n * 2; i++)
            out[i] = 0;
        return;
    }

    // CgbSound writes the registers again once each frame. Thus one read for
    // each render call stays in step with the engine.
    //
    // NR10 first: pulse_sync starts the sweep unit on a trigger, and needs the
    // period and shift loaded before then.
    {
        u8 r0 = REG_NR10;

        sPulse[0].sweepPeriod = (r0 >> 4) & 7;
        sPulse[0].sweepDir = (r0 >> 3) & 1;
        sPulse[0].sweepShift = r0 & 7;
    }
    pulse_sync(&sPulse[0], (vu8 *)REG_ADDR_NR11, (vu8 *)REG_ADDR_NR12,
               (vu8 *)REG_ADDR_NR13, (vu8 *)REG_ADDR_NR14, sampleRate);
    pulse_sync(&sPulse[1], (vu8 *)REG_ADDR_NR21, (vu8 *)REG_ADDR_NR22,
               (vu8 *)REG_ADDR_NR23, (vu8 *)REG_ADDR_NR24, sampleRate);
    wave_sync(&sWave, sampleRate);
    noise_sync(&sNoise, sampleRate);

    nr50 = REG_NR50;
    nr51 = REG_NR51;
    rightVol = nr50 & 7;
    leftVol = (nr50 >> 4) & 7;
    ratio = kPsgRatio[REG_SOUNDCNT_H & 3];

    // Master volume, PSG to DirectSound ratio and output gain, as one
    // multiplier for each side, calculated once. The 28 is 7 (NR50 full scale)
    // times 4 (the denominator of the ratio). ARMv6 has no divide instruction,
    // so this must stay out of the loop for each sample.
    leftVol = (leftVol * PSG_GAIN * ratio) / 28;
    rightVol = (rightVol * PSG_GAIN * ratio) / 28;

    seqStepInc = (u32)(((u64)512u << 16) / (u32)sampleRate);

    for (i = 0; i < n; i++)
    {
        s32 right = 0, left = 0;
        u8 s1, s2, s3, s4;

        // ---- frame sequencer, 512 Hz ----
        sSeqPhase += seqStepInc;
        while (sSeqPhase >= (1u << 16))
        {
            sSeqPhase -= (1u << 16);
            sSeqStep = (u8)((sSeqStep + 1) & 7);

            if ((sSeqStep & 1) == 0)   // steps 0,2,4,6: length at 256 Hz
            {
                if (sPulse[0].lengthOn && sPulse[0].length && --sPulse[0].length == 0)
                    sPulse[0].on = 0;
                if (sPulse[1].lengthOn && sPulse[1].length && --sPulse[1].length == 0)
                    sPulse[1].on = 0;
                if (sWave.lengthOn && sWave.length && --sWave.length == 0)
                    sWave.on = 0;
                if (sNoise.lengthOn && sNoise.length && --sNoise.length == 0)
                    sNoise.on = 0;
            }
            if (sSeqStep == 2 || sSeqStep == 6)   // sweep at 128 Hz
            {
                if (pulse_clock_sweep(&sPulse[0]))
                    sPulse[0].step = pulse_step(sPulse[0].sweepShadow, sampleRate);
            }
            if (sSeqStep == 7)                    // envelope at 64 Hz
            {
                pulse_clock_env(&sPulse[0]);
                pulse_clock_env(&sPulse[1]);
                noise_clock_env(&sNoise);
            }
        }

        // ---- generators ----
        s1 = pulse_output(&sPulse[0]);
        s2 = pulse_output(&sPulse[1]);
        s3 = wave_output(&sWave);
        s4 = noise_output(&sNoise);

        // Masked to the waveform length, not left to wrap at 2^32. The duty
        // lookup needs (phase mod 8) anyway. An unbounded accumulator would at
        // some time make the noise delta below read as tens of thousands of
        // LFSR clocks in one sample.
        sPulse[0].phase = (sPulse[0].phase + sPulse[0].step) & ((8u << 16) - 1);
        sPulse[1].phase = (sPulse[1].phase + sPulse[1].step) & ((8u << 16) - 1);
        sWave.phase = (sWave.phase + sWave.step) & ((32u << 16) - 1);
        {
            u32 clocks;

            sNoise.phase += sNoise.step;
            clocks = sNoise.phase >> 16;
            sNoise.phase &= 0xFFFFu;
            noise_advance(&sNoise, clocks);
        }

        // ---- panning (NR51) ----
        if (nr51 & 0x01) right += s1;
        if (nr51 & 0x02) right += s2;
        if (nr51 & 0x04) right += s3;
        if (nr51 & 0x08) right += s4;
        if (nr51 & 0x10) left += s1;
        if (nr51 & 0x20) left += s2;
        if (nr51 & 0x40) left += s3;
        if (nr51 & 0x80) left += s4;

        left *= leftVol;
        right *= rightVol;

        // ---- DC blocker, for each side ----
        //
        // The DAC of the GB idles at mid-scale, so a channel that turns off
        // would make a step in the output. This tracks the mean and subtracts
        // it. That removes both the offset and the click, as the output
        // capacitor of the console does.
        sDcAccL += ((left << 8) - sDcAccL) >> DC_SHIFT;
        sDcAccR += ((right << 8) - sDcAccR) >> DC_SHIFT;
        left -= sDcAccL >> 8;
        right -= sDcAccR >> 8;

        if (left > 32767) left = 32767;
        else if (left < -32768) left = -32768;
        if (right > 32767) right = 32767;
        else if (right < -32768) right = -32768;

        out[i * 2] = (s16)left;
        out[i * 2 + 1] = (s16)right;
    }
}
