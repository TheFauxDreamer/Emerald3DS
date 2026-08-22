// Software PPU - optimized C port of pokeemerald-wasm/web/app.js rasteriser.
//
// Address translation note: app.js indexes one big WASM heap with absolute GBA
// addresses (e.g. u16[(REG + 0x40) >> 1]). Here the four regions are separate
// base pointers and every access is (base, byte-offset).
//
// EXACTNESS CONTRACT: output is bit-identical to the straight per-pixel port of
// app.js (validated by rp2350/ppu_host_test.c against WASM reference dumps).
// The optimizations below only hoist work whose inputs cannot change during a
// render (registers/palette/VRAM/OAM are a static snapshot for the frame):
//   - palette -> RGB LUTs built once per frame (incl. a brighten/darken LUT,
//     since blend effects 2/3 depend only on the source colour)
//   - window x-masks built once per scanline instead of per pixel
//   - text BGs fetch each map entry / tile row once per 8-pixel span
//   - affine BG/OBJ coordinates advance incrementally (same int32 arithmetic)
//   - per-pass blend classification replaces per-pixel BLDCNT decoding
//   - BG configs and the OAM sprite list are decoded once per frame
//
// PRESENTATION ORDER: the reference renders layer-by-layer over the full frame;
// here each scanline is composed completely (backdrop -> BG passes -> sprites)
// in a private line buffer and only then written to the framebuffer. Per-pixel
// results are identical (pixels only ever interact with themselves: blending
// reads the same pixel's composited-so-far colour and layer), but the scanout
// never sees a half-composited frame -- on-device this kills the flashing that
// scanning out mid-render caused. The line buffer is seeded from the existing
// framebuffer row so window-masked holes keep their old contents, exactly like
// the reference leaving those framebuffer pixels untouched.
//
// Quirks of the reference that MUST be preserved: window bit 5 gates the
// backdrop fill (skipped pixels keep the previous frame's contents); alpha
// blending reads the "below" pixel and layer byte that may be stale from the
// previous frame during the backdrop pass; sprites can alpha-blend over
// sprites; OBJ-window (DISPCNT bit 15) enables windowing but has no region.

#include "ppu.h"

#include <stdbool.h>
#include <string.h>

#define WIDTH  PPU_WIDTH
#define HEIGHT PPU_HEIGHT

// Bulky per-frame state lives in the EWRAM region's slack on-device (the SDK
// RAM region is full); host builds and the standalone display test keep it in
// ordinary .bss.
#ifdef PPU_LUTS_IN_EWRAM
#define PPU_EWRAM __attribute__((section(".ewram_top")))
#else
#define PPU_EWRAM
#endif

// ---- region base pointers (set by ppu_set_memory) --------------------------
static const uint8_t *REGb;
static const uint8_t *PALb;
static const uint8_t *VRAMb;
static const uint8_t *OAMb;

static inline uint8_t  r8(const uint8_t *b, uint32_t off) { return b[off]; }
static inline uint16_t ld16(const uint8_t *b, uint32_t off) {
    uint16_t v;
    __builtin_memcpy(&v, b + off, 2);   // single ldrh; all callers pass even offsets
    return v;
}
static inline uint16_t reg16(uint32_t off) { return ld16(REGb, off); }

void ppu_set_memory(const void *reg, const void *pal, const void *vram, const void *oam) {
    REGb = (const uint8_t *)reg;
    PALb = (const uint8_t *)pal;
    VRAMb = (const uint8_t *)vram;
    OAMb = (const uint8_t *)oam;
}

// ---- render target (set for the duration of a render) ----------------------
static uint8_t  *g_fb888;   // RGB888 target (PPU_PIXELS*3) or NULL
static uint16_t *g_fb565;   // RGB565 target (PPU_PIXELS)   or NULL
static uint8_t  *g_layer;   // per-pixel layer byte, PPU_PIXELS

static inline int32_t signed16(uint32_t v) { return (int32_t)(v << 16) >> 16; }
static inline int32_t signed28(uint32_t v) { return (int32_t)(v << 4) >> 4; }
static inline uint32_t word(uint32_t off) {
    return (uint32_t)reg16(off) | ((uint32_t)reg16(off + 2) << 16);
}
static inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// ---- per-frame hoisted state ------------------------------------------------
static struct {
    uint16_t dispcnt;
    int effect;          // BLDCNT effect 0-3
    int srcTargets;      // BLDCNT bits 0-5
    int dstTargets;      // BLDCNT bits 8-13
    int eva, evb, evy;   // clamped to 16
    bool windowsOn;      // DISPCNT & 0xe000
    int mapping1d;       // DISPCNT & 0x40
    uint16_t win0h, win1h, win0v, win1v, winin, winout;
} F;

// Palette LUTs, rebuilt each frame. pal888 packs gbaColor() as r|g<<8|b<<16;
// pal565 is the same colour quantised exactly as the reference's 565 store;
// pal565fx has blend effect 2/3 (brighten/darken) pre-applied, which is exact
// because those effects depend only on the source colour (and stay in 0..255).
static uint32_t pal888[512]   PPU_EWRAM;
static uint16_t pal565[512]   PPU_EWRAM;
static uint16_t pal565fx[512] PPU_EWRAM;
// Below-pixel side of the 565 alpha blend (effect 1), premultiplied per frame:
// bevb5[f] = unpack5(f)*evb, bevb6[f] = unpack6(f)*evb, with exactly
// line_read's 565 unpack. Folds the *255/31|63 divisions and the evb multiply
// of emit()'s blend arm into one load per component (192 B; evb <= 16, so
// values fit uint16). Not in EWRAM: that region is packed to the byte.
static uint16_t bevb5[32], bevb6[64];

// ---- windows ----------------------------------------------------------------
static bool inWindowRange(int value, uint16_t range) {
    int start = range >> 8;
    int end = range & 0xff;
    return start <= end ? (value >= start && value < end)
                        : (value >= start || value < end);
}

// Window mask for one scanline (only valid when F.windowsOn). Window registers
// are frame-constant, so the row is cached across passes within the frame.
static uint8_t winrow[WIDTH] __attribute__((aligned(4)));
static int winrow_y;
static int winrow_u;   // the row's uniform mask value, or -1 if not uniform

static void winrowFill(uint16_t hrange, uint8_t val) {
    int start = hrange >> 8;
    int end = hrange & 0xff;
    if (start <= end) {
        if (start < WIDTH) memset(&winrow[start], val, (end < WIDTH ? end : WIDTH) - start);
    } else {
        if (end > 0) memset(&winrow[0], val, end < WIDTH ? end : WIDTH);
        if (start < WIDTH) memset(&winrow[start], val, WIDTH - start);
    }
}

// Returns the row's mask array, or NULL meaning "mask is 0x3f everywhere".
static const uint8_t *winRowFor(int y) {
    if (!F.windowsOn) return NULL;
    if (winrow_y != y) {
        winrow_y = y;
        memset(winrow, F.winout & 0x3f, WIDTH);
        // win1 first, then win0 overwrites: matches the reference's win0-first
        // priority check.
        if ((F.dispcnt & 0x4000) && inWindowRange(y, F.win1v))
            winrowFill(F.win1h, (F.winin >> 8) & 0x3f);
        if ((F.dispcnt & 0x2000) && inWindowRange(y, F.win0v))
            winrowFill(F.win0h, F.winin & 0x3f);
        // Uniformity sweep (60 word compares). The overworld keeps WIN0
        // covering the whole screen, so most gameplay lines are uniform and
        // every pass on them can drop per-pixel masking (see passWinRow).
        const uint32_t *p = (const uint32_t *)winrow;
        uint32_t w0 = p[0];
        winrow_u = (uint8_t)w0 * 0x01010101u == w0 ? (int)(w0 & 0xff) : -1;
        if (winrow_u >= 0)
            for (int i = 1; i < WIDTH / 4; i++)
                if (p[i] != w0) { winrow_u = -1; break; }
    }
    return winrow;
}

static int g_passLayer;   // tentative; defined with the emit machinery below
static int g_passMode;

// Resolve the window mask for the CURRENT pass (call after setPass) on line y.
// Sets *skip when the mask blocks the pass's layer across the whole line.
// Returns NULL when no per-pixel masking is needed: either windows are off, or
// the row is uniform, lets the layer through, and its effects bit cannot change
// the output (plain stores ignore it; otherwise it must read 0x20, the value
// emitMasked assumes for a NULL row). Exactness-neutral by construction.
static const uint8_t *passWinRow(int y, bool *skip) {
    *skip = false;
    const uint8_t *wr = winRowFor(y);
    if (!wr || winrow_u < 0) return wr;
    if (!(winrow_u & g_passLayer)) { *skip = true; return wr; }
    if (g_passMode == 0 || (winrow_u & 0x20)) return NULL;
    return wr;
}

// ---- scanline buffers --------------------------------------------------------
// The current scanline is composed here and flushed to the framebuffer whole.
static uint16_t line565[WIDTH]     PPU_EWRAM;
static uint8_t  line888[WIDTH * 3] PPU_EWRAM;
static int g_rowBase;   // y * WIDTH: g_layer index of the line's first pixel

static inline void lineInit(int y) {
    g_rowBase = y * WIDTH;
    // Seed from the framebuffer so pixels nothing draws to (window-masked
    // backdrop) keep their previous contents, as the reference leaves them.
    if (g_fb888) memcpy(line888, &g_fb888[g_rowBase * 3], WIDTH * 3);
    else memcpy(line565, &g_fb565[g_rowBase], WIDTH * 2);
}

static inline void lineFlush(int y) {
    if (g_fb888) memcpy(&g_fb888[y * WIDTH * 3], line888, WIDTH * 3);
    else memcpy(&g_fb565[y * WIDTH], line565, WIDTH * 2);
}

// ---- pixel emission ----------------------------------------------------------
// Per-pass state. passMode: 0 = plain store, 1 = alpha blend (effect 1),
// 2 = brighten/darken via pal565fx (effects 2/3).
static int g_passLayer;
static int g_passMode;

static void setPass(int layerBit) {
    g_passLayer = layerBit;
    if (F.effect == 0 || !(F.srcTargets & layerBit)) g_passMode = 0;
    else if (F.effect == 1) g_passMode = 1;
    else g_passMode = 2;
}

static inline void store888(int x, int r, int g, int b) {
    uint8_t *p = &line888[x * 3];
    p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b;
}

static inline void store_pal(int x, int palIdx) {
    if (g_fb888) {
        uint32_t c = pal888[palIdx];
        store888(x, c & 0xff, (c >> 8) & 0xff, c >> 16);
    } else {
        line565[x] = pal565[palIdx];
    }
}

// Read back the line's composited-so-far pixel as 0-255 components (blending).
typedef struct { int r, g, b; } rgb_t;
static inline rgb_t line_read(int x) {
    rgb_t c;
    if (g_fb888) {
        const uint8_t *p = &line888[x * 3];
        c.r = p[0]; c.g = p[1]; c.b = p[2];
    } else {
        uint16_t v = line565[x];
        c.r = ((v >> 11) & 0x1f) * 255 / 31;
        c.g = ((v >> 5) & 0x3f) * 255 / 63;
        c.b = (v & 0x1f) * 255 / 31;
    }
    return c;
}

// Apply blend effect 2/3 to a packed 888 colour (888 backend only; the 565
// backend uses pal565fx).
static inline void store888fx(int x, uint32_t c) {
    int r = c & 0xff, g = (c >> 8) & 0xff, b = c >> 16;
    if (F.effect == 2) {
        r += ((255 - r) * F.evy) >> 4;
        g += ((255 - g) * F.evy) >> 4;
        b += ((255 - b) * F.evy) >> 4;
    } else {
        r -= (r * F.evy) >> 4;
        g -= (g * F.evy) >> 4;
        b -= (b * F.evy) >> 4;
    }
    store888(x, r, g, b);
}

// Emit one non-transparent pixel that already passed the window layer test.
// 'effects' is the window's colour-effects gate (mask & 0x20; 0x20 if no window).
static inline void emit(int x, int palIdx, int effects) {
    if (g_passMode == 0 || !effects) {
        store_pal(x, palIdx);
    } else if (g_passMode == 2) {
        if (g_fb888) store888fx(x, pal888[palIdx]);
        else line565[x] = pal565fx[palIdx];
    } else {  // alpha blend
        if (F.dstTargets & g_layer[g_rowBase + x]) {
            uint32_t c = pal888[palIdx];
            rgb_t below = line_read(x);
            int r = clamp255((int)((c & 0xff) * F.eva + below.r * F.evb) >> 4);
            int g = clamp255((int)(((c >> 8) & 0xff) * F.eva + below.g * F.evb) >> 4);
            int b = clamp255((int)((c >> 16) * F.eva + below.b * F.evb) >> 4);
            if (g_fb888) store888(x, r, g, b);
            else line565[x] = (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
        } else {
            store_pal(x, palIdx);
        }
    }
    g_layer[g_rowBase + x] = (uint8_t)g_passLayer;
}

// Specialized alpha-blend emit for the no-window/565 hot path (passMode 1,
// wr == NULL, g_fb565): exactly emit()'s blend arm with the below pixel's
// unpack+evb multiply taken from bevb5/bevb6 and all frame state passed in
// registers (the char-typed layer stores otherwise force per-pixel reloads
// of F.* and g_rowBase). clamp255 reduces to min(255): no term is negative.
// noinline is load-bearing: inlining this body into the 7 call sites bloated
// textBgLine/spritesLine enough to wreck the NON-blend hot loops' register
// allocation (+27% on blend-free scenes); as a call, they are untouched.
static __attribute__((noinline)) void emitBlend565(int x, int palIdx, int eva, int dst,
                                                   uint8_t lb, uint8_t *ly) {
    if (dst & ly[x]) {
        uint32_t c = pal888[palIdx];
        uint16_t v = line565[x];
        int r = (int)((c & 0xff) * (uint32_t)eva + bevb5[(v >> 11) & 0x1f]) >> 4;
        int g = (int)(((c >> 8) & 0xff) * (uint32_t)eva + bevb6[(v >> 5) & 0x3f]) >> 4;
        int b = (int)((c >> 16) * (uint32_t)eva + bevb5[v & 0x1f]) >> 4;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        line565[x] = (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
    } else {
        line565[x] = pal565[palIdx];
    }
    ly[x] = lb;
}

// Window-checked emit. wr == NULL means no windows (mask 0x3f).
static inline void emitMasked(int x, int palIdx, const uint8_t *wr) {
    int effects = 0x20;
    if (wr) {
        uint8_t m = wr[x];
        if (!(m & g_passLayer)) return;
        effects = m & 0x20;
    }
    emit(x, palIdx, effects);
}

// ---- frame state -------------------------------------------------------------
static void buildFrameState(void) {
    F.dispcnt = reg16(0);
    uint16_t bldcnt = reg16(0x50);
    F.effect = (bldcnt >> 6) & 3;
    F.srcTargets = bldcnt & 0x3f;
    F.dstTargets = (bldcnt >> 8) & 0x3f;
    uint16_t alpha = reg16(0x52);
    F.eva = (alpha & 0x1f) < 16 ? (alpha & 0x1f) : 16;
    F.evb = ((alpha >> 8) & 0x1f) < 16 ? ((alpha >> 8) & 0x1f) : 16;
    F.evy = (reg16(0x54) & 0x1f) < 16 ? (reg16(0x54) & 0x1f) : 16;
    F.windowsOn = (F.dispcnt & 0xe000) != 0;
    F.mapping1d = F.dispcnt & 0x40;
    F.win0h = reg16(0x40); F.win1h = reg16(0x42);
    F.win0v = reg16(0x44); F.win1v = reg16(0x46);
    F.winin = reg16(0x48); F.winout = reg16(0x4a);
    winrow_y = -1;
    if (F.effect == 1) {
        for (int i = 0; i < 32; i++) bevb5[i] = (uint16_t)(i * 255 / 31 * F.evb);
        for (int i = 0; i < 64; i++) bevb6[i] = (uint16_t)(i * 255 / 63 * F.evb);
    }

    for (int i = 0; i < 512; i++) {
        uint16_t v = ld16(PALb, i * 2);
        int r = (v & 31) * 255 / 31;
        int g = ((v >> 5) & 31) * 255 / 31;
        int b = ((v >> 10) & 31) * 255 / 31;
        pal888[i] = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
        pal565[i] = (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
        if (F.effect == 2) {
            int fr = r + (((255 - r) * F.evy) >> 4);
            int fg = g + (((255 - g) * F.evy) >> 4);
            int fb = b + (((255 - b) * F.evy) >> 4);
            pal565fx[i] = (uint16_t)(((fr & 0xf8) << 8) | ((fg & 0xfc) << 3) | (fb >> 3));
        } else if (F.effect == 3) {
            int fr = r - ((r * F.evy) >> 4);
            int fg = g - ((g * F.evy) >> 4);
            int fb = b - ((b * F.evy) >> 4);
            pal565fx[i] = (uint16_t)(((fr & 0xf8) << 8) | ((fg & 0xfc) << 3) | (fb >> 3));
        }
    }
}

// ---- per-frame BG configs ----------------------------------------------------
typedef struct {
    uint8_t bg, affine, priority;
    uint32_t charBase, screenBase;
    // text
    int color256, wmask, hmask, hofs, vofs, sizebits;
    // affine
    int asize, wrap, tilesPerRow;
    int32_t pa, pb, pc, pd, refX, refY;
} bgcfg_t;

static bgcfg_t g_bg[4];
static int g_nbg;

static void buildBgConfigs(uint16_t dispcnt) {
    int mode = dispcnt & 7;
    g_nbg = 0;
    for (int bg = 0; bg < 4; bg++) {
        if (!(dispcnt & (0x100 << bg))) continue;
        int affine = -1;
        if (mode == 0) affine = 0;
        else if (mode == 1 && bg < 2) affine = 0;
        else if (mode == 1 && bg == 2) affine = 1;
        else if (mode == 2 && bg >= 2) affine = 1;
        if (affine < 0) continue;
        bgcfg_t *c = &g_bg[g_nbg++];
        uint16_t cnt = reg16(8 + bg * 2);
        c->bg = (uint8_t)bg;
        c->affine = (uint8_t)affine;
        c->priority = cnt & 3;
        c->charBase = ((cnt >> 2) & 3) * 0x4000;
        c->screenBase = ((cnt >> 8) & 31) * 0x800;
        if (!affine) {
            int size = (cnt >> 14) & 3;
            c->sizebits = size;
            c->color256 = cnt & 0x80;
            c->wmask = ((size & 1) ? 512 : 256) - 1;
            c->hmask = ((size & 2) ? 512 : 256) - 1;
            c->hofs = reg16(0x10 + bg * 4) & 511;
            c->vofs = reg16(0x12 + bg * 4) & 511;
        } else {
            static const int sizes[4] = {128, 256, 512, 1024};
            c->asize = sizes[(cnt >> 14) & 3];
            c->wrap = cnt & 0x2000;
            c->tilesPerRow = c->asize >> 3;
            uint32_t reg = (bg == 2) ? 0x20 : 0x30;
            c->pa = signed16(reg16(reg));
            c->pb = signed16(reg16(reg + 2));
            c->pc = signed16(reg16(reg + 4));
            c->pd = signed16(reg16(reg + 6));
            c->refX = signed28(word(reg + 8));
            c->refY = signed28(word(reg + 12));
        }
    }
}

// ---- per-frame sprite list -----------------------------------------------------
typedef struct {
    int16_t ox, oy;
    uint8_t w, h, drawW, drawH;
    uint8_t affine, flipH, flipV, color256, palette, priority;
    uint16_t tileBase;
    int16_t pa, pb, pc, pd;
} sprite_t;

// Sprite dimension table [shape][size] -> {w,h}.
static const uint8_t obj_sizes[3][4][2] = {
    {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
    {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
    {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
};

static sprite_t g_spr[128] PPU_EWRAM;
static int g_nspr;
// Per-priority sprite index lists (OAM order preserved within each), so the
// per-scanline walk only touches sprites of the pass's priority instead of
// skip-scanning the whole list 4 times per line.
static uint8_t g_sprByPrio[4][128] PPU_EWRAM;
static int g_nsprByPrio[4];

static void buildSpriteList(uint16_t dispcnt) {
    g_nspr = 0;
    g_nsprByPrio[0] = g_nsprByPrio[1] = g_nsprByPrio[2] = g_nsprByPrio[3] = 0;
    if (!(dispcnt & 0x1000)) return;
    // OAM order ascending; render walks the list backwards to keep the
    // reference's 127 -> 0 draw order.
    for (int i = 0; i < 128; i++) {
        uint32_t base = i * 8;
        uint16_t a0 = ld16(OAMb, base);
        uint16_t a1 = ld16(OAMb, base + 2);
        uint16_t a2 = ld16(OAMb, base + 4);
        int affineMode = (a0 >> 8) & 3;
        int affine = affineMode & 1;
        if (!affine && (a0 & 0x0200)) continue;  // disabled
        int shape = (a0 >> 14) & 3;
        if (shape == 3) continue;
        sprite_t *s = &g_spr[g_nspr++];
        s->w = obj_sizes[shape][(a1 >> 14) & 3][0];
        s->h = obj_sizes[shape][(a1 >> 14) & 3][1];
        s->affine = (uint8_t)affine;
        s->drawW = (affineMode == 3) ? s->w * 2 : s->w;
        s->drawH = (affineMode == 3) ? s->h * 2 : s->h;
        s->color256 = (a0 & 0x2000) ? 1 : 0;
        s->priority = (a2 >> 10) & 3;
        s->palette = (a2 >> 12) & 15;
        s->tileBase = a2 & 0x3ff;
        s->flipH = (!affine && (a1 & 0x1000)) ? 1 : 0;
        s->flipV = (!affine && (a1 & 0x2000)) ? 1 : 0;
        int ox = a1 & 511;
        int oy = a0 & 255;
        if (ox > 240) ox -= 512;
        if (oy > 160) oy -= 256;
        s->ox = (int16_t)ox;
        s->oy = (int16_t)oy;
        if (affine) {
            uint32_t mb = ((a1 >> 9) & 31) * 32;
            s->pa = (int16_t)signed16(ld16(OAMb, mb + 6));
            s->pb = (int16_t)signed16(ld16(OAMb, mb + 14));
            s->pc = (int16_t)signed16(ld16(OAMb, mb + 22));
            s->pd = (int16_t)signed16(ld16(OAMb, mb + 30));
        }
        int prio = s->priority;
        g_sprByPrio[prio][g_nsprByPrio[prio]++] = (uint8_t)(g_nspr - 1);
    }
}

// ---- per-scanline renderers ----------------------------------------------------
static void backdropLine(int y) {
    setPass(0x20);
    bool skip;
    const uint8_t *wr = passWinRow(y, &skip);
    if (skip) return;   // backdrop fill gated off: line keeps its old contents
    if (!wr && g_passMode != 1) {
        if (g_fb888) {
            uint32_t c = pal888[0];
            for (int x = 0; x < WIDTH; x++) {
                if (g_passMode == 2) store888fx(x, c);
                else store888(x, c & 0xff, (c >> 8) & 0xff, c >> 16);
            }
        } else {
            uint16_t c = (g_passMode == 2) ? pal565fx[0] : pal565[0];
            for (int x = 0; x < WIDTH; x++) line565[x] = c;
        }
        memset(&g_layer[g_rowBase], 0x20, WIDTH);
        return;
    }
    for (int x = 0; x < WIDTH; x++)
        emitMasked(x, 0, wr);
}

static void textBgLine(const bgcfg_t *c, int y) {
    setPass(1 << c->bg);
    bool skip;
    const uint8_t *wr = passWinRow(y, &skip);
    if (skip) return;
    // Hot path: no per-pixel mask, plain stores, 565 backend (the common
    // overworld config). Stores pal565 + the layer byte directly -- exactly
    // what emitMasked does for this case, minus its per-pixel dispatch.
    const bool fast = !wr && g_passMode == 0 && g_fb565;
    // Blend hot path (title screens, alpha-heavy scenes): same shape, emits
    // through emitBlend565. Frame-state locals are hoisted per SPAN inside the
    // branch (hoisting them per line kept them live across the mode-0 loops
    // and cost ~7% there from register pressure).
    const bool fastblend = !wr && g_passMode == 1 && g_fb565;
    int sy = (y + c->vofs) & c->hmask;
    uint32_t blockYOff = (sy >= 256) ? (uint32_t)(c->sizebits == 3 ? 2 : 1) * 0x800 : 0;
    uint32_t mapRow = c->screenBase + blockYOff + (uint32_t)((sy & 255) >> 3) * 64;
    int x = 0;
    while (x < WIDTH) {
        int sx = (x + c->hofs) & c->wmask;
        int span = 8 - (sx & 7);
        if (span > WIDTH - x) span = WIDTH - x;
        uint16_t entry = ld16(VRAMb, mapRow + (sx >= 256 ? 0x800 : 0)
                                          + (uint32_t)((sx & 255) >> 3) * 2);
        int tile = entry & 0x3ff;
        int py = (entry & 0x800) ? 7 - (sy & 7) : (sy & 7);
        int q0 = sx & 7;
        if (c->color256) {
            const uint8_t *trow = VRAMb + c->charBase + (uint32_t)tile * 64 + (uint32_t)py * 8;
            uint32_t lo, hi;   // fully transparent rows are common; skip whole
            memcpy(&lo, trow, 4); memcpy(&hi, trow + 4, 4);
            if (lo | hi) {
                if (fast) {
                    uint16_t *lp = &line565[x];
                    uint8_t *ly = &g_layer[g_rowBase + x];
                    uint8_t lb = (uint8_t)g_passLayer;
                    if (entry & 0x400) {
                        for (int k = 0; k < span; k++) {
                            int ci = trow[7 - (q0 + k)];
                            if (ci) { lp[k] = pal565[ci]; ly[k] = lb; }
                        }
                    } else {
                        for (int k = 0; k < span; k++) {
                            int ci = trow[q0 + k];
                            if (ci) { lp[k] = pal565[ci]; ly[k] = lb; }
                        }
                    }
                } else if (fastblend) {
                    const int eva = F.eva, dst = F.dstTargets;
                    const uint8_t blb = (uint8_t)g_passLayer;
                    uint8_t *const bly = &g_layer[g_rowBase];
                    if (entry & 0x400) {
                        for (int k = 0; k < span; k++) {
                            int ci = trow[7 - (q0 + k)];
                            if (ci) emitBlend565(x + k, ci, eva, dst, blb, bly);
                        }
                    } else {
                        for (int k = 0; k < span; k++) {
                            int ci = trow[q0 + k];
                            if (ci) emitBlend565(x + k, ci, eva, dst, blb, bly);
                        }
                    }
                } else if (entry & 0x400) {
                    for (int k = 0; k < span; k++) {
                        int ci = trow[7 - (q0 + k)];
                        if (ci) emitMasked(x + k, ci, wr);
                    }
                } else {
                    for (int k = 0; k < span; k++) {
                        int ci = trow[q0 + k];
                        if (ci) emitMasked(x + k, ci, wr);
                    }
                }
            }
        } else {
            int palBase = ((entry >> 12) & 15) * 16;
            // One aligned word holds the whole 8-px row (nibble k at bits 4k);
            // skip fully transparent rows, extract pixels without re-loading.
            uint32_t row32;
            memcpy(&row32, VRAMb + c->charBase + (uint32_t)tile * 32 + (uint32_t)py * 4, 4);
            if (row32) {
                if (fast) {
                    uint16_t *lp = &line565[x];
                    uint8_t *ly = &g_layer[g_rowBase + x];
                    uint8_t lb = (uint8_t)g_passLayer;
                    const uint16_t *pb = &pal565[palBase];
                    if (entry & 0x400) {
                        for (int k = 0; k < span; k++) {
                            uint32_t ci = (row32 >> ((7 - (q0 + k)) * 4)) & 15;
                            if (ci) { lp[k] = pb[ci]; ly[k] = lb; }
                        }
                    } else {
                        uint32_t r = row32 >> (q0 * 4);
                        for (int k = 0; k < span; k++, r >>= 4) {
                            uint32_t ci = r & 15;
                            if (ci) { lp[k] = pb[ci]; ly[k] = lb; }
                        }
                    }
                } else if (fastblend) {
                    const int eva = F.eva, dst = F.dstTargets;
                    const uint8_t blb = (uint8_t)g_passLayer;
                    uint8_t *const bly = &g_layer[g_rowBase];
                    for (int k = 0; k < span; k++) {
                        int px = (entry & 0x400) ? 7 - (q0 + k) : (q0 + k);
                        int ci = (row32 >> (px * 4)) & 15;
                        if (ci) emitBlend565(x + k, palBase + ci, eva, dst, blb, bly);
                    }
                } else {
                    for (int k = 0; k < span; k++) {
                        int px = (entry & 0x400) ? 7 - (q0 + k) : (q0 + k);
                        int ci = (row32 >> (px * 4)) & 15;
                        if (ci) emitMasked(x + k, palBase + ci, wr);
                    }
                }
            }
        }
        x += span;
    }
}

static void affineBgLine(const bgcfg_t *c, int y) {
    setPass(1 << c->bg);
    bool skip;
    const uint8_t *wr = passWinRow(y, &skip);
    if (skip) return;
    // Same int32 arithmetic as (refX + pa*x + pb*y), advanced incrementally.
    int32_t cx = c->refX + c->pb * y;
    int32_t cy = c->refY + c->pd * y;
    // Hot path (same condition as textBgLine): no per-pixel mask, plain
    // stores, 565 backend. Hoists the wrap-vs-bounds branch out of the loop
    // and copies the config into locals -- the layer-byte stores are char
    // stores, so without locals the compiler must re-load c->pa/pc/etc every
    // pixel. Same fetch arithmetic and stores as emitMasked's path.
    if (!wr && g_passMode == 0 && g_fb565) {
        const int32_t pa = c->pa, pc = c->pc;
        const uint32_t tpr = (uint32_t)c->tilesPerRow;
        const uint8_t *map = VRAMb + c->screenBase;
        const uint8_t *chr = VRAMb + c->charBase;
        uint8_t *ly = &g_layer[g_rowBase];
        const uint8_t lb = (uint8_t)g_passLayer;
        if (pa == 256 && pc == 0) {
            // Pure translation (identity matrix, just scrolled) -- how games
            // typically use affine BGs for plain images (title backdrop,
            // intro scenery). sx advances exactly 1/px ((cx+256)>>8 ==
            // (cx>>8)+1 for any cx) and sy is line-constant, so the line
            // renders in 8-px tile spans like a text BG: one map fetch and
            // one transparent-row check per span instead of two dependent
            // VRAM loads per pixel. Texels sampled are identical.
            int32_t syt = cy >> 8;
            int xa = 0, xb = WIDTH;
            uint32_t sx;
            if (c->wrap) {
                const uint32_t m = (uint32_t)c->asize - 1;
                syt &= (int32_t)m;
                sx = (uint32_t)(cx >> 8) & m;
                const uint8_t *mrow = map + ((uint32_t)syt >> 3) * tpr;
                const uint8_t *crow = chr + ((uint32_t)syt & 7) * 8;
                int x = 0;
                while (x < WIDTH) {
                    int span = 8 - (int)(sx & 7);
                    if (span > WIDTH - x) span = WIDTH - x;
                    const uint8_t *trow = crow + (uint32_t)mrow[sx >> 3] * 64 + (sx & 7);
                    for (int k = 0; k < span; k++) {
                        uint32_t ci = trow[k];
                        if (ci) { line565[x + k] = pal565[ci]; ly[x + k] = lb; }
                    }
                    x += span;
                    sx = (sx + (uint32_t)span) & m;   // spans end on tile
                }                                     // boundaries: safe wrap
                return;
            }
            const int32_t sz = c->asize;
            if ((uint32_t)syt >= (uint32_t)sz) return;   // fully off-map line
            int32_t sx0 = cx >> 8;                       // sx at x = 0
            if (sx0 < 0) xa = -sx0;
            if (sx0 + WIDTH > sz) xb = (int)(sz - sx0);
            const uint8_t *mrow = map + ((uint32_t)syt >> 3) * tpr;
            const uint8_t *crow = chr + ((uint32_t)syt & 7) * 8;
            int x = xa;
            while (x < xb) {
                sx = (uint32_t)(sx0 + x);
                int span = 8 - (int)(sx & 7);
                if (span > xb - x) span = xb - x;
                const uint8_t *trow = crow + (uint32_t)mrow[sx >> 3] * 64 + (sx & 7);
                for (int k = 0; k < span; k++) {
                    uint32_t ci = trow[k];
                    if (ci) { line565[x + k] = pal565[ci]; ly[x + k] = lb; }
                }
                x += span;
            }
            return;
        }
        if (c->wrap) {
            const uint32_t m = (uint32_t)c->asize - 1;
            for (int x = 0; x < WIDTH; x++, cx += pa, cy += pc) {
                uint32_t sx = (uint32_t)(cx >> 8) & m;
                uint32_t sy = (uint32_t)(cy >> 8) & m;
                uint32_t tile = map[(sy >> 3) * tpr + (sx >> 3)];
                uint32_t ci = chr[tile * 64 + (sy & 7) * 8 + (sx & 7)];
                if (ci) { line565[x] = pal565[ci]; ly[x] = lb; }
            }
        } else {
            const uint32_t sz = (uint32_t)c->asize;
            for (int x = 0; x < WIDTH; x++, cx += pa, cy += pc) {
                int32_t sx = cx >> 8;
                int32_t sy = cy >> 8;
                if ((uint32_t)sx >= sz || (uint32_t)sy >= sz) continue;
                uint32_t tile = map[((uint32_t)sy >> 3) * tpr + ((uint32_t)sx >> 3)];
                uint32_t ci = chr[tile * 64 + ((uint32_t)sy & 7) * 8 + ((uint32_t)sx & 7)];
                if (ci) { line565[x] = pal565[ci]; ly[x] = lb; }
            }
        }
        return;
    }
    for (int x = 0; x < WIDTH; x++, cx += c->pa, cy += c->pc) {
        int32_t sx = cx >> 8;
        int32_t sy = cy >> 8;
        if (c->wrap) {
            sx &= c->asize - 1;
            sy &= c->asize - 1;
        } else if (sx < 0 || sy < 0 || sx >= c->asize || sy >= c->asize) {
            continue;
        }
        int tile = r8(VRAMb, c->screenBase + (uint32_t)(sy >> 3) * c->tilesPerRow + (sx >> 3));
        int ci = r8(VRAMb, c->charBase + (uint32_t)tile * 64 + (uint32_t)(sy & 7) * 8 + (sx & 7));
        if (ci) emitMasked(x, ci, wr);
    }
}

// ---- sprites ---------------------------------------------------------------
static uint32_t objTileOffset(uint32_t tileBase, int tileX, int tileY,
                              int width, int color256) {
    return F.mapping1d
        ? tileBase + tileY * (color256 ? (width >> 2) : (width >> 3)) + tileX * (color256 ? 2 : 1)
        : tileBase + tileY * 32 + tileX * (color256 ? 2 : 1);
}

static bool objPixel(const sprite_t *s, int x, int y, int *palIdx) {
    uint32_t tileOffset = objTileOffset(s->tileBase, x >> 3, y >> 3, s->w, s->color256);
    int colorIndex;
    if (s->color256) {
        colorIndex = r8(VRAMb, 0x10000 + tileOffset * 32 + (y & 7) * 8 + (x & 7));
    } else {
        int packed = r8(VRAMb, 0x10000 + tileOffset * 32 + (y & 7) * 4 + ((x & 7) >> 1));
        colorIndex = (x & 1) ? (packed >> 4) : (packed & 15);
    }
    if (!colorIndex) return false;
    *palIdx = 0x100 + (s->color256 ? colorIndex : s->palette * 16 + colorIndex);
    return true;
}

// Render the sprites' slice of scanline y. priority < 0 means "all priorities"
// (bitmap modes). Lists are walked backwards = reference's 127 -> 0 OAM order
// (the per-priority buckets keep OAM order, so this is the same sequence).
static void spritesLine(int priority, int y) {
    const uint8_t *bucket = NULL;
    int n;
    if (priority >= 0) {
        n = g_nsprByPrio[priority];
        bucket = g_sprByPrio[priority];
    } else {
        n = g_nspr;
    }
    if (!n) return;
    setPass(0x10);
    bool skip;
    const uint8_t *wr = passWinRow(y, &skip);
    if (skip) return;
    // Same hot paths as textBgLine: plain stores / specialized alpha blend,
    // no mask, 565 backend. Sprite blending over sprites is the reference
    // quirk emitBlend565 preserves by reading the layer byte it just wrote.
    const bool fast = !wr && g_passMode == 0 && g_fb565;
    const bool fastblend = !wr && g_passMode == 1 && g_fb565;

    for (int bi = n - 1; bi >= 0; bi--) {
        const sprite_t *s = &g_spr[bucket ? bucket[bi] : bi];
        int row = y - s->oy;
        if (row < 0 || row >= s->drawH) continue;
        int x0 = (s->ox < 0) ? -s->ox : 0;
        int x1 = (s->ox + s->drawW > WIDTH) ? WIDTH - s->ox : s->drawW;
        if (x0 >= x1) continue;

        if (s->affine) {
            int w = s->w, h = s->h;
            int dy = row - s->drawH / 2;
            int texCx = w / 2, texCy = h / 2;
            // (pa*dx + pb*dy) advanced incrementally in x (same int math).
            int32_t fx = s->pa * (x0 - s->drawW / 2) + s->pb * dy;
            int32_t fy = s->pc * (x0 - s->drawW / 2) + s->pd * dy;
            const int32_t pa = s->pa, pc = s->pc;
            if (fast) {
                // Direct pal565+layer stores, bounds test folded to unsigned
                // compares; locals keep pa/pc in registers across the
                // char-typed layer stores.
                uint16_t *lp = &line565[s->ox];
                uint8_t *ly = &g_layer[g_rowBase + s->ox];
                for (int x = x0; x < x1; x++, fx += pa, fy += pc) {
                    int px = (fx >> 8) + texCx;
                    int py = (fy >> 8) + texCy;
                    if ((uint32_t)px >= (uint32_t)w || (uint32_t)py >= (uint32_t)h) continue;
                    int palIdx;
                    if (objPixel(s, px, py, &palIdx)) {
                        lp[x] = pal565[palIdx];
                        ly[x] = 0x10;
                    }
                }
                continue;
            }
            if (fastblend) {
                const int eva = F.eva, dst = F.dstTargets;
                uint8_t *const bly = &g_layer[g_rowBase];
                const int ox = s->ox;
                for (int x = x0; x < x1; x++, fx += pa, fy += pc) {
                    int px = (fx >> 8) + texCx;
                    int py = (fy >> 8) + texCy;
                    if ((uint32_t)px >= (uint32_t)w || (uint32_t)py >= (uint32_t)h) continue;
                    int palIdx;
                    if (objPixel(s, px, py, &palIdx))
                        emitBlend565(ox + x, palIdx, eva, dst, 0x10, bly);
                }
                continue;
            }
            for (int x = x0; x < x1; x++, fx += pa, fy += pc) {
                int px = (fx >> 8) + texCx;
                int py = (fy >> 8) + texCy;
                if (px < 0 || py < 0 || px >= w || py >= h) continue;
                int palIdx;
                if (objPixel(s, px, py, &palIdx))
                    emitMasked(s->ox + x, palIdx, wr);
            }
        } else {
            int py = s->flipV ? s->h - 1 - row : row;
            int tileY = py >> 3, pyIn = py & 7;
            int x = x0;
            while (x < x1) {
                int px = s->flipH ? s->w - 1 - x : x;
                // Pixels left in this texture tile (px runs toward 0 when
                // flipped, toward 7 otherwise).
                int span = s->flipH ? (px & 7) + 1 : 8 - (px & 7);
                if (span > x1 - x) span = x1 - x;
                uint32_t tOff = objTileOffset(s->tileBase, px >> 3, tileY, s->w, s->color256);
                if (s->color256) {
                    const uint8_t *trow = VRAMb + 0x10000 + tOff * 32 + (uint32_t)pyIn * 8;
                    uint32_t lo, hi;   // skip fully transparent rows
                    memcpy(&lo, trow, 4); memcpy(&hi, trow + 4, 4);
                    if (lo | hi) {
                        if (fast) {
                            uint16_t *lp = &line565[s->ox + x];
                            uint8_t *ly = &g_layer[g_rowBase + s->ox + x];
                            for (int k = 0; k < span; k++) {
                                int p = s->flipH ? (px & 7) - k : (px & 7) + k;
                                int ci = trow[p];
                                if (ci) { lp[k] = pal565[0x100 + ci]; ly[k] = 0x10; }
                            }
                        } else if (fastblend) {
                            const int eva = F.eva, dst = F.dstTargets;
                            uint8_t *const bly = &g_layer[g_rowBase];
                            for (int k = 0; k < span; k++) {
                                int p = s->flipH ? (px & 7) - k : (px & 7) + k;
                                int ci = trow[p];
                                if (ci) emitBlend565(s->ox + x + k, 0x100 + ci,
                                                     eva, dst, 0x10, bly);
                            }
                        } else {
                            for (int k = 0; k < span; k++) {
                                int p = s->flipH ? (px & 7) - k : (px & 7) + k;
                                int ci = trow[p];
                                if (ci) emitMasked(s->ox + x + k, 0x100 + ci, wr);
                            }
                        }
                    }
                } else {
                    uint32_t row32;   // 8-px row in one word, nibble k at bits 4k
                    memcpy(&row32, VRAMb + 0x10000 + tOff * 32 + (uint32_t)pyIn * 4, 4);
                    if (row32) {
                        int palBase = 0x100 + s->palette * 16;
                        if (fast) {
                            uint16_t *lp = &line565[s->ox + x];
                            uint8_t *ly = &g_layer[g_rowBase + s->ox + x];
                            const uint16_t *pb = &pal565[palBase];
                            for (int k = 0; k < span; k++) {
                                int p = s->flipH ? (px & 7) - k : (px & 7) + k;
                                int ci = (row32 >> (p * 4)) & 15;
                                if (ci) { lp[k] = pb[ci]; ly[k] = 0x10; }
                            }
                        } else if (fastblend) {
                            const int eva = F.eva, dst = F.dstTargets;
                            uint8_t *const bly = &g_layer[g_rowBase];
                            for (int k = 0; k < span; k++) {
                                int p = s->flipH ? (px & 7) - k : (px & 7) + k;
                                int ci = (row32 >> (p * 4)) & 15;
                                if (ci) emitBlend565(s->ox + x + k, palBase + ci,
                                                     eva, dst, 0x10, bly);
                            }
                        } else {
                            for (int k = 0; k < span; k++) {
                                int p = s->flipH ? (px & 7) - k : (px & 7) + k;
                                int ci = (row32 >> (p * 4)) & 15;
                                if (ci) emitMasked(s->ox + x + k, palBase + ci, wr);
                            }
                        }
                    }
                }
                x += span;
            }
        }
    }
}

// ---- bitmap-mode scanlines ---------------------------------------------------
// Like the reference, the bitmap itself ignores windows/blending and tags every
// pixel layer 0x04; sprites composite on top through the normal emit path.
static void bitmap3Line(void) {
    uint32_t off = (uint32_t)g_rowBase * 2;
    for (int x = 0; x < WIDTH; x++) {
        uint16_t v = ld16(VRAMb, off + (uint32_t)x * 2);
        int r = (v & 31) * 255 / 31;
        int g = ((v >> 5) & 31) * 255 / 31;
        int b = ((v >> 10) & 31) * 255 / 31;
        if (g_fb888) store888(x, r, g, b);
        else line565[x] = (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
    }
    memset(&g_layer[g_rowBase], 0x04, WIDTH);
}

static void bitmap4Line(uint16_t dispcnt) {
    uint32_t page = (dispcnt & 0x10) ? 0xA000 : 0;
    for (int x = 0; x < WIDTH; x++)
        store_pal(x, r8(VRAMb, page + g_rowBase + x));
    memset(&g_layer[g_rowBase], 0x04, WIDTH);
}

// ---- top level -------------------------------------------------------------
#ifdef PPU_PROFILE
// Per-pass time attribution, accumulated across renders until the consumer
// reads + clears them. Slots: 0 frame-state build, 1 line seed/flush memcpy,
// 2 backdrop, 3 text BGs, 4 affine BGs, 5 sprites. ppu_prof_now() is supplied
// by the host (time_us_64 on device).
extern uint64_t ppu_prof_now(void);
uint32_t ppu_prof_us[6];
uint32_t ppu_prof_frames;
#define PSLOT(s) do { uint64_t _t = ppu_prof_now(); \
                      ppu_prof_us[s] += (uint32_t)(_t - _pt); _pt = _t; } while (0)
#else
#define PSLOT(s) ((void)_pt)
#endif

static void renderFrame(void) {
#ifdef PPU_PROFILE
    uint64_t _pt = ppu_prof_now();
#else
    int _pt = 0;
#endif
    buildFrameState();
    uint16_t dispcnt = F.dispcnt;
    int mode = dispcnt & 7;
    buildBgConfigs(dispcnt);
    buildSpriteList(dispcnt);
    PSLOT(0);

    for (int y = 0; y < HEIGHT; y++) {
        lineInit(y);
        PSLOT(1);
        if (mode == 3) {
            bitmap3Line();
            PSLOT(3);
            spritesLine(-1, y);
            PSLOT(5);
        } else if (mode == 4) {
            bitmap4Line(dispcnt);
            PSLOT(3);
            spritesLine(-1, y);
            PSLOT(5);
        } else {
            backdropLine(y);
            PSLOT(2);
            for (int priority = 3; priority >= 0; priority--) {
                for (int i = 0; i < g_nbg; i++) {
                    if (g_bg[i].priority == priority) {
                        if (g_bg[i].affine) { affineBgLine(&g_bg[i], y); PSLOT(4); }
                        else { textBgLine(&g_bg[i], y); PSLOT(3); }
                    }
                }
                spritesLine(priority, y);
                PSLOT(5);
            }
        }
        lineFlush(y);
        PSLOT(1);
    }
#ifdef PPU_PROFILE
    ppu_prof_frames++;
#endif
}

void ppu_render_rgb888(uint8_t *img, uint8_t *layer) {
    g_fb888 = img;
    g_fb565 = NULL;
    g_layer = layer;
    renderFrame();
}

void ppu_render_rgb565(uint16_t *out, uint8_t *layer) {
    g_fb888 = NULL;
    g_fb565 = out;
    g_layer = layer;
    renderFrame();
}
