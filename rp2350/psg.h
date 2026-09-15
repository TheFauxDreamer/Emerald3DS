// The four PSG (CGB) sound channels of the GBA, rendered in software.
//
// The GBA mixes two DirectSound sample channels with the original Game Boy
// generators: square, square, wave and noise. The file rp2350/m4a_mix.c adds
// this half to the DirectSound half. This half has about half of the real
// voices in Emerald, and without it the music sounds thin.
//
// Nothing here knows about m4a. CgbSound() (src/m4a.c) already keeps all four
// channels, and writes real values into REG_NR10..REG_NR44, the wave RAM and
// SOUNDCNT_L/H/X, as on hardware. This file only reads those registers and
// makes samples from them, as the audio hardware of the GBA does. Thus the two
// parts stay independent.

#ifndef GUARD_RP2350_PSG_H
#define GUARD_RP2350_PSG_H

#include "global.h"

// Clear all channel state. Call this before the first render. A second call is
// safe.
void PsgReset(void);

// Render `n` sample frames into `out` as interleaved left and right pairs. Thus
// `out` must hold 2*n samples. The values use the signed range of the
// DirectSound mix, shifted left by 8. A full-scale DirectSound sample of 127 is
// 127 << 8 here, so the two can be added directly.
//
// Stereo, not mono, because NR51 pans each of the four channels, and the music
// of Emerald uses it. A mono mix here would lose the pan before later code
// could keep it.
//
// The `sampleRate` argument is the output rate of the mixer in Hz
// (SoundInfo.pcmFreq, about 13379).
void PsgRender(s16 *out, s32 n, s32 sampleRate);

#endif // GUARD_RP2350_PSG_H
