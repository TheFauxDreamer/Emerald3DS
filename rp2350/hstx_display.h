// HSTX -> DVI/HDMI display driver for the RP2350 Pokemon Emerald port.
//
// Scans a 240x160 RGB565 framebuffer out as 640x480p60 over HSTX, upscaled 2x
// (to 480x320) and centred with black borders. Scanout is a self-running
// control-block DMA ring (see hstx_display.c): two control channels feed the
// data channel the whole frame structure from SRAM with no CPU involvement,
// so the signal survives arbitrarily late IRQs and multi-ms CPU stalls. The
// CPU's only per-frame job (in the advisory scanout IRQ) is copying upcoming
// framebuffer rows into the two content line blocks ahead of the beam.

#ifndef RP2350_HSTX_DISPLAY_H
#define RP2350_HSTX_DISPLAY_H

#include <stdint.h>

#define DISP_FB_W 240
#define DISP_FB_H 160

// Configure clocks (252 MHz sys -> 25.2 MHz pixel clock), the HSTX RGB565
// encoder, the GPIO12-19 TMDS pinout, and the ping/pong scanout DMA, then start
// output. 'fb' points at a DISP_FB_W*DISP_FB_H RGB565 framebuffer (SRAM or
// cached PSRAM). Returns immediately; scanout runs from the DMA IRQ thereafter.
// One-call form: configures hardware AND starts scanout on the calling core.
void hstx_display_init(const uint16_t *fb);

// Optional overlay composited over the framebuffer at scanout line-fill time
// (always on top; immune to render-order flicker). 'buf' is w*h RGB565 at
// framebuffer position (x, y); NULL disables. The buffer is read from the
// scanout IRQ -- keep it valid while set.
void hstx_display_set_overlay(const uint16_t *buf, int x, int y, int w, int h);

// Two-phase form: init_hw configures clocks/encoder/pins/DMA but does not
// start; start binds the scanout IRQ to the CALLING core's NVIC and begins
// output. The scanout IRQ must live on a core with no long IRQ-disabled
// sections (USB/stdio) -- in the game that is core 1.
void hstx_display_init_hw(const uint16_t *fb);
void hstx_display_start(void);

// Scanout health telemetry: completed output frames (should advance ~60/s) and
// the number of times the watchdog hard-reset the scanout.
uint32_t hstx_display_frame_count(void);
uint32_t hstx_display_restart_count(void);
// Raw HSTX_FIFO_STAT (LEVEL + sticky WOF overflow flag).
uint32_t hstx_display_fifo_stat(void);
// Max us between scanout IRQs since the last call (normal max ~95 us: one IRQ
// per control block, the longest covering 3 scanlines).
uint32_t hstx_display_max_irq_gap(void);
// Scanout DMA ring position (a raw read pointer). Advances continuously while
// scanout is alive, even with IRQs starved -- distinguishes "display fine,
// render core busy" from "scanout actually dead" in the watchdog.
uint32_t hstx_display_dma_pos(void);
// Microseconds until the beam next reaches the first content scanline
// (framebuffer row 0 on screen). Sweeps 16667 -> 0 then wraps. Granularity is
// one control block (<= 3 scanlines, ~95 us). Used to align render starts to
// the scanout so the live framebuffer never shows a tear seam.
uint32_t hstx_display_us_to_content_start(void);

// Full scanout reset (hard block reset of DMA+HSTX + reprogram + restart).
// With the control-block ring this is a should-never-fire backstop. Bumps
// restart_count. Callable from thread context.
void hstx_display_recover(void);

#endif // RP2350_HSTX_DISPLAY_H
