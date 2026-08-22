// I2S audio output driver for the RP2350 Pokemon Emerald port (PCM5102A DAC).
//
// A PIO state machine (see i2s_audio.pio) clocks 16-bit stereo I2S out of three
// GPIOs; a DMA channel streams a ping-pong pair of sample buffers into the PIO
// TX FIFO with no CPU in the sample path. When a buffer drains, the DMA IRQ
// re-arms the other (already-filled) buffer and calls a fill callback to refill
// the one that just finished.
//
// Wiring (PCM5102A module):
//   DIN  -> GP20      module DIN
//   BCK  -> GP21      module BCK   (BCK and LRCK must stay adjacent, BCK first)
//   LRCK -> GP22      module LCK / LRCK
//   SCK  -> GND       (internal PLL: no master clock needed)
//   FMT  -> GND       (I2S format)   XSMT -> 3V3 (un-mute)
//   VIN  -> 3V3       GND -> GND
//
// PIO0/PIO1 are otherwise unused in this firmware; the display is on HSTX. The
// scanout owns DMA_IRQ_0, so this driver uses DMA_IRQ_1. The fill callback runs
// in that DMA IRQ on whichever core calls i2s_audio_init -- keep it cheap (copy
// from a thread-filled ring; never run the m4a mixer inside it).

#ifndef RP2350_I2S_AUDIO_H
#define RP2350_I2S_AUDIO_H

#include <stdint.h>

// Default pinout (see header comment). Must satisfy: LRCK == BCK + 1.
#define I2S_DIN_PIN  20
#define I2S_BCK_PIN  21
#define I2S_LRCK_PIN 22

// Stereo frames per DMA buffer. 256 frames @ 13.44 kHz = ~19 ms, so the IRQ
// fires roughly once per game frame with plenty of refill slack. Kept modest
// because the game build parks both buffers in IWRAM slack (see i2s_audio.c).
#ifndef I2S_FRAMES_PER_BUF
#define I2S_FRAMES_PER_BUF 256
#endif

// Fill callback: write 'nframes' packed stereo samples (one uint32 each, see
// i2s_pack) into 'dst'. Called from the DMA IRQ when a buffer drains. MUST be
// cheap and non-blocking -- do the m4a mix in thread context and copy here.
typedef void (*i2s_fill_fn)(void *ctx, uint32_t *dst, uint32_t nframes);

// Claim a PIO SM + DMA channel, configure GP20-22, and start output at
// 'sample_rate' Hz. Both buffers start as silence and are then driven by
// 'fill'. If 'fill' is NULL, output stays silent. Binds the DMA IRQ to the
// calling core. Returns true on success (false if no PIO/SM/DMA was free).
int i2s_audio_init(uint32_t sample_rate, i2s_fill_fn fill, void *ctx);

// Stop output and release the PIO SM, DMA channel, and IRQ handler.
void i2s_audio_deinit(void);

// Pack a signed 16-bit L/R pair into one DMA word. Right is the high half to
// match i2s_audio.pio's bitloop order; swap if channels come out reversed.
static inline uint32_t i2s_pack(int16_t left, int16_t right) {
    return ((uint32_t)(uint16_t)right << 16) | (uint16_t)(uint16_t)left;
}

// Telemetry: total buffers played (advances ~ sample_rate / I2S_FRAMES_PER_BUF
// per second while alive) and the raw PIO TX FIFO level (0..8; should hover
// non-empty -- a sustained 0 means starvation).
uint32_t i2s_audio_buffers_played(void);
uint32_t i2s_audio_fifo_level(void);

// Diagnostics: the DMA channel claimed (-1 if init failed), the PIO block index
// (0/1) and SM, and whether init succeeded. Used by the liveness telemetry to
// tell "init failed / resource collision" apart from "DMA stalled".
int i2s_audio_dma_chan(void);
int i2s_audio_pio_index(void);
int i2s_audio_sm(void);

#endif // RP2350_I2S_AUDIO_H
