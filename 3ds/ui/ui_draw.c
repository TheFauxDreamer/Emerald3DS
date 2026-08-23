// Bottom-screen drawing primitives. See draw.h for why these exist.

#include "global.h"
#include "text_window.h"
#include "pokemon_icon.h"
#include "item_icon.h"
#include "graphics.h"                 // gStatusGfx_Icons, gStatusPal_Icons
#include "decompress.h"
#include "menu.h"                     // gStandardMenuPalette
#include "option_menu.h"              // Ctr3dsLiveWindowFrameType
#include "constants/characters.h"     // TEXT_COLOR_*
#include "constants/party_menu.h"     // AILMENT_*

#include "ui_draw.h"
#include "ui_shell.h"                 // UI_COL_SHADOW

static u16 sFb[UI_W * UI_H];

u16 *UiFb(void) { return sFb; }

// GBA palettes are BGR555 with the high bit unused; the 3DS texture is RGB565.
// Green gains a bit, so replicate the top bit rather than leaving it dark.
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

void UiClear(u16 color)
{
    for (int i = 0; i < UI_W * UI_H; i++)
        sFb[i] = color;
}

void UiFillRect(int x, int y, int w, int h, u16 color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > UI_W) w = UI_W - x;
    if (y + h > UI_H) h = UI_H - y;
    if (w <= 0 || h <= 0)
        return;

    for (int row = 0; row < h; row++)
    {
        u16 *dst = &sFb[(y + row) * UI_W + x];
        for (int col = 0; col < w; col++)
            dst[col] = color;
    }
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

void UiBlit4bppTile(int x, int y, const u8 *tile, const u16 *pal, int transparent0)
{
    for (int row = 0; row < 8; row++)
    {
        int py = y + row;
        if (py < 0 || py >= UI_H)
            continue;

        const u8 *src = tile + row * 4;   // 8 pixels, 2 per byte
        u16 *dst = &sFb[py * UI_W];

        for (int col = 0; col < 8; col++)
        {
            int px = x + col;
            if (px < 0 || px >= UI_W)
                continue;

            // Low nibble is the left pixel of each byte.
            u32 idx = (col & 1) ? (src[col >> 1] >> 4) : (src[col >> 1] & 0xF);
            if (idx == 0 && transparent0)
                continue;

            dst[px] = pal[idx];
        }
    }
}

// The player picks one of 20 borders in Options -> Frame; honouring it is what
// makes the second screen read as part of the game rather than an overlay.
// GetWindowFrameTilesPal() is the game's own accessor and is bounds-checked
// (src/text_window.c), so a corrupt setting falls back to frame 0 rather than
// reading past the table.
//
// Every frame is a 3x3 nine-slice: the game loads 0x120 bytes = 9 tiles, in
// row-major order, with a 16-colour palette. Corners are drawn once; edges and
// centre repeat.
u8 UiFrameId(void)
{
    // While the options menu is open the player's choice lives in that menu's
    // task and is not written to the save block until they leave. Prefer the
    // live value, so the border previews here at the same moment it does on the
    // top screen rather than snapping when the menu closes.
    s16 live = Ctr3dsLiveWindowFrameType();

    if (live >= 0)
        return (u8)live;

    return gSaveBlock2Ptr->optionsWindowFrameType;
}

void UiWindowFrame(int tx, int ty, int wTiles, int hTiles)
{
    // Keyed on the frame id, not a one-shot flag: the player can change the
    // setting at any time and a stale palette would silently mismatch the tiles.
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

            // Opaque: the frame is the background, nothing shows through it.
            UiBlit4bppTile((tx + col) * 8, (ty + row) * 8, tile, pal, FALSE);
        }
    }
}

// Text drawn ON a frame must use the game's own menu colours, not a fixed
// white: the 20 frames run from light to dark, and Emerald prints dark-on-light
// on all of them. These are the exact indices its menus use
// (include/constants/characters.h) out of gStandardMenuPalette (src/menu.c).
u16 UiThemeText(void)
{
    return UiBgr555ToRgb565(gStandardMenuPalette[TEXT_COLOR_DARK_GRAY]);
}

u16 UiThemeShadow(void)
{
    return UiBgr555ToRgb565(gStandardMenuPalette[TEXT_COLOR_LIGHT_GRAY]);
}

void UiMonIcon(int x, int y, u16 species, u32 personality)
{
    const u8 *gfx = GetMonIconPtr(species, personality, FALSE);
    const u16 *gbaPal = GetValidMonIconPalettePtr(species);
    u16 pal[16];

    if (gfx == NULL || gbaPal == NULL)
        return;

    UiLoadPal(pal, gbaPal, 16);

    // 32x32 sprite, 1D mapping: 16 consecutive tiles, four per row.
    for (int t = 0; t < 16; t++)
        UiBlit4bppTile(x + (t % 4) * 8, y + (t / 4) * 8, gfx + t * 32, pal, TRUE);
}

// Item icons are stored LZ-compressed as 3x3 tiles and expanded into a 4x4
// sprite, so this follows the game's own sequence: decompress, then let its
// CopyItemIconPicTo4x4Buffer do the rearrangement (src/item_icon.c).
//
// Buffers are static rather than Alloc'd. This runs from the per-frame hook,
// and churning the game's heap every repaint would be a poor neighbour.
void UiItemIcon(int x, int y, u16 itemId)
{
    static u8  raw[0x120];     // 3x3 tiles, the size AllocItemIconTemporaryBuffers uses
    static u8  tiles[0x200];   // 4x4 tiles
    static u16 gbaPal[16];
    u16 pal[16];

    const void *pic    = GetItemIconPicOrPalette(itemId, 0);
    const void *palSrc = GetItemIconPicOrPalette(itemId, 1);

    if (pic == NULL || palSrc == NULL)
        return;

    // CopyItemIconPicTo4x4Buffer only writes three 3-tile rows, so the fourth
    // column and row keep whatever was there. The game gets this from
    // AllocZeroed; a reused static has to be cleared by hand or the previous
    // item bleeds through.
    for (u32 i = 0; i < sizeof(tiles); i++)
        tiles[i] = 0;

    LZDecompressWram((const u32 *)pic, raw);
    CopyItemIconPicTo4x4Buffer(raw, tiles);
    LZDecompressWram((const u32 *)palSrc, gbaPal);

    UiLoadPal(pal, gbaPal, 16);

    for (int t = 0; t < 16; t++)
        UiBlit4bppTile(x + (t % 4) * 8, y + (t / 4) * 8, tiles + t * 32, pal, TRUE);
}

// The party menu's own ailment art, so PSN here is the same badge PSN is there.
// One 32x64 sheet of eight 32x8 badges, four tiles each, in the order the anim
// table declares (src/data/party_menu.h): PSN, PRZ, SLP, FRZ, BRN, PKRS, FNT,
// blank. UpdatePartyMonAilmentGfx() selects with `status - 1`, so that is the
// index arithmetic below.
//
// Index 0 of the sheet's palette is the transparency marker and every badge
// carries its own colours in the remaining slots, so one palette covers all of
// them and an ordinary transparent blit is all that is needed.
void UiStatusIcon(int x, int y, u8 ailment)
{
    static u8   tiles[0x400];      // the size sSpriteSheet_StatusIcons declares
    static u16  pal[16];
    static bool8 loaded;

    const u8 *icon;

    // Matches UpdatePartyMonAilmentGfx(): the party menu hides the sprite for
    // both of these rather than drawing anything.
    if (ailment == AILMENT_NONE || ailment == AILMENT_PKRS || ailment > AILMENT_FNT)
        return;

    if (!loaded)
    {
        u16 gbaPal[16];

        // The decompressor is bounded only by the size word in the data, so
        // check it against the destinations rather than trust it. gbaPal in
        // particular is on the stack.
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

// Generated rather than stored. Row r counts from the tip and spans columns
// (W/2 - r) to (W/2 + r); those two end columns are the outline and everything
// between them is fill. The last row is the flat base, all outline.
//
// The outline is on every side deliberately. The player picks one of 20 window
// frames and they run from near-white to near-black, so an arrow relying on its
// fill colour alone would disappear against half of them.
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

        UiFillRect(x + left,  py, 1, 1, UI_COL_SHADOW);
        UiFillRect(x + right, py, 1, 1, UI_COL_SHADOW);

        if (right - left > 1)
            UiFillRect(x + left + 1, py, right - left - 1, 1, fill);
    }
}

int UiHit(const CtrTouchState *t, int x, int y, int w, int h)
{
    return t->x >= x && t->x < x + w && t->y >= y && t->y < y + h;
}
