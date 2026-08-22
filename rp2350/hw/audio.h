// m4a -> I2S audio integration layer for the RP2350 Pokemon Emerald port.
//
// Sits between the m4a software mixer (the future C port of m4a_1.s's
// SoundMainRAM; see Rp2350MixFrame) and the I2S DMA driver (i2s_audio.c):
//   - Rp2350AudioFrame() runs the mixer once per game frame (thread context,
//     core 0) and pushes its PCM into a lock-free SPSC ring.
//   - the I2S DMA IRQ drains the ring into the PIO TX FIFO (see audio.c's
//     fill callback), expanding mono -> stereo, with silence on underrun.
// The ring decouples the game's per-frame, occasionally-stalling production
// from the steady DMA consumption, exactly like the GBA's DirectSound DMA
// double-buffer decouples the VBlank mixer from the sound hardware.

#ifndef RP2350_AUDIO_H
#define RP2350_AUDIO_H

#include <stdint.h>

// m4a produces this many signed-8-bit mono samples per VBlank at Emerald's
// SOUND_MODE_FREQ_13379 (gPcmSamplesPerVBlankTable index 4; m4a_internal.h).
#define AUDIO_SAMPLES_PER_FRAME 224

// I2S output rate. The game is locked to the 60.00 Hz scanout, so it produces
// AUDIO_SAMPLES_PER_FRAME samples 60x/s; matching the consume rate to that
// keeps the ring from slowly drifting full or empty. (The true GBA rate is
// 13379 Hz at 59.73 fps -- we run 0.5% fast because we're pinned to 60.)
#define AUDIO_SAMPLE_RATE (AUDIO_SAMPLES_PER_FRAME * 60)   // 13440 Hz

// Bring up the I2S driver + ring. Call once from main() on core 0, after the
// HSTX clocks are configured (it claims a PIO SM, a DMA channel, and DMA_IRQ_1
// on the calling core).
void Rp2350AudioInit(void);

// Run the mixer for one game frame and enqueue its PCM. Call once per game
// frame from Rp2350PresentFrame() (core 0, thread context).
void Rp2350AudioFrame(void);

// Telemetry for the liveness print: DMA buffers played, ring underruns (IRQ
// found the ring empty), and current ring occupancy in samples. Any pointer
// may be NULL.
void Rp2350AudioStats(uint32_t *played, uint32_t *underruns, uint32_t *ring_fill);

// THE MIXER SEAM. Produce up to 'n' signed-8-bit mono samples at
// AUDIO_SAMPLE_RATE into 'out'; return the number actually produced (0 ==
// silence). A weak default in audio.c returns 0. The audio milestone replaces
// it with the C port of SoundMainRAM, or a wrapper that calls m4aSoundMain()
// and copies the freshly-mixed half of gSoundInfo.pcmBuffer.
int Rp2350MixFrame(int8_t *out, int n);

#endif // RP2350_AUDIO_H
