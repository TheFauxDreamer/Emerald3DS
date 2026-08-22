// I2S audio bring-up test: plays a 440 Hz sine on the PCM5102A so the wiring
// (DIN=GP20, BCK=GP21, LRCK=GP22, SCK->GND, XSMT->3V3, FMT->GND) can be
// validated on real silicon before wiring m4a into the game -- mirrors the
// per-feature bring-up tests (hstx_test, psram_test).
//
// Expect a clean, steady tone. Symptoms and likely causes:
//   - silence                -> XSMT not pulled high, or DIN/BCK/LRCK swapped
//   - buzz / wrong pitch      -> BCK/LRCK not adjacent, or clkdiv wrong
//   - one channel only        -> L/R pack order (see i2s_pack); harmless here
//   - "played" not advancing  -> DMA/PIO not running (check the printf below)

#include <math.h>
#include <stdio.h>

#include "pico/stdlib.h"

#include "i2s_audio.h"

#define SAMPLE_RATE 32000u
#define TONE_HZ     440u
#define AMPLITUDE   8000        // ~ -12 dBFS, comfortable on headphones

// 256-entry sine LUT (filled in main; the fill callback runs in IRQ context so
// it only does a table lookup + phase advance, never sinf()).
static int16_t sine_lut[256];

// 8.24 phase accumulator; step = TONE_HZ / SAMPLE_RATE of a full turn per frame.
static uint32_t phase;
static const uint32_t phase_step =
    (uint32_t)(((uint64_t)TONE_HZ << 24) / SAMPLE_RATE);

static void fill_sine(void *ctx, uint32_t *dst, uint32_t nframes) {
    (void)ctx;
    uint32_t p = phase;
    for (uint32_t i = 0; i < nframes; i++) {
        int16_t s = sine_lut[(p >> 16) & 0xFF];
        dst[i] = i2s_pack(s, s);
        p += phase_step;
    }
    phase = p;
}

int main(void) {
    stdio_init_all();
    sleep_ms(1500);
    printf("\n=== RP2350 I2S audio test (PCM5102A) ===\n");
    printf("pins: DIN=GP%d BCK=GP%d LRCK=GP%d  rate=%u Hz tone=%u Hz\n",
           I2S_DIN_PIN, I2S_BCK_PIN, I2S_LRCK_PIN, SAMPLE_RATE, TONE_HZ);

    for (int i = 0; i < 256; i++) {
        sine_lut[i] = (int16_t)lrintf(AMPLITUDE * sinf(2.0f * (float)M_PI * i / 256.0f));
    }

    if (!i2s_audio_init(SAMPLE_RATE, fill_sine, NULL)) {
        printf("!! i2s_audio_init failed (no free PIO/SM/DMA)\n");
        for (;;) tight_loop_contents();
    }
    printf("playing... (Ctrl-C the console to stop reading)\n");

    for (;;) {
        sleep_ms(2000);
        printf("played=%lu  fifo=%lu\n",
               (unsigned long)i2s_audio_buffers_played(),
               (unsigned long)i2s_audio_fifo_level());
    }
}
