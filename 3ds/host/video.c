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
// Scaling: 240x160 * 1.5 = 360x240, which fills the top screen's full height
// exactly, with a 20 px pillarbox each side. Nearest-neighbour keeps it sharp.

#include <3ds.h>
#include <citro2d.h>
#include <string.h>

#include "../bridge.h"
#include "trace.h"
#include "../../rp2350/ppu.h"

#define TOP_TEX_W  256
#define TOP_TEX_H  256
#define BOT_TEX_W  512
#define BOT_TEX_H  256

#define GBA_SCALE  1.5f
#define GBA_DRAW_X ((400.0f - CTR_GBA_WIDTH  * GBA_SCALE) / 2.0f)   // 20.0
#define GBA_DRAW_Y ((240.0f - CTR_GBA_HEIGHT * GBA_SCALE) / 2.0f)   //  0.0

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

    // Nearest keeps GBA pixels crisp at 1.5x instead of smearing them.
    C3D_TexSetFilter(&sTopTex, GPU_NEAREST, GPU_NEAREST);
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

// Copy a linear w x h RGB565 image into a wider staging buffer, then let the
// transfer engine tile it into the texture.
static void upload(uint16_t *stage, int stageW, const uint16_t *src,
                   int w, int h, C3D_Tex *tex)
{
    for (int y = 0; y < h; y++)
        memcpy(stage + (size_t)y * stageW, src + (size_t)y * w, (size_t)w * 2);

    GSPGPU_FlushDataCache(stage, (size_t)stageW * h * sizeof(uint16_t));
    C3D_SyncDisplayTransfer((u32 *)stage, GX_BUFFER_DIM(stageW, h),
                            (u32 *)tex->data, GX_BUFFER_DIM(stageW, h),
                            TEX_TRANSFER_FLAGS);
}

void CtrVideoPresent(void)
{
    if (!sReady)
        return;

    // Rasterise the frame the game just finished writing.
    ppu_render_rgb565(sGbaFrame, sGbaLayer);

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
    upload(sTopStage, TOP_TEX_W, sGbaFrame, CTR_GBA_WIDTH, CTR_GBA_HEIGHT, &sTopTex);

    // The bottom screen is mostly static, so only re-tile it when the UI says
    // something actually changed.
    if (CtrBottomIsDirty()) {
        upload(sBotStage, BOT_TEX_W, CtrBottomFramebuffer(),
               CTR_BOTTOM_WIDTH, CTR_BOTTOM_HEIGHT, &sBotTex);
        CtrBottomClearDirty();
    }

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

#if CTR_BOOT_DIAG
    // Liveness, visible without the log: the GBA image is 360 px on a 400 px
    // screen, so this shows as a 20 px bar down each side. Cycling bars mean the
    // frame loop is running; a permanently black screen means it is not.
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
    C2D_DrawImageAt(sTopImage, GBA_DRAW_X, GBA_DRAW_Y, 0.0f, NULL,
                    GBA_SCALE, GBA_SCALE);

    C2D_TargetClear(sBotTarget, C2D_Color32(0, 0, 0, 0xFF));
    C2D_SceneBegin(sBotTarget);
    C2D_DrawImageAt(sBotImage, 0.0f, 0.0f, 0.0f, NULL, 1.0f, 1.0f);

    C3D_FrameEnd(0);
}
