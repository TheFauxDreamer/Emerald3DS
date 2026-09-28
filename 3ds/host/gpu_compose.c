// The GPU compositor. See gpu_compose.h for the interface.
//
// The rules are those of rp2350/ppu.c, not of the GBA. Where the two differ,
// this file does what ppu.c does:
// - Mosaic is ignored.
// - An OBJ in mode 1 (semi-transparent) draws opaque.
// - A sprite can alpha-blend over a sprite.
// - A window edge at x = 0 or 240 continues into the wide margin.
//
// What it cannot do, GpuComposeCheck() refuses (the GPU_WHY_* bits): the
// bitmap modes, the OBJ window, and the two reference quirks that read the
// previous frame. Those need the old pixels, and this file clears the surface.
//
// HOW A FRAME IS DRAWN
//
// 1. Tiles. Each tile is decoded once into a 1024x1024 RGBA5551 atlas, for
//    each palette that uses it. A slot is decoded again only when its bytes
//    change, or a colour that it uses changes. BLDY brighten and darken go
//    into the colours of a second palette set, so they come out exactly as
//    ppu.c makes them.
// 2. Text BGs. Each has a texture as large as its map, and one more for the
//    side map of the wide field. Only the cells on the screen whose tile,
//    palette or flip changed are drawn again. Then a BG costs one quad for
//    each window region. (This is the v0.1.1 change of pokeemerald-3Ds-
//    dualscreen that made its field fast on an Old 3DS.)
// 3. The surface. It is cleared to the backdrop, then each window region
//    draws its layers from priority 3 to 0: the BGs from the highest number
//    down, then the sprites of that priority, OAM 127 to 0.
//
// ALPHA BLEND (BLDCNT effect 1)
//
// A first-target pixel blends only if the pixel below it is a second target.
// Stencil bit 0 records "a second target is here". A first-target layer draws
// twice. The first pass is where bit 0 is clear: plain colour, and it sets a
// mark bit so the second pass skips those pixels. The second pass is where
// bit 0 is set and the mark is clear: blended. Seven marks are used in turn,
// then one quad clears them. Sprites that blend draw one at a time, because a
// sprite can land on a sprite of the same pass.
//
// AFFINE LAYERS
//
// A translation (pa = pd = 256, pb = pc = 0) draws as tiles at whole pixels,
// exactly. Any other matrix draws each tile as a transformed quad under a
// scissor. The quads are placed so that each pixel centre samples the texel
// that the GBA formula gives, plus 1/512 texel so that no pixel lands on an
// edge. The GPU interpolates with less precision than that, so a pixel on a
// tile edge can take its neighbour's texel. That is the one known difference.

#include <3ds.h>
#include <citro2d.h>
#include <string.h>

#include "../bridge.h"
#include "trace.h"
#include "gpu_compose.h"

const char *const kGpuWhyName[GPU_WHY_COUNT] = {
    "bitmap", "objwin", "window", "blendback",
    "singular", "fade", "budget", "vram",
};

#define GBA_W 240
#define GBA_H 160

// The surface. GBA x = 0 is column ORIGIN_X, as in the ppu.c stage of video.c.
#define SURF_W   512
#define SURF_H   256
#define ORIGIN_X CTR_WIDE_MARGIN

// The tile atlas: 128 x 128 slots of 8x8.
#define ATLAS_SIZE  1024
#define SLOT_COUNT  16384
#define HASH_BITS   15
#define HASH_SIZE   (1u << HASH_BITS)
// A frame uses fewer new tiles than this. Above it, empty the cache between
// frames, never during one: a slot that a queued quad reads must not change.
#define SLOT_RESET_AT (SLOT_COUNT - 6144)

// Palette ids: 0-15 BG banks, 16-31 OBJ banks, 32 BG 256-colour, 33 OBJ
// 256-colour. Add PAL_FX for the same palette with BLDY applied.
#define PAL_BG256  32
#define PAL_OBJ256 33
#define PAL_FX     34
#define PAL_IDS    (2 * PAL_FX)

// Offset from the GBA sample point, in texels. See AFFINE LAYERS.
#define AFFINE_BIAS (1.0f / 512.0f)

// A fade changes this many BG banks or more in one frame. ppu.c draws such a
// frame, because every visible cell of every BG would change.
#define FADE_BANKS 8

// Most tile quads that one affine BG may take in one window region.
#define AFFINE_TILE_BUDGET 6144

// ---- frame state --------------------------------------------------------------

static struct {
    const uint8_t *reg, *pal, *vram, *oam;
    uint16_t dispcnt;
    int mode;
    int effect, src, dst, eva, evb, evy;
    int xbeg, xend;                 // the drawn columns, margins included
    const uint16_t *side[4];
    unsigned sideDelta;
} F;

static inline uint16_t rd16(const uint8_t *p, unsigned off)
{
    return (uint16_t)(p[off] | (p[off + 1] << 8));
}

static inline uint16_t reg16(unsigned off)
{
    return rd16(F.reg, off);
}

static inline int32_t sext16(uint32_t v) { return (int32_t)(v << 16) >> 16; }
static inline int32_t sext28(uint32_t v) { return (int32_t)(v << 4) >> 4; }

static void load_state(const GpuComposeInput *in)
{
    uint16_t bldcnt, alpha;

    F.reg = in->reg;
    F.pal = in->pal;
    F.vram = in->vram;
    F.oam = in->oam;
    F.dispcnt = reg16(0);
    F.mode = F.dispcnt & 7;
    bldcnt = reg16(0x50);
    alpha = reg16(0x52);
    F.effect = (bldcnt >> 6) & 3;
    F.src = bldcnt & 0x3f;
    F.dst = (bldcnt >> 8) & 0x3f;
    F.eva = (alpha & 0x1f) < 16 ? (alpha & 0x1f) : 16;
    F.evb = ((alpha >> 8) & 0x1f) < 16 ? ((alpha >> 8) & 0x1f) : 16;
    F.evy = (reg16(0x54) & 0x1f) < 16 ? (reg16(0x54) & 0x1f) : 16;
    F.xbeg = -in->margin;
    F.xend = GBA_W + in->margin;
    for (int i = 0; i < 4; i++)
        F.side[i] = in->margin ? in->side[i] : NULL;
    F.sideDelta = in->sideDelta;
}

// ---- window regions -------------------------------------------------------------
//
// The same clamps as ppu.c (inWindowRange, winrowFill): an end past the screen
// or before the start means the screen edge, and a start at or past the end
// means empty. Horizontally, an edge at 0 or 240 continues into the margin.

typedef struct {
    int x0, x1, y0, y1;
    uint8_t mask;
} Rect;

#define RECT_MAX 25

static int win_h(uint16_t r, int *a, int *b)
{
    int s = r >> 8, e = r & 0xff;

    if (s > e || e > GBA_W)
        e = GBA_W;
    if (s >= e)
        return 0;
    *a = (s == 0) ? F.xbeg : s;
    *b = (e == GBA_W) ? F.xend : e;
    return 1;
}

static int win_v(uint16_t r, int *a, int *b)
{
    int s = r >> 8, e = r & 0xff;

    if (s > e || e > GBA_H)
        e = GBA_H;
    if (s >= e)
        return 0;
    *a = s;
    *b = e;
    return 1;
}

static void sort_unique(int *v, int *n)
{
    int k = 0;

    for (int i = 1; i < *n; i++)
        for (int j = i; j > 0 && v[j] < v[j - 1]; j--) {
            int t = v[j];
            v[j] = v[j - 1];
            v[j - 1] = t;
        }
    for (int i = 0; i < *n; i++)
        if (k == 0 || v[i] != v[k - 1])
            v[k++] = v[i];
    *n = k;
}

// Cut the screen into rectangles of one window mask each. Returns the count.
static int build_rects(Rect *out)
{
    int on[2], wx0[2], wx1[2], wy0[2], wy1[2];
    int xs[6], ys[6], nx = 0, ny = 0, n = 0;

    if (!(F.dispcnt & 0xe000)) {
        out[0] = (Rect){ F.xbeg, F.xend, 0, GBA_H, 0x3f };
        return 1;
    }

    xs[nx++] = F.xbeg;
    xs[nx++] = F.xend;
    ys[ny++] = 0;
    ys[ny++] = GBA_H;
    for (int w = 0; w < 2; w++) {
        on[w] = (F.dispcnt & (0x2000 << w))
             && win_h(reg16(0x40 + 2 * w), &wx0[w], &wx1[w])
             && win_v(reg16(0x44 + 2 * w), &wy0[w], &wy1[w]);
        if (on[w]) {
            xs[nx++] = wx0[w];
            xs[nx++] = wx1[w];
            ys[ny++] = wy0[w];
            ys[ny++] = wy1[w];
        }
    }
    sort_unique(xs, &nx);
    sort_unique(ys, &ny);

    for (int j = 0; j + 1 < ny; j++)
        for (int i = 0; i + 1 < nx; i++) {
            int x = xs[i], y = ys[j];
            uint8_t mask = reg16(0x4a) & 0x3f;

            if (on[1] && x >= wx0[1] && x < wx1[1] && y >= wy0[1] && y < wy1[1])
                mask = (reg16(0x48) >> 8) & 0x3f;
            if (on[0] && x >= wx0[0] && x < wx1[0] && y >= wy0[0] && y < wy1[0])
                mask = reg16(0x48) & 0x3f;
            out[n++] = (Rect){ x, xs[i + 1], y, ys[j + 1], mask };
        }
    return n;
}

// ---- the check ------------------------------------------------------------------

static uint8_t  sPrevPal[0x200];   // the BG palette of the last frame
static int      sPrevFx = -1;      // effect and level of the last frame
static int      sPrevValid;

static int affine_singular(int pa, int pb, int pc, int pd)
{
    return (int32_t)pa * pd - (int32_t)pb * pc == 0;
}

unsigned GpuComposeCheck(const GpuComposeInput *in)
{
    unsigned why = 0;
    int banks = 0, fx;

    load_state(in);

    if (F.mode > 2)
        why |= GPU_WHY_BITMAP;
    if (F.dispcnt & 0x8000)
        why |= GPU_WHY_OBJWIN;
    if (F.effect == 1 && (F.src & 0x20))
        why |= GPU_WHY_BLENDBACK;

    // A region without bit 5 skips the backdrop, and ppu.c keeps the pixels of
    // the frame before there.
    if (!(why & GPU_WHY_OBJWIN) && (F.dispcnt & 0x6000)) {
        Rect r[RECT_MAX];
        int n = build_rects(r);

        for (int i = 0; i < n; i++)
            if (!(r[i].mask & 0x20))
                why |= GPU_WHY_WINDOW;
    }

    if (F.mode >= 1 && F.mode <= 2) {
        for (int bg = 2; bg < 4; bg++) {
            uint32_t base = (bg == 2) ? 0x20 : 0x30;

            if (!(F.dispcnt & (0x100 << bg)) || (F.mode == 1 && bg == 3))
                continue;
            if (affine_singular(sext16(reg16(base)), sext16(reg16(base + 2)),
                                sext16(reg16(base + 4)), sext16(reg16(base + 6))))
                why |= GPU_WHY_SINGULAR;
        }
    }
    if (F.dispcnt & 0x1000) {
        for (int i = 0; i < 128; i++) {
            uint16_t a0 = rd16(F.oam, i * 8), a1 = rd16(F.oam, i * 8 + 2);
            uint32_t mb;

            if (!(a0 & 0x100) || ((a0 >> 14) & 3) == 3 || ((a0 >> 10) & 3) == 2)
                continue;
            mb = ((a1 >> 9) & 31) * 32;
            if (affine_singular(sext16(rd16(F.oam, mb + 6)), sext16(rd16(F.oam, mb + 14)),
                                sext16(rd16(F.oam, mb + 22)), sext16(rd16(F.oam, mb + 30))))
                why |= GPU_WHY_SINGULAR;
        }
    }

    // A fade: many BG banks changed since the last frame, or BLDY moved on a
    // BG that it targets.
    for (int b = 0; b < 16; b++)
        if (memcmp(sPrevPal + b * 32, F.pal + b * 32, 32) != 0)
            banks++;
    fx = (F.effect >= 2) ? (F.effect << 8) | F.evy : -1;
    if (sPrevValid && banks >= FADE_BANKS)
        why |= GPU_WHY_FADE;
    if (sPrevValid && fx != sPrevFx && (F.effect >= 2 || sPrevFx >= 0)
        && (F.src & (F.dispcnt >> 8) & 0x0f))
        why |= GPU_WHY_FADE;
    memcpy(sPrevPal, F.pal, sizeof(sPrevPal));
    sPrevFx = fx;
    sPrevValid = 1;

    return why;
}

// ---- palettes -------------------------------------------------------------------
//
// Two RGBA5551 copies of palette RAM: plain, and with BLDY applied as ppu.c
// applies it (pal565fx). A palette id has a version, and the bits of the
// colours that the last version step changed.

static uint16_t sColN[512], sColF[512];
static uint32_t sPalVer[PAL_IDS];
static uint8_t  sPalChg[PAL_IDS][32];
static uint8_t  sPalTouched[PAL_IDS];
static int      sPalValid;

static inline uint16_t rgba5551(int r5, int g5, int b5)
{
    return (uint16_t)((r5 << 11) | (g5 << 6) | (b5 << 1) | 1);
}

// ppu.c's 8-bit component of a 5-bit one.
static inline int c8(int c5)
{
    return c5 * 255 / 31;
}

static inline int fx8(int c)
{
    return (F.effect == 2) ? c + (((255 - c) * F.evy) >> 4) : c - ((c * F.evy) >> 4);
}

static void pal_mark(int id, int bit)
{
    if (!sPalTouched[id]) {
        sPalTouched[id] = 1;
        memset(sPalChg[id], 0, sizeof(sPalChg[id]));
        sPalVer[id]++;
    }
    sPalChg[id][bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

// Palette index i changed in the copy that starts at id offset 'fx'.
static void pal_changed(int i, int fx)
{
    int bank = i >> 4;

    pal_mark(fx + bank, i & 15);
    pal_mark(fx + (i < 256 ? PAL_BG256 : PAL_OBJ256), i & 255);
}

static void update_palettes(void)
{
    memset(sPalTouched, 0, sizeof(sPalTouched));

    for (int i = 0; i < 512; i++) {
        uint16_t c = rd16(F.pal, i * 2);
        int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        uint16_t n = rgba5551(r, g, b);

        if (!sPalValid || n != sColN[i]) {
            sColN[i] = n;
            pal_changed(i, 0);
        }
        if (F.effect >= 2) {
            uint16_t f = rgba5551(fx8(c8(r)) >> 3, fx8(c8(g)) >> 3, fx8(c8(b)) >> 3);

            if (!sPalValid || f != sColF[i]) {
                sColF[i] = f;
                pal_changed(i, PAL_FX);
            }
        }
    }
    sPalValid = 1;
}

// ---- the tile atlas ---------------------------------------------------------------

typedef struct {
    uint32_t key;        // (address / 32) * PAL_IDS + palette id + 1
    uint32_t serial;     // new at each decode, never used again; 0 = none yet
    uint32_t checked;    // the frame of the last check
    uint32_t palVer;     // the palette version of the decode
    uint8_t  bytes[64];
    uint8_t  used[32];   // the colour indices that the tile uses
    uint8_t  visible;
} Slot;

static Slot     sSlot[SLOT_COUNT];
static uint16_t sHash[HASH_SIZE];   // slot + 1, or 0
static unsigned sSlotsUsed;
static uint32_t sSerial;
static uint32_t sFrame;
static int      sResetCache;
static C3D_Tex  sAtlas;
static uint8_t  sMorton[64];

static unsigned sFail;              // GPU_WHY_* bits of this draw
static unsigned sQuads, sDecodes;   // counted for the log

static void cache_reset(void)
{
    memset(sHash, 0, sizeof(sHash));
    sSlotsUsed = 0;
}

static int pal_still_good(const Slot *s, unsigned id)
{
    uint32_t ver = sPalVer[id];

    if (s->palVer == ver)
        return 1;
    if (s->palVer + 1 != ver)
        return 0;
    for (int i = 0; i < 32; i++)
        if (s->used[i] & sPalChg[id][i])
            return 0;
    return 1;
}

static void decode(unsigned slot, Slot *s, unsigned id, int c256)
{
    const uint16_t *col = (id >= PAL_FX) ? sColF : sColN;
    unsigned base, pid = id % PAL_FX;
    uint16_t *dst = (uint16_t *)sAtlas.data + slot * 64;

    if (pid < 32)
        base = (pid < 16) ? pid * 16 : 256 + (pid - 16) * 16;
    else
        base = (pid == PAL_BG256) ? 0 : 256;

    memset(s->used, 0, sizeof(s->used));
    s->visible = 0;
    for (int p = 0; p < 64; p++) {
        int idx = c256 ? s->bytes[p] : (s->bytes[p >> 1] >> ((p & 1) * 4)) & 15;

        dst[sMorton[p]] = idx ? col[base + idx] : 0;
        if (idx) {
            s->used[idx >> 3] |= (uint8_t)(1u << (idx & 7));
            s->visible = 1;
        }
    }
    s->serial = ++sSerial;
    sDecodes++;
}

// The slot of a tile in one palette. -1 for a tile with no visible pixel, or
// outside VRAM. -2 when the cache is full: the draw must stop.
static int get_slot(unsigned addr, unsigned id, int c256)
{
    unsigned size = c256 ? 64 : 32;
    uint32_t key;
    unsigned h, idx;
    Slot *s;

    if (addr + size > 0x18000)
        return -1;
    key = (addr >> 5) * PAL_IDS + id + 1;
    h = (key * 2654435761u) >> (32 - HASH_BITS);
    while (sHash[h] && sSlot[sHash[h] - 1].key != key)
        h = (h + 1) & (HASH_SIZE - 1);
    if (!sHash[h]) {
        if (sSlotsUsed >= SLOT_COUNT) {
            sResetCache = 1;
            sFail |= GPU_WHY_BUDGET;
            return -2;
        }
        sHash[h] = (uint16_t)++sSlotsUsed;
        s = &sSlot[sSlotsUsed - 1];
        s->key = key;
        s->serial = 0;
        s->checked = 0;
    }
    idx = sHash[h] - 1;
    s = &sSlot[idx];
    if (s->checked != sFrame) {
        const uint8_t *src = F.vram + addr;

        if (s->serial == 0 || memcmp(s->bytes, src, size) != 0 || !pal_still_good(s, id)) {
            memcpy(s->bytes, src, size);
            decode(idx, s, id, c256);
        }
        s->palVer = sPalVer[id];
        s->checked = sFrame;
    }
    return s->visible ? (int)idx : -1;
}

// ---- quads ------------------------------------------------------------------------

// A w x h part of a texture, texel (u, v) at its top left, drawn at (x, y). A
// negative scale mirrors it in place (citro2d swaps the texture coordinates).
static int quad(C3D_Tex *tex, float u, float v, int w, int h,
                float x, float y, float sx, float sy)
{
    const float tw = tex->width, th = tex->height;
    Tex3DS_SubTexture sub = {
        (u16)w, (u16)h, u / tw, 1.0f - v / th, (u + w) / tw, 1.0f - (v + h) / th
    };

    if (!C2D_DrawImageAt((C2D_Image){ tex, &sub }, x, y, 0.0f, NULL, sx, sy)) {
        sFail |= GPU_WHY_BUDGET;
        return 0;
    }
    sQuads++;
    return 1;
}

static inline float slot_u(int slot) { return (float)((slot & 127) * 8); }
static inline float slot_v(int slot) { return (float)((slot >> 7) * 8); }

// A full 8x8 tile at (x, y) of the current target.
static int tile_at(int slot, float x, float y, int fh, int fv)
{
    return quad(&sAtlas, slot_u(slot), slot_v(slot), 8, 8, x, y, fh ? -1.0f : 1.0f,
                fv ? -1.0f : 1.0f);
}

// A tile at GBA (x, y), cut to the clip rectangle, on the surface.
static int tile_clipped(int slot, int x, int y, int fh, int fv, const Rect *c)
{
    int a = c->x0 - x, b = c->x1 - x, t = c->y0 - y, d = c->y1 - y;
    int u0, v0;

    if (a < 0) a = 0;
    if (b > 8) b = 8;
    if (t < 0) t = 0;
    if (d > 8) d = 8;
    if (a >= b || t >= d)
        return 1;
    u0 = fh ? 8 - b : a;
    v0 = fv ? 8 - d : t;
    return quad(&sAtlas, slot_u(slot) + u0, slot_v(slot) + v0, b - a, d - t,
                (float)(ORIGIN_X + x + a), (float)(y + t), fh ? -1.0f : 1.0f,
                fv ? -1.0f : 1.0f);
}

static int clip_to(Rect *out, const Rect *a, int x0, int x1, int y0, int y1)
{
    out->x0 = a->x0 > x0 ? a->x0 : x0;
    out->x1 = a->x1 < x1 ? a->x1 : x1;
    out->y0 = a->y0 > y0 ? a->y0 : y0;
    out->y1 = a->y1 < y1 ? a->y1 : y1;
    out->mask = a->mask;
    return out->x0 < out->x1 && out->y0 < out->y1;
}

// ---- GPU state --------------------------------------------------------------------

static C3D_Tex           sSurf;
static C3D_RenderTarget *sSurfTarget;
static int               sReady;

static void blend_opaque(void)
{
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
}

// out = src * EVA/16 + dst * EVB/16, each part clamped by the GPU.
static void blend_alpha(void)
{
    unsigned a = (unsigned)(F.eva * 255 + 8) / 16, b = (unsigned)(F.evb * 255 + 8) / 16;

    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_COLOR, GPU_CONSTANT_ALPHA,
                   GPU_ONE, GPU_ZERO);
    C3D_BlendingColor(a | (a << 8) | (a << 16) | (b << 24));
}

static void state_begin(void)
{
    C2D_Flush();
    C2D_ViewReset();
    C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaTest(true, GPU_GREATER, 0);
    blend_opaque();
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0xff, 0);
    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}

// Put back what citro2d and video.c expect: C3D_Init's blend and C2D_Prepare's
// depth test.
static void state_end(void)
{
    C2D_Flush();
    C2D_ViewReset();
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0xff, 0);
    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_ALL);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}

// Scissor to a rectangle in GBA coordinates. The framebuffer counts its rows
// from the other edge.
static void scissor(const Rect *r)
{
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(ORIGIN_X + r->x0), (u32)(SURF_H - r->y1),
                   (u32)(ORIGIN_X + r->x1), (u32)(SURF_H - r->y0));
}

static void scissor_off(void)
{
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}

// Screen = M * t + (tx, ty), for t in the texel space of an affine layer.
static void view_affine(float m00, float m01, float m10, float m11, float tx, float ty)
{
    C3D_Mtx m;

    Mtx_Identity(&m);
    m.r[0] = FVec4_New(m00, m01, 0.0f, tx);
    m.r[1] = FVec4_New(m10, m11, 0.0f, ty);
    C2D_ViewRestore(&m);
}

// ---- text BG textures -----------------------------------------------------------

#define CELLS_MAX (64 * 64)

typedef struct {
    C3D_Tex tex;
    C3D_RenderTarget *target;
    int w, h;            // 0 = no texture
    int release;         // GpuComposeMaintain deletes it
    int valid;           // sig[] describes the texture
    uint32_t sig[CELLS_MAX];
} Layer;

static Layer sMain[4], sSide[4];

static void layer_free(Layer *L)
{
    if (L->target)
        C3D_RenderTargetDelete(L->target);
    if (L->w)
        C3D_TexDelete(&L->tex);
    L->target = NULL;
    L->w = L->h = 0;
    L->release = 0;
    L->valid = 0;
}

// A texture of w x h for the layer. 0 if it cannot have one this frame.
static int layer_ensure(Layer *L, int w, int h)
{
    if (L->w == w && L->h == h && !L->release)
        return 1;
    if (L->w) {
        L->release = 1;   // a new size: free it between frames first
        return 0;
    }
    if (!C3D_TexInitVRAM(&L->tex, (u16)w, (u16)h, GPU_RGBA5551))
        return 0;
    L->target = C3D_RenderTargetCreateFromTex(&L->tex, GPU_TEXFACE_2D, 0, -1);
    if (!L->target) {
        C3D_TexDelete(&L->tex);
        return 0;
    }
    C3D_TexSetFilter(&L->tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&L->tex, GPU_REPEAT, GPU_REPEAT);
    L->w = w;
    L->h = h;
    L->valid = 0;
    return 1;
}

// One BG of this frame, decoded as ppu.c's buildBgConfigs does.
typedef struct {
    int bg, affine, prio, fx;
    unsigned chars, screen;
    int c256, size, cols, rows, hofs, vofs;
    int asize, wrap;
    int32_t pa, pb, pc, pd, refX, refY;
} Bg;

static Bg  sBg[4];
static int sNbg;

static void build_bgs(void)
{
    sNbg = 0;
    for (int bg = 0; bg < 4; bg++) {
        int affine = -1;
        uint16_t cnt;
        Bg *c;

        if (!(F.dispcnt & (0x100 << bg)))
            continue;
        if (F.mode == 0 || (F.mode == 1 && bg < 2))
            affine = 0;
        else if ((F.mode == 1 && bg == 2) || (F.mode == 2 && bg >= 2))
            affine = 1;
        if (affine < 0)
            continue;
        cnt = reg16(8 + bg * 2);
        c = &sBg[sNbg++];
        c->bg = bg;
        c->affine = affine;
        c->prio = cnt & 3;
        c->chars = ((cnt >> 2) & 3) * 0x4000;
        c->screen = ((cnt >> 8) & 31) * 0x800;
        c->fx = F.effect >= 2 && (F.src & (1 << bg));
        if (!affine) {
            c->size = (cnt >> 14) & 3;
            c->c256 = (cnt & 0x80) != 0;
            c->cols = (c->size & 1) ? 64 : 32;
            c->rows = (c->size & 2) ? 64 : 32;
            c->hofs = reg16(0x10 + bg * 4) & 511;
            c->vofs = reg16(0x12 + bg * 4) & 511;
        } else {
            static const int sizes[4] = { 128, 256, 512, 1024 };
            uint32_t base = (bg == 2) ? 0x20 : 0x30;

            c->asize = sizes[(cnt >> 14) & 3];
            c->wrap = (cnt & 0x2000) != 0;
            c->pa = sext16(reg16(base));
            c->pb = sext16(reg16(base + 2));
            c->pc = sext16(reg16(base + 4));
            c->pd = sext16(reg16(base + 6));
            c->refX = sext28((uint32_t)reg16(base + 8) | ((uint32_t)reg16(base + 10) << 16));
            c->refY = sext28((uint32_t)reg16(base + 12) | ((uint32_t)reg16(base + 14) << 16));
        }
    }
}

// The map entry of cell (col, row) of a text BG, as ppu.c's textBgLine reads it.
static uint16_t text_entry(const Bg *c, int col, int row)
{
    unsigned block = 0, off;

    if (col >= 32)
        block += 1;
    if (row >= 32)
        block += (c->size == 3) ? 2 : 1;
    off = c->screen + block * 0x800 + (unsigned)(row & 31) * 64 + (unsigned)(col & 31) * 2;
    return off + 2 <= 0x18000 ? rd16(F.vram, off) : 0;
}

// Per-call memo: map entries repeat, and one compare is cheaper than a lookup.
static uint32_t sMemoStamp[1024], sStamp;
static uint16_t sMemoEntry[1024];
static int16_t  sMemoSlot[1024];

static int entry_slot(const Bg *c, uint16_t entry)
{
    unsigned t = entry & 1023;
    int slot;

    if (sMemoStamp[t] == sStamp && sMemoEntry[t] == entry)
        return sMemoSlot[t];
    if (c->c256)
        slot = get_slot(c->chars + t * 64, PAL_BG256 + (c->fx ? PAL_FX : 0), 1);
    else
        slot = get_slot(c->chars + t * 32, (entry >> 12) + (c->fx ? PAL_FX : 0), 0);
    sMemoStamp[t] = sStamp;
    sMemoEntry[t] = entry;
    sMemoSlot[t] = (int16_t)slot;
    return slot;
}

// The cells of one texture that the screen shows, with what they must hold.
typedef struct {
    uint16_t cell;
    int16_t  slot;
    uint16_t entry;
} Cell;

static Cell sCells[CELLS_MAX];

// Add the cells of rows [r0, r0 + nr) and columns [c0, c0 + nc), both wrapped.
static int visible_cells(const Bg *c, const uint16_t *side, int cols, int rows,
                         int c0, int nc, int r0, int nr, int n)
{
    for (int j = 0; j < nr; j++) {
        int row = (r0 + j) & (rows - 1);

        for (int i = 0; i < nc; i++) {
            int col = (c0 + i) & (cols - 1);
            uint16_t entry = side ? side[row * 64 + col] : text_entry(c, col, row);
            int slot = entry_slot(c, entry);

            if (slot == -2)
                return -1;
            sCells[n].cell = (uint16_t)(row * cols + col);
            sCells[n].slot = (int16_t)slot;
            sCells[n].entry = entry;
            n++;
        }
    }
    return n;
}

static inline uint32_t cell_sig(const Cell *k)
{
    if (k->slot < 0)
        return 0;
    return 0x80000000u | ((sSlot[k->slot].serial << 2) & 0x7ffffffcu)
         | ((k->entry >> 10) & 3);
}

// Bring the visible cells of a texture up to date. 0 on failure.
static int layer_update(Layer *L, int n, int cols)
{
    int dirty = 0, full;

    for (int i = 0; i < n; i++)
        if (!L->valid || cell_sig(&sCells[i]) != L->sig[sCells[i].cell])
            dirty++;
    if (dirty == 0)
        return 1;

    // Many changed cells: a GX fill is cheaper than a clear quad for each.
    full = !L->valid || dirty > 256;
    if (full) {
        // Colour only: the texture has no depth buffer to fill.
        C2D_Flush();
        C3D_FrameSplit(0);
        C3D_RenderTargetClear(L->target, C3D_CLEAR_COLOR, 0, 0);
        memset(L->sig, 0, sizeof(L->sig));
    }
    C2D_SceneBegin(L->target);
    C2D_ViewReset();

    if (!full) {
        // Clear the changed cells first: a transparent texel must not keep
        // the old tile. Alpha 0 fails the alpha test, so turn it off.
        C2D_Flush();
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        for (int i = 0; i < n; i++) {
            const Cell *k = &sCells[i];

            if (cell_sig(k) == L->sig[k->cell])
                continue;
            if (!C2D_DrawRectSolid((float)((k->cell % cols) * 8), (float)((k->cell / cols) * 8),
                                   0.0f, 8.0f, 8.0f, 0)) {
                sFail |= GPU_WHY_BUDGET;
                break;
            }
            sQuads++;
        }
        C2D_Flush();
        C3D_AlphaTest(true, GPU_GREATER, 0);
    }
    for (int i = 0; i < n && !sFail; i++) {
        const Cell *k = &sCells[i];
        uint32_t sig = cell_sig(k);

        if (sig == L->sig[k->cell])
            continue;
        if (k->slot >= 0
            && !tile_at(k->slot, (float)((k->cell % cols) * 8), (float)((k->cell / cols) * 8),
                        k->entry & 0x400, k->entry & 0x800))
            break;
        L->sig[k->cell] = sig;
    }
    if (sFail) {
        L->valid = 0;
        return 0;
    }
    L->valid = 1;
    return 1;
}

// Update the main and side textures of a text BG for this frame.
static int text_prepare(const Bg *c)
{
    int w = c->cols * 8, h = c->rows * 8, n;
    int r0 = c->vofs >> 3, nr = ((c->vofs + GBA_H - 1) >> 3) - r0 + 1;

    if (!layer_ensure(&sMain[c->bg], w, h)) {
        sFail |= GPU_WHY_VRAM;
        return 0;
    }
    ++sStamp;
    {
        int c0 = c->hofs >> 3, nc = ((c->hofs + GBA_W - 1) >> 3) - c0 + 1;

        n = visible_cells(c, NULL, c->cols, c->rows, c0, nc, r0, nr, 0);
        if (n < 0 || !layer_update(&sMain[c->bg], n, c->cols))
            return 0;
    }

    if (F.side[c->bg] && F.xbeg < 0) {
        const uint16_t *side = F.side[c->bg];
        int m = -F.xbeg;
        int sr0 = (c->vofs & 255) >> 3;
        unsigned base = (unsigned)c->hofs + F.sideDelta;
        int lc0 = (int)(((base + (unsigned)F.xbeg) & 511) >> 3);
        int lc1 = (int)(((base - 1) & 511) >> 3);
        int rc0 = (int)(((base + GBA_W) & 511) >> 3);
        int rc1 = (int)(((base + GBA_W + m - 1) & 511) >> 3);

        if (!layer_ensure(&sSide[c->bg], 512, 256)) {
            sFail |= GPU_WHY_VRAM;
            return 0;
        }
        ++sStamp;
        n = visible_cells(c, side, 64, 32, lc0, ((lc1 - lc0) & 63) + 1, sr0, nr, 0);
        if (n >= 0)
            n = visible_cells(c, side, 64, 32, rc0, ((rc1 - rc0) & 63) + 1, sr0, nr, n);
        if (n < 0 || !layer_update(&sSide[c->bg], n, 64))
            return 0;
    }
    return 1;
}

// ---- emitting layers ----------------------------------------------------------

typedef struct {
    int16_t ox, oy;
    uint8_t w, h, dw, dh;
    uint8_t affine, fh, fv, c256, pal;
    uint16_t tile;
    int16_t pa, pb, pc, pd;
} Spr;

static Spr     sSpr[128];
static uint8_t sByPrio[4][128];
static int     sNbyPrio[4];
static int     sSprFx;

static void build_sprites(void)
{
    static const uint8_t sizes[3][4][2] = {
        { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } },
        { { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } },
        { { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } },
    };
    int n = 0;

    sNbyPrio[0] = sNbyPrio[1] = sNbyPrio[2] = sNbyPrio[3] = 0;
    sSprFx = F.effect >= 2 && (F.src & 0x10);
    if (!(F.dispcnt & 0x1000))
        return;
    for (int i = 0; i < 128; i++) {
        uint16_t a0 = rd16(F.oam, i * 8), a1 = rd16(F.oam, i * 8 + 2), a2 = rd16(F.oam, i * 8 + 4);
        int am = (a0 >> 8) & 3, affine = am & 1, shape = (a0 >> 14) & 3, ox, oy, prio;
        Spr *s;

        if (!affine && (a0 & 0x200))
            continue;
        if (shape == 3)
            continue;
        // OBJ mode 2 marks the OBJ window and never draws. With the window
        // on, the check has already refused the frame.
        if (((a0 >> 10) & 3) == 2)
            continue;
        s = &sSpr[n];
        s->w = sizes[shape][(a1 >> 14) & 3][0];
        s->h = sizes[shape][(a1 >> 14) & 3][1];
        s->affine = (uint8_t)affine;
        s->dw = (am == 3) ? s->w * 2 : s->w;
        s->dh = (am == 3) ? s->h * 2 : s->h;
        s->c256 = (a0 & 0x2000) != 0;
        s->pal = (a2 >> 12) & 15;
        s->tile = a2 & 0x3ff;
        s->fh = !affine && (a1 & 0x1000);
        s->fv = !affine && (a1 & 0x2000);
        ox = a1 & 511;
        oy = a0 & 255;
        if (ox > F.xend)
            ox -= 512;
        if (oy > GBA_H)
            oy -= 256;
        s->ox = (int16_t)ox;
        s->oy = (int16_t)oy;
        if (affine) {
            uint32_t mb = ((a1 >> 9) & 31) * 32;

            s->pa = (int16_t)sext16(rd16(F.oam, mb + 6));
            s->pb = (int16_t)sext16(rd16(F.oam, mb + 14));
            s->pc = (int16_t)sext16(rd16(F.oam, mb + 22));
            s->pd = (int16_t)sext16(rd16(F.oam, mb + 30));
        }
        prio = (a2 >> 10) & 3;
        sByPrio[prio][sNbyPrio[prio]++] = (uint8_t)n;
        n++;
    }
}

// ppu.c's objTileOffset.
static unsigned obj_tile(const Spr *s, int tx, int ty)
{
    if (F.dispcnt & 0x40)
        return s->tile + ty * (s->c256 ? (s->w >> 2) : (s->w >> 3)) + tx * (s->c256 ? 2 : 1);
    return s->tile + ty * 32 + tx * (s->c256 ? 2 : 1);
}

static int spr_slot(const Spr *s, int tx, int ty)
{
    unsigned addr = 0x10000 + obj_tile(s, tx, ty) * 32;
    unsigned id = s->c256 ? PAL_OBJ256 : 16u + s->pal;

    return get_slot(addr, id + (sSprFx ? PAL_FX : 0), s->c256);
}

static int is_identity(int pa, int pb, int pc, int pd)
{
    return pa == 256 && pb == 0 && pc == 0 && pd == 256;
}

// One sprite, cut to the region.
static void emit_sprite(const Spr *s, const Rect *r)
{
    Rect box, clip;

    if (!clip_to(&box, r, F.xbeg, F.xend, 0, GBA_H))
        return;
    if (!clip_to(&clip, &box, s->ox, s->ox + s->dw, s->oy, s->oy + s->dh))
        return;

    if (!s->affine || is_identity(s->pa, s->pb, s->pc, s->pd)) {
        // The identity puts texel (x, y) at box offset (x, y) + (dw - w) / 2.
        int x0 = s->ox + (s->dw - s->w) / 2, y0 = s->oy + (s->dh - s->h) / 2;
        int tw = s->w / 8, th = s->h / 8;

        for (int j = 0; j < th; j++)
            for (int i = 0; i < tw; i++) {
                int slot = spr_slot(s, s->fh ? tw - 1 - i : i, s->fv ? th - 1 - j : j);

                if (slot == -2)
                    return;
                if (slot >= 0 && !tile_clipped(slot, x0 + i * 8, y0 + j * 8, s->fh, s->fv, &clip))
                    return;
            }
        return;
    }

    {
        float inv = 256.0f / (float)((int32_t)s->pa * s->pd - (int32_t)s->pb * s->pc);
        float m00 = s->pd * inv, m01 = -s->pb * inv, m10 = -s->pc * inv, m11 = s->pa * inv;
        float ctx = s->w / 2 + AFFINE_BIAS, cty = s->h / 2 + AFFINE_BIAS;
        float csx = s->ox + s->dw / 2 + 0.5f + ORIGIN_X, csy = s->oy + s->dh / 2 + 0.5f;

        scissor(&clip);
        view_affine(m00, m01, m10, m11, csx - (m00 * ctx + m01 * cty), csy - (m10 * ctx + m11 * cty));
        for (int ty = 0; ty < s->h / 8 && !sFail; ty++)
            for (int tx = 0; tx < s->w / 8; tx++) {
                int slot = spr_slot(s, tx, ty);

                if (slot == -2)
                    break;
                if (slot >= 0 && !tile_at(slot, (float)(tx * 8), (float)(ty * 8), 0, 0))
                    break;
            }
        C2D_ViewReset();
        scissor_off();
    }
}

static void emit_text(const Bg *c, const Rect *r)
{
    Rect k;

    if (clip_to(&k, r, 0, GBA_W, 0, GBA_H)) {
        Layer *L = &sMain[c->bg];

        quad(&L->tex, (float)((k.x0 + c->hofs) & (L->w - 1)), (float)((k.y0 + c->vofs) & (L->h - 1)),
             k.x1 - k.x0, k.y1 - k.y0, (float)(ORIGIN_X + k.x0), (float)k.y0, 1.0f, 1.0f);
    }
    if (F.side[c->bg] && F.xbeg < 0) {
        Layer *S = &sSide[c->bg];
        int part[2][2] = { { F.xbeg, 0 }, { GBA_W, F.xend } };

        for (int p = 0; p < 2; p++) {
            unsigned u;

            if (!clip_to(&k, r, part[p][0], part[p][1], 0, GBA_H))
                continue;
            u = ((unsigned)k.x0 + (unsigned)c->hofs + F.sideDelta) & 511;
            quad(&S->tex, (float)u, (float)((k.y0 + c->vofs) & 255), k.x1 - k.x0, k.y1 - k.y0,
                 (float)(ORIGIN_X + k.x0), (float)k.y0, 1.0f, 1.0f);
        }
    }
}

static int affine_slot(const Bg *c, int tx, int ty)
{
    int tiles = c->asize >> 3;
    unsigned at;

    if (c->wrap) {
        tx &= tiles - 1;
        ty &= tiles - 1;
    } else if (tx < 0 || ty < 0 || tx >= tiles || ty >= tiles) {
        return -1;
    }
    at = c->screen + (unsigned)ty * (unsigned)tiles + (unsigned)tx;
    if (at >= 0x18000)
        return -1;
    return get_slot(c->chars + F.vram[at] * 64u, PAL_BG256 + (c->fx ? PAL_FX : 0), 1);
}

static inline int floor_div8(int v)
{
    return v >> 3;   // arithmetic shift: floor for negative values too
}

static void emit_affine(const Bg *c, const Rect *r)
{
    Rect k;

    if (!clip_to(&k, r, 0, GBA_W, 0, GBA_H))
        return;

    if (is_identity(c->pa, c->pb, c->pc, c->pd)) {
        // Pixel (x, y) shows texel (x + ox, y + oy), as ppu.c's fast path.
        int ox = c->refX >> 8, oy = c->refY >> 8;
        int tx0 = floor_div8(k.x0 + ox), tx1 = floor_div8(k.x1 - 1 + ox);
        int ty0 = floor_div8(k.y0 + oy), ty1 = floor_div8(k.y1 - 1 + oy);

        for (int ty = ty0; ty <= ty1; ty++)
            for (int tx = tx0; tx <= tx1; tx++) {
                int slot = affine_slot(c, tx, ty);

                if (slot == -2)
                    return;
                if (slot >= 0 && !tile_clipped(slot, tx * 8 - ox, ty * 8 - oy, 0, 0, &k))
                    return;
            }
        return;
    }

    {
        float inv = 256.0f / (float)((int32_t)c->pa * c->pd - (int32_t)c->pb * c->pc);
        float m00 = c->pd * inv, m01 = -c->pb * inv, m10 = -c->pc * inv, m11 = c->pa * inv;
        float rx = c->refX / 256.0f + AFFINE_BIAS, ry = c->refY / 256.0f + AFFINE_BIAS;
        int32_t lo[2] = { INT32_MAX, INT32_MAX }, hi[2] = { INT32_MIN, INT32_MIN };
        int tx0, tx1, ty0, ty1, tiles = c->asize >> 3;

        // The texels at the four corner pixels bound the tiles to draw.
        for (int corner = 0; corner < 4; corner++) {
            int32_t x = (corner & 1) ? k.x1 - 1 : k.x0, y = (corner & 2) ? k.y1 - 1 : k.y0;
            int32_t tx = (c->refX + c->pa * x + c->pb * y) >> 8;
            int32_t ty = (c->refY + c->pc * x + c->pd * y) >> 8;

            if (tx < lo[0]) lo[0] = tx;
            if (tx > hi[0]) hi[0] = tx;
            if (ty < lo[1]) lo[1] = ty;
            if (ty > hi[1]) hi[1] = ty;
        }
        tx0 = floor_div8(lo[0]) - 1;
        tx1 = floor_div8(hi[0]) + 1;
        ty0 = floor_div8(lo[1]) - 1;
        ty1 = floor_div8(hi[1]) + 1;
        if (!c->wrap) {
            if (tx0 < 0) tx0 = 0;
            if (ty0 < 0) ty0 = 0;
            if (tx1 >= tiles) tx1 = tiles - 1;
            if (ty1 >= tiles) ty1 = tiles - 1;
        }
        if (tx1 < tx0 || ty1 < ty0)
            return;
        if ((tx1 - tx0 + 1) * (ty1 - ty0 + 1) > AFFINE_TILE_BUDGET) {
            sFail |= GPU_WHY_BUDGET;
            return;
        }

        scissor(&k);
        view_affine(m00, m01, m10, m11, 0.5f + ORIGIN_X - (m00 * rx + m01 * ry),
                    0.5f - (m10 * rx + m11 * ry));
        for (int ty = ty0; ty <= ty1 && !sFail; ty++)
            for (int tx = tx0; tx <= tx1; tx++) {
                int slot = affine_slot(c, tx, ty);

                if (slot == -2)
                    break;
                if (slot >= 0 && !tile_at(slot, (float)(tx * 8), (float)(ty * 8), 0, 0))
                    break;
            }
        C2D_ViewReset();
        scissor_off();
    }
}

// ---- stencil passes -------------------------------------------------------------

static int      sStencil;   // effect 1 with a first target: bit 0 is kept
static unsigned sMark;      // the next mark bit, 0x02 to 0x80

enum { E_TEXT, E_AFFINE, E_SPRITE, E_SPRITES };

typedef struct {
    int kind;
    const Bg *bg;
    const Spr *spr;
    int prio;
    const Rect *r;
} Emit;

static void emit(const Emit *e)
{
    switch (e->kind) {
    case E_TEXT:   emit_text(e->bg, e->r); break;
    case E_AFFINE: emit_affine(e->bg, e->r); break;
    case E_SPRITE: emit_sprite(e->spr, e->r); break;
    case E_SPRITES:
        for (int i = sNbyPrio[e->prio] - 1; i >= 0 && !sFail; i--)
            emit_sprite(&sSpr[sByPrio[e->prio][i]], e->r);
        break;
    }
}

// Clear the mark bits, keep bit 0.
static void clear_marks(void)
{
    C2D_Flush();
    C3D_DepthTest(true, GPU_ALWAYS, 0);
    C3D_StencilTest(true, GPU_ALWAYS, 0, 0xff, 0xfe);
    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);
    if (!C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, (float)SURF_W, (float)GBA_H, C2D_Color32(0, 0, 0, 255)))
        sFail |= GPU_WHY_BUDGET;
    C2D_Flush();
    C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_COLOR);
    sMark = 0x02;
}

// Draw one layer of one region, with the blend that BLDCNT gives it.
static void run(const Emit *e, int bit)
{
    int dst = (F.dst & bit) != 0;
    unsigned m;

    if (!sStencil) {
        emit(e);
        return;
    }
    if (!(F.effect == 1 && (F.src & bit))) {
        // Opaque, and bit 0 says whether this layer is a second target.
        C2D_Flush();
        C3D_StencilTest(true, GPU_ALWAYS, dst, 0xff, 0x01);
        C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);
        emit(e);
        return;
    }

    if (sMark > 0x80)
        clear_marks();
    m = sMark;
    sMark <<= 1;

    // Pass 1, no second target below: plain colour. Set the mark, and bit 0 if
    // this layer is a second target (both bits are 0 here, so INVERT sets them).
    C2D_Flush();
    blend_opaque();
    C3D_StencilTest(true, GPU_EQUAL, 0, 0x01, (int)(m | (unsigned)dst));
    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_INVERT);
    emit(e);

    // Pass 2, a second target below and no mark: blend. Clear bit 0 if this
    // layer is not a second target.
    C2D_Flush();
    blend_alpha();
    C3D_StencilTest(true, GPU_EQUAL, 1, (int)(0x01 | m), dst ? 0 : 0x01);
    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_INVERT);
    emit(e);

    C2D_Flush();
    blend_opaque();
}

// ---- the draw -------------------------------------------------------------------

static void compose_rect(const Rect *r)
{
    for (int prio = 3; prio >= 0 && !sFail; prio--) {
        for (int i = sNbg - 1; i >= 0 && !sFail; i--) {
            const Bg *c = &sBg[i];
            Emit e = { c->affine ? E_AFFINE : E_TEXT, c, NULL, prio, r };

            if (c->prio != prio || !(r->mask & (1 << c->bg)))
                continue;
            run(&e, 1 << c->bg);
        }
        if (!sNbyPrio[prio] || !(r->mask & 0x10) || sFail)
            continue;
        if (sStencil && F.effect == 1 && (F.src & 0x10)) {
            // One at a time: a sprite can land on a sprite of the same pass.
            for (int i = sNbyPrio[prio] - 1; i >= 0 && !sFail; i--) {
                Emit e = { E_SPRITE, NULL, &sSpr[sByPrio[prio][i]], prio, r };

                run(&e, 0x10);
            }
        } else {
            Emit e = { E_SPRITES, NULL, NULL, prio, r };

            run(&e, 0x10);
        }
    }
}

// The backdrop, as ppu.c's backdropLine: palette 0, with BLDY if it is a
// first target. As RGBA8 for the clear, in the byte order of the GPU.
static uint32_t backdrop_rgba(void)
{
    uint16_t c = rd16(F.pal, 0);
    int r = c8(c & 31), g = c8((c >> 5) & 31), b = c8((c >> 10) & 31);

    if (F.effect >= 2 && (F.src & 0x20)) {
        r = fx8(r);
        g = fx8(g);
        b = fx8(b);
    }
    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | 0xff;
}

unsigned GpuComposeDraw(const GpuComposeInput *in)
{
    Rect r[RECT_MAX];
    int nr;

    if (!sReady)
        return GPU_WHY_VRAM;

    load_state(in);
    sFail = 0;
    sQuads = 0;
    sDecodes = 0;
    sFrame++;
    if (sResetCache || sSlotsUsed > SLOT_RESET_AT) {
        cache_reset();
        sResetCache = 0;
    }
    update_palettes();
    build_bgs();
    build_sprites();
    nr = build_rects(r);

    state_begin();

    // 1. The text BG textures. Only BGs that some region shows.
    for (int i = 0; i < sNbg && !sFail; i++) {
        unsigned shown = 0;

        for (int j = 0; j < nr; j++)
            shown |= r[j].mask;
        if (!sBg[i].affine && (shown & (1u << sBg[i].bg)))
            text_prepare(&sBg[i]);
    }
    if (sFail) {
        state_end();
        return sFail;
    }
    // A texture that was just drawn must be flushed before a quad samples it.
    C2D_Flush();
    C3D_FrameSplit(0);

    // 2. The surface: backdrop and stencil, then the regions.
    sStencil = F.effect == 1 && (F.src & 0x1f);
    sMark = 0x02;
    C3D_RenderTargetClear(sSurfTarget, C3D_CLEAR_ALL, backdrop_rgba(),
                          (F.dst & 0x20) ? 0x01000000u : 0);
    C2D_SceneBegin(sSurfTarget);
    C2D_ViewReset();
    for (int i = 0; i < nr && !sFail; i++)
        compose_rect(&r[i]);

    state_end();
    C3D_FrameSplit(0);
    return sFail;
}

// ---- set-up ---------------------------------------------------------------------

C3D_Tex *GpuComposeTexture(void)
{
    return &sSurf;
}

void GpuComposeStats(unsigned *quads, unsigned *decodes)
{
    *quads = sQuads;
    *decodes = sDecodes;
}

int GpuComposeInit(void)
{
    // The pixel order inside a PICA 8x8 tile.
    for (int i = 0; i < 64; i++) {
        unsigned x = i & 7, y = i >> 3;

        sMorton[i] = (uint8_t)((x & 1) | ((y & 1) << 1) | ((x & 2) << 1)
                             | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
    }

    if (!C3D_TexInit(&sAtlas, ATLAS_SIZE, ATLAS_SIZE, GPU_RGBA5551))
        goto fail;
    C3D_TexSetFilter(&sAtlas, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sAtlas, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    memset(sAtlas.data, 0, (size_t)ATLAS_SIZE * ATLAS_SIZE * 2);

    // A render target must be in VRAM. D24S8 for the blend stencil.
    if (!C3D_TexInitVRAM(&sSurf, SURF_W, SURF_H, GPU_RGBA8))
        goto fail;
    C3D_TexSetFilter(&sSurf, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sSurf, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    sSurfTarget = C3D_RenderTargetCreateFromTex(&sSurf, GPU_TEXFACE_2D, 0,
                                                GPU_RB_DEPTH24_STENCIL8);
    if (!sSurfTarget)
        goto fail;

    cache_reset();
    sReady = 1;
    CtrLog("emerald3ds: GPU compositor ready (VRAM free %lu)\n",
           (unsigned long)vramSpaceFree());
    return 1;

fail:
    CtrLog("emerald3ds: GPU compositor off: no memory (linear free %lu, VRAM free %lu)\n",
           (unsigned long)linearSpaceFree(), (unsigned long)vramSpaceFree());
    GpuComposeExit();
    return 0;
}

void GpuComposeExit(void)
{
    for (int i = 0; i < 4; i++) {
        layer_free(&sMain[i]);
        layer_free(&sSide[i]);
    }
    if (sSurfTarget)
        C3D_RenderTargetDelete(sSurfTarget);
    sSurfTarget = NULL;
    if (sSurf.data)
        C3D_TexDelete(&sSurf);
    if (sAtlas.data)
        C3D_TexDelete(&sAtlas);
    memset(&sSurf, 0, sizeof(sSurf));
    memset(&sAtlas, 0, sizeof(sAtlas));
    sReady = 0;
}

void GpuComposeMaintain(void)
{
    for (int i = 0; i < 4; i++) {
        if (sMain[i].release)
            layer_free(&sMain[i]);
        if (sSide[i].release)
            layer_free(&sSide[i]);
    }
}

void GpuComposeReadback(void *dst, int stride, int rgba8, void *bounce)
{
    const int bpp = rgba8 ? 4 : 2;
    const size_t size = (size_t)SURF_W * SURF_H * bpp;
    const u32 fmt = rgba8 ? GX_TRANSFER_FMT_RGBA8 : GX_TRANSFER_FMT_RGB565;

    if (!sReady)
        return;
    // Write back anything the CPU holds for the buffer, so that it cannot
    // land on top of the transfer later.
    GSPGPU_FlushDataCache(bounce, size);
    C3D_SyncDisplayTransfer((u32 *)sSurf.data, GX_BUFFER_DIM(SURF_W, SURF_H),
                            (u32 *)bounce, GX_BUFFER_DIM(SURF_W, SURF_H),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
                            GX_TRANSFER_RAW_COPY(0) |
                            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                            GX_TRANSFER_OUT_FORMAT(fmt) |
                            GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    GSPGPU_InvalidateDataCache(bounce, size);
    for (int y = 0; y < GBA_H; y++)
        memcpy((uint8_t *)dst + (size_t)y * stride * bpp,
               (const uint8_t *)bounce + (size_t)y * SURF_W * bpp, (size_t)stride * bpp);
}
