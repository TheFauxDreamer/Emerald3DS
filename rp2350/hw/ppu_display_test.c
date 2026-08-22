// PPU + display integration test on real hardware. Loads a captured GBA memory
// snapshot (an overworld frame), renders it through the software PPU into an
// SRAM framebuffer, and scans it out over HSTX (2x, centred). This is the first
// real Pokemon Emerald frame produced by the C PPU shown on the monitor.
//
// snapshot_data.h is generated (gitignored) from a tools/wasm_ppu_dump.mjs dump
// directory's reg/pal/vram/oam .bin files. Pick a frame with DISPCNT != 0 (many
// transition frames are display-off and render legitimately black), e.g.:
//   node tools/wasm_ppu_dump.mjs tools/wasm_replays/mudkip_starter.txt /tmp/d --no-build
//   then emit C arrays snap_reg/pal/vram/oam from /tmp/d/023230-route101-rescue-start/.

#include <stdio.h>

#include "pico/stdlib.h"
#include "../ppu.h"
#include "../hstx_display.h"
#include "snapshot_data.h"   // const snap_reg/pal/vram/oam

static uint16_t fb[DISP_FB_W * DISP_FB_H];          // 76.8 KB RGB565 framebuffer
static uint8_t  ppu_layer[PPU_PIXELS];              // PPU per-pixel layer scratch

int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    ppu_set_memory(snap_reg, snap_pal, snap_vram, snap_oam);
    ppu_render_rgb565(fb, ppu_layer);
    printf("PPU rendered snapshot: fb[80,120]=%04x fb[0,0]=%04x\n",
           fb[80 * DISP_FB_W + 120], fb[0]);

    hstx_display_init(fb);

    uint n = 0;
    while (1) {
        printf("ppu_display_test: overworld snapshot -> PPU -> 640x480 (%u)\n", n++);
        sleep_ms(1000);
    }
}
