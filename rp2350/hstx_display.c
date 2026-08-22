// HSTX 640x480p60 scanout of a 240x160 RGB565 framebuffer, 2x upscaled and
// centred. See hstx_display.h. The HSTX/clock setup mirrors the validated
// rp2350/hw/hstx_test.c colour-bar bring-up.
//
// SCANOUT IS A CONTROL-BLOCK DMA RING -- NO CPU IN THE RE-ARM LOOP.
//
// The previous ping-pong design re-armed the next line's DMA channel from the
// IRQ handler, so one IRQ serviced later than a scanline (~32 us) made the
// chain re-run a stale channel, slipped the HSTX expander's command/data
// phase, and corrupted the signal until a full block reset (a visible blink
// while the monitor re-synced). Here the re-arm itself is DMA:
//
//   DATA (ch0) streams one control block's words into the HSTX FIFO,
//     then chains to
//   CB_COUNT (ch1), which copies the next cb's word count into DATA's
//     AL3_TRANS_COUNT alias (no trigger), then chains to
//   CB_ADDR (ch2), which copies the next cb's read address into DATA's
//     AL3_READ_ADDR_TRIG alias, retriggering DATA.
//
// CB_COUNT/CB_ADDR read from two 256-entry rings (1024 B each, hardware
// read-ring wrap) that describe exactly one video frame, so the three
// channels regenerate the whole vertical structure forever with no CPU help.
// The IRQ is now advisory: it only copies upcoming framebuffer rows into the
// content blocks and counts vsyncs, re-deriving its position from CB_ADDR's
// read_addr every time (so late or coalesced IRQs self-resync). A late IRQ
// shows one stale scanline; it can no longer lose the signal. This also lets
// scanout survive multi-millisecond CPU stalls (e.g. future flash-save
// writes with XIP suspended), since everything it reads lives in SRAM.
//
// The 256 cbs tile the 525-line frame exactly -- no filler:
//   16 vblank cbs over 45 lines (multi-line command lists):
//     [0..3]    front porch, 10 lines  (3+3+2+2)
//     [4]       vsync,        2 lines
//     [5..15]   back porch,  33 lines  (11 x 3)
//   240 active cbs, each streaming a 256-word line block TWICE (count=512;
//   DATA's 1024 B read-ring wraps back to the block base) = 2x vertical
//   upscale for free:
//     [16..55]   top border,    40 cbs -> shared black block
//     [56..215]  content rows 0..159  -> content_block[row & 1]
//     [216..255] bottom border, 40 cbs -> shared black block
//
// Each line block is a self-contained 256-word scanline: sync commands,
// TMDS_REPEAT black side borders, and TMDS|480 with 240 pixel-doubled data
// words. Exactly 256 real words -- no NOP padding, because the expander pops
// at most one FIFO word per clk_hstx cycle, so a long NOP run would starve
// the output serialiser mid-stream (the datasheet calls this out explicitly).

#include "hstx_display.h"

#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/resets.h"
#include "hardware/timer.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/vreg.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/structs/timer.h"

// ---- DVI timing (CEA 640x480p60) -------------------------------------------
#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu
#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

// clk_hstx = clk_sys / HSTX_CLK_DIV; must yield 126 MHz (252 Mbps DDR TMDS).
#define HSTX_SYS_KHZ 252000
#define HSTX_CLK_DIV 2

#define MODE_H_FRONT_PORCH   16
#define MODE_H_SYNC_WIDTH    96
#define MODE_H_BACK_PORCH    48
#define MODE_H_ACTIVE_PIXELS 640
#define MODE_V_FRONT_PORCH   10
#define MODE_V_SYNC_WIDTH    2
#define MODE_V_BACK_PORCH    33
#define MODE_V_ACTIVE_LINES  480

// ---- 2x-upscaled, centred placement of the 240x160 image -------------------
#define HBORDER 80                 // (640 - 240*2) / 2
#define VBORDER 80                 // (480 - 160*2) / 2

#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

// ---- DMA channels -----------------------------------------------------------
#define DMACH_DATA     0
#define DMACH_CB_COUNT 1
#define DMACH_CB_ADDR  2

// ---- control-block rings (exactly fill SCRATCH_Y's free lower 2 KB) --------
#define CB_ENTRIES        256
#define CB_VBLANK_FIRST   0
#define CB_ACTIVE_FIRST   16    // top border starts here
#define CB_CONTENT_FIRST  56    // content row 0
#define CB_CONTENT_LAST   215   // content row 159
static uint32_t cb_count[CB_ENTRIES]
    __attribute__((aligned(1024), section(".scratch_y.cbring")));
static uint32_t cb_addr[CB_ENTRIES]
    __attribute__((aligned(1024), section(".scratch_y.cbring")));

// ---- line blocks ------------------------------------------------------------
// A self-contained 256-word scanline, streamed twice per cb (count=512) with
// DATA's read-ring wrapping at the 1024 B block boundary:
//   [0..7]    h-sync commands (front porch, sync, back porch; NOPs match the
//             proven pico-examples interleave)
//   [8..9]    TMDS_REPEAT|80 + black word          (left border)
//   [10]      TMDS|480
//   [11..250] 240 data words, one framebuffer pixel doubled per word
//   [251..252] TMDS_REPEAT|76 + black word         (right border, part 1)
//   [253..255] TMDS|4 + 2 black words              (right border, part 2;
//             splitting 80 as 76+4 makes the block EXACTLY 256 words)
// Border pixels go through TMDS_REPEAT (pop one word, re-encode repeatedly;
// the encoder keeps DC balance across repeats), so borders cost 2 words
// instead of 40 and the whole 640 px line fits one wrappable block.
#define BLOCK_WORDS 256
#define BLOCK_PX_OFF 11
// The game target's SDK RAM region is full; its black block rides in EWRAM
// slack instead (same SRAM, equally DMA-readable). Other targets use the
// default linker script, which has no .ewram_top section.
#ifdef DISP_BLACK_IN_EWRAM
#define BLACK_BLOCK_SECTION __attribute__((section(".ewram_top.dispblack")))
#else
#define BLACK_BLOCK_SECTION
#endif
static uint32_t content_block[2][BLOCK_WORDS] __attribute__((aligned(1024)));
static uint32_t black_block[BLOCK_WORDS]      __attribute__((aligned(1024))) BLACK_BLOCK_SECTION;

// Multi-line vblank command lists (7 words per line). Aligned so a list never
// crosses DATA's 1024 B read-ring boundary mid-stream.
static uint32_t vb_off3[21] __attribute__((aligned(128)));  // 3 lines, vsync off
static uint32_t vb_off2[14] __attribute__((aligned(64)));   // 2 lines, vsync off
static uint32_t vb_on2[14]  __attribute__((aligned(64)));   // 2 lines, vsync ON

static const uint16_t *g_fb;

// Optional small overlay (e.g. the FPS counter), composited over the
// framebuffer at line-fill time so it is always on top and immune to
// render-order flicker. 'ov_buf' is ov_w*ov_h RGB565; NULL disables.
static const uint16_t *volatile ov_buf;
static int ov_x, ov_y, ov_w, ov_h;

void hstx_display_set_overlay(const uint16_t *buf, int x, int y, int w, int h) {
    ov_buf = NULL;   // disable while the geometry changes (IRQ reads these)
    ov_x = x; ov_y = y; ov_w = w; ov_h = h;
    ov_buf = buf;
}

static uint32_t vsync_count = 0;
static uint32_t restart_count = 0;
static uint32_t last_irq_t = 0;
static uint32_t max_irq_gap = 0;
static int last_filled = -1;
static uint32_t prev_streaming = 0;

uint32_t hstx_display_frame_count(void)   { return vsync_count; }
uint32_t hstx_display_restart_count(void) { return restart_count; }
uint32_t hstx_display_fifo_stat(void)     { return hstx_fifo_hw->stat; }
// Scanout DMA position (CB_ADDR ring read pointer). Advances continuously
// while the ring runs, even if IRQs are starved -- the watchdog uses this to
// tell "display fine, core 1 busy" from "scanout actually dead".
uint32_t hstx_display_dma_pos(void) { return dma_hw->ch[DMACH_CB_ADDR].read_addr; }
// us until the beam next reaches content row 0 (the first content cb). Derives
// the position from CB_ADDR's read pointer like the IRQ does, so it is safe to
// call from any context. Uses the start line of the cb about to be fed -- at
// most 3 scanlines (~95 us) ahead of the true beam, which only makes callers
// start earlier (the safe direction for render alignment).
uint32_t hstx_display_us_to_content_start(void) {
    // Start line of each vblank cb (3+3+2+2 front porch, 2 vsync, 11x3 back
    // porch); active cb i covers lines 45 + 2*(i - CB_ACTIVE_FIRST).
    static const uint8_t vb_line[16] =
        {0, 3, 6, 8, 10, 12, 15, 18, 21, 24, 27, 30, 33, 36, 39, 42};
    uint32_t next = ((dma_hw->ch[DMACH_CB_ADDR].read_addr - (uintptr_t)cb_addr)
                     / 4) % CB_ENTRIES;
    uint32_t line = next < CB_ACTIVE_FIRST
        ? vb_line[next] : 45 + 2 * (next - CB_ACTIVE_FIRST);
    const uint32_t content_line = 45 + 2 * (CB_CONTENT_FIRST - CB_ACTIVE_FIRST);
    const uint32_t total = 525;
    uint32_t to_lines = (content_line + total - line) % total;
    return to_lines * 2000u / 63u;   // 31.746 us/line (800 px at 25.2 MHz)
}
// Max us between scanout IRQs since last call. One IRQ per cb: healthy max is
// ~95 us (a 3-line vblank cb). Larger gaps are harmless now (stale pixels at
// worst) but still indicate IRQ pressure on the render core.
uint32_t hstx_display_max_irq_gap(void) {
    uint32_t v = max_irq_gap;
    max_irq_gap = 0;
    return v;
}

// Copy framebuffer row 'row' into its content block, one doubled pixel per
// data word (the expander emits both 16-bit halves -> 2x horizontal upscale).
static void __scratch_x("disp") fill_row(int row) {
    const uint16_t *src = &g_fb[row * DISP_FB_W];
    uint32_t *out = &content_block[row & 1][BLOCK_PX_OFF];
    for (int x = 0; x < DISP_FB_W; x++) {
        uint32_t c = src[x];
        out[x] = c | (c << 16);
    }
    const uint16_t *ov = ov_buf;
    if (ov && row >= ov_y && row < ov_y + ov_h) {
        const uint16_t *orow = &ov[(row - ov_y) * ov_w];
        for (int x = 0; x < ov_w; x++) {
            uint32_t c = orow[x];
            out[ov_x + x] = c | (c << 16);
        }
    }
}

// Advisory IRQ, one per DATA cb completion (~every 2 scanlines). Derives the
// ring position from CB_ADDR's read pointer (never from its own bookkeeping,
// so late/coalesced IRQs self-resync), keeps the content blocks one row ahead
// of the beam, and counts frames. If it runs late the only artefact is a
// stale doubled scanline -- the DMA ring keeps the signal itself perfect.
void __scratch_x("disp") hstx_dma_irq(void) {
    uint32_t t = timer0_hw->timerawl;
    uint32_t gap = t - last_irq_t;
    last_irq_t = t;
    if (gap > max_irq_gap) max_irq_gap = gap;
    dma_hw->intr = 1u << DMACH_DATA;

    // CB_ADDR has normally already fed the next cb when we get here, so the
    // cb now streaming is (next - 1). Catching the chain mid-flight reads one
    // lower; the fill below is idempotent, so that costs nothing.
    uint32_t next = (dma_hw->ch[DMACH_CB_ADDR].read_addr - (uintptr_t)cb_addr) / 4;
    uint32_t streaming = (next + CB_ENTRIES - 1) % CB_ENTRIES;

    if (streaming < prev_streaming) vsync_count++;
    prev_streaming = streaming;

    // While content row r streams, fill row r+1 into the block that just went
    // idle (it last held row r-1). During vblank/borders, prepare row 0.
    int target = 0;
    if (streaming >= CB_CONTENT_FIRST && streaming <= CB_CONTENT_LAST)
        target = (int)(streaming - CB_CONTENT_FIRST) + 1;
    if (target < DISP_FB_H && target != last_filled) {
        fill_row(target);
        last_filled = target;
    }
}

// ---- static frame construction ----------------------------------------------
// 7 words of h-sync commands for one vblank line (no active video: the back
// porch repeat covers the active region too).
static uint32_t *emit_vblank_line(uint32_t *p, uint32_t sync_hi, uint32_t sync_lo) {
    *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH;
    *p++ = sync_hi;
    *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH;
    *p++ = sync_lo;
    *p++ = HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS);
    *p++ = sync_hi;
    *p++ = HSTX_CMD_NOP;
    return p;
}

// Build one 256-word active-line block around an existing pixel area.
static void build_line_block(uint32_t *b) {
    b[0] = HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH;
    b[1] = SYNC_V1_H1;
    b[2] = HSTX_CMD_NOP;
    b[3] = HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH;
    b[4] = SYNC_V1_H0;
    b[5] = HSTX_CMD_NOP;
    b[6] = HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH;
    b[7] = SYNC_V1_H1;
    b[8] = HSTX_CMD_TMDS_REPEAT | HBORDER;
    b[9] = 0;                                   // black, doubled
    b[10] = HSTX_CMD_TMDS | (DISP_FB_W * 2);
    // [11..250] pixel data, owned by fill_row (black block: stays zero)
    b[251] = HSTX_CMD_TMDS_REPEAT | (HBORDER - 4);
    b[252] = 0;
    b[253] = HSTX_CMD_TMDS | 4;
    b[254] = 0;
    b[255] = 0;
}

static void build_frame_ring(void) {
    uint32_t *p;
    p = vb_off3;
    for (int i = 0; i < 3; i++) p = emit_vblank_line(p, SYNC_V1_H1, SYNC_V1_H0);
    p = vb_off2;
    for (int i = 0; i < 2; i++) p = emit_vblank_line(p, SYNC_V1_H1, SYNC_V1_H0);
    p = vb_on2;
    for (int i = 0; i < 2; i++) p = emit_vblank_line(p, SYNC_V0_H1, SYNC_V0_H0);

    build_line_block(content_block[0]);
    build_line_block(content_block[1]);
    build_line_block(black_block);

    int n = 0;
    // Front porch: 3+3+2+2 = 10 lines.
    cb_count[n] = 21; cb_addr[n] = (uintptr_t)vb_off3; n++;
    cb_count[n] = 21; cb_addr[n] = (uintptr_t)vb_off3; n++;
    cb_count[n] = 14; cb_addr[n] = (uintptr_t)vb_off2; n++;
    cb_count[n] = 14; cb_addr[n] = (uintptr_t)vb_off2; n++;
    // Vsync: 2 lines.
    cb_count[n] = 14; cb_addr[n] = (uintptr_t)vb_on2; n++;
    // Back porch: 11 x 3 = 33 lines.
    for (int i = 0; i < 11; i++) {
        cb_count[n] = 21; cb_addr[n] = (uintptr_t)vb_off3; n++;
    }
    // Active: 240 line-pair cbs, each block streamed twice via read-ring wrap.
    for (int i = 0; i < MODE_V_ACTIVE_LINES / 2; i++) {
        int dy = 2 * i;
        cb_count[n] = 2 * BLOCK_WORDS;
        if (dy >= VBORDER && dy < VBORDER + DISP_FB_H * 2)
            cb_addr[n] = (uintptr_t)content_block[((dy - VBORDER) / 2) & 1];
        else
            cb_addr[n] = (uintptr_t)black_block;
        n++;
    }
    // The tiling above must consume the rings exactly (hardware wrap at 256).
    if (n != CB_ENTRIES) panic("cb ring tiling: %d", n);
}

// Program the HSTX expander/serialiser (RGB565 TMDS encoder: lane2=red(5b),
// lane1=green(6b), lane0=blue(5b)). Shared by init and wedge recovery.
static void hstx_setup_encoder(void) {
    hstx_ctrl_hw->expand_tmds =
        4  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
        8  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB   |
        5  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
        3  << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |
        4  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
        29 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;
    hstx_ctrl_hw->expand_shift =
        2 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        16 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;
}

// TMDS pinout (Pico DVI Sock layout): clk on GP14/15, D0->GP12, D1->GP18,
// D2->GP16. Shared by init and recovery (an HSTX block reset clears bit[]).
static void hstx_setup_lanes(void) {
    hstx_ctrl_hw->bit[2] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[3] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    for (uint lane = 0; lane < 3; ++lane) {
        static const int lane_to_output_bit[3] = {0, 6, 4};
        int bit = lane_to_output_bit[lane];
        uint32_t sel = (lane * 10) << HSTX_CTRL_BIT0_SEL_P_LSB |
                       (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        hstx_ctrl_hw->bit[bit] = sel;
        hstx_ctrl_hw->bit[bit + 1] = sel | HSTX_CTRL_BIT0_INV_BITS;
    }
}

// Configure the three ring channels from scratch and start output at the top
// of the vertical sequence. Shared by init and recovery.
static void hstx_start_dma(void) {
    last_filled = -1;
    prev_streaming = 0;

    dma_channel_config c;

    // DATA: control-block words -> HSTX FIFO, DREQ-paced. Read-ring at 1024 B
    // so a count=512 cb streams its 1024 B-aligned line block twice.
    c = dma_channel_get_default_config(DMACH_DATA);
    channel_config_set_dreq(&c, DREQ_HSTX);
    channel_config_set_chain_to(&c, DMACH_CB_COUNT);
    channel_config_set_ring(&c, false, 10);
    channel_config_set_high_priority(&c, true);
    dma_channel_configure(DMACH_DATA, &c, &hstx_fifo_hw->fifo,
                          vb_off3 /*placeholder; cb ring overwrites*/, 21, false);

    // CB_COUNT: next cb's word count -> DATA's AL3_TRANS_COUNT (no trigger).
    c = dma_channel_get_default_config(DMACH_CB_COUNT);
    channel_config_set_chain_to(&c, DMACH_CB_ADDR);
    channel_config_set_ring(&c, false, 10);
    channel_config_set_high_priority(&c, true);
    dma_channel_configure(DMACH_CB_COUNT, &c,
                          &dma_hw->ch[DMACH_DATA].al3_transfer_count,
                          cb_count, 1, false);

    // CB_ADDR: next cb's read address -> DATA's AL3_READ_ADDR_TRIG (triggers).
    c = dma_channel_get_default_config(DMACH_CB_ADDR);
    channel_config_set_ring(&c, false, 10);
    channel_config_set_high_priority(&c, true);
    dma_channel_configure(DMACH_CB_ADDR, &c,
                          &dma_hw->ch[DMACH_DATA].al3_read_addr_trig,
                          cb_addr, 1, false);

    dma_hw->ints0 = 1u << DMACH_DATA;
    dma_channel_start(DMACH_CB_COUNT);  // feeds cb[0] and kicks the ring
}

// Recover a wedged scanout. With the control-block ring this should never
// fire (there is no CPU re-arm to miss), but it stays as the backstop for
// anything unforeseen (e.g. an electrical glitch corrupting HSTX state).
// Hard-resets the DMA and HSTX blocks (nothing else in this firmware uses
// real DMA; the game's GBA DMA is software) and reprograms everything.
void hstx_display_recover(void) {
    reset_block(RESETS_RESET_DMA_BITS | RESETS_RESET_HSTX_BITS);
    unreset_block_wait(RESETS_RESET_DMA_BITS | RESETS_RESET_HSTX_BITS);

    clock_configure(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS,
                    clock_get_hz(clk_sys), clock_get_hz(clk_sys) / HSTX_CLK_DIV);
    hstx_setup_encoder();
    hstx_setup_lanes();
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    restart_count++;
    dma_hw->ints0 = 1u << DMACH_DATA;
    dma_hw->inte0 = 1u << DMACH_DATA;
    hstx_start_dma();
}

// Print the scanout-relevant hardware state (clock, HSTX, ring channels)
// over stdio, for diagnosing a dead display from the console.
void hstx_debug_dump(void) {
    printf("HSTX clk_hstx ctrl=%08lx div=%08lx  csr=%08lx fifo=%08lx\n",
           (unsigned long)clocks_hw->clk[clk_hstx].ctrl,
           (unsigned long)clocks_hw->clk[clk_hstx].div,
           (unsigned long)hstx_ctrl_hw->csr,
           (unsigned long)hstx_fifo_hw->stat);
    for (int c = DMACH_DATA; c <= DMACH_CB_ADDR; c++)
        printf("DMA ch%d ctrl=%08lx rd=%08lx cnt=%08lx busy=%d\n", c,
               (unsigned long)dma_hw->ch[c].ctrl_trig,
               (unsigned long)dma_hw->ch[c].read_addr,
               (unsigned long)dma_hw->ch[c].transfer_count,
               dma_channel_is_busy(c));
    printf("DMA intr=%08lx inte0=%08lx ints0=%08lx  vsync=%lu streaming=%lu filled=%d\n",
           (unsigned long)dma_hw->intr, (unsigned long)dma_hw->inte0,
           (unsigned long)dma_hw->ints0, (unsigned long)vsync_count,
           (unsigned long)prev_streaming, last_filled);
}

void hstx_display_init_hw(const uint16_t *fb) {
    g_fb = fb;

    // Reserve the three scanout channels in the SDK's claim registry so later
    // dma_claim_unused_channel() callers (e.g. the I2S audio driver) skip them.
    // The ring uses fixed channel numbers, so claiming is bookkeeping only.
    dma_channel_claim(DMACH_DATA);
    dma_channel_claim(DMACH_CB_COUNT);
    dma_channel_claim(DMACH_CB_ADDR);

    build_frame_ring();

    // 252 MHz system clock; clk_hstx = clk_sys/2 = 126 MHz, /5 shift clock =
    // 25.2 MHz pixel clock (640x480p60). Keeps XIP flash at 126 MHz, under the
    // W25Q128's 133 MHz ceiling. Vreg raised from the 1.10 V default as margin
    // for the dual-core + scanout DMA + QMI load.
    vreg_set_voltage(VREG_VOLTAGE_1_15);
    busy_wait_us(1000);
    set_sys_clock_khz(HSTX_SYS_KHZ, true);
    clock_configure(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS,
                    clock_get_hz(clk_sys), clock_get_hz(clk_sys) / HSTX_CLK_DIV);

    hstx_setup_encoder();
    hstx_setup_lanes();
    for (int i = 12; i <= 19; ++i)
        gpio_set_function(i, 0); // HSTX

    // Give DMA priority on the bus so the HSTX FIFO never underflows. Safe
    // with SRAM-resident ring and line blocks (reads are never QMI-bound).
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
}

void hstx_display_start(void) {
    dma_hw->inte0 = 1u << DMACH_DATA;
    irq_set_exclusive_handler(DMA_IRQ_0, hstx_dma_irq);
    // The IRQ is advisory (row fills + vsync counting); the DMA ring keeps
    // the signal alive regardless. It still runs at high priority on the
    // render core so content rows stay fresh under load.
    irq_set_priority(DMA_IRQ_0, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_enabled(DMA_IRQ_0, true);
    hstx_start_dma();
}

void hstx_display_init(const uint16_t *fb) {
    hstx_display_init_hw(fb);
    hstx_display_start();
}
