// Bottom-screen drawing primitives. See draw.h for why these exist.

#include "global.h"
#include "text_window.h"
#include "pokemon_icon.h"
#include "item_icon.h"
#include "graphics.h"                 // gStatusGfx_Icons, gStatusPal_Icons
#include "data.h"                     // gMonFrontPicTable, gMonPaletteTable
#include "battle.h"                   // struct DisableStruct, for the header below
#include "battle_interface.h"         // GetHPBarLevel
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

// One 8x8 8bpp tile: 64 bytes, one byte per pixel, row-major. Simpler than the
// 4bpp path -- no nibble to unpack -- but the palette is NOT 16 entries.
//
// An 8bpp GBA background has no palette-bank field in its map, so the byte IS an
// absolute index into the whole 256-entry BG palette. The region map's tiles
// therefore carry values around 112, because the game loads its 32 colours at
// BG_PLTT_ID(7) (src/region_map.c). `pal` must be sized to match: give it 256
// entries and fill the slice the art actually uses.
void UiBlit8bppTile(int x, int y, const u8 *tile, const u16 *pal, int transparent0)
{
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

    // Null until a file is loaded (src/load_save.c). This is read from the
    // shell's repaint hash on the very first frame, so without the guard it is
    // a null dereference at offset 0x14 -- an instant data abort on hardware,
    // which Azahar happened to tolerate. Frame 0 is Emerald's own default.
    if (gSaveBlock2Ptr == NULL)
        return 0;

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

// A Pokedex front sprite. Every mon pic is 64x64 4bpp
// (src/data/pokemon_graphics/front_pic_coordinates.h says so explicitly), laid
// out as 64 consecutive tiles in 1D sprite order, which is the same convention
// UiMonIcon uses at 4x4.
//
// LoadSpecialPokePic_DontHandleDeoxys is the game's own loader and is safe from
// here: it is an LZ77UnCompWram plus DrawSpindaSpots, with no allocation and no
// OAM. It also resolves the Unown letter, which is why the personality matters.
//
// The buffer is MAX_MON_PIC_FRAMES frames, NOT one. Six species (Poochyena,
// Marshtomp, Swablu, Blaziken, Walrein, Rayquaza) ship a 64x256 front sheet of
// four animation frames that decompresses to 8192 bytes, and the size in
// gMonFrontPicTable is the size of ONE frame, so it does not bound the write.
// The game sizes its own destinations the same way, at
// MON_PIC_SIZE * MAX_MON_PIC_FRAMES (src/battle_gfx_sfx_util.c:1297).
//
// Getting this wrong is not a quiet overrun: at 2048 bytes it ran 6 KB past the
// end and repainted the neighbouring statics in this file, one of which is the
// cached window-frame palette. The symptom was every other tab's border and
// background changing colour, differently for each Pokedex entry viewed.
//
// Cached on species. A dex cursor moving down a list repaints the whole screen
// each step, and re-expanding the sheet every time would be pure waste.
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

        // Both destinations checked against the size the data actually claims,
        // rather than trusting the tables to agree with the buffers.
        if (GetDecompressedDataSize(gMonFrontPicTable[species].data) > sizeof(pic)
         || GetDecompressedDataSize(gMonPaletteTable[species].data) > sizeof(gbaPal))
            return;

        // Unown and Spinda are the only two whose art depends on a stored
        // personality; GetPokedexMonPersonality (static, src/pokedex.c:4654) is
        // just these two fields, so this is that function inlined.
        u32 personality = (species == SPECIES_UNOWN)  ? gSaveBlock2Ptr->pokedex.unownPersonality
                        : (species == SPECIES_SPINDA) ? gSaveBlock2Ptr->pokedex.spindaPersonality
                        : 0;

        LoadSpecialPokePic_DontHandleDeoxys(&gMonFrontPicTable[species], pic,
                                            species, personality, TRUE);
        LZDecompressWram(gMonPaletteTable[species].data, gbaPal);
        UiLoadPal(pal, gbaPal, 16);
        cachedSpecies = species;
    }

    // Frame 0 only. Tiles are row-major, so the first 64 are the top 64x64 of
    // the sheet, which is the still pose for the animated species too.
    for (int t = 0; t < 64; t++)
        UiBlit4bppTile(x + (t % 8) * 8, y + (t / 8) * 8, pic + t * 32, pal, TRUE);
}

// The Pokedex "caught" marker. This is graphics/pokedex/caught_ball.png
// transcribed rather than linked: the game holds it in sCaughtBall_Gfx, which is
// static to src/pokedex.c. Same 7x7 shape the dex itself draws.
//
// Fixed red and white rather than theme colours. A Poke Ball is recognisable
// because of its colours, and its own dark outline carries it on any of the 20
// window frames.
void UiPokeball(int x, int y)
{
    // 0 transparent, 1 outline, 2 upper half, 3 lower half.
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
                UiFillRect(x + col, y + row, 1, 1, kColors[kBall[row][col]]);
}

// A species footprint: 4 tiles of 1bpp, arranged 2x2, which is what
// DrawFootprint does (src/pokedex.c:4585). Declared extern here because the
// table is a plain global with no public header, the same situation as
// gPokedexEntries in tab_dex.c.
extern const u8 *const gMonFootprintTable[];

void UiFootprint(int x, int y, u16 species, u16 color)
{
    const u8 *gfx;

    if (species == SPECIES_NONE || species >= NUM_SPECIES)
        return;

    gfx = gMonFootprintTable[species];
    if (gfx == NULL)
        return;

    // 4 tiles of 8 bytes, one byte per 8-pixel row, low bit leftmost.
    for (int t = 0; t < 4; t++)
    {
        int tx = x + (t % 2) * 8;
        int ty = y + (t / 2) * 8;

        for (int row = 0; row < 8; row++)
        {
            u8 bits = gfx[t * 8 + row];

            for (int col = 0; col < 8; col++)
                if (bits & (1 << col))
                    UiFillRect(tx + col, ty + row, 1, 1, color);
        }
    }
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

// A move/species type badge: the game's own 32x16 icon, so FIRE here is the
// same FIRE the summary screen shows.
//
// One sheet of 23 icons (18 types plus the 5 contest categories) at 0x100 bytes
// each, in type order, and a palette of three 16-colour banks that the icons
// share between them -- Ctr3dsGetTypeIconPalBank says which bank a type wants.
// Both come through the PLATFORM_3DS accessors in src/pokemon_summary_screen.c
// because the sheet's palette table is file-static there.
//
// Loaded once and kept, like UiStatusIcon: both destinations are size-checked
// before either decompress, because LZDecompressWram is bounded only by the
// size word in its own input and an overrun lands in this file's neighbouring
// statics. See the note above UiMonPic for what that looked like the last time.
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

    // 32x16, so 8 tiles in 1D sprite order: four across, two down.
    for (int t = 0; t < 8; t++)
        UiBlit4bppTile(x + (t % 4) * 8, y + (t / 4) * 8, icon + t * 32, bank, TRUE);

    #undef TYPE_ICON_COUNT
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

// The party grid, the party detail view and the BAG tab's target picker all draw
// this, so it lives here rather than in whichever tab happened to want it first.
// `hp` is a parameter rather than read from the mon so a caller can pass an
// animated value: the party tab slides its bars, the picker shows the truth.
void UiHpBar(int x, int y, int w, u32 hp, u32 maxHp)
{
    u16 light, dark;
    u32 filled;

    UiFillRect(x, y, w, 8, UI_COL_HP_BACK);

    if (maxHp == 0)
        return;

    // The game's own thresholds via its own function, rather than a
    // reimplementation of the 50/20 percent split that could disagree at the
    // boundaries: GetHPBarLevel compares a ROUNDED pixel count from
    // GetScaledHPFraction, not the exact ratio.
    switch (GetHPBarLevel((s16)hp, (s16)maxHp))
    {
    case HP_BAR_FULL:
    case HP_BAR_GREEN:  light = UI_COL_HP_HIGH_L; dark = UI_COL_HP_HIGH; break;
    case HP_BAR_YELLOW: light = UI_COL_HP_MID_L;  dark = UI_COL_HP_MID;  break;
    default:            light = UI_COL_HP_LOW_L;  dark = UI_COL_HP_LOW;  break;
    }

    filled = (hp * (u32)w) / maxHp;
    // Any surviving HP should show at least a sliver rather than reading as 0.
    if (filled == 0 && hp > 0)
        filled = 1;

    // Light over dark, the way the game's own two-tone bar reads.
    UiFillRect(x, y, (int)filled, 4, light);
    UiFillRect(x, y + 4, (int)filled, 4, dark);
}

// The cursor Emerald stamps beside the selected battle menu entry, the one that
// sits next to FIGHT / BAG / POKEMON / RUN.
//
// Transcribed from tiles 1 and 2 of graphics/battle_interface/textbox.png, the
// pair ActionSelectionCreateCursorAt copies into the menu window
// (src/battle_controller_player.c). Transcribed rather than decompressed
// because that sheet is 256 tiles and only two of them are wanted; UiPokeball
// above sets the same precedent for a glyph this small. The blank rows above
// and below it, and the blank column each side, are dropped here, so this is
// the ink and nothing else.
//
// The two values are the source's own palette roles: 1 is index 9, the body,
// and 2 is index 7, the shadow trailing its lower edge. In the battle textbox
// palette those are a dark ink and a light shadow -- the same pair Emerald
// prints menu text with -- so they map onto the theme colours here rather than
// onto fixed ones. A fixed colour would disappear against half of the 20 window
// frames, which is the same reason UiArrow carries an outline.
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
                UiFillRect(x + col, y + row, 1, 1, (ink == 1) ? body : shadow);
        }
    }
}

int UiHit(const CtrTouchState *t, int x, int y, int w, int h)
{
    return t->x >= x && t->x < x + w && t->y >= y && t->y < y + h;
}
