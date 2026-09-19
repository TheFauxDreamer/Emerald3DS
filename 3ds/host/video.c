// Presentation: the software PPU's output on the top screen, and the game-drawn
// touch UI on the bottom screen.
//
// The PPU (rp2350/ppu.c) renders the GBA frame into a linear 240x160 RGB565
// buffer. Two facts apply before it can go on the screen:
// - PICA200 textures must have power-of-two sizes and are stored in 8x8
//   Morton-order tiles. The display-transfer engine does the tiling. Its input
//   and output widths must agree, so the PPU output first goes into a buffer
//   that is 256px wide. The file ppu.c does not change, so ppu_validate.sh
//   stays meaningful.
// - The GPU reads these buffers directly, so they must be in linear memory.
//   Flush the ARM11 cache before each transfer.
//
// The EXTRA tab selects the scale (see kTopScales). The default 1.5x gives
// 360x240, which fills the screen height with 20px on each side. The texture
// filter follows the scale (see apply_top_filter).

#include <3ds.h>
#include <citro2d.h>
#include <string.h>

#include "../bridge.h"
#include "trace.h"
#include "../../rp2350/ppu.h"

void CtrSettingsMarkDirty(void);   // 3ds/host/settings.c

#define TOP_TEX_W  256
#define TOP_TEX_H  256
#define BOT_TEX_W  512
#define BOT_TEX_H  256

#define TOP_SCREEN_W 400.0f
#define TOP_SCREEN_H 240.0f

// X and Y are separate because FILL stretches horizontally to cover a 5:3 panel
// with 3:2 content. C2D_DrawImageAt always takes two scales.
static const struct { float sx, sy; } kTopScales[CTR_TOP_SCALE_COUNT] = {
    [CTR_TOP_SCALE_1X]   = { 1.0f, 1.0f },
    [CTR_TOP_SCALE_1_5X] = { 1.5f, 1.5f },
    [CTR_TOP_SCALE_FILL] = { TOP_SCREEN_W / CTR_GBA_WIDTH,
                             TOP_SCREEN_H / CTR_GBA_HEIGHT },
};

static int sTopScale = CTR_TOP_SCALE_DEFAULT;

// Selects the texture filter for the current scale. It is defined below, next
// to the texture. See the comment there.
static void apply_top_filter(void);

// Sets the value with no write, for CtrSettingsLoad(). A write during the load
// that gave the value is unnecessary, and a read-only SD card would get a write
// attempt at each boot.
void Ctr3dsApplyTopScale(int mode)
{
    if (mode < 0 || mode >= CTR_TOP_SCALE_COUNT)
        mode = CTR_TOP_SCALE_DEFAULT;

    sTopScale = mode;
    apply_top_filter();
}

int Ctr3dsGetTopScale(void)
{
    return sTopScale;
}

void Ctr3dsSetTopScale(int mode)
{
    int before = sTopScale;

    Ctr3dsApplyTopScale(mode);

    // Queue a write only when the value changes. A tap on the active button
    // costs nothing.
    if (sTopScale != before)
        CtrSettingsMarkDirty();
}

// Linear in, tiled out, no scaling, no vertical flip.
#define TEX_TRANSFER_FLAGS                                   \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) |   \
     GX_TRANSFER_RAW_COPY(0) |                               \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |         \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |        \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static C3D_RenderTarget *sTopTarget, *sBotTarget;

static C3D_Tex             sTopTex, sBotTex;
static Tex3DS_SubTexture   sTopSub, sBotSub;
static C2D_Image           sTopImage, sBotImage;

static uint16_t *sTopStage;   // TOP_TEX_W x CTR_GBA_HEIGHT, linear
static uint16_t *sBotStage;   // BOT_TEX_W x CTR_BOTTOM_HEIGHT, linear

// The rasterizer's per-pixel layer bytes (see ppu.h).
//
// The picture itself has no buffer of its own here. The rasterizer composes
// straight into sTopStage, the buffer the texture upload already needed, at its
// 256 stride. That removes two full-frame copies from every frame: the line
// buffer's seed and flush inside the PPU, and the 240-to-256 copy that
// upload() used to make. See ppu_render_rgb565_direct in rp2350/ppu.h for the
// one rule it asks of a caller, which this file meets because nothing reads the
// picture until the render has returned.
//
// The rasterizer owns both. In threaded mode the worker writes them, and the
// main thread reads them only after it collects the render. They keep state
// between frames (masked pixels keep their old colour, and the blend reads the
// old layer byte), so neither is ever double-buffered.
static uint8_t  sGbaLayer[CTR_GBA_WIDTH * CTR_GBA_HEIGHT];

static int sReady;

// ---- the rasterizer on a second core ----------------------------------------
//
// The rasterizer does not need core 0. It reads four memory regions and writes
// a picture. Thus it runs on another core while the main thread paints the
// bottom screen and feeds the audio:
//
//   New 3DS: core 2, the second application core. CanAccessCore2 in
//            3ds/emerald3ds.rsf permits it.
//   Old 3DS: nothing. The firmware refuses an application any share of the
//            system core, so this is always the inline case there. See
//            ppu_thread_start() for what was tried.
//   Neither: inline on core 0, as before. CTR_PPU_THREAD=0 builds this too.
//
// The same frame, not a pipeline. The render starts when the game's frame is
// complete and is collected before this frame's top upload. Thus the screen
// shows the frame that the game just made, and the input latency does not
// change. The rasterizer overlaps with the bottom paint, the bottom upload and
// the audio.
//
// The isolation rule, which makes this thread safe: the worker reads only the
// snapshot below and writes only sTopStage and sGbaLayer. It never reads
// gGbaMem, so nothing that the main thread does can tear the picture. A change
// from a touch handler shows in the next frame. The worker never calls
// CtrProfile, CtrLogSlow or CtrLog, because their tables have no lock and are
// for the main thread only. It returns its time in sPpuTicks.
//
// The PPU (rp2350/ppu.c) does not change. It already treats the regions as a
// static snapshot during a render.

#ifndef CTR_PPU_THREAD
#define CTR_PPU_THREAD 1
#endif

// What the rasterizer reads, at the sizes in ppu.h. All the registers that it
// reads (the last is BLDY at 0x54) are in the first 0x60 bytes.
#define SNAP_REG_SIZE   0x60
#define SNAP_PAL_SIZE   0x400
#define SNAP_VRAM_SIZE  0x18000
#define SNAP_OAM_SIZE   0x400

static uint8_t sSnapReg[SNAP_REG_SIZE]   __attribute__((aligned(32)));
static uint8_t sSnapPal[SNAP_PAL_SIZE]   __attribute__((aligned(32)));
static uint8_t sSnapVram[SNAP_VRAM_SIZE] __attribute__((aligned(32)));
static uint8_t sSnapOam[SNAP_OAM_SIZE]   __attribute__((aligned(32)));

// The live regions in gGbaMem: the source of the copy when threaded, and the
// rasterizer's own input when inline.
static const void *sLiveReg, *sLivePal, *sLiveVram, *sLiveOam;

// The highest priority that userland can ask for is 0x18. On its own core, the
// worker competes with no thread of this port, so this matters only against the
// system's threads on core 1.
#define PPU_THREAD_PRIO     0x18
#define PPU_STACK_SIZE      (16 * 1024)   // the rasterizer's state is static
// What to ask PM for on the Old 3DS system core, the most first. 80 is the
// documented maximum for an application.
static const int kSyscorePercent[] = { 80, 70, 50 };
#define PPU_START_WAIT_MS   250           // see ppu_try_core

static Thread     sPpuThread;
static int        sPpuCore = -1;          // the worker's core, or -1 for inline
static LightEvent sPpuKick;               // main -> worker: a snapshot is ready
static LightEvent sPpuDone;               // worker -> main: the picture is done
static volatile int sPpuQuit;
static volatile unsigned long long sPpuTicks;   // the worker's last render time
static int        sPpuPending;            // main thread only: a render is out

// The start-up handshake: one for each core tried, never used again. See
// ppu_try_core.
enum { PPU_START_PENDING, PPU_START_RUNNING, PPU_START_ABANDONED };
static volatile int sPpuStart[2];

static void ppu_worker(void *arg)
{
    volatile int *start = (volatile int *)arg;

    // Claim the start first. If the main thread was first, it has stopped
    // waiting for this thread and rasterizes without it. Then leave without
    // touching any shared state.
    if (!__sync_bool_compare_and_swap(start, PPU_START_PENDING, PPU_START_RUNNING))
        return;

    for (;;) {
        unsigned long long t;

        LightEvent_Wait(&sPpuKick);
        if (sPpuQuit)
            break;

        // This pairs with the barrier before the kick. This render reads the
        // snapshot that the main thread just wrote.
        __dmb();

        t = svcGetSystemTick();
        ppu_render_rgb565_direct(sTopStage, TOP_TEX_W, sGbaLayer);
        sPpuTicks = svcGetSystemTick() - t;

        // The picture must be visible to the main thread before the signal.
        __dmb();
        LightEvent_Signal(&sPpuDone);
    }
}

// Start a worker on `core`, and make sure that it runs there.
//
// A thread that exists but never runs is the one failure here that does not
// fail safely. The main thread would wait for its first picture forever, with
// nothing in the log. Thus the worker must report first.
//
// A compare-and-swap on this attempt's own flag lets exactly one side decide:
// the worker when it starts, or the main thread after PPU_START_WAIT_MS. A
// worker that starts too late sees that, and returns without touching the
// events. Thus it cannot take a kick for a worker on another core. Its handle
// is left, not joined, because a join on a thread that never runs would hang
// here.
static int ppu_try_core(int core, volatile int *start)
{
    Thread t;

    *start = PPU_START_PENDING;
    t = threadCreate(ppu_worker, (void *)start, PPU_STACK_SIZE,
                     PPU_THREAD_PRIO, core, false);
    if (t == NULL) {
        // Say which refusal this is. A kernel that will not put a thread on
        // this core at all and a thread that runs but gets no time both end
        // with the rasterizer inline, and the fallback line alone cannot tell
        // them apart.
        CtrLog("emerald3ds: rasteriser thread on core %d refused by the "
               "kernel\n", core);
        return 0;
    }

    for (int ms = 0; ms < PPU_START_WAIT_MS && *start == PPU_START_PENDING; ms++)
        svcSleepThread(1000000LL);

    if (__sync_bool_compare_and_swap(start, PPU_START_PENDING, PPU_START_ABANDONED)) {
        CtrLog("emerald3ds: rasteriser thread on core %d was created but "
               "never ran\n", core);
        return 0;
    }

    sPpuThread = t;
    sPpuCore = core;
    return 1;
}

// The core for the rasterizer, the most capable first. Log every result,
// because "core 2" and "fell back to core 0" give the same symptoms otherwise.
static void ppu_thread_start(void)
{
    bool isNew3ds = false;
    Result rc = 0;

    LightEvent_Init(&sPpuKick, RESET_ONESHOT);
    LightEvent_Init(&sPpuDone, RESET_ONESHOT);
    sPpuQuit = 0;

    // Try core 2 only where it exists. An emulator in Old 3DS mode has two
    // cores, and a request for a third can cause problems.
    APT_CheckNew3DS(&isNew3ds);
    if (isNew3ds && ppu_try_core(2, &sPpuStart[0])) {
        CtrLog("emerald3ds: rasteriser on core 2\n");
        return;
    }

    // Core 1 is the Old 3DS system core. The kernel gives an application no
    // time on it until PM grants a share, and PM refuses a share that it thinks
    // the console cannot spare, so ask for less before giving up. A base 3DS
    // refused 80 percent with "not implemented" while the exheader affinity
    // mask was 1, which barred core 1 outright. That mask is now 3
    // (3ds/emerald3ds.rsf).
    for (unsigned i = 0; i < sizeof(kSyscorePercent) / sizeof(kSyscorePercent[0]); i++) {
        rc = APT_SetAppCpuTimeLimit(kSyscorePercent[i]);
        if (R_FAILED(rc))
            continue;
        if (ppu_try_core(1, &sPpuStart[1])) {
            CtrLog("emerald3ds: rasteriser on core 1 (%d%% of the system core)\n",
                   kSyscorePercent[i]);
            return;
        }
        break;   // the time limit was granted, so the core itself refused
    }

    // Try it anyway. A thread that gets no time is caught and named by the
    // handshake in ppu_try_core(), which tells a refusal by PM apart from a
    // refusal by the kernel. Without this the log blames the time limit for
    // both.
    if (R_FAILED(rc) && ppu_try_core(1, &sPpuStart[1])) {
        CtrLog("emerald3ds: rasteriser on core 1 (no time limit; rc=0x%08lX)\n",
               (unsigned long)rc);
        return;
    }

    CtrLog("emerald3ds: rasteriser inline on core 0 "
           "(no second core; time limit rc=0x%08lX)\n", (unsigned long)rc);
}

static void ppu_thread_stop(void)
{
    if (sPpuCore < 0)
        return;

    if (sPpuPending) {
        LightEvent_Wait(&sPpuDone);
        sPpuPending = 0;
    }

    sPpuQuit = 1;
    LightEvent_Signal(&sPpuKick);
    threadJoin(sPpuThread, U64_MAX);
    threadFree(sPpuThread);
    sPpuThread = NULL;
    sPpuCore = -1;
}

// How long the last present blocked at its sync point. The display divider in
// 3ds/host/main.c subtracts it to see the work alone, because what it decides
// from must not change when it engages.
static unsigned long long sWaitTicks;

unsigned long long CtrVideoLastWaitTicks(void)
{
    return sWaitTicks;
}

// Set once, by ppu_thread_start() in CtrVideoInit(), which main() calls before
// CtrBottomInit(). The bottom screen reads it to select its tuning, so it is
// constant for every reader.
int Ctr3dsRasteriserOnOwnCore(void)
{
    return sPpuCore >= 0;
}

// Copy the video state out of gGbaMem and start the rasterizer on it.
//
// Rp2350PresentFrame() (3ds/host/main.c) calls this on the frame that will
// show. It runs after the game's frame and VBlankIntr() are complete, and
// before the bottom screen paints, so the two overlap. The copy is about 99 KB
// and takes a fraction of a millisecond. It makes the isolation rule above
// true, with no rule for what the paint can touch.
//
// No effect when inline: CtrVideoPresent() then rasterizes gGbaMem directly.
void CtrVideoRenderBegin(void)
{
    unsigned long long t;

    if (!sReady || sPpuCore < 0 || sPpuPending)
        return;

    t = CtrTicksNow();

    memcpy(sSnapReg,  sLiveReg,  SNAP_REG_SIZE);
    memcpy(sSnapPal,  sLivePal,  SNAP_PAL_SIZE);
    memcpy(sSnapVram, sLiveVram, SNAP_VRAM_SIZE);
    memcpy(sSnapOam,  sLiveOam,  SNAP_OAM_SIZE);

    CtrProfile("ppu.snap", t);

    sPpuPending = 1;
    __dmb();
    LightEvent_Signal(&sPpuKick);
}

// Use nearest only where it is correct.
//
// At 1x, nearest is correct: one GBA pixel is one 3DS pixel, and a filter would
// only blur it.
//
// At 1.5x, nearest is wrong. Both 240 -> 360 and 160 -> 240 are 3:2, so nearest
// makes source columns and rows 1, 2, 1, 2 pixels wide. Glyph strokes and
// sprite edges are then one or two pixels thick at random. FILL is worse, at
// 5:3 horizontally. GPU_LINEAR is the 3DS GPU's bilinear filter, and it costs
// nothing extra. (open_agb_firm also uses a filtered scale by default.)
//
// Thus: nearest at the integer scale, linear at the two fractional scales.
//
// It is safe to call before the texture exists. CtrSettingsLoad() applies the
// saved scale through Ctr3dsApplyTopScale() before CtrVideoInit() runs, so the
// guard is here, in the only function that touches the texture.
static void apply_top_filter(void)
{
    GPU_TEXTURE_FILTER_PARAM f =
        (sTopScale == CTR_TOP_SCALE_1X) ? GPU_NEAREST : GPU_LINEAR;

    if (!sReady)
        return;   // CtrVideoInit() applies it when the texture exists

    C3D_TexSetFilter(&sTopTex, f, f);
}

// Kept so that the diagnostics below can read the registers. The
// ppu_set_memory() call uses these, and video.c does not need them again.
//
// This is the live region. The rasterizer reads the snapshot when it has its
// own core, so a diagnostic that must match the picture uses ppu_regs() below.
static const uint8_t *sRegBase;

// The registers and the OAM that the LAST render read.
static const uint8_t *ppu_regs(void)
{
    return sPpuCore >= 0 ? sSnapReg : sRegBase;
}

static const uint8_t *ppu_oam(void)
{
    return sPpuCore >= 0 ? sSnapOam : (const uint8_t *)sLiveOam;
}

static uint16_t read16(const uint8_t *p, int off)
{
    return (uint16_t)(p[off] | (p[off + 1] << 8));
}

// A rasterizer frame longer than this cannot fit in a 16.6 ms budget when it
// shares a core with the game. Report the first one, with the registers that
// decide the cost, so that a log names the scene instead of leaving it to
// guesswork. Once for each boot: this is a diagnostic, not a monitor.
//
// The costly shapes, in the order that they are likely: a window that is
// partial in x, which puts every layer of those lines on the per-pixel path; a
// brightness effect in BLDCNT, which has no fast path at all; and affine
// backgrounds or affine sprites, which are per-pixel by nature. See
// passWinRow() and setPass() in rp2350/ppu.c.
#define PPU_SLOW_TICKS   ((unsigned long long)SYSCLOCK_ARM11 / 125)   // 8 ms
#define PPU_SLOW_RUN     60     // a second of them: a boot spike must not win
#define PPU_SLOW_REPORTS 3      // and a later scene still gets its turn

static void log_slow_scene(unsigned long long ticks)
{
    static unsigned run;
    static int reports;
    const uint8_t *r = ppu_regs();
    const uint8_t *oam = ppu_oam();
    unsigned affine = 0;

    // A run, not one frame. The intro alone has frames over the threshold, and
    // they say nothing about the scene that holds the port at 30 fps.
    if (ticks < PPU_SLOW_TICKS) {
        run = 0;
        return;
    }

    if (++run != PPU_SLOW_RUN || reports >= PPU_SLOW_REPORTS)
        return;
    if (r == NULL || oam == NULL)
        return;

    reports++;

    for (int i = 0; i < 128; i++) {
        // Bit 8 of attribute 0 is the rotation and scaling flag.
        if (read16(oam, i * 8) & 0x0100)
            affine++;
    }

    CtrLog("emerald3ds: slow scene %u us DISPCNT=%04X WIN0H=%04X WIN1H=%04X "
           "WININ=%04X WINOUT=%04X BLDCNT=%04X BLDALPHA=%04X BLDY=%04X "
           "affobj=%u\n",
           (unsigned)(ticks / (SYSCLOCK_ARM11 / 1000000)),
           read16(r, 0x00), read16(r, 0x40), read16(r, 0x42),
           read16(r, 0x48), read16(r, 0x4A), read16(r, 0x50),
           read16(r, 0x52), read16(r, 0x54), affine);
}

static void init_subtex(Tex3DS_SubTexture *sub, int w, int h, int texW, int texH)
{
    sub->width  = (u16)w;
    sub->height = (u16)h;
    sub->left   = 0.0f;
    sub->top    = 1.0f;
    sub->right  = (float)w / (float)texW;
    sub->bottom = 1.0f - (float)h / (float)texH;
}

int CtrVideoInit(void)
{
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    sTopTarget = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    sBotTarget = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    if (sTopTarget == NULL || sBotTarget == NULL)
        return 0;

    if (!C3D_TexInit(&sTopTex, TOP_TEX_W, TOP_TEX_H, GPU_RGB565) ||
        !C3D_TexInit(&sBotTex, BOT_TEX_W, BOT_TEX_H, GPU_RGB565))
        return 0;

    // The bottom screen draws at 1:1 with no resampling, so nearest is always
    // correct there. The top filter depends on the scale, and the end of this
    // function sets it. The function apply_top_filter() does nothing until
    // sReady, so the early call from CtrSettingsLoad() is safe.
    C3D_TexSetFilter(&sBotTex, GPU_NEAREST, GPU_NEAREST);

    init_subtex(&sTopSub, CTR_GBA_WIDTH, CTR_GBA_HEIGHT, TOP_TEX_W, TOP_TEX_H);
    init_subtex(&sBotSub, CTR_BOTTOM_WIDTH, CTR_BOTTOM_HEIGHT, BOT_TEX_W, BOT_TEX_H);
    sTopImage = (C2D_Image){ &sTopTex, &sTopSub };
    sBotImage = (C2D_Image){ &sBotTex, &sBotSub };

    sTopStage = linearAlloc(TOP_TEX_W * CTR_GBA_HEIGHT * sizeof(uint16_t));
    sBotStage = linearAlloc(BOT_TEX_W * CTR_BOTTOM_HEIGHT * sizeof(uint16_t));
    if (sTopStage == NULL || sBotStage == NULL)
        return 0;

    memset(sTopStage, 0, TOP_TEX_W * CTR_GBA_HEIGHT * sizeof(uint16_t));
    memset(sBotStage, 0, BOT_TEX_W * CTR_BOTTOM_HEIGHT * sizeof(uint16_t));

    // Point the PPU at its input. This must occur after Ctr3dsInitGbaMemory().
    //
    // Do this once: the snapshot when a second core rasterizes, the game's own
    // memory when the main thread does. See CtrVideoRenderBegin.
    const void *reg, *pal, *vram, *oam;
    CtrGetGbaRegions(&reg, &pal, &vram, &oam);
    sLiveReg = reg;
    sLivePal = pal;
    sLiveVram = vram;
    sLiveOam = oam;

    if (CTR_PPU_THREAD)
        ppu_thread_start();
    else
        CtrLog("emerald3ds: rasteriser inline on core 0 (CTR_PPU_THREAD=0)\n");

    if (sPpuCore >= 0)
        ppu_set_memory(sSnapReg, sSnapPal, sSnapVram, sSnapOam);
    else
        ppu_set_memory(reg, pal, vram, oam);
    sRegBase = (const uint8_t *)reg;

    sReady = 1;

    // After sReady, or the guard inside it would ignore this call too. The
    // filter is read only when the texture is bound, so the order in init does
    // not matter.
    apply_top_filter();

    return 1;
}

void CtrVideoExit(void)
{
    if (!sReady)
        return;

    // First: the worker can still be writing to sTopStage.
    ppu_thread_stop();

    linearFree(sTopStage);
    linearFree(sBotStage);
    C3D_TexDelete(&sTopTex);
    C3D_TexDelete(&sBotTex);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    sReady = 0;
}

#if CTR_BOOT_DIAG
// Shown once, before AgbMain(), so a black screen has only one meaning. A blue
// top and a green bottom mean that this file works and the game stopped.
void CtrDiagSplash(void)
{
    if (!sReady)
        return;

    // A few frames: double buffering can lose one.
    for (int i = 0; i < 4; i++) {
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(sTopTarget, C2D_Color32(0x00, 0x00, 0xC0, 0xFF));
        C2D_SceneBegin(sTopTarget);
        C2D_TargetClear(sBotTarget, C2D_Color32(0x00, 0x60, 0x00, 0xFF));
        C2D_SceneBegin(sBotTarget);
        C3D_FrameEnd(0);
    }
    CtrTrace("emerald3ds: splash presented (video path works)\n");
}
#endif

// Three profile names for each caller, not one name with a suffix built at run
// time. CtrProfile uses the string's address as the key, so a built name never
// matches itself.
//
// The upload is in three parts, so the log shows which part costs the time. The
// answer selects the fix: a narrower transfer, a smaller flush, or no block on
// the transfer. The top screen has the same three parts as a control. It
// uploads on each frame at 256x160 against the bottom's 512x240.
static const char *const kProfTop[3] = {
    "upload.top.copy", "upload.top.flush", "upload.top.xfer"
};
static const char *const kProfBot[3] = {
    "upload.bot.copy", "upload.bot.flush", "upload.bot.xfer"
};

// Copy a linear w x h RGB565 image into a wider staging buffer. Then the
// transfer engine tiles it into the texture.
//
// The top screen uses this on every frame. The bottom screen uses it when a
// second core rasterizes.
//
// The top upload is always a full image, because the game's frame is new on
// each frame. At 256x160 it moves 81,920 bytes, which fits.
//
// The bottom upload is three times larger, about 2 ms with the copy. With a
// second core, it runs before the join in CtrVideoPresent, in time that core 0
// would spend waiting. Thus the picture shows on the frame of the paint.
// Without a second core, the inline path sends it in slices (below).
static void upload(uint16_t *stage, int stageW, const uint16_t *src,
                   int w, int h, C3D_Tex *tex, const char *const *prof)
{
    unsigned long long t = CtrTicksNow();

    // A NULL src means the caller already composed into the stage, which is
    // what the top screen does now. There is then nothing to copy and no
    // prof[0] sample to take, so that stage simply stops appearing in the log.
    if (src != NULL) {
        for (int y = 0; y < h; y++)
            memcpy(stage + (size_t)y * stageW, src + (size_t)y * w,
                   (size_t)w * 2);
        CtrProfile(prof[0], t);
    }

    t = CtrTicksNow();

    // This flushes the full stage, the padding too. The value of stageW is 512
    // for a 320-wide bottom screen, so 37% of this flush and of the transfer is
    // blank.
    GSPGPU_FlushDataCache(stage, (size_t)stageW * h * sizeof(uint16_t));

    CtrProfile(prof[1], t);
    t = CtrTicksNow();

    C3D_SyncDisplayTransfer((u32 *)stage, GX_BUFFER_DIM(stageW, h),
                            (u32 *)tex->data, GX_BUFFER_DIM(stageW, h),
                            TEX_TRANSFER_FLAGS);

    CtrProfile(prof[2], t);
}

// The bottom screen's upload on the inline path, one slice at a time. With a
// second core, none of this runs: upload() above sends the full picture, in
// parallel with the rasterizer.
//
// On the inline path, the top screen must hold 60fps, and the bottom screen can
// arrive late. A full bottom upload in one frame costs the game a VBlank. The
// measured rate is fps = 3600 / (60 + repaints per second).
//
// A full bottom upload moves 245,760 bytes through a blocking transfer, three
// times the top screen. Thus the frame budget selects the slice size: 48 rows
// is 49,152 bytes. A repaint takes five frames to reach the panel. On this path
// the animations step five times a second, so the delay is not visible.
//
// The value 48 is a multiple of 8. A tiled texture stores eight rows in a
// strip, and the strips are in order. Thus any band that starts on an 8-row
// boundary is one contiguous run in both buffers. The source is at row *
// BOT_TEX_W, and the destination is at (row / 8) * BOT_TEX_W * 8.
// TEX_TRANSFER_FLAGS has no flip and no scaling, so a band lands where it came
// from.
//
// Only hardware can confirm this destination arithmetic. If it is wrong, the
// 48-row bands show in the wrong order. That is easy to see and harms nothing
// else.
#define BOT_CHUNK_ROWS 48

static int sBotRow = CTR_BOTTOM_HEIGHT;   // next row; the height means idle

// Copy the full image and slice only the transfer. The UI can repaint again
// during a run. A source copied in pieces over several frames would tear. The
// copy of 153,600 bytes is cheap.
static void snapshot_bottom(void)
{
    const uint16_t *fb = CtrBottomFramebuffer();
    unsigned long long t = CtrTicksNow();

    for (int y = 0; y < CTR_BOTTOM_HEIGHT; y++)
        memcpy(sBotStage + (size_t)y * BOT_TEX_W,
               fb + (size_t)y * CTR_BOTTOM_WIDTH,
               CTR_BOTTOM_WIDTH * sizeof(uint16_t));

    CtrProfile(kProfBot[0], t);
}

static void upload_bottom_slice(void)
{
    const int left = CTR_BOTTOM_HEIGHT - sBotRow;
    const int rows = (left < BOT_CHUNK_ROWS) ? left : BOT_CHUNK_ROWS;

    uint16_t *src = sBotStage + (size_t)sBotRow * BOT_TEX_W;
    uint8_t  *dst = (uint8_t *)sBotTex.data
                  + (size_t)(sBotRow / 8) * BOT_TEX_W * 8 * sizeof(uint16_t);

    unsigned long long t;

    // Flush only the band that moves next, not the full stage. The total work
    // is the same, spread over the frames.
    t = CtrTicksNow();
    GSPGPU_FlushDataCache(src, (size_t)rows * BOT_TEX_W * sizeof(uint16_t));
    CtrProfile(kProfBot[1], t);

    t = CtrTicksNow();
    C3D_SyncDisplayTransfer((u32 *)src, GX_BUFFER_DIM(BOT_TEX_W, rows),
                            (u32 *)dst, GX_BUFFER_DIM(BOT_TEX_W, rows),
                            TEX_TRANSFER_FLAGS);
    CtrProfile(kProfBot[2], t);

    sBotRow += rows;
}

// Dropped frames, counted.
//
// The stages above tell the cost of each part, but not if the frame was on
// time. A missed VBlank only raises the mean of `framebegin`. Thus this
// measures the displayed frame from one sync point to the next. `frame` reports
// the mean and worst period. Every 600 frames, a line tells how many frames
// were late. A stutter fix must bring that number to zero.
//
// Late means more than 25 ms, one and a half frames. A 33 ms period is one
// missed VBlank. A HOME menu visit counts as one late frame, which is
// acceptable.
#define FRAME_LATE_TICKS    ((unsigned long long)SYSCLOCK_ARM11 / 40)
#define FRAME_REPORT_EVERY  600

static void note_frame_period(void)
{
    static unsigned long long sLast;
    static unsigned sFrames, sLate;

    unsigned long long now = CtrTicksNow();

    if (sLast != 0) {
        CtrProfile("frame", sLast);
        if (now - sLast > FRAME_LATE_TICKS)
            sLate++;
    }
    sLast = now;

    if (++sFrames >= FRAME_REPORT_EVERY) {
        // No line means that no frame was late, so a normal log stays short.
        if (sLate > 0)
            CtrLog("emerald3ds: %u of the last %u frames missed VBlank\n",
                   sLate, sFrames);
        sFrames = 0;
        sLate = 0;
    }
}

void CtrVideoPresent(void)
{
    // Measure the parts separately. The top upload runs on every displayed
    // frame and the bottom one only after a repaint, so the top is the control.
    // If both are slow, the transfer path is at fault. If only the bottom is
    // slow, the cause is the 512-wide stride or the rare cold path.
    unsigned int tPresent = CtrTimeNowMs();
    unsigned int t0;

    if (!sReady)
        return;

    // The full bottom screen, before the join, when a second core rasterizes.
    // It reads only the UI's own framebuffer, which is final after
    // CtrBottomUpdate() returns. The rasterizer never touches that buffer or
    // the bottom texture, so this can run during the render. Its ~2 ms comes
    // from time that `ppu.wait` would spend idle.
    if (sPpuCore >= 0 && CtrBottomIsDirty()) {
        t0 = CtrTimeNowMs();
        upload(sBotStage, BOT_TEX_W, CtrBottomFramebuffer(),
               CTR_BOTTOM_WIDTH, CTR_BOTTOM_HEIGHT, &sBotTex, kProfBot);
        CtrBottomClearDirty();
        CtrLogSlow("upload.bot", t0);
    }

    // The frame that the game just wrote: collect it from the worker, or
    // rasterize it here when there is no worker.
    t0 = CtrTimeNowMs();
    unsigned long long ppuTicks;
    if (sPpuCore >= 0) {
        unsigned long long tw;

        // Only a guard. Rp2350PresentFrame() starts the render on each
        // presented frame, so a render is always out here.
        if (!sPpuPending)
            CtrVideoRenderBegin();

        // The ppu.wait stage is the time when the main thread had nothing to
        // do. Near zero means that the paint, the bottom upload and the audio
        // took as long as the rasterizer. Close to `ppu` means that the second
        // core saved nearly all of it.
        tw = CtrTicksNow();
        LightEvent_Wait(&sPpuDone);
        __dmb();
        sPpuPending = 0;
        CtrProfile("ppu.wait", tw);

        // The worker's own measurement, reported here because CtrProfile is
        // only for the main thread. The stage name is the same as on the inline
        // path, so the logs of the two builds compare directly.
        ppuTicks = sPpuTicks;
        CtrProfile("ppu", CtrTicksNow() - ppuTicks);
    } else {
        unsigned long long tp = CtrTicksNow();
        ppu_render_rgb565_direct(sTopStage, TOP_TEX_W, sGbaLayer);
        ppuTicks = CtrTicksNow() - tp;
        CtrProfile("ppu", tp);
    }
    CtrLogSlow("ppu", t0);
    log_slow_scene(ppuTicks);

#if CTR_BOOT_DIAG
    // Two facts show where a black screen comes from:
    // - DISPCNT == 0x0080 (forced blank) or 0: the game does not drive the
    //   display, so black is correct. The fault is upstream.
    // - Otherwise, a fully black frame means that the game drives the display,
    //   and the PPU or its memory pointers are at fault.
    {
        static unsigned frame;
        frame++;
        if (frame <= 3 || frame == 60 || frame == 600) {
            uint16_t dispcnt = sRegBase ? (uint16_t)(sRegBase[0] | (sRegBase[1] << 8)) : 0xFFFF;
            unsigned nonblack = 0;
            for (int y = 0; y < CTR_GBA_HEIGHT; y++)
                for (int x = 0; x < CTR_GBA_WIDTH; x++)
                    if (sTopStage[y * TOP_TEX_W + x]) nonblack++;
            CtrTrace("emerald3ds: frame %u DISPCNT=%04x nonblack=%u/%u\n",
                     frame, dispcnt, nonblack,
                     (unsigned)(CTR_GBA_WIDTH * CTR_GBA_HEIGHT));
        }
    }
#endif
    t0 = CtrTimeNowMs();
    upload(sTopStage, TOP_TEX_W, NULL, CTR_GBA_WIDTH, CTR_GBA_HEIGHT,
           &sTopTex, kProfTop);
    CtrLogSlow("upload.top", t0);

    // The inline path's bottom upload. The bottom screen is mostly static, so
    // tile it again only when the UI changed. Send it one slice for each frame,
    // because this path has no idle time, and no single frame must lose its
    // budget to it. The second-core path uploaded above.
    //
    // Clear the flag when the snapshot is taken, not when the run ends. A
    // repaint during a run is a new picture. It gets its own run after this
    // one, and does not tear into this one.
    if (sPpuCore < 0) {
        if (sBotRow >= CTR_BOTTOM_HEIGHT && CtrBottomIsDirty()) {
            snapshot_bottom();
            CtrBottomClearDirty();
            sBotRow = 0;
        }

        if (sBotRow < CTR_BOTTOM_HEIGHT) {
            t0 = CtrTimeNowMs();
            upload_bottom_slice();
            CtrLogSlow("upload.bot", t0);
        }
    }

    // The frame's sync point, and the best measure of the spare time. With
    // C3D_FRAME_SYNCDRAW, the wait for the previous frame's render is here, at
    // Begin, not at C3D_FrameEnd.
    //
    // Thus read this as the spare time. Milliseconds mean that the frame has
    // space. Near zero means that the port has no time left, and any new work
    // costs a frame.
    {
        unsigned long long tb = CtrTicksNow();
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        sWaitTicks = CtrTicksNow() - tb;
        CtrProfile("framebegin", tb);
        note_frame_period();
    }

#if CTR_BOOT_DIAG
    // Liveness without the log: moving bars mean that the frame loop runs, and
    // a black screen means that it does not. How much shows depends on the
    // scale: 20px on each side at 1.5x, a wide border at 1x, and nothing in
    // FILL.
    {
        static unsigned tick;
        tick++;
        u8 phase = (u8)((tick / 15) & 3);
        C2D_TargetClear(sTopTarget, C2D_Color32(phase == 1 ? 0x80 : 0x00,
                                                phase == 2 ? 0x80 : 0x00,
                                                phase == 3 ? 0x80 : 0x00, 0xFF));
    }
#else
    C2D_TargetClear(sTopTarget, C2D_Color32(0, 0, 0, 0xFF));
#endif
    C2D_SceneBegin(sTopTarget);
    {
        // Calculated, not in a table: the offset always centers the scaled
        // image, so the two always agree.
        float sx = kTopScales[sTopScale].sx;
        float sy = kTopScales[sTopScale].sy;
        float x  = (TOP_SCREEN_W - CTR_GBA_WIDTH  * sx) / 2.0f;
        float y  = (TOP_SCREEN_H - CTR_GBA_HEIGHT * sy) / 2.0f;

        C2D_DrawImageAt(sTopImage, x, y, 0.0f, NULL, sx, sy);
    }

    C2D_TargetClear(sBotTarget, C2D_Color32(0, 0, 0, 0xFF));
    C2D_SceneBegin(sBotTarget);
    C2D_DrawImageAt(sBotImage, 0.0f, 0.0f, 0.0f, NULL, 1.0f, 1.0f);

    t0 = CtrTimeNowMs();
    {
        unsigned long long tf = CtrTicksNow();
        C3D_FrameEnd(0);
        // This is not the VBlank wait. With C3D_FRAME_SYNCDRAW, the wait is at
        // C3D_FrameBegin above. This is only the submit. It stays measured,
        // because a submit that takes milliseconds means that the GPU or the
        // GSP event thread is late.
        CtrProfile("frameend", tf);
    }
    CtrLogSlow("frameend", t0);

    CtrLogSlow("present", tPresent);
}
