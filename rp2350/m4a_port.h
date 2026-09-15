// The shared interface between the two halves of the m4a port.
//
// The port keeps the engine apart from the code around it:
// - m4a_engine.c is a new version of each symbol that src/m4a_1.s exports: the
//   song command interpreter and the PCM mixer. It is portable C, because the
//   Cortex-M33 of the RP2350 can execute only Thumb-2, not the original ARMv4T
//   ARM-mode assembly.
// - m4a_mix.c holds the additions of the port. These are the frame entry points
//   for the host audio drivers, the PSG sum (hardware on a GBA), the telemetry
//   and the A/B switches. The original has none of this.
//
// The split is on that line because the engine half is replaceable, and the
// interface half is not. A target that can run the original assembly, such as
// the 3DS with its ARMv6K ARM11, can assemble src/m4a_1.s in place of
// m4a_engine.c. All of this file still applies.
//
// This file declares all that both halves use. The file m4a_mix.c defines all
// of it, because that half is always present.

#ifndef RP2350_M4A_PORT_H
#define RP2350_M4A_PORT_H

#include "global.h"
#include "gba/m4a_internal.h"

// The peak of each subsystem, so each half of the mixer can show if it has
// sound. A silent PSG with a good DirectSound is a different problem from two
// silent halves.
extern volatile u32 gM4aDbgDsPeak;    // largest |sample| of the DirectSound mix
extern volatile u32 gM4aDbgPsgPeak;   // the same, of the PSG synthesizer
extern volatile u32 gM4aDbgCryPeak;   // the same, of SpecialSample
extern volatile u32 gM4aDbgClipped;   // samples the final clamp had to catch

// How often the DirectSound accumulator wrapped.
//
// MixChannel adds each active channel into an s8 buffer, so a sum past +-127
// wraps to the opposite sign, and does not saturate. The GBA does the same. No
// other counter here shows it: the peak stays <= 127, but the waveform makes a
// full-scale step. With five active channels, check this first.
//
// It stays 0 in a build with the original assembly, which does its own mixing.
extern volatile u32 gM4aDbgDsWrap;

// Audio A/B switches, which the EXTRA tab sets. They are on by default, so a
// usual boot uses the real mixer. See Rp2350SetAudioDebug in 3ds/bridge.h.
extern volatile u8 gM4aPsgOn;
extern volatile u8 gM4aReverbOn;
extern volatile u8 gM4aDsOn;

// Engine telemetry, which the mixer entry points publish once each frame.
extern volatile u32 gM4aDbgIdent;
extern volatile s32 gM4aDbgSpvb;
extern volatile u32 gM4aDbgBgmStatus;
extern volatile u32 gM4aDbgZeroRet;

// The window of pcmBuffer that the engine rendered into on this frame, as a
// sample offset. The engine in use defines it, because only the engine knows.
s32 Rp2350MixWindowOffset(void);

#endif // RP2350_M4A_PORT_H
