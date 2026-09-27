// Software PPU for the RP2350 Pokemon Emerald port.
//
// A faithful C port of the reference rasteriser in pokeemerald-wasm/web/app.js.
// It reads the GBA-layout I/O registers, palette, VRAM and OAM (which the game
// still writes at their original offsets, now plain SRAM) and produces a
// 240x160 framebuffer. The module is address-agnostic: call ppu_set_memory()
// with the four region base pointers so the same code runs on-device against
// the SRAM map AND on the host against a captured memory snapshot.

#ifndef RP2350_PPU_H
#define RP2350_PPU_H

#include <stdint.h>

#define PPU_WIDTH  240
#define PPU_HEIGHT 160
#define PPU_PIXELS (PPU_WIDTH * PPU_HEIGHT)

// The widest margin that ppu_set_wide() can ask for, in pixels on each side of
// the 240. A build that never draws margins keeps 0, and the PPU then compiles
// to what it was before margins existed. Only the 3DS build sets it (to 16, in
// 3ds/Makefile). Every file that includes this header must see the same value.
#ifndef PPU_MAX_MARGIN
#define PPU_MAX_MARGIN 0
#endif

// The layer scratch that each render takes. With no margin it is PPU_PIXELS.
#define PPU_LAYER_STRIDE (PPU_WIDTH + 2 * PPU_MAX_MARGIN)
#define PPU_LAYER_BYTES  (PPU_LAYER_STRIDE * PPU_HEIGHT)

// Point the PPU at the four GBA memory regions:
//   reg  - I/O registers   (GBA 0x04000000, >= 0x60 bytes used)
//   pal  - palette RAM      (GBA 0x05000000, 0x400 bytes)
//   vram - video RAM        (GBA 0x06000000, 0x18000 bytes)
//   oam  - object attr RAM  (GBA 0x07000000, 0x400 bytes)
void ppu_set_memory(const void *reg, const void *pal, const void *vram, const void *oam);

// Render the current frame into an RGB888 buffer (PPU_PIXELS*3 bytes), using
// 'layer' (PPU_LAYER_BYTES) as per-pixel layer scratch. RGB888 matches app.js
// gbaColor() byte-for-byte so output can be pixel-diffed against the reference.
// This is the validation reference path; the device uses ppu_render_rgb565.
void ppu_render_rgb888(uint8_t *img, uint8_t *layer);

// Render the current frame straight into an RGB565 buffer (out: PPU_PIXELS
// uint16_t) for the HSTX scanout, using 'layer' (PPU_LAYER_BYTES) as per-pixel
// layer scratch. No RGB888 scratch needed — pixels are quantised on store and
// the "below" pixel is unpacked from 565 for alpha blending. The component
// pipeline is otherwise identical to ppu_render_rgb888, so results match within
// the 565 round-trip rounding (<=1 LSB/channel), not byte-for-byte.
void ppu_render_rgb565(uint16_t *out, uint8_t *layer);

// The same render, composed straight into 'out' at a row stride of 'stride'
// uint16_t (>= PPU_WIDTH), with no intermediate line buffer. It saves the
// per-line seed and flush memcpys, 153,600 bytes a frame, AND lets a caller
// whose upload buffer is wider than 240 skip a second full-frame copy.
//
// Only for a target that is NOT scanned out while a line is half composed. The
// RP2350 scans out of its framebuffer directly and must keep using
// ppu_render_rgb565; a caller that reads the picture only after the render has
// returned, such as a texture upload, can use this.
//
// 'layer' is PPU_LAYER_BYTES either way.
void ppu_render_rgb565_direct(uint16_t *out, int stride, uint8_t *layer);

#if PPU_MAX_MARGIN
// Draw 'margin' more pixels on each side of the 240, in the next
// ppu_render_rgb565_direct calls. The other two entry points never draw
// margins. 0 turns the margins off, and the render is then the same as with no
// margins at all. The value is rounded down to a multiple of 4 and limited to
// PPU_MAX_MARGIN.
//
// 'out' of ppu_render_rgb565_direct is still the pixel at GBA x = 0. Thus each
// row of the target must have 'margin' valid pixels before it and after its
// 240, and the stride must hold 240 + 2 * margin.
//
// In the margins:
// - A text BG draws only if sideMap[bg] is not NULL. That map is a 64x32 ring of
//   text map entries, row by row. It holds the columns that a 256-wide map does
//   not, and the column of screen pixel x is
//   ((x + BGxHOFS + sideDelta) & 511) >> 3. Its rows are the rows of the VRAM
//   map. The 240 columns in the middle always come from VRAM.
// - An affine BG does not draw. The margin keeps the backdrop.
// - Sprites draw as they would on a wider screen.
// - A window edge at x = 0 extends to the left margin, and an edge at 240 or
//   more extends to the right margin.
void ppu_set_wide(int margin, const uint16_t *const sideMap[4], unsigned sideDelta);
#endif

#endif // RP2350_PPU_H
