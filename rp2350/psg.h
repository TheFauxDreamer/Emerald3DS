// The GBA's four PSG (CGB) sound channels, rendered in software.
//
// The GBA mixes two DirectSound sample channels with the Game Boy's original
// square/square/wave/noise generators. rp2350/m4a_1.c renders the DirectSound
// half; this renders the other half, which is roughly half of Emerald's real
// voices and the reason music sounds thin without it.
//
// Nothing here knows about m4a. CgbSound() (src/m4a.c) already maintains all
// four channels and writes real values into REG_NR10..REG_NR44, the wave RAM
// and SOUNDCNT_L/H/X, exactly as it would on hardware. This file only reads
// those registers and turns them into samples, the same job the GBA's audio
// hardware does, so the two stay decoupled.

#ifndef GUARD_RP2350_PSG_H
#define GUARD_RP2350_PSG_H

#include "global.h"

// Clear all channel state. Call before the first render; safe to call again.
void PsgReset(void);

// Render `n` mono samples into `out`, in the same signed domain the
// DirectSound mix uses shifted left by 8 -- a full-scale DirectSound sample of
// 127 corresponds to 127 << 8 here, so the two can simply be added.
//
// `sampleRate` is the mixer's output rate in Hz (SoundInfo.pcmFreq, ~13379).
void PsgRender(s16 *out, s32 n, s32 sampleRate);

#endif // GUARD_RP2350_PSG_H
