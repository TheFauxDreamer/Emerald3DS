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

// Point the PPU at the four GBA memory regions:
//   reg  - I/O registers   (GBA 0x04000000, >= 0x60 bytes used)
//   pal  - palette RAM      (GBA 0x05000000, 0x400 bytes)
//   vram - video RAM        (GBA 0x06000000, 0x18000 bytes)
//   oam  - object attr RAM  (GBA 0x07000000, 0x400 bytes)
void ppu_set_memory(const void *reg, const void *pal, const void *vram, const void *oam);

// Render the current frame into an RGB888 buffer (PPU_PIXELS*3 bytes), using
// 'layer' (PPU_PIXELS bytes) as per-pixel layer scratch. RGB888 matches app.js
// gbaColor() byte-for-byte so output can be pixel-diffed against the reference.
// This is the validation reference path; the device uses ppu_render_rgb565.
void ppu_render_rgb888(uint8_t *img, uint8_t *layer);

// Render the current frame straight into an RGB565 buffer (out: PPU_PIXELS
// uint16_t) for the HSTX scanout, using 'layer' (PPU_PIXELS bytes) as per-pixel
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
// 'layer' stays PPU_PIXELS bytes at a stride of PPU_WIDTH either way.
void ppu_render_rgb565_direct(uint16_t *out, int stride, uint8_t *layer);

#endif // RP2350_PPU_H
