// Display-driver test: render a recognizable pattern into a 240x160 RGB565
// framebuffer and scan it out over HSTX (2x upscaled, centred in 640x480).
// Verifies scaling, centring and orientation on the monitor before wiring the
// PPU. Uses an SRAM framebuffer first (no PSRAM-latency concerns).

#include <stdio.h>

#include "pico/stdlib.h"
#include "../hstx_display.h"

static uint16_t fb[DISP_FB_W * DISP_FB_H];

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

static void draw_pattern(void) {
    for (int y = 0; y < DISP_FB_H; y++) {
        for (int x = 0; x < DISP_FB_W; x++) {
            // Red ramps left->right, blue top->bottom: shows orientation + scale.
            uint8_t r = (uint8_t)(x * 255 / (DISP_FB_W - 1));
            uint8_t b = (uint8_t)(y * 255 / (DISP_FB_H - 1));
            uint16_t c = rgb565(r, 0, b);
            // 1px white frame around the whole 240x160 image (shows the content rect).
            if (x == 0 || y == 0 || x == DISP_FB_W - 1 || y == DISP_FB_H - 1)
                c = 0xffff;
            // 16px green square in the top-left corner marks the origin.
            if (x < 16 && y < 16) c = rgb565(0, 255, 0);
            fb[y * DISP_FB_W + x] = c;
        }
    }
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);
    draw_pattern();
    hstx_display_init(fb);

    uint n = 0;
    while (1) {
        printf("display_test: 240x160 -> 640x480 (2x), frame buffer in SRAM (%u)\n", n++);
        sleep_ms(1000);
    }
}
