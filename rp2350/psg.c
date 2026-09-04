// Software rendering of the GBA's four PSG (CGB) sound channels. See psg.h.
//
// Why a full register-level implementation rather than reading the m4a channel
// structs directly: CgbSound() does NOT drive volume per frame. It programs the
// hardware envelope and sweep units -- writing a step time and direction into
// NRx2 and re-triggering through NRx4 -- and then lets them ramp on their own
// between its 60 Hz updates. A synthesiser that ignored those units would get
// every non-trivial attack and decay wrong, so they are implemented here.
//
// Structure follows the hardware: a 512 Hz frame sequencer drives the length,
// envelope and sweep units, while each channel's waveform is generated from a
// Q16 phase accumulator stepped once per output sample. That is not
// cycle-accurate, but the audible behaviour is set by the sequencer, and the
// accumulator is exact to within one output sample at 13 kHz.
//
// Written from the Pan Docs / GBATEK register descriptions rather than ported
// from an emulator, so it carries no licence obligations into rp2350/.

#include "global.h"
#include "psg.h"

// ---------------------------------------------------------------- constants --

// Duty patterns, one bit per step of the 8-step cycle, bit N = step N.
//   12.5% 00000001   25% 10000001   50% 10000111   75% 01111110
static const u8 kDutyTable[4] = { 0x80, 0x81, 0xE1, 0x7E };

// Noise divisor by NR43 code. Code 0 is the "0.5" case, which is 8 here
// because the whole table is pre-multiplied by 16.
static const u8 kNoiseDivisor[8] = { 8, 16, 32, 48, 64, 80, 96, 112 };

// Wave channel output shift by NR32 bits 5-6: mute, 100%, 50%, 25%.
// Index 0 is handled as silence rather than a shift.
static const u8 kWaveShift[4] = { 4, 0, 1, 2 };

// PSG:DirectSound balance by SOUNDCNT_H bits 0-1: 25%, 50%, 100%, reserved.
// Expressed as a numerator over 4.
static const u8 kPsgRatio[4] = { 1, 2, 4, 4 };

// How loud one PSG step is in the output domain. A channel swings +/-7.5 after
// centring, so four channels at full tilt reach +/-30; at this gain that is
// +/-23040, comfortably inside a full-scale DirectSound note at +/-32512.
//
// This is the balance knob. If PSG drowns out the sampled instruments, or
// disappears under them, this is the number to move. 512 is chosen so that even
// the theoretical maximum (four channels at volume 15, both sides, master
// volume 7) lands at 30720 rather than clipping.
#define PSG_GAIN 512

// The output DC blocker's time constant, as a right shift. The GB's DAC idles
// at mid-scale and a channel switching off would otherwise step the output,
// which is heard as a click. Tracking the mean and subtracting it removes both
// the offset and the click, the same job the console's output capacitor does.
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
static s32 sDcAcc;        // Q16 running mean, for the DC blocker

void PsgReset(void)
{
    memset(sPulse, 0, sizeof(sPulse));
    memset(&sWave, 0, sizeof(sWave));
    memset(&sNoise, 0, sizeof(sNoise));
    sSeqPhase = 0;
    sSeqStep = 0;
    sDcAcc = 0;
    sNoise.lfsr = 0x7FFF;
}

// ------------------------------------------------------------- frequencies --
//
// A pulse channel's output frequency is 131072 / (2048 - x) Hz and its duty
// pattern has 8 steps, so the step rate is 1048576 / (2048 - x). The wave
// channel walks 32 samples at 2097152 / (2048 - x). Both are computed in 64-bit
// because the numerator shifted up by 16 does not fit in 32.

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

    // Shift 14 and 15 are documented as not producing output.
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

// Returns the new frequency, and clears `on` if it overflowed, which is how the
// hardware stops a rising sweep that runs off the top of the range.
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

// Returns TRUE when the shadow frequency actually moved, so the caller only
// recomputes the phase step then. Recomputing it unconditionally would overwrite
// the pitch of every non-sweeping note from a shadow register that was never
// loaded.
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

// ------------------------------------------------------------- register read --
//
// Trigger detection by polling rather than by intercepting writes: nothing here
// sees the store CgbSound makes, so a set bit 7 in NRx4 is taken as "this note
// was just started" and then cleared, which is what the hardware does to that
// bit anyway. Without the clear, every frame would restart every note.

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
        // CgbSound re-triggers EVERY frame it changes volume, not just when a
        // note starts: it writes the freshly computed envelope volume into
        // NRx2's top nibble and then sets bit 7 to make the hardware latch it.
        // So a trigger here means "take this volume", and only a trigger on a
        // channel that was off means "a note began".
        bool8 wasOff = !p->on;

        *nrx4 = (u8)(r4 & 0x7F);

        p->on = 1;
        p->volume = r2 >> 4;
        p->envTimer = 0;

        // Deliberately NO phase reset. Hardware does not reset the duty step on
        // trigger, and doing it here would restart the waveform 60 times a
        // second, which is heard as a buzz rather than a note.

        // Length comes from NRx1 the way the hardware reads it, whatever the
        // engine chose to put there. Reloaded on a real note start, or when the
        // counter has run out.
        if (wasOff || p->length == 0)
            p->length = (u16)(64 - (r1 & 0x3F));

        if (wasOff)
        {
            // Sweep arms from the frequency the note actually started on.
            p->sweepShadow = freq;
            p->sweepTimer = 0;
            p->sweepOn = (p->sweepPeriod != 0 || p->sweepShift != 0);
        }
    }

    // Volume 0 with a downward envelope leaves the DAC off, which silences the
    // channel outright rather than merely making it quiet.
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
        // Unlike the pulse channels, CgbSound triggers channel 3 once per note
        // (it clears its own bit 7 afterwards), so this really is a note start
        // and the wave position may be reset, which hardware does.
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
        // Same per-frame re-trigger as the pulse channels, so the LFSR is only
        // reloaded on a genuine note start. Reseeding it every frame would make
        // the noise periodic at 60 Hz, which sounds like a rasp rather than a
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

    // 32 nibble samples across the 16 bytes CgbSound loaded, high nibble first.
    idx = (w->phase >> 16) & 31;
    sample = ram[idx >> 1];
    sample = (idx & 1) ? (sample & 0xF) : (sample >> 4);

    return (u8)(sample >> w->shift);
}

static u8 noise_output(struct PsgNoise *nz)
{
    if (!nz->on)
        return 0;

    // Bit 0 low means output high, matching the hardware's inversion.
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

// -------------------------------------------------------------------- render --

void PsgRender(s16 *out, s32 n, s32 sampleRate)
{
    u8 nr50, nr51;
    s32 rightVol, leftVol, ratio;
    u32 seqStepInc;
    s32 i;

    if (out == NULL || n <= 0 || sampleRate <= 0)
        return;

    // Lazy init, so no caller has to remember to reset us. PsgReset() stays
    // public for an explicit reset (a soft reset, a mode change).
    {
        static bool8 sInitialised = FALSE;

        if (!sInitialised)
        {
            sInitialised = TRUE;
            PsgReset();
        }
    }

    // Master switch. With the APU off the hardware outputs nothing at all.
    if (!(REG_SOUNDCNT_X & 0x80))
    {
        for (i = 0; i < n; i++)
            out[i] = 0;
        return;
    }

    // Registers are rewritten by CgbSound once per frame, so reading them once
    // per render call is exactly in step with the engine.
    // NR10 first: pulse_sync arms the sweep unit on a trigger and needs the
    // period and shift already loaded when it does.
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

    seqStepInc = (u32)(((u64)512u << 16) / (u32)sampleRate);

    for (i = 0; i < n; i++)
    {
        s32 right = 0, left = 0, mono;
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

        // Masked to the waveform length rather than left to wrap at 2^32:
        // (phase mod 8) is what the duty lookup wants anyway, and an unbounded
        // accumulator would eventually make the noise delta below read as tens
        // of thousands of LFSR clocks in a single sample.
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

        // ---- panning (NR51), then master volume (NR50) ----
        if (nr51 & 0x01) right += s1;
        if (nr51 & 0x02) right += s2;
        if (nr51 & 0x04) right += s3;
        if (nr51 & 0x08) right += s4;
        if (nr51 & 0x10) left += s1;
        if (nr51 & 0x20) left += s2;
        if (nr51 & 0x40) left += s3;
        if (nr51 & 0x80) left += s4;

        // (left*lv/7 + right*rv/7) / 2, folded into a single divide.
        mono = (left * leftVol + right * rightVol) / 14;

        // ---- DC blocker, then scale into the DirectSound domain ----
        sDcAcc += ((mono << 16) - sDcAcc) >> DC_SHIFT;
        mono -= sDcAcc >> 16;

        mono = (mono * PSG_GAIN * ratio) >> 2;

        if (mono > 32767)
            mono = 32767;
        else if (mono < -32768)
            mono = -32768;

        out[i] = (s16)mono;
    }
}
