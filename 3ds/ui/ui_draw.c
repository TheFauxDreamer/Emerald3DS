// Bottom-screen drawing primitives. See ui_draw.h.

#include "global.h"
#include "text_window.h"
#include "pokemon_icon.h"
#include "item_icon.h"
#include "graphics.h"                 // gStatusGfx_Icons, gStatusPal_Icons
#include "data.h"                     // gMonFrontPicTable, gMonPaletteTable
#include "battle.h"                   // struct DisableStruct
#include "battle_interface.h"         // GetHPBarLevel
#include "battle_anim.h"              // ItemIdToBallId
#include "decompress.h"
#include "menu.h"                     // gStandardMenuPalette
#include "option_menu.h"              // Ctr3dsLiveWindowFrameType
#include "constants/characters.h"     // TEXT_COLOR_*
#include "pokemon_summary_screen.h"   // Ctr3dsGetTypeIconGfx, type icon sheet
#include "constants/party_menu.h"     // AILMENT_*
#include "constants/species.h"        // SPECIES_UNOWN, SPECIES_SPINDA

#include "ui_draw.h"
#include "ui_shell.h"                 // UI_COL_SHADOW

static u16 sFb[UI_W * UI_H];

u16 *UiFb(void) { return sFb; }

// GBA palettes are BGR555 and the high bit is not used. The 3DS texture is
// RGB565. Green gets one more bit, so copy the top bit into it.
u16 UiBgr555ToRgb565(u16 c)
{
    u32 r = (c      ) & 0x1F;
    u32 g = (c >>  5) & 0x1F;
    u32 b = (c >> 10) & 0x1F;
    return (u16)((r << 11) | ((g << 1) | (g >> 4)) << 5 | b);
}

void UiLoadPal(u16 *dst, const u16 *src, int count)
{
    for (int i = 0; i < count; i++)
        dst[i] = UiBgr555ToRgb565(src[i]);
}

// Two pixels for each store, for UiFillRect below. UI_W is 320 and sFb is a u16
// array at file scope, so y * UI_W is always even. The alignment of a row
// depends only on x. Only an odd x or an odd width needs a single-pixel edge.
#define UI_PIX2(c) (((u32)(c) << 16) | (u32)(c))

// Keep this as a simple loop. The paired version below is slower here
// (measured). The compiler already makes a good loop over one long array.
// UiFillRect is different, because its rows are short. Measure again before you
// change this.
void UiClear(u16 color)
{
    for (int i = 0; i < UI_W * UI_H; i++)
        sFb[i] = color;
}

void UiFillRect(int x, int y, int w, int h, u16 color)
{
    u32 pair = UI_PIX2(color);

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > UI_W) w = UI_W - x;
    if (y + h > UI_H) h = UI_H - y;
    if (w <= 0 || h <= 0)
        return;

    for (int row = 0; row < h; row++)
    {
        u16 *dst = &sFb[(y + row) * UI_W + x];
        int col = 0;

        // An odd start: write one pixel to reach an aligned pair. UI_W is 320,
        // so the alignment depends only on x. This side of the seam has no
        // stdint, so there is no pointer arithmetic here.
        if ((x & 1) != 0 && col < w)
            dst[col++] = color;

        for (; col + 1 < w; col += 2)
            *(u32 *)&dst[col] = pair;

        if (col < w)
            dst[col] = color;
    }
}

// One pixel, clipped. The small glyphs below (the Poke Ball, the chevron, the
// sparkle, the footprint) draw one pixel at a time. This clip needs only one
// pair of branches, which is much cheaper than a call to UiFillRect.
static inline void UiPixel(int x, int y, u16 color)
{
    if ((unsigned)x < (unsigned)UI_W && (unsigned)y < (unsigned)UI_H)
        sFb[y * UI_W + x] = color;
}

void UiRect(int x, int y, int w, int h, u16 color)
{
    if (w <= 0 || h <= 0)
        return;
    UiFillRect(x, y, w, 1, color);
    UiFillRect(x, y + h - 1, w, 1, color);
    UiFillRect(x, y, 1, h, color);
    UiFillRect(x + w - 1, y, 1, h, color);
}

// This is the most frequent function on the screen. Every window frame, icon
// and type badge uses it, and a party grid repaint blits about a thousand
// tiles.
//
// Thus the clip is outside the pixel loop. A tile that is fully on the screen
// (nearly all tiles) takes the fast path, with only the transparency test per
// pixel. Only a tile that crosses an edge takes the slow path.
void UiBlit4bppTile(int x, int y, const u8 *tile, const u16 *pal, int transparent0)
{
    if (x >= 0 && y >= 0 && x + 8 <= UI_W && y + 8 <= UI_H)
    {
        for (int row = 0; row < 8; row++)
        {
            const u8 *src = tile + row * 4;   // 8 pixels, 2 in each byte
            u16 *dst = &sFb[(y + row) * UI_W + x];

            for (int col = 0; col < 8; col += 2)
            {
                // The low nibble is the left pixel of each byte.
                u8 b = src[col >> 1];
                u32 lo = b & 0xF;
                u32 hi = b >> 4;

                if (lo != 0 || !transparent0)
                    dst[col] = pal[lo];
                if (hi != 0 || !transparent0)
                    dst[col + 1] = pal[hi];
            }
        }
        return;
    }

    for (int row = 0; row < 8; row++)
    {
        int py = y + row;
        if (py < 0 || py >= UI_H)
            continue;

        const u8 *src = tile + row * 4;
        u16 *dst = &sFb[py * UI_W];

        for (int col = 0; col < 8; col++)
        {
            int px = x + col;
            if (px < 0 || px >= UI_W)
                continue;

            u32 idx = (col & 1) ? (src[col >> 1] >> 4) : (src[col >> 1] & 0xF);
            if (idx == 0 && transparent0)
                continue;

            dst[px] = pal[idx];
        }
    }
}

// One 8x8 8bpp tile: 64 bytes, one byte for each pixel, in row order. There is
// no nibble to unpack, but the palette does not have 16 entries.
//
// An 8bpp GBA background has no palette bank, so the byte is an absolute index
// into the 256-entry BG palette. The region map's tiles use values near 112,
// because the game loads its 32 colors at BG_PLTT_ID(7). `pal` must have 256
// entries, with the part that the art uses filled.
void UiBlit8bppTile(int x, int y, const u8 *tile, const u16 *pal, int transparent0)
{
    // The same clip outside the loop as the 4bpp path. The region map is 8bpp
    // and covers most of the MAP tab.
    if (x >= 0 && y >= 0 && x + 8 <= UI_W && y + 8 <= UI_H)
    {
        for (int row = 0; row < 8; row++)
        {
            const u8 *src = tile + row * 8;
            u16 *dst = &sFb[(y + row) * UI_W + x];

            for (int col = 0; col < 8; col++)
                if (src[col] != 0 || !transparent0)
                    dst[col] = pal[src[col]];
        }
        return;
    }

    for (int row = 0; row < 8; row++)
    {
        int py = y + row;
        if (py < 0 || py >= UI_H)
            continue;

        const u8 *src = tile + row * 8;
        u16 *dst = &sFb[py * UI_W];

        for (int col = 0; col < 8; col++)
        {
            int px = x + col;
            if (px < 0 || px >= UI_W)
                continue;

            if (src[col] == 0 && transparent0)
                continue;

            dst[px] = pal[src[col]];
        }
    }
}

// The player selects one of 20 borders in Options -> Frame. The second screen
// uses it, so it looks like part of the game. GetWindowFrameTilesPal() is the
// game's own accessor and checks its bounds, so a bad setting gives frame 0.
//
// Every frame is a 3x3 nine-slice: 9 tiles (0x120 bytes) in row order, with a
// 16-color palette. The corners draw once. The edges and the center repeat.
u8 UiFrameId(void)
{
    // While the options menu is open, the player's choice is in that menu's
    // task. The save block gets it only when the menu closes. Use the live
    // value, so the border here changes at the same time as on the top screen.
    s16 live = Ctr3dsLiveWindowFrameType();

    if (live >= 0)
        return (u8)live;

    // NULL until a file loads (src/load_save.c). The shell's repaint hash reads
    // this on the first frame. Without the guard, it reads address 0x14, which
    // faults on hardware. Frame 0 is the game's default.
    if (gSaveBlock2Ptr == NULL)
        return 0;

    return gSaveBlock2Ptr->optionsWindowFrameType;
}

void UiWindowFrame(int tx, int ty, int wTiles, int hTiles)
{
    // The key is the frame id, not a one-time flag. The player can change the
    // setting at any time, and a stale palette does not match the tiles.
    static u16 pal[16];
    static int cachedId = -1;

    u8 frameId = UiFrameId();
    const struct TilesPal *frame = GetWindowFrameTilesPal(frameId);

    if (cachedId != (int)frameId)
    {
        UiLoadPal(pal, frame->pal, 16);
        cachedId = (int)frameId;
    }

    if (wTiles < 2 || hTiles < 2)
        return;

    for (int row = 0; row < hTiles; row++)
    {
        int sy = (row == 0) ? 0 : (row == hTiles - 1 ? 2 : 1);

        for (int col = 0; col < wTiles; col++)
        {
            int sx = (col == 0) ? 0 : (col == wTiles - 1 ? 2 : 1);
            const u8 *tile = frame->tiles + (sy * 3 + sx) * 32;

            // Opaque: the frame is the background, and nothing shows through
            // it.
            UiBlit4bppTile((tx + col) * 8, (ty + row) * 8, tile, pal, FALSE);
        }
    }
}

// Text on a frame must use the game's own menu colors, not a fixed white. The
// 20 frames go from light to dark, and the game prints dark text on all of
// them. These are the indices that its menus use from gStandardMenuPalette.
u16 UiThemeText(void)
{
    return UiBgr555ToRgb565(gStandardMenuPalette[TEXT_COLOR_DARK_GRAY]);
}

u16 UiThemeShadow(void)
{
    return UiBgr555ToRgb565(gStandardMenuPalette[TEXT_COLOR_LIGHT_GRAY]);
}

void UiMonIconFrame(int x, int y, u16 species, u32 personality, u8 frame)
{
    const u8 *gfx = GetMonIconPtr(species, personality, FALSE);
    const u16 *gbaPal = GetValidMonIconPalettePtr(species);
    u16 pal[16];

    if (gfx == NULL || gbaPal == NULL)
        return;

    // Each gMonIconTable entry points to the full 32x64 sheet, so the second
    // frame starts one frame of tiles later: 16 tiles of 32 bytes. The game
    // gets the same frame with ANIMCMD_FRAME(1, ...) on a 32x32 sprite in 1D
    // mapping.
    gfx += (frame & 1) * (16 * 32);

    UiLoadPal(pal, gbaPal, 16);

    // A 32x32 sprite in 1D mapping: 16 tiles in sequence, four in each row.
    for (int t = 0; t < 16; t++)
        UiBlit4bppTile(x + (t % 4) * 8, y + (t / 4) * 8, gfx + t * 32, pal, TRUE);
}

void UiMonIcon(int x, int y, u16 species, u32 personality)
{
    UiMonIconFrame(x, y, species, personality, 0);
}

// The same art with each ink pixel in one color.
//
// A 4bpp blit is a palette lookup, so a palette with 15 equal ink entries gives
// a silhouette with no new blitter. Index 0 stays transparent, so the outline
// has the icon's real shape.
//
// This does not read the palette, so it draws even when the icon palette is
// missing. Only the gfx pointer can stop it.
void UiMonIconSilhouette(int x, int y, u16 species, u32 personality, u16 color)
{
    const u8 *gfx = GetMonIconPtr(species, personality, FALSE);
    u16 pal[16];

    if (gfx == NULL)
        return;

    for (int i = 0; i < 16; i++)
        pal[i] = color;

    for (int t = 0; t < 16; t++)
        UiBlit4bppTile(x + (t % 4) * 8, y + (t / 4) * 8, gfx + t * 32, pal, TRUE);
}

// Item icons are LZ-compressed as 3x3 tiles and expand into a 4x4 sprite. This
// follows the game's steps: decompress, then CopyItemIconPicTo4x4Buffer
// (src/item_icon.c).
//
// The buffers are static, not Alloc'd. This runs every frame, and must not use
// the game's heap.
void UiItemIcon(int x, int y, u16 itemId)
{
    static u8  raw[0x120];     // 3x3 tiles, as AllocItemIconTemporaryBuffers
    static u8  tiles[0x200];   // 4x4 tiles
    static u16 gbaPal[16];
    u16 pal[16];

    const void *pic    = GetItemIconPicOrPalette(itemId, 0);
    const void *palSrc = GetItemIconPicOrPalette(itemId, 1);

    if (pic == NULL || palSrc == NULL)
        return;

    // CopyItemIconPicTo4x4Buffer writes only three rows of three tiles, so the
    // fourth column and row keep old data. The game uses AllocZeroed. Clear
    // this static buffer, or the previous item shows through.
    for (u32 i = 0; i < sizeof(tiles); i++)
        tiles[i] = 0;

    LZDecompressWram((const u32 *)pic, raw);
    CopyItemIconPicTo4x4Buffer(raw, tiles);
    LZDecompressWram((const u32 *)palSrc, gbaPal);

    UiLoadPal(pal, gbaPal, 16);

    for (int t = 0; t < 16; t++)
        UiBlit4bppTile(x + (t % 4) * 8, y + (t / 4) * 8, tiles + t * 32, pal, TRUE);
}

// A Pokedex front sprite. Every mon picture is 64x64 4bpp, as 64 tiles in 1D
// sprite order, like UiMonIcon at 4x4.
//
// LoadSpecialPokePic_DontHandleDeoxys is the game's own loader, and it is safe
// here: it decompresses and draws the Spinda spots, with no allocation and no
// OAM. It also selects the Unown letter, which is why the personality matters.
//
// The buffer holds MAX_MON_PIC_FRAMES frames, not one. Six species have a front
// sheet of four frames that decompresses to 8192 bytes. The size in
// gMonFrontPicTable is the size of one frame, so it does not limit the write.
// The game sizes its own buffers the same way (src/battle_gfx_sfx_util.c). A
// smaller buffer overruns the statics of this file, and the window-frame
// palette changes color.
//
// Cached on species. The dex cursor repaints the full screen at each step, so
// do not expand the sheet each time.
void UiMonPic(int x, int y, u16 species)
{
    static u8  pic[MON_PIC_SIZE * MAX_MON_PIC_FRAMES];
    static u16 pal[16];
    static u16 cachedSpecies = SPECIES_NONE;

    if (species == SPECIES_NONE || species >= NUM_SPECIES)
        return;

    if (cachedSpecies != species)
    {
        u16 gbaPal[16];

        // Check both destinations against the size in the data. Do not assume
        // that the tables agree with the buffers.
        if (GetDecompressedDataSize(gMonFrontPicTable[species].data) > sizeof(pic)
         || GetDecompressedDataSize(gMonPaletteTable[species].data) > sizeof(gbaPal))
            return;

        // Only Unown and Spinda have art that depends on the personality.
        // GetPokedexMonPersonality (static, src/pokedex.c) reads only these two
        // fields, so this is a copy of it.
        u32 personality = (species == SPECIES_UNOWN)  ? gSaveBlock2Ptr->pokedex.unownPersonality
                        : (species == SPECIES_SPINDA) ? gSaveBlock2Ptr->pokedex.spindaPersonality
                        : 0;

        LoadSpecialPokePic_DontHandleDeoxys(&gMonFrontPicTable[species], pic,
                                            species, personality, TRUE);
        LZDecompressWram(gMonPaletteTable[species].data, gbaPal);
        UiLoadPal(pal, gbaPal, 16);
        cachedSpecies = species;
    }

    // Frame 0 only. The tiles are in row order, so the first 64 are the top
    // 64x64 of the sheet. That is the still pose for the animated species too.
    for (int t = 0; t < 64; t++)
        UiBlit4bppTile(x + (t % 8) * 8, y + (t / 8) * 8, pic + t * 32, pal, TRUE);
}

// The Pokedex "caught" marker, copied from graphics/pokedex/caught_ball.png.
// The game keeps it in sCaughtBall_Gfx, which is static to src/pokedex.c. It
// has the same 7x7 shape as in the dex.
//
// Fixed red and white, not theme colors. A Poke Ball is known by its colors,
// and its dark outline makes it clear on all 20 frames.
void UiPokeball(int x, int y)
{
    // Color 0 is transparent, 1 the outline, 2 the top half and 3 the bottom
    // half.
    static const u8 kBall[UI_BALL_H][UI_BALL_W] =
    {
        { 0,0,1,1,1,0,0 },
        { 0,1,2,2,2,1,0 },
        { 1,2,1,1,2,2,1 },
        { 1,1,1,3,1,1,1 },
        { 1,3,3,1,1,3,1 },
        { 0,1,3,3,3,1,0 },
        { 0,0,1,1,1,0,0 },
    };
    static const u16 kColors[4] = { 0, UI_COL_SHADOW, UI_COL_BALL_TOP, UI_COL_BALL_BOTTOM };

    for (int row = 0; row < UI_BALL_H; row++)
        for (int col = 0; col < UI_BALL_W; col++)
            if (kBall[row][col])
                UiPixel(x + col, y + row, kColors[kBall[row][col]]);
}

// A specific kind of ball, from the game's throw sprites (graphics/balls). They
// are the only per-ball art that fits a text row. The bag icons are 24x24,
// which is too large.
//
// Each sheet is 16x48: three 16x16 frames of 384 bytes. Frame 0 is the closed
// ball, which sBallAnimSeq0 selects for a ball at rest (src/pokeball.c). At
// 16px wide, the tiles are two per row, so frame 0 is the first four tiles as
// 2x2.
//
// Cached on the ball kind, like UiTypeIcon. The quick-throw strip repaints on
// each hash change while it is up. Check the size of both destinations before
// each decompress (see the note above UiMonPic).
void UiBallIcon(int x, int y, u16 itemId)
{
    static u8  tiles[384];
    static u16 pal[16];
    static u8  cachedBall = POKEBALL_COUNT;   // not a ball, so it loads

    // Anything that is not a ball gives BALL_POKE, so this cannot index outside
    // the tables.
    u8 ballId = ItemIdToBallId(itemId);

    if (cachedBall != ballId)
    {
        const u32 *gfxLZ = gBallSpriteSheets[ballId].data;
        const u32 *palLZ = gBallSpritePalettes[ballId].data;
        u16 gbaPal[16];

        if (GetDecompressedDataSize(gfxLZ) > sizeof(tiles)
         || GetDecompressedDataSize(palLZ) > sizeof(gbaPal))
            return;

        LZDecompressWram(gfxLZ, tiles);
        LZDecompressWram(palLZ, gbaPal);
        UiLoadPal(pal, gbaPal, 16);
        cachedBall = ballId;
    }

    for (int t = 0; t < 4; t++)
        UiBlit4bppTile(x + (t % 2) * 8, y + (t / 2) * 8, tiles + t * 32, pal, TRUE);
}

// A species footprint: 4 tiles of 1bpp in a 2x2 layout, as in DrawFootprint
// (src/pokedex.c). The table is a global with no public header, so it is
// declared extern here, like gPokedexEntries in tab_dex.c.
extern const u8 *const gMonFootprintTable[];

void UiFootprint(int x, int y, u16 species, u16 color)
{
    const u8 *gfx;

    if (species == SPECIES_NONE || species >= NUM_SPECIES)
        return;

    gfx = gMonFootprintTable[species];
    if (gfx == NULL)
        return;

    // Four tiles of 8 bytes, one byte for each 8-pixel row, the low bit on the
    // left.
    for (int t = 0; t < 4; t++)
    {
        int tx = x + (t % 2) * 8;
        int ty = y + (t / 2) * 8;

        for (int row = 0; row < 8; row++)
        {
            u8 bits = gfx[t * 8 + row];

            for (int col = 0; col < 8; col++)
                if (bits & (1 << col))
                    UiPixel(tx + col, ty + row, color);
        }
    }
}

// The party menu's own status art, so PSN here is the same badge as there. One
// 32x64 sheet holds eight 32x8 badges of four tiles each. The order is that of
// the anim table: PSN, PRZ, SLP, FRZ, BRN, PKRS, FNT, blank.
// UpdatePartyMonAilmentGfx() uses `status - 1`, and so does the index below.
//
// Index 0 of the palette is transparent, and each badge has its own colors in
// the other slots. Thus one palette and a transparent blit are enough.
//
// CNF (UI_STATUS_CNF) is not in the sheet. Confusion is in
// gBattleMons[].status2, and the game never draws it. Thus it is drawn here
// with the sheet's geometry, so it looks like the other badges:
// - The pill is 20px wide at x 6..25 of the 32, with a lighter pixel on each
//   round corner.
// - The letters are 4x6, at the same x as on the other badges. N comes from BRN
//   and F from FRZ. C follows the style of SLP's S.
// - This table is only the pill, like sChevron below, so it draws 6px in.
//
// Color 0 is transparent, 1 the body, 2 the corner pixels and 3 the letters.
#define CNF_INK_X  6
#define CNF_INK_W  20

static const u8 sConfusionBadge[8][CNF_INK_W] =
{
    {0,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,0},
    {2,1,1,1,3,3,1,1,3,1,1,3,1,3,3,3,3,1,1,2},
    {1,1,1,3,1,1,3,1,3,3,1,3,1,3,1,1,1,1,1,1},
    {1,1,1,3,1,1,1,1,3,1,3,3,1,3,1,1,1,1,1,1},
    {1,1,1,3,1,1,1,1,3,1,1,3,1,3,3,3,1,1,1,1},
    {1,1,1,3,1,1,3,1,3,1,1,3,1,3,1,1,1,1,1,1},
    {2,1,1,1,3,3,1,1,3,1,1,3,1,3,1,1,1,1,1,2},
    {0,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,0},
};

// Teal, because no other badge uses it. FRZ's pale blue is the nearest, and
// this is darker and greener, so the two are different when a badge alternates.
// The values are BGR555, like the sheet's, and the corner is a little lighter
// than the body, as in the sheet.
static const u16 sConfusionPal[4] =
{
    0,
    0x5288,     // body,   RGB( 65,164,164)
    0x630C,     // corner, RGB( 98,197,197)
    0x7FFF,     // lettering, the sheet's white
};

static void DrawConfusionBadge(int x, int y)
{
    u16 pal[4];

    for (int i = 1; i < 4; i++)
        pal[i] = UiBgr555ToRgb565(sConfusionPal[i]);

    for (int row = 0; row < 8; row++)
    {
        for (int col = 0; col < CNF_INK_W; col++)
        {
            u8 ink = sConfusionBadge[row][col];

            if (ink != 0)
                UiPixel(x + CNF_INK_X + col, y + row, pal[ink]);
        }
    }
}

void UiStatusIcon(int x, int y, u8 ailment)
{
    static u8   tiles[0x400];      // sSpriteSheet_StatusIcons' size
    static u16  pal[16];
    static bool8 loaded;

    const u8 *icon;

    if (ailment == UI_STATUS_CNF)
    {
        DrawConfusionBadge(x, y);
        return;
    }

    // As in UpdatePartyMonAilmentGfx(): the party menu hides the sprite for
    // both of these.
    if (ailment == AILMENT_NONE || ailment == AILMENT_PKRS || ailment > AILMENT_FNT)
        return;

    if (!loaded)
    {
        u16 gbaPal[16];

        // The decompressor uses only the size word in the data, so check it
        // against the destinations. The gbaPal array is on the stack.
        if (GetDecompressedDataSize(gStatusGfx_Icons) > sizeof(tiles)
         || GetDecompressedDataSize(gStatusPal_Icons) > sizeof(gbaPal))
            return;

        LZDecompressWram(gStatusGfx_Icons, tiles);
        LZDecompressWram(gStatusPal_Icons, gbaPal);
        UiLoadPal(pal, gbaPal, 16);
        loaded = TRUE;
    }

    icon = tiles + (ailment - 1) * 4 * 32;

    for (int t = 0; t < 4; t++)
        UiBlit4bppTile(x + t * 8, y, icon + t * 32, pal, TRUE);
}

// A move or species type badge: the game's own 32x16 icon, so FIRE here is the
// same as on the summary screen.
//
// One sheet of 23 icons (18 types and 5 contest categories) at 0x100 bytes
// each, in type order. The icons share a palette of three 16-color banks.
// Ctr3dsGetTypeIconPalBank gives the bank of a type. The data comes through the
// PLATFORM_3DS accessors in src/pokemon_summary_screen.c, because the palette
// table is file-static there.
//
// Loaded once and kept, like UiStatusIcon. Check the size of both destinations
// before each decompress (see the note above UiMonPic).
void UiTypeIcon(int x, int y, u8 type)
{
    #define TYPE_ICON_COUNT (NUMBER_OF_MON_TYPES + CONTEST_CATEGORIES_COUNT)

    static u8    tiles[TYPE_ICON_COUNT * CTR_TYPE_ICON_BYTES];
    static u16   pal[3 * 16];
    static bool8 loaded;

    const u8 *icon;
    const u16 *bank;

    if (type >= TYPE_ICON_COUNT)
        return;

    if (!loaded)
    {
        const u32 *gfxLZ;
        const u32 *palLZ;
        u16 gbaPal[3 * 16];

        Ctr3dsGetTypeIconGfx(&gfxLZ, &palLZ);

        if (GetDecompressedDataSize(gfxLZ) > sizeof(tiles)
         || GetDecompressedDataSize(palLZ) > sizeof(gbaPal))
            return;

        LZDecompressWram(gfxLZ, tiles);
        LZDecompressWram(palLZ, gbaPal);
        UiLoadPal(pal, gbaPal, 3 * 16);
        loaded = TRUE;
    }

    icon = tiles + (u32)type * CTR_TYPE_ICON_BYTES;
    bank = pal + Ctr3dsGetTypeIconPalBank(type) * 16;

    // The icon is 32x16, so 8 tiles in 1D sprite order: four across and two
    // down.
    for (int t = 0; t < 8; t++)
        UiBlit4bppTile(x + (t % 4) * 8, y + (t / 4) * 8, icon + t * 32, bank, TRUE);

    #undef TYPE_ICON_COUNT
}

// Calculated, not stored. Row r counts from the tip and spans columns (W/2 - r)
// to (W/2 + r). The two end columns are the outline and the pixels between them
// are the fill. The last row is the flat base, all outline.
//
// The outline is on every side. The 20 window frames go from near white to near
// black, so a fill color alone is not visible on half of them.
void UiArrow(int x, int y, bool8 up, u16 fill)
{
    const int mid = UI_ARROW_W / 2;

    for (int r = 0; r < UI_ARROW_H; r++)
    {
        int py = y + (up ? r : (UI_ARROW_H - 1 - r));
        int left, right;

        if (r == UI_ARROW_H - 1)
        {
            UiFillRect(x, py, UI_ARROW_W, 1, UI_COL_SHADOW);
            continue;
        }

        left  = mid - r;
        right = mid + r;

        UiPixel(x + left,  py, UI_COL_SHADOW);
        UiPixel(x + right, py, UI_COL_SHADOW);

        if (right - left > 1)
            UiFillRect(x + left + 1, py, right - left - 1, 1, fill);
    }
}

// The party grid, the detail view and the BAG picker all draw this, so it is
// here. `hp` is a parameter, so a caller can give an animated value. The party
// tab slides its bars. The picker shows the real value.
void UiHpBar(int x, int y, int w, u32 hp, u32 maxHp)
{
    u16 light, dark;
    u32 filled;

    UiFillRect(x, y, w, 8, UI_COL_HP_BACK);

    if (maxHp == 0)
        return;

    // Use the game's own function for the thresholds. GetHPBarLevel compares a
    // rounded pixel count from GetScaledHPFraction, not the exact ratio, so a
    // copy of the 50/20 percent split could disagree at the limits.
    switch (GetHPBarLevel((s16)hp, (s16)maxHp))
    {
    case HP_BAR_FULL:
    case HP_BAR_GREEN:  light = UI_COL_HP_HIGH_L; dark = UI_COL_HP_HIGH; break;
    case HP_BAR_YELLOW: light = UI_COL_HP_MID_L;  dark = UI_COL_HP_MID;  break;
    default:            light = UI_COL_HP_LOW_L;  dark = UI_COL_HP_LOW;  break;
    }

    filled = (hp * (u32)w) / maxHp;
    // Any HP above zero shows at least one pixel.
    if (filled == 0 && hp > 0)
        filled = 1;

    // Light over dark, like the game's two-tone bar.
    UiFillRect(x, y, (int)filled, 4, light);
    UiFillRect(x, y + 4, (int)filled, 4, dark);
}

// The cursor that the game puts next to the selected battle menu entry (FIGHT,
// BAG, POKEMON, RUN).
//
// Copied from tiles 1 and 2 of graphics/battle_interface/textbox.png, which
// ActionSelectionCreateCursorAt copies into the menu window. That sheet has 256
// tiles and only two are necessary, so it is copied here and not decompressed.
// The blank rows and columns around it are not included.
//
// The two values are the source's palette roles: 1 is index 9, the body, and 2
// is index 7, the shadow on its lower edge. They are the same dark ink and
// light shadow as the game's menu text, so they map to the theme colors. A
// fixed color is not visible on half of the 20 window frames.
static const u8 sChevron[UI_CHEVRON_H][UI_CHEVRON_W] =
{
    {1, 1, 0, 0, 0, 0},
    {1, 1, 1, 0, 0, 0},
    {1, 1, 1, 1, 0, 0},
    {1, 1, 1, 1, 1, 0},
    {1, 1, 1, 1, 1, 2},
    {1, 1, 1, 1, 1, 2},
    {1, 1, 1, 1, 2, 2},
    {1, 1, 1, 2, 2, 0},
    {1, 1, 2, 2, 0, 0},
    {0, 2, 2, 0, 0, 0},
};

void UiChevron(int x, int y)
{
    u16 body = UiThemeText();
    u16 shadow = UiThemeShadow();

    for (int row = 0; row < UI_CHEVRON_H; row++)
    {
        for (int col = 0; col < UI_CHEVRON_W; col++)
        {
            u8 ink = sChevron[row][col];

            if (ink != 0)
                UiPixel(x + col, y + row, (ink == 1) ? body : shadow);
        }
    }
}

// The gold sparkle that the game shows around a shiny, copied from
// graphics/battle_anims/sprites/gold_stars.png (ANIM_TAG_GOLD_STARS, used by
// TryShinyAnimation). UI_COL_SHINY* comes from the same art.
//
// That sheet is 16x24: six 8x8 tiles with three stars. Tiles 0-3 are a 16x16,
// tile 4 is an 8x8, and tile 5 is a small twinkle. All three are copied, so the
// twinkle uses the artist's own frames and needs no scaler.
//
// The sheet has only six tiles, so it is copied here and not decompressed at
// run time.
//
// The values are the source's palette roles: 6 is the pale gold, 7 the gold
// body and 8 the orange edge. UiSparkle maps them to the three UI_COL_SHINY*
// constants.
#define SPARKLE_BIG_W 16
#define SPARKLE_BIG_H 14
#define SPARKLE_MID_W 6
#define SPARKLE_MID_H 6
#define SPARKLE_SML_W 3
#define SPARKLE_SML_H 3

static const u8 sSparkleBig[SPARKLE_BIG_H][SPARKLE_BIG_W] =
{
    {0,0,0,0,0,0,0,8,8,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,7,7,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,8,6,6,8,0,0,0,0,0,0},
    {0,0,0,0,0,0,7,6,6,7,0,0,0,0,0,0},
    {0,0,0,0,0,0,7,6,6,7,0,0,0,0,0,0},
    {8,7,7,7,7,6,6,6,6,6,6,7,7,7,7,8},
    {0,8,7,6,6,6,6,6,6,6,6,6,6,7,8,0},
    {0,0,0,8,6,6,6,6,6,6,6,6,8,0,0,0},
    {0,0,0,0,7,6,6,6,6,6,6,7,0,0,0,0},
    {0,0,0,0,8,6,6,6,6,6,6,8,0,0,0,0},
    {0,0,0,0,8,6,6,7,7,6,6,8,0,0,0,0},
    {0,0,0,8,6,6,7,0,0,7,6,6,8,0,0,0},
    {0,0,0,8,6,8,0,0,0,0,8,6,8,0,0,0},
    {0,0,0,7,0,0,0,0,0,0,0,0,7,0,0,0},
};

static const u8 sSparkleMid[SPARKLE_MID_H][SPARKLE_MID_W] =
{
    {0,0,8,7,0,0},
    {0,0,6,6,0,0},
    {8,7,6,6,7,8},
    {0,7,6,6,7,0},
    {0,6,7,7,6,0},
    {0,6,0,0,6,0},
};

static const u8 sSparkleSml[SPARKLE_SML_H][SPARKLE_SML_W] =
{
    {0,6,0},
    {6,6,6},
    {0,6,0},
};

// The source's palette roles 6 to 8 are the three steps of the ramp. UiSparkle
// gives the gold ramp. UiSparkleRamp takes any ramp of the same shape, which
// gives each achievement category its own color.
#define SPARKLE_ROLE_PALE 6
#define SPARKLE_ROLE_BODY 7
#define SPARKLE_ROLE_EDGE 8

void UiSparkle(int cx, int cy, u8 size)
{
    UiSparkleRamp(cx, cy, size, UI_COL_SHINY_PALE, UI_COL_SHINY, UI_COL_SHINY_EDGE);
}

void UiSparkleRamp(int cx, int cy, u8 size, u16 pale, u16 body, u16 edge)
{
    // The values ax and ay are the star's bright horizontal axis in each frame,
    // which (cx, cy) names. Do not center on the bounding box. Each frame has a
    // longer bottom than top, so the twinkle would move up as it grows.
    static const struct
    {
        const u8 *ink;
        u8 w, h, ax, ay;
    } sFrames[UI_SPARKLE_SIZES] =
    {
        { &sSparkleSml[0][0], SPARKLE_SML_W, SPARKLE_SML_H, 1, 1 },
        { &sSparkleMid[0][0], SPARKLE_MID_W, SPARKLE_MID_H, 3, 2 },
        { &sSparkleBig[0][0], SPARKLE_BIG_W, SPARKLE_BIG_H, 8, 5 },
    };

    const u8 *ink;
    int w, h, x, y;

    if (size >= UI_SPARKLE_SIZES)
        return;

    ink = sFrames[size].ink;
    w   = sFrames[size].w;
    h   = sFrames[size].h;
    x   = cx - sFrames[size].ax;
    y   = cy - sFrames[size].ay;

    for (int row = 0; row < h; row++)
    {
        for (int col = 0; col < w; col++)
        {
            u8 role = ink[row * w + col];

            if (role == SPARKLE_ROLE_PALE)
                UiPixel(x + col, y + row, pale);
            else if (role == SPARKLE_ROLE_BODY)
                UiPixel(x + col, y + row, body);
            else if (role == SPARKLE_ROLE_EDGE)
                UiPixel(x + col, y + row, edge);
        }
    }
}

// The last full paint: 153,600 bytes of .bss. It lets a step move an icon with
// no full repaint. The UI layer uses no heap, so this is a static like every
// other cache here.
static u16 sSnap[UI_W * UI_H];
static int sSnapValid;

void UiSnapshot(void)
{
    for (int i = 0; i < UI_W * UI_H; i++)
        sSnap[i] = sFb[i];

    sSnapValid = 1;
}

int UiHasSnapshot(void)
{
    return sSnapValid;
}

void UiRestoreRect(int x, int y, int w, int h)
{
    if (!sSnapValid)
        return;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > UI_W) w = UI_W - x;
    if (y + h > UI_H) h = UI_H - y;
    if (w <= 0 || h <= 0)
        return;

    for (int row = 0; row < h; row++)
    {
        int base = (y + row) * UI_W + x;

        for (int col = 0; col < w; col++)
            sFb[base + col] = sSnap[base + col];
    }
}

int UiHit(const CtrTouchState *t, int x, int y, int w, int h)
{
    return t->x >= x && t->x < x + w && t->y >= y && t->y < y + h;
}

// Press-and-hold auto-repeat. See the rules above UiHold in ui_draw.h.
//
// The state comes from this frame's touch, not from earlier frames. A press
// that slides off the control stops the repeat, and each press starts its own
// delay from zero. When a tab is swapped out under a finger, its handler stops.
// A kept counter would then make the next press repeat at once.
bool8 UiHoldRepeat(UiHold *h, const CtrTouchState *t, int x, int y, int w, int hgt)
{
    int inside = UiHit(t, x, y, w, hgt);

    if (t->justPressed)
    {
        h->frames = 0;
        h->next = UI_HOLD_DELAY;
        h->repeated = 0;
    }

    if (t->touching && inside)
    {
        h->frames++;

        if (h->frames < h->next)
            return FALSE;

        h->next = (u16)(h->frames + (h->frames >= UI_HOLD_FAST_AT
                                     ? UI_HOLD_FAST : UI_HOLD_PERIOD));
        h->repeated = 1;
        return TRUE;
    }

    {
        // A release inside the control that did not repeat is a normal tap. It
        // acts on release, so a touch that slides off does not trigger it.
        bool8 tap = (t->justReleased && inside && h->frames != 0 && !h->repeated);

        h->frames = 0;
        h->next = UI_HOLD_DELAY;
        h->repeated = 0;
        return tap;
    }
}
