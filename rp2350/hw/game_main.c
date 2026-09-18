// RP2350 boot entry for the full Pokemon Emerald game.
//
// Core 0 brings up clocks/stdio, zeroes the GBA RAM regions (NOLOAD SRAM, not
// cleared by the SDK crt0), starts the HSTX scanout and the core 1 render
// worker, then calls AgbMain() -- the game's superloop (src/main.c), paced to
// the GBA's 59.73 Hz by Rp2350PresentFrame(). Core 1 runs the software PPU,
// re-rendering the most recently completed game frame into the framebuffer the
// HSTX DMA is scanning out; if rendering is slower than the game, intermediate
// frames are skipped.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "hardware/structs/sio.h"

#include "../ppu.h"
#include "../hstx_display.h"
#include "audio.h"
#include "i2s_audio.h"

// m4a engine debug snapshot (rp2350/m4a_mix.c, in the game archive).
void Rp2350AudioDebug(uint32_t *ident, int32_t *spvb, uint32_t *bgmStatus, uint32_t *zeroRet);

extern void AgbMain(void);

// HardFault recorder: a fault used to look like a silent freeze (core 1 keeps
// scanning out the last frame). Stash the stacked PC/LR + core number in
// watchdog scratch registers (which survive a watchdog reboot; the SDK only
// uses scratch 4-7), reboot, and report at the next boot. Both cores share
// the vector table, so this catches faults on either.
#define FAULT_MAGIC 0xFA170BADu

void isr_hardfault(void) {
    uint32_t *sp;
    __asm volatile("mrs %0, msp" : "=r"(sp));
    watchdog_hw->scratch[0] = FAULT_MAGIC;
    watchdog_hw->scratch[1] = sp[6];                          // stacked PC
    watchdog_hw->scratch[2] = *(volatile uint32_t *)0xE000ED28; // CFSR
    watchdog_hw->scratch[3] = *(volatile uint32_t *)0xE000ED38; // BFAR
    watchdog_reboot(0, 0, 10);
    for (;;) tight_loop_contents();
}

static void report_last_fault(void) {
    if (watchdog_hw->scratch[0] == FAULT_MAGIC) {
        printf("!! previous boot HARDFAULT: pc=%08lx cfsr=%08lx bfar=%08lx\n",
               (unsigned long)watchdog_hw->scratch[1],
               (unsigned long)watchdog_hw->scratch[2],
               (unsigned long)watchdog_hw->scratch[3]);
        watchdog_hw->scratch[0] = 0;
    }
}

// GBA regions — must match the #if RP2350 remap in include/gba/defines.h.
#define EWRAM_BASE 0x20000000u
#define IWRAM_BASE 0x20040000u
#define VRAM_BASE  0x20048000u
#define PLTT_BASE  0x20060000u
#define OAM_BASE   0x20060400u
#define REG_BASE_  0x20060800u

// Display framebuffer + PPU layer scratch. ppu_render_rgb565 renders straight
// into 'g_fb' (76.8 KB); hstx_display scans g_fb out at 640x480p60 (2x,
// centred) continuously from its DMA IRQ on core 0.
static uint16_t g_fb[DISP_FB_W * DISP_FB_H];
static uint8_t  g_ppu_layer[PPU_PIXELS];

// Core 1 stack lives in EWRAM's slack (see memmap_emerald.ld) because the SDK
// RAM region is full and SCRATCH_X was shrunk to reclaim framebuffer space.
static uint8_t core1_stack[2048] __attribute__((section(".ewram_top"), aligned(8)));

// Buttons: GP0-GP9, externally wired active-low, internal pull-ups. GPIO n
// maps to REG_KEYINPUT bit n -- both are active-low, so the register value is
// a direct copy of the pin levels:
//   GP0=A  GP1=B  GP2=Select  GP3=Start  GP4=Right
//   GP5=Left  GP6=Up  GP7=Down  GP8=R  GP9=L
// (GPIO 12-19 are HSTX; GP0-9 are free on this board: UART0 default is 12/13.)
#define BUTTON_GPIO_MASK 0x3FFu

static void buttons_init(void) {
    for (int i = 0; i <= 9; i++) {
        gpio_init(i);
        gpio_set_dir(i, GPIO_IN);
        gpio_pull_up(i);
    }
}

static volatile uint32_t g_frame_req;   // bumped by core 0 per game frame
static volatile uint32_t g_frame_done;  // last request core 1 has rendered
static volatile uint32_t g_render_us;   // duration of core 1's last render
static volatile uint32_t g_align_us;    // core 1's last vsync-alignment wait

// ---- vsync-aligned render start (tear-free) ----------------------------------
// The PPU renders into g_fb while the HSTX DMA scans it out, so an unaligned
// render races the beam and the displayed frame mixes two generations -- a
// visible tear seam. Starting the render a computed lead before the beam
// reaches content row 0 keeps every displayed frame single-generation:
//   - lead >= render/160:       a fast render stays AHEAD of the beam from row
//     0 down (the beam consumes a framebuffer row per 63.5 us), so the pass
//     under way shows the complete NEW frame;
//   - lead >= render - 10095us: every row is rendered before the beam's NEXT
//     pass reads it (10095 = 159 rows * 63.5 us). Renders slower than the beam
//     also never catch the current pass (it cleanly shows the OLD frame).
// Both bounds hold => no seam for renders up to ~26 ms. The lead is predicted
// from the previous frame's render time; ALIGN_MARGIN_US covers the cb-granular
// beam readback and small prediction error. A misprediction on a scene change
// costs at most one seam frame.
//
// Side effect: waiting for the beam locks the game to the 60.00 Hz scanout
// (16667 us/frame) -- see PACE_FLOOR_US below.
#define ALIGN_RENDER_MAX_US 15500   // predicted render too big for a beam slot:
                                    // free-run (old behaviour) instead of
                                    // quantizing a 45 fps scene down to 30
#define ALIGN_MARGIN_US 800

static void render_align_wait(void) {
    uint32_t r = g_render_us;
    g_align_us = 0;
    if (r >= ALIGN_RENDER_MAX_US) return;
    uint32_t lead = r / 160;                               // row-0 bound
    if (r > 10095 && r - 10095 > lead) lead = r - 10095;   // row-159 bound
    lead += ALIGN_MARGIN_US;
    // Wait for the beam to cross INTO [lead - margin, lead] us before content
    // row 0. Being already past the window means waiting for the next sweep
    // (starting late instead would put the seam back). Timeout: scanout dead
    // or recovering -- render unaligned rather than hanging the game.
    uint64_t t0 = time_us_64();
    for (;;) {
        uint32_t t_to = hstx_display_us_to_content_start();
        if (t_to <= lead && lead - t_to <= ALIGN_MARGIN_US) break;
        if (time_us_64() - t0 > 20000) break;
        tight_loop_contents();
    }
    g_align_us = (uint32_t)(time_us_64() - t0);
}

// ---- FPS overlay -------------------------------------------------------------
// Game-logic frames per wall-clock second (== displayed rate under the
// render-complete sync: heavy scenes drop below 60). Measured on core 0 over
// 0.5 s windows and drawn into a small buffer that the scanout driver
// composites at line-fill time -- always on top, never flickers (stamping it
// into the framebuffer flickered: the PPU overwrites those rows early in each
// render and the re-stamp only landed at render end). Toggle with 'p'.
static volatile uint32_t g_fps = 0;
static bool g_fps_show = true;

#define FPS_OV_W 11
#define FPS_OV_H 8
#define FPS_OV_X (DISP_FB_W - FPS_OV_W - 1)
#define FPS_OV_Y 1
static uint16_t fps_ov[FPS_OV_W * FPS_OV_H];

// 4x6 digit glyphs, one row per byte, bit 3 = leftmost pixel.
static const uint8_t fps_font[10][6] = {
    {0x6, 0x9, 0x9, 0x9, 0x9, 0x6}, {0x2, 0x6, 0x2, 0x2, 0x2, 0x7},
    {0x6, 0x9, 0x1, 0x2, 0x4, 0xF}, {0xE, 0x1, 0x6, 0x1, 0x9, 0x6},
    {0x2, 0x6, 0xA, 0xF, 0x2, 0x2}, {0xF, 0x8, 0xE, 0x1, 0x9, 0x6},
    {0x6, 0x8, 0xE, 0x9, 0x9, 0x6}, {0xF, 0x1, 0x2, 0x2, 0x4, 0x4},
    {0x6, 0x9, 0x6, 0x9, 0x9, 0x6}, {0x6, 0x9, 0x9, 0x7, 0x1, 0x6},
};

// Redraw the two digits (white on black) into the overlay buffer.
static void fps_overlay_draw(void) {
    uint32_t fps = g_fps;
    if (fps > 99) fps = 99;
    memset(fps_ov, 0, sizeof fps_ov);
    for (int y = 0; y < 6; y++) {
        uint8_t hi = fps_font[fps / 10][y], lo = fps_font[fps % 10][y];
        uint16_t *row = &fps_ov[(y + 1) * FPS_OV_W + 1];
        for (int x = 0; x < 4; x++) {
            if (hi & (8u >> x)) row[x] = 0xFFFF;
            if (lo & (8u >> x)) row[x + 5] = 0xFFFF;
        }
    }
}

static void core1_render(void) {
    // Save-flash writes (rp2350/hw/flash_save.c) suspend XIP, which this
    // core executes from: register as a lockout victim so flash_safe_execute
    // can park this core in the SDK's RAM-resident handler during ops. The
    // scanout IRQ below outranks the lockout IRQ and is fully SRAM-resident,
    // so the display keeps updating even mid-erase.
    flash_safe_execute_core_init();
    // Scanout IRQ lives on this core: it is advisory now (the control-block
    // DMA ring keeps the signal alive without it -- see hstx_display.c), but
    // keeping it off core 0's USB/stdio IRQ traffic means content rows stay
    // fresh. Render work below is freely preempted by it.
    hstx_display_start();
    for (;;) {
        uint32_t req = g_frame_req;
        if (req == g_frame_done) {
            tight_loop_contents();
            continue;
        }
        render_align_wait();
        uint64_t t0 = time_us_64();
        ppu_render_rgb565(g_fb, g_ppu_layer);
        g_render_us = (uint32_t)(time_us_64() - t0);
        g_frame_done = req;   // requests that arrived mid-render are skipped
    }
}

// Minimal view of the game's main state block (struct Main in include/global.h):
// only callback2 is read, and the first two fields' offsets are fixed.
extern struct { void (*callback1)(void); void (*callback2)(void); } gMain;

// GBA frame period: 280896 cycles at 16.78 MHz = 16743 us (59.73 Hz). The game
// no longer paces to it directly: when renders fit the frame, core 1's
// vsync-aligned render start locks the game to the 60.00 Hz scanout
// (16667 us/frame, +0.45% speed vs a real GBA -- imperceptible, and game
// frames map 1:1 onto display refreshes, which kills the 59.73-on-60 judder).
// The timer pacing below is only a floor for when alignment is off (heavy
// scenes, dead scanout). It MUST stay under the 16667 us beam period: a floor
// above it beats against the beam lock (the timer pushes a frame past its
// beam slot every ~220 frames -> a 28 ms hiccup about once a second).
#define PACE_FLOOR_US 16600

// ---- non-blocking print queue --------------------------------------------------
// stdio writes BLOCK at ~0.28 ms/char measured (stdio-UART at 115200 + USB-CDC):
// the 5 s stats burst was an 80-110 ms frame spike, and the keys= line a ~2.5 ms
// hitch on every press/release. Frame-path telemetry is queued here instead and
// Rp2350PresentFrame drains a few bytes per frame, so no single frame pays more
// than a sliver of stdio time. Messages are dropped whole if the queue is full.
// Console-triggered output (dumps, test results) stays on plain printf: those
// intentionally pause the game.
// Lives in the EWRAM slack's alignment gap (the SDK RAM region is full); 512
// covers the ~400-byte 5 s stats burst. Power of two for the index mask.
static char pq[512] __attribute__((section(".ewram_top.pq")));
static unsigned pq_w, pq_r;           // pq_w - pq_r = bytes pending
static void pq_printf(const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof tmp - 1) n = (int)sizeof tmp - 1;
    if (sizeof pq - (pq_w - pq_r) < (unsigned)n) return;   // full: drop message
    for (int i = 0; i < n; i++) pq[pq_w++ & (sizeof pq - 1)] = tmp[i];
}
static void pq_drain(int budget) {
    while (budget-- > 0 && pq_r != pq_w) putchar(pq[pq_r++ & (sizeof pq - 1)]);
}

// ---- frame-time telemetry ----------------------------------------------------
// Per-frame breakdown of where the frame period goes, to diagnose stutter:
//   logic  = game logic + VBlankIntr (previous PresentFrame return -> this entry)
//   render = core 1's PPU render of this frame (g_render_us)
//   pace   = pacing wait (only nonzero when the game is at/above 60 fps)
//   rwait  = stall after pacing waiting for core 1 (render longer than the slot)
//   period = entry-to-entry frame time (what the player perceives)
// Aggregated over the 5 s liveness window; 't' toggles a per-frame trace line.
static struct {
    uint32_t n;
    uint32_t logic_sum, logic_max;
    uint32_t render_sum, render_max;
    uint32_t rwait_sum, rwait_max;
    uint32_t align_sum, align_max;   // core 1's vsync-alignment wait
    uint32_t period_max;
    uint32_t hist[5];     // period <17.5 / <20 / <25 / <34 / >=34 ms
    uint32_t uncapped;    // frames with neither pacing nor alignment wait
} g_ft;
static bool g_ft_trace;
static bool g_keys_debug;

// PPU per-pass profiling (ppu.c, PPU_PROFILE): core 1 accumulates, the 5 s
// stats print on core 0 reads + clears. Racy by design; stats only.
extern uint32_t ppu_prof_us[6];
extern uint32_t ppu_prof_frames;
uint64_t ppu_prof_now(void) { return time_us_64(); }

static void ft_account(uint32_t period, uint32_t logic, uint32_t render,
                       uint32_t pace, uint32_t rwait) {
    uint32_t align = g_align_us;   // core 1's wait for this frame's render
    g_ft.n++;
    g_ft.logic_sum += logic;
    if (logic > g_ft.logic_max) g_ft.logic_max = logic;
    g_ft.render_sum += render;
    if (render > g_ft.render_max) g_ft.render_max = render;
    g_ft.rwait_sum += rwait;
    if (rwait > g_ft.rwait_max) g_ft.rwait_max = rwait;
    g_ft.align_sum += align;
    if (align > g_ft.align_max) g_ft.align_max = align;
    if (period > g_ft.period_max) g_ft.period_max = period;
    g_ft.hist[period < 17500 ? 0 : period < 20000 ? 1 :
              period < 25000 ? 2 : period < 34000 ? 3 : 4]++;
    if (pace < 100 && align < 100) g_ft.uncapped++;   // no throttle: flat out
    if (g_ft_trace)
        pq_printf("T %lu %lu %lu %lu %lu %lu\n", (unsigned long)period,
                  (unsigned long)logic, (unsigned long)render,
                  (unsigned long)pace, (unsigned long)rwait,
                  (unsigned long)align);
}

// Per-frame hook (overrides the weak no-op in bios.c). Called at the end of
// every WasmRunFrame() (src/main.c), after the game has written this frame's
// VRAM/palette/OAM/registers to their SRAM-mapped addresses. Hands the frame to
// core 1, paces the game to the GBA frame rate, then waits for the render to
// finish so the next game frame never mutates VRAM mid-render -- every
// displayed frame is internally consistent. The render-wait costs nothing when
// rendering fits the frame period; heavy scenes briefly run below 60 fps.
void Rp2350PresentFrame(void)
{
    // Telemetry: time since the last PresentFrame return = game logic +
    // VBlankIntr; entry-to-entry = the perceived frame period.
    static uint64_t ft_last_entry, ft_last_exit;
    uint64_t ft_entry = time_us_64();
    uint32_t ft_logic = ft_last_exit ? (uint32_t)(ft_entry - ft_last_exit) : 0;
    uint32_t ft_period = ft_last_entry ? (uint32_t)(ft_entry - ft_last_entry) : 0;
    ft_last_entry = ft_entry;

    // Sample the buttons for the next game frame's ReadKeys(). GPIO n ==
    // KEYINPUT bit n, both active-low, so the pin levels copy straight in.
    // Console-injected presses (see below) clear their bits for a few frames,
    // enabling remote play/debugging over USB-CDC.
    static uint16_t inj_mask;
    static int inj_frames;
    uint16_t keys = (uint16_t)(gpio_get_all() & BUTTON_GPIO_MASK);
    if (inj_frames > 0) {
        keys &= (uint16_t)~inj_mask;
        inj_frames--;
    }
    *(volatile uint16_t *)(REG_BASE_ + 0x130) = keys;

    // Input debug ('k' toggle, default off -- it used to be a per-press frame
    // hitch): log every change. A missing line here with a button held means
    // the firmware isn't seeing the pin change.
    static uint16_t last_keys = BUTTON_GPIO_MASK;
    if (keys != last_keys) {
        if (g_keys_debug) pq_printf("keys=%03x\n", keys);
        last_keys = keys;
    }

    // Mix one frame of audio and enqueue it into the I2S ring. Cheap today
    // (the mixer seam returns silence); becomes the m4a mix cost once revived.
    Rp2350AudioFrame();

    g_frame_req++;

    static uint64_t next_us;
    uint64_t now = time_us_64();
    if (next_us == 0) next_us = now;
    next_us += PACE_FLOOR_US;
    if (now < next_us) {
        while (time_us_64() < next_us) tight_loop_contents();
    } else if (now - next_us > 2 * PACE_FLOOR_US) {
        // Cap the pacing debt at ~2 frames. Larger debts (a render-bound scene,
        // a load spike) would otherwise be repaid by running the game ABOVE
        // 60 fps for up to the whole debt -- a visible speed burst right after
        // every heavy stretch. Up to 2 frames still absorbs one-off jitter.
        next_us = now;
    }
    uint64_t ft_paced = time_us_64();

    while (g_frame_done != g_frame_req) tight_loop_contents();

    ft_account(ft_period, ft_logic, g_render_us,
               (uint32_t)(ft_paced - now), (uint32_t)(time_us_64() - ft_paced));
    pq_drain(32);   // ~32 stdio bytes/frame: telemetry dribbles out hitch-free

    // Commit any pending save-flash page write (single-byte tail of a save
    // sector, e.g. the sector signature byte). No-op when nothing is pending.
    {
        extern void Rp2350SaveSync(void);
        Rp2350SaveSync();
    }

    // FPS overlay source: game frames per wall-clock over 0.5 s windows.
    {
        static uint64_t fps_win_start;
        static uint32_t fps_win_frames;
        fps_win_frames++;
        uint64_t t = time_us_64();
        if (fps_win_start == 0) fps_win_start = t;
        uint64_t el = t - fps_win_start;
        if (el >= 500000) {
            g_fps = (uint32_t)(((uint64_t)fps_win_frames * 1000000 + el / 2) / el);
            fps_win_start = t;
            fps_win_frames = 0;
            if (g_fps_show) fps_overlay_draw();
        }
    }

    // Scanout watchdog. With the control-block DMA ring the scanout has no
    // CPU re-arm to miss, so this is a should-never-fire backstop. The vsync
    // counter alone is not trustworthy any more (it is maintained by the
    // advisory IRQ, so IRQ starvation freezes it while the display is fine);
    // only reset when the rate is bad AND the DMA ring position is frozen --
    // i.e. the hardware itself has actually stopped.
    {
        static uint64_t win_start_us;
        static uint32_t win_vsync0;
        static uint32_t win_pos0;
        static int bad_windows;
        uint32_t vs = hstx_display_frame_count();
        uint32_t pos = hstx_display_dma_pos();
        if (win_start_us == 0) {
            win_start_us = now;
            win_vsync0 = vs;
            win_pos0 = pos;
        } else if (now - win_start_us >= 1000000) {
            uint32_t rate = vs - win_vsync0;
            if ((rate < 50 || rate > 70) && pos == win_pos0) {
                if (++bad_windows >= 2) {
                    hstx_display_recover();
                    bad_windows = 0;
                }
            } else {
                bad_windows = 0;
            }
            win_start_us = now;
            win_vsync0 = hstx_display_frame_count();
            win_pos0 = hstx_display_dma_pos();
        }
    }

    // Liveness/perf stats roughly every 5 s of game time. vsync should advance
    // ~60/s; a stalled vsync or nonzero restarts = scanout trouble.
    static uint32_t n;
    if (++n % 300 == 0) {
        pq_printf("frame=%lu render=%luus cb2=%p dispcnt=%04x vsync=%lu restarts=%lu "
                  "fifostat=%08lx maxgap=%luus\n",
                  (unsigned long)g_frame_req, (unsigned long)g_render_us,
                  (void *)gMain.callback2, *(volatile uint16_t *)REG_BASE_,
                  (unsigned long)hstx_display_frame_count(),
                  (unsigned long)hstx_display_restart_count(),
                  (unsigned long)hstx_display_fifo_stat(),
                  (unsigned long)hstx_display_max_irq_gap());
        {
            uint32_t a_played, a_under, a_fill;
            Rp2350AudioStats(&a_played, &a_under, &a_fill);
            pq_printf("audio played=%lu underruns=%lu ring=%lu chan=%d pio=%d sm=%d fifo=%lu\n",
                      (unsigned long)a_played, (unsigned long)a_under,
                      (unsigned long)a_fill,
                      i2s_audio_dma_chan(), i2s_audio_pio_index(),
                      i2s_audio_sm(), (unsigned long)i2s_audio_fifo_level());
            {
                uint32_t m_ident, m_bgm, m_zero; int32_t m_spvb;
                Rp2350AudioDebug(&m_ident, &m_spvb, &m_bgm, &m_zero);
                pq_printf("m4a ident=%08lx spvb=%ld bgmStatus=%08lx zeroRet=%lu\n",
                          (unsigned long)m_ident, (long)m_spvb,
                          (unsigned long)m_bgm, (unsigned long)m_zero);
            }
        }
        if (g_ft.n)
            pq_printf("ft n=%lu logic=%lu/%luus render=%lu/%luus rwait=%lu/%luus "
                      "align=%lu/%luus permax=%luus hist=%lu/%lu/%lu/%lu/%lu uncapped=%lu\n",
                      (unsigned long)g_ft.n,
                      (unsigned long)(g_ft.logic_sum / g_ft.n), (unsigned long)g_ft.logic_max,
                      (unsigned long)(g_ft.render_sum / g_ft.n), (unsigned long)g_ft.render_max,
                      (unsigned long)(g_ft.rwait_sum / g_ft.n), (unsigned long)g_ft.rwait_max,
                      (unsigned long)(g_ft.align_sum / g_ft.n), (unsigned long)g_ft.align_max,
                      (unsigned long)g_ft.period_max,
                      (unsigned long)g_ft.hist[0], (unsigned long)g_ft.hist[1],
                      (unsigned long)g_ft.hist[2], (unsigned long)g_ft.hist[3],
                      (unsigned long)g_ft.hist[4], (unsigned long)g_ft.uncapped);
        memset(&g_ft, 0, sizeof g_ft);
        if (ppu_prof_frames) {
            uint32_t pf = ppu_prof_frames;
            pq_printf("ppu n=%lu state=%lu line=%lu back=%lu text=%lu affine=%lu "
                      "spr=%luus\n", (unsigned long)pf,
                      (unsigned long)(ppu_prof_us[0] / pf),
                      (unsigned long)(ppu_prof_us[1] / pf),
                      (unsigned long)(ppu_prof_us[2] / pf),
                      (unsigned long)(ppu_prof_us[3] / pf),
                      (unsigned long)(ppu_prof_us[4] / pf),
                      (unsigned long)(ppu_prof_us[5] / pf));
            memset((void *)ppu_prof_us, 0, sizeof ppu_prof_us);
            ppu_prof_frames = 0;
        }
    }

    // Console debug: 'd' dumps REG/PLTT/OAM, 'v' dumps VRAM, 'f' dumps the
    // rendered framebuffer (hex over USB-CDC), so the live GBA state and the
    // displayed image can be inspected on the host (the game pauses for the
    // dump's duration; the pacing resync above absorbs the gap).
    int ch = getchar_timeout_us(0);
    if (ch == 't') {   // per-frame timing trace toggle
        g_ft_trace = !g_ft_trace;
        printf("ft trace %s\n", g_ft_trace ? "on" : "off");
    }
    if (ch == 'k') {   // keys= input debug toggle
        g_keys_debug = !g_keys_debug;
        printf("keys debug %s\n", g_keys_debug ? "on" : "off");
    }
    if (ch == 'p') {   // FPS overlay toggle
        g_fps_show = !g_fps_show;
        hstx_display_set_overlay(g_fps_show ? fps_ov : NULL,
                                 FPS_OV_X, FPS_OV_Y, FPS_OV_W, FPS_OV_H);
    }
    // Remote input: uppercase char = hold that button for 6 frames.
    // A B S(tart) E(=Select) U D L R (d-pad); KEYINPUT bit order.
    {
        static const char inj_chars[] = "ABESRLUD";
        for (int b = 0; b < 8; b++) {
            if (ch == inj_chars[b]) {
                inj_mask = (uint16_t)(1u << b);
                inj_frames = 6;
            }
        }
    }
    if (ch == 'h') {
        // Scanout hardware state, for diagnosing a dead display.
        extern void hstx_debug_dump(void);
        hstx_debug_dump();
    }
    if (ch == 'W') {
        // Factory-erase the whole 128 KB save region (all sectors -> 0xFF =
        // "no save", like a brand-new cart). The region ships with whatever
        // bytes the flash last held there, which the game sees as a corrupt
        // save until the first real save overwrites it.
        extern unsigned short Rp2350SaveEraseChip(void);
        printf("erasing save region... rc=%u\n", Rp2350SaveEraseChip());
    }
    if (ch == 'w') {
        // Save-flash smoke test on sector 31 (recorded battle; expendable).
        // Borrows g_ppu_layer for the 4 KB pattern -- safe here: core 1 is
        // idle (render-done was awaited above) and the layer is render
        // scratch. Leaves the sector erased (0xFF) afterwards.
        extern unsigned short Rp2350SaveEraseSector(unsigned short);
        extern unsigned short Rp2350SaveProgramSector(unsigned short, unsigned char *);
        uint8_t *pat = g_ppu_layer;
        const volatile uint8_t *rd = (const volatile uint8_t *)(0x10FE0000u + 31 * 4096);
        int okp = 1, okff = 1;
        for (int i = 0; i < 4096; i++) pat[i] = (uint8_t)(i * 7 + 3);
        uint16_t r1 = Rp2350SaveEraseSector(31);
        uint16_t r2 = Rp2350SaveProgramSector(31, pat);
        for (int i = 0; i < 4096; i++) if (rd[i] != (uint8_t)(i * 7 + 3)) { okp = 0; break; }
        uint16_t r3 = Rp2350SaveEraseSector(31);
        for (int i = 0; i < 4096; i++) if (rd[i] != 0xFF) { okff = 0; break; }
        printf("flashsave test: erase=%u prog=%u pattern_ok=%d erase2=%u ff_ok=%d\n",
               r1, r2, okp, r3, okff);
    }
    if (ch == 'd' || ch == 'v' || ch == 'f') {
        static const struct { const char *name; uintptr_t base; uint32_t len; } regions[] = {
            {"reg", REG_BASE_, 0x100}, {"pal", PLTT_BASE, 0x400},
            {"oam", OAM_BASE, 0x400}, {"vram", VRAM_BASE, 0x18000},
            {"fb", (uintptr_t)g_fb, sizeof g_fb},
        };
        int lo = (ch == 'd') ? 0 : (ch == 'v') ? 3 : 4;
        int hi = (ch == 'd') ? 2 : (ch == 'v') ? 3 : 4;
        for (int r = lo; r <= hi; r++) {
            printf("DUMP %s %lu\n", regions[r].name, (unsigned long)regions[r].len);
            const uint8_t *p = (const uint8_t *)regions[r].base;
            for (uint32_t i = 0; i < regions[r].len; i += 32) {
                for (uint32_t j = 0; j < 32; j++) printf("%02x", p[i + j]);
                printf("\n");
            }
        }
        printf("ENDDUMP\n");
    }

    ft_last_exit = time_us_64();   // logic timing starts here (excludes dumps)
}

int main(void)
{
    stdio_init_all();
    sleep_ms(1500);
    printf("\n=== Pokemon Emerald on RP2350 ===\n");
    report_last_fault();

    // Zero the GBA RAM regions (uninitialized SRAM).
    memset((void *)EWRAM_BASE, 0, 256 * 1024);
    memset((void *)IWRAM_BASE, 0,  32 * 1024);
    memset((void *)VRAM_BASE,  0,  96 * 1024);
    memset((void *)PLTT_BASE,  0,  1024);
    memset((void *)OAM_BASE,   0,  1024);
    memset((void *)REG_BASE_,  0,  1024);

    // GBA keys are active-low: REG_KEYINPUT bit clear == pressed. A zeroed REG
    // region reads as "all buttons held", which trips the A+B+Start+Select soft
    // reset (-> RFU code that hangs). Set "no buttons" until the first frame's
    // button sample in Rp2350PresentFrame.
    *(volatile uint16_t *)(REG_BASE_ + 0x130) = 0x03FF;  // REG_KEYINPUT
    buttons_init();

    // Point the software PPU at the game's SRAM-mapped GBA regions, configure
    // the HSTX scanout hardware (raises clk_sys to 252 MHz), then launch the
    // core 1 worker, which starts the scanout (binding its IRQ to core 1) and
    // renders. g_fb shows garbage until core 1's first render fills it.
    ppu_set_memory((const void *)REG_BASE_, (const void *)PLTT_BASE,
                   (const void *)VRAM_BASE, (const void *)OAM_BASE);
    hstx_display_init_hw(g_fb);
    fps_overlay_draw();
    hstx_display_set_overlay(fps_ov, FPS_OV_X, FPS_OV_Y, FPS_OV_W, FPS_OV_H);
    multicore_launch_core1_with_stack(core1_render, (uint32_t *)core1_stack,
                                      sizeof core1_stack);

    // I2S audio out (PCM5102A on GP20-22). Claims a PIO SM + DMA_IRQ_1 on this
    // core (core 0). Outputs silence until the m4a mixer is ported (Rp2350Mix-
    // Frame); the pipeline runs regardless, pumped per frame by Rp2350AudioFrame.
    Rp2350AudioInit();
    printf("PPU + HSTX + core1 + I2S audio up (clk_sys %lu Hz); entering AgbMain()...\n",
           (unsigned long)clock_get_hz(clk_sys));

    AgbMain();  // superloops forever

    printf("!! AgbMain returned -- unexpected\n");
    for (;;) tight_loop_contents();
}
