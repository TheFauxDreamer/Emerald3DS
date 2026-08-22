// HSTX -> DVI/HDMI bring-up test for the WeAct Core2350B + HSTX-to-HDMI breakout.
//
// Outputs 640x480@60 over the 4 TMDS pairs on GPIO 12-19. Renders 8 vertical
// SMPTE-style colour bars in RGB565 so any wrong-lane / colour-swap is obvious
// on the monitor, and so we exercise the exact RGB565 path the PPU will use.
//
// Memory trick: every active scanline is identical, so instead of a 600 KB
// 640x480 framebuffer (won't fit SRAM) we point all active lines at one shared
// 640-pixel line buffer.
//
// Wired pinout (matches pico-examples Pico-DVI-Sock exactly):
//   GP12 D0+  GP13 D0-   GP14 CK+  GP15 CK-
//   GP16 D2+  GP17 D2-   GP18 D1+  GP19 D1-

#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "pico/stdlib.h"

// ----------------------------------------------------------------------------
// DVI constants (CEA 640x480p60)

#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu

#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

#define MODE_H_FRONT_PORCH   16
#define MODE_H_SYNC_WIDTH    96
#define MODE_H_BACK_PORCH    48
#define MODE_H_ACTIVE_PIXELS 640

#define MODE_V_FRONT_PORCH   10
#define MODE_V_SYNC_WIDTH    2
#define MODE_V_BACK_PORCH    33
#define MODE_V_ACTIVE_LINES  480

#define MODE_V_TOTAL_LINES  (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + \
                             MODE_V_BACK_PORCH  + MODE_V_ACTIVE_LINES)

#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

// ----------------------------------------------------------------------------
// HSTX command lists (padded with NOPs >= FIFO size, as in the SDK example)

static uint32_t vblank_line_vsync_off[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH, SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,  SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS), SYNC_V1_H1,
    HSTX_CMD_NOP
};

static uint32_t vblank_line_vsync_on[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH, SYNC_V0_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,  SYNC_V0_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS), SYNC_V0_H1,
    HSTX_CMD_NOP
};

static uint32_t vactive_line[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH, SYNC_V1_H1, HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,  SYNC_V1_H0, HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,  SYNC_V1_H1,
    HSTX_CMD_TMDS       | MODE_H_ACTIVE_PIXELS
};

// One shared scanline of RGB565 colour bars; all 480 active lines reuse it.
static uint16_t linebuf[MODE_H_ACTIVE_PIXELS];

// ----------------------------------------------------------------------------
// DMA double-buffer (ping/pong), same structure as the SDK example

#define DMACH_PING 0
#define DMACH_PONG 1

static bool dma_pong = false;
static uint v_scanline = 2;
static bool vactive_cmdlist_posted = false;

void __scratch_x("") dma_irq_handler() {
    uint ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1u << ch_num;
    dma_pong = !dma_pong;

    if (v_scanline >= MODE_V_FRONT_PORCH && v_scanline < (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH)) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_on;
        ch->transfer_count = count_of(vblank_line_vsync_on);
    } else if (v_scanline < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_off;
        ch->transfer_count = count_of(vblank_line_vsync_off);
    } else if (!vactive_cmdlist_posted) {
        ch->read_addr = (uintptr_t)vactive_line;
        ch->transfer_count = count_of(vactive_line);
        vactive_cmdlist_posted = true;
    } else {
        // Every active line shows the same colour bars -> reuse one buffer.
        ch->read_addr = (uintptr_t)linebuf;
        ch->transfer_count = MODE_H_ACTIVE_PIXELS * sizeof(uint16_t) / sizeof(uint32_t);
        vactive_cmdlist_posted = false;
    }

    if (!vactive_cmdlist_posted)
        v_scanline = (v_scanline + 1) % MODE_V_TOTAL_LINES;
}

// ----------------------------------------------------------------------------

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
}

int main(void) {
    // Bring up stdio FIRST at the default clock so the USB-CDC console works
    // even if the clock switch below fails (don't use required=true: it panics
    // and hangs, killing both video and serial).
    stdio_init_all();
    sleep_ms(2500); // let the USB host enumerate before we print/switch clocks

    // The HSTX serialiser shifts 2 bits per clk_hstx cycle, so the pixel clock
    // is clk_hstx / 5. 640x480p60 wants ~25.2 MHz, i.e. clk_hstx = 126 MHz.
    // The HSTX divider is integer-only (1..3), so we run the whole system at
    // 126 MHz and feed clk_hstx from clk_sys at div 1. 126 MHz is below the
    // 150 MHz default (flash/XIP-safe) and leaves pll_usb=48 MHz for USB-CDC.
    bool clk_ok = set_sys_clock_khz(126000, false);
    printf("set_sys_clock_khz(126000) -> %s (clk_sys now %lu Hz)\n",
           clk_ok ? "OK" : "FAILED", clock_get_hz(clk_sys));

    clock_configure(clk_hstx, 0,
                    CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS,
                    clock_get_hz(clk_sys), clock_get_hz(clk_sys));

    // 8 classic colour bars, left to right.
    static const uint16_t bars[8] = {
        0xFFFF, // white
        0xFFE0, // yellow   (R+G)
        0x07FF, // cyan      (G+B)
        0x07E0, // green
        0xF81F, // magenta   (R+B)
        0xF800, // red
        0x001F, // blue
        0x0000, // black
    };
    for (int x = 0; x < MODE_H_ACTIVE_PIXELS; ++x)
        linebuf[x] = bars[x / (MODE_H_ACTIVE_PIXELS / 8)];

    // RGB565 TMDS expander config: lane2=red(5b), lane1=green(6b), lane0=blue(5b).
    hstx_ctrl_hw->expand_tmds =
        4  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
        8  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB   |
        5  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
        3  << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |
        4  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
        29 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

    // 16bpp: 2 pixels per 32-bit word, advance 16 bits per pixel.
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

    // Clock pair on bit[2]/bit[3] (GP14/15); data lanes mapped to the wired pins.
    hstx_ctrl_hw->bit[2] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[3] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    for (uint lane = 0; lane < 3; ++lane) {
        static const int lane_to_output_bit[3] = {0, 6, 4}; // D0->GP12, D1->GP18, D2->GP16
        int bit = lane_to_output_bit[lane];
        uint32_t sel =
            (lane * 10    ) << HSTX_CTRL_BIT0_SEL_P_LSB |
            (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        hstx_ctrl_hw->bit[bit]     = sel;
        hstx_ctrl_hw->bit[bit + 1] = sel | HSTX_CTRL_BIT0_INV_BITS;
    }

    for (int i = 12; i <= 19; ++i)
        gpio_set_function(i, 0); // HSTX

    dma_channel_config c;
    c = dma_channel_get_default_config(DMACH_PING);
    channel_config_set_chain_to(&c, DMACH_PONG);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PING, &c, &hstx_fifo_hw->fifo,
                          vblank_line_vsync_off, count_of(vblank_line_vsync_off), false);
    c = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&c, DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PONG, &c, &hstx_fifo_hw->fifo,
                          vblank_line_vsync_off, count_of(vblank_line_vsync_off), false);

    dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    dma_hw->inte0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
    dma_channel_start(DMACH_PING);

    uint n = 0;
    while (1) {
        // Report the actual configured clocks so we can confirm the pixel clock
        // is 126/5 = 25.2 MHz (clk_hstx should read 126000000).
        printf("HSTX test (%u): clk_sys=%lu Hz  clk_hstx=%lu Hz  (pixclk=%lu Hz)\n",
               n++, clock_get_hz(clk_sys), clock_get_hz(clk_hstx),
               clock_get_hz(clk_hstx) / 5);
        sleep_ms(1000);
    }
}
