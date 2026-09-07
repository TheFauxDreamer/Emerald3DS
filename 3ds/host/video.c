// Presentation: the software PPU's output on the top screen, the game-drawn
// touch UI on the bottom.
//
// rp2350/ppu.c renders the GBA frame into a plain 240x160 RGB565 buffer. Two
// things stand between that and the screen:
//
//   1. PICA200 textures must have power-of-two dimensions and are stored
//      swizzled (8x8 Morton-ordered tiles), so a linear image cannot be handed
//      to the GPU as-is. The display-transfer engine does the tiling for us,
//      which is why the staging buffers are 256 px wide rather than 240: the
//      transfer's input and output widths must agree, so the PPU output is
//      row-copied into a 256-stride buffer first. ppu.c itself stays untouched,
//      which keeps ppu_validate.sh meaningful.
//   2. The GPU reads these buffers directly, so they must live in linear memory
//      and the ARM11 cache must be flushed before each transfer.
//
// Scaling is selectable at runtime from the EXTRA tab (see kTopScales below).
// The default 1.5x gives 360x240, filling the top screen's height exactly with a
// 20 px pillarbox each side. The texture filter follows the scale rather than
// being fixed: point sampling is right only at 1x, and is the cause of the
// uneven pixel widths at the two fractional scales. See apply_top_filter.

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

// X and Y are separate because FILL is the only way to cover a 5:3 panel with
// 3:2 content, and it does so by stretching horizontally. C2D_DrawImageAt has
// always taken two scales, so this costs nothing.
static const struct { float sx, sy; } kTopScales[CTR_TOP_SCALE_COUNT] = {
    [CTR_TOP_SCALE_1X]   = { 1.0f, 1.0f },
    [CTR_TOP_SCALE_1_5X] = { 1.5f, 1.5f },
    [CTR_TOP_SCALE_FILL] = { TOP_SCREEN_W / CTR_GBA_WIDTH,
                             TOP_SCREEN_H / CTR_GBA_HEIGHT },
};

static int sTopScale = CTR_TOP_SCALE_DEFAULT;

// Chooses the texture filter for the current scale. Defined below, next to the
// texture it configures; see the comment there for why this is not simply
// nearest everywhere.
static void apply_top_filter(void);

// Set without persisting. CtrSettingsLoad() uses this: writing the file back
// out during the load that produced it would be pointless churn, and would turn
// a read-only SD card into a write attempt on every boot.
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

    // Only queue a write when something actually changed: re-tapping the
    // active button should not cost one.
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

// PPU output and its per-pixel layer scratch (see ppu.h).
static uint16_t sGbaFrame[CTR_GBA_WIDTH * CTR_GBA_HEIGHT];
static uint8_t  sGbaLayer[CTR_GBA_WIDTH * CTR_GBA_HEIGHT];

static int sReady;

// Nearest only where nearest is actually correct.
//
// This used to be GPU_NEAREST unconditionally, "to keep GBA pixels crisp", and
// at 1x that is exactly right: one GBA pixel is one 3DS pixel, no resampling
// happens, and any filter would only blur a perfect image.
//
// At 1.5x it is the bug. 240 -> 360 and 160 -> 240 are both 3:2, so nearest has
// no choice but to emit source columns and rows as 1,2,1,2,... pixels wide.
// Every glyph stroke and every sprite edge is then randomly one or two pixels
// thick depending on where it happens to land, which is the "pixels are not
// aligned" artifacting. It is not a race and not a bug in the upload path; it
// is what point sampling a 3:2 ratio means. FILL is worse still, being 5:3
// horizontally.
//
// open_agb_firm, which is how most people play GBA titles on a 3DS, does not
// point sample either: its `scaler` setting is none / bilinear / matrix and it
// defaults to `matrix`, a filtered scale. GPU_LINEAR is the 3DS GPU's built-in
// equivalent of its `bilinear`, and it costs nothing extra to sample.
//
// So: nearest at the integer scale, linear at the two fractional ones.
//
// Safe to call before the texture exists. CtrSettingsLoad() restores the saved
// scale through Ctr3dsApplyTopScale() before CtrVideoInit() has run, so the
// guard lives here rather than at the call site: this is the only function that
// touches the texture, and putting the check anywhere else needs sReady visible
// higher up the file than it is declared.
static void apply_top_filter(void)
{
    GPU_TEXTURE_FILTER_PARAM f =
        (sTopScale == CTR_TOP_SCALE_1X) ? GPU_NEAREST : GPU_LINEAR;

    if (!sReady)
        return;   // CtrVideoInit() applies it itself once the texture exists

    C3D_TexSetFilter(&sTopTex, f, f);
}

#if CTR_BOOT_DIAG
// Kept so the diagnostics below can read DISPCNT; ppu_set_memory() otherwise
// consumes these and video.c never needs them again.
static const uint8_t *sRegBase;
#endif

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

    // The bottom screen is drawn 1:1 and never resampled, so nearest is always
    // right there. The top screen depends on the scale, and its filter is set
    // at the bottom of this function: apply_top_filter() is a no-op until
    // sReady, precisely so the pre-init call from CtrSettingsLoad() is safe.
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

    // Point the PPU at the game's memory regions. Must happen after
    // Ctr3dsInitGbaMemory().
    const void *reg, *pal, *vram, *oam;
    CtrGetGbaRegions(&reg, &pal, &vram, &oam);
    ppu_set_memory(reg, pal, vram, oam);
#if CTR_BOOT_DIAG
    sRegBase = (const uint8_t *)reg;
#endif

    sReady = 1;

    // After sReady, or the guard inside it would swallow this one too. The
    // filter is only read when the texture is bound, so setting it last in init
    // is no different from setting it first.
    apply_top_filter();

    return 1;
}

void CtrVideoExit(void)
{
    if (!sReady)
        return;

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
// Presented once, before AgbMain(), so a black screen stops being ambiguous.
// Without it "hung inside the game's init" and "never got as far as running"
// look identical. Solid blue top / green bottom means everything in this file
// works and the game is what stalled.
void CtrDiagSplash(void)
{
    if (!sReady)
        return;

    // A few frames: one alone can be lost to double buffering.
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

// Three profile names per caller, not one name plus a suffix built at runtime:
// CtrProfile keys on the string's ADDRESS, so a constructed name would never
// match itself and every sample would open a new stage.
//
// Split three ways because that is the open question. A bottom repaint costs
// about a whole VBlank and the paint itself is nowhere near that, so the cost
// is somewhere in here -- and which of the three it is decides whether the fix
// is a narrower transfer, a smaller flush, or not blocking on the transfer at
// all. The top screen carries the same three as the control: it uploads on
// every frame at 256x160 against the bottom's 512x240, so it is the same code
// doing a third of the work.
static const char *const kProfTop[3] = {
    "upload.top.copy", "upload.top.flush", "upload.top.xfer"
};
static const char *const kProfBot[3] = {
    "upload.bot.copy", "upload.bot.flush", "upload.bot.xfer"
};

// Copy a linear w x h RGB565 image into a wider staging buffer, then let the
// transfer engine tile it into the texture.
//
// The TOP screen's path, and now only that. It stays whole-image because it has
// to be: the game's frame is new every frame and there is nowhere to spread the
// cost to. At 256x160 it moves 81,920 bytes, which fits. The bottom screen is
// three times that and got its own sliced path below.
static void upload(uint16_t *stage, int stageW, const uint16_t *src,
                   int w, int h, C3D_Tex *tex, const char *const *prof)
{
    unsigned long long t = CtrTicksNow();

    for (int y = 0; y < h; y++)
        memcpy(stage + (size_t)y * stageW, src + (size_t)y * w, (size_t)w * 2);

    CtrProfile(prof[0], t);
    t = CtrTicksNow();

    // Note this flushes the WHOLE stage, padding included: stageW is 512 for a
    // 320-wide bottom screen, so 37% of both this and the transfer below is
    // blank. Whether that matters is what the numbers are for.
    GSPGPU_FlushDataCache(stage, (size_t)stageW * h * sizeof(uint16_t));

    CtrProfile(prof[1], t);
    t = CtrTicksNow();

    C3D_SyncDisplayTransfer((u32 *)stage, GX_BUFFER_DIM(stageW, h),
                            (u32 *)tex->data, GX_BUFFER_DIM(stageW, h),
                            TEX_TRANSFER_FLAGS);

    CtrProfile(prof[2], t);
}

// The bottom screen's upload, pushed a slice at a time.
//
// The asymmetry here is the whole design: the TOP screen has to hold 60fps, and
// the bottom is allowed to arrive late. Before this, it was not allowed to --
// a full bottom upload happened inside one frame, and it cost the game a whole
// VBlank every time the UI repainted. Measured through the animations that
// caused it: repainting 60 times a second ran the game at 30fps, 10 times a
// second at 53, which is fps = 3600 / (60 + repaints per second).
//
// A full bottom upload moves 245,760 bytes -- THREE TIMES the top screen's,
// because the stage is 512 wide for a 320-wide image -- through a blocking
// transfer. So the frame budget picks the slice, not the picture: 48 rows is
// 49,152 bytes, under two thirds of what the top screen already uploads every
// frame without trouble. A repaint takes five frames to reach the panel instead
// of one, and on a screen whose animations step five times a second that is
// invisible.
//
// 48 because it is a multiple of 8. A tiled texture stores eight rows to a
// strip and strips run in order, so any 8-row-aligned band is a contiguous run
// in BOTH buffers: the source at row * BOT_TEX_W, the destination at
// (row / 8) * BOT_TEX_W * 8. TEX_TRANSFER_FLAGS has no flip and no scaling, so
// a band lands where it was taken from.
//
// That destination arithmetic is the one thing in here that hardware has to
// confirm. If it is wrong the bottom screen shows its 48-row bands stacked in
// the wrong order -- unmistakable, and harmless to everything else.
#define BOT_CHUNK_ROWS 48

static int sBotRow = CTR_BOTTOM_HEIGHT;   // next row to push; height means idle

// Copied whole, and only the transfer is sliced. The UI can repaint again while
// a slice run is in flight, and a source copied in pieces across those frames
// would tear between them. 153,600 bytes in one pass is cheap; it was never the
// expensive half.
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

    // Flush only the band about to move, not the whole stage. Same total work
    // across a repaint, spread over the frames that do it.
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

void CtrVideoPresent(void)
{
    // Timed in pieces, because "presenting is slow" is not a finding. The top
    // upload runs on every displayed frame and the bottom one only after the UI
    // has repainted, so the top is the control: if both overrun, the transfer
    // path is at fault; if only the bottom does, it is something about the
    // 512-wide stride or about this being the once-in-a-while cold path.
    unsigned int tPresent = CtrTimeNowMs();
    unsigned int t0;

    if (!sReady)
        return;

    // Rasterise the frame the game just finished writing.
    t0 = CtrTimeNowMs();
    {
        unsigned long long tp = CtrTicksNow();
        ppu_render_rgb565(sGbaFrame, sGbaLayer);
        CtrProfile("ppu", tp);
    }
    CtrLogSlow("ppu", t0);

#if CTR_BOOT_DIAG
    // Two facts decide where a black screen comes from:
    //   DISPCNT == 0x0080 (forced blank) or 0 -> the game is not driving the
    //   display, so the PPU is right to emit black; the fault is upstream.
    //   Otherwise a wholly black frame means the game IS driving the display
    //   and the PPU or its memory pointers are at fault.
    {
        static unsigned frame;
        frame++;
        if (frame <= 3 || frame == 60 || frame == 600) {
            uint16_t dispcnt = sRegBase ? (uint16_t)(sRegBase[0] | (sRegBase[1] << 8)) : 0xFFFF;
            unsigned nonblack = 0;
            for (int i = 0; i < CTR_GBA_WIDTH * CTR_GBA_HEIGHT; i++)
                if (sGbaFrame[i]) nonblack++;
            CtrTrace("emerald3ds: frame %u DISPCNT=%04x nonblack=%u/%u\n",
                     frame, dispcnt, nonblack,
                     (unsigned)(CTR_GBA_WIDTH * CTR_GBA_HEIGHT));
        }
    }
#endif
    t0 = CtrTimeNowMs();
    upload(sTopStage, TOP_TEX_W, sGbaFrame, CTR_GBA_WIDTH, CTR_GBA_HEIGHT,
           &sTopTex, kProfTop);
    CtrLogSlow("upload.top", t0);

    // The bottom screen is mostly static, so only re-tile it when the UI says
    // something actually changed -- and then hand it over a slice per frame
    // rather than all at once, so no single frame loses its budget to it.
    //
    // Cleared as soon as the snapshot is taken, not when the run finishes: a
    // repaint arriving mid-run is a NEW picture, and it gets its own run after
    // this one rather than being lost or tearing into it.
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

    // THE frame's sync point, and the one number that says whether this port has
    // any budget left. C3D_FRAME_SYNCDRAW waits here, at Begin, for the previous
    // frame's rendering -- not at C3D_FrameEnd, whatever the comment below that
    // call used to claim. Measured `frameend` came back at 311 us, which is what
    // proved it: a real VBlank wait on a frame with slack is milliseconds.
    //
    // So read this as the slack. Milliseconds means the frame has room and
    // something is briefly overrunning it; near zero means the port is already
    // saturated and any added work at all costs a frame.
    {
        unsigned long long tb = CtrTicksNow();
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        CtrProfile("framebegin", tb);
    }

#if CTR_BOOT_DIAG
    // Liveness, visible without the log: cycling bars mean the frame loop is
    // running, a permanently black screen means it is not. How much shows
    // depends on the scale mode -- 20 px each side at the default 1.5x, a wide
    // border at 1x, and nothing at all in FILL, where the image covers the
    // whole panel.
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
        // Derived, not tabulated: the offset is always whatever centres the
        // scaled image, so the two can never disagree.
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
        // NOT the VBlank wait, despite what this comment said for a long time.
        // Measured at 311 us mean, which is far too short to be one: with
        // C3D_FRAME_SYNCDRAW the wait is at C3D_FrameBegin above. This is just
        // the submit, and it is kept measured because a submit that starts
        // taking milliseconds means the GPU or the GSP event thread is behind.
        CtrProfile("frameend", tf);
    }
    CtrLogSlow("frameend", t0);

    CtrLogSlow("present", tPresent);
}
