// Shared seam between the two halves of the m4a port.
//
// The port splits the engine from the plumbing around it:
//
//   m4a_engine.c  reimplements every symbol src/m4a_1.s exports -- the song
//                 command interpreter and the PCM mixer. Portable C, because
//                 the RP2350's Cortex-M33 is Thumb-2 only and physically
//                 cannot execute the original ARMv4T ARM-mode assembly.
//
//   m4a_mix.c     the port's OWN additions: the frame entry points the host
//                 audio drivers call, the PSG summing that the GBA does in
//                 hardware, the telemetry, and the A/B switches. None of this
//                 exists in the reference at all.
//
// The split is along that line and not an arbitrary one, because the engine
// half is replaceable and the seam half is not. A target that CAN run the
// original assembly -- the 3DS, whose ARM11 is ARMv6K -- can assemble
// src/m4a_1.s and drop m4a_engine.c, and everything here still applies.
//
// Anything both halves touch is declared here. Everything is DEFINED in
// m4a_mix.c, which is the half that is always present.

#ifndef RP2350_M4A_PORT_H
#define RP2350_M4A_PORT_H

#include "global.h"
#include "gba/m4a_internal.h"

// Per-subsystem peaks, so "is there sound" can be answered for each half of the
// mixer separately instead of for the sum. A silent PSG with a healthy
// DirectSound reads very differently from both being silent.
extern volatile u32 gM4aDbgDsPeak;    // largest |sample| out of the DirectSound mix
extern volatile u32 gM4aDbgPsgPeak;   // ... out of the PSG synthesiser
extern volatile u32 gM4aDbgCryPeak;   // ... out of the compressed/reverse path
extern volatile u32 gM4aDbgClipped;   // samples the final clamp had to catch

// Audio A/B switches, driven from the EXTRA tab. Default on, so a normal boot
// is the real mixer. See Rp2350SetAudioDebug in 3ds/bridge.h.
extern volatile u8 gM4aPsgOn;
extern volatile u8 gM4aReverbOn;
extern volatile u8 gM4aDsOn;

// Engine-side telemetry, published once per frame by the mixer entry points.
extern volatile u32 gM4aDbgIdent;
extern volatile s32 gM4aDbgSpvb;
extern volatile u32 gM4aDbgBgmStatus;
extern volatile u32 gM4aDbgZeroRet;

// Which window of pcmBuffer the engine rendered into this frame, as a sample
// offset. Defined by whichever engine is in use, because only it knows.
s32 Rp2350MixWindowOffset(void);

#endif // RP2350_M4A_PORT_H
