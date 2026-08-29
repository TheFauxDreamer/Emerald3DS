// MAP tab: Hoenn's region map, drawn at 1:1, with a marker where the player is.
//
// The art is the game's own -- the same tiles, tilemap and palette the PokeNav
// draws -- reached through the accessors in the PLATFORM_3DS block of
// src/region_map.c, because they are file-static there. Nothing in this file
// writes game state.
//
// Three things about that art are not obvious and are worth stating once, since
// getting any of them wrong produces a plausible-looking wrong picture:
//
//   1. The background is 8bpp, not 4bpp. UiBlit4bppTile cannot draw it.
//
//   2. Its tilemap is an AFFINE one: BG2 is set up with BG_ATTR_SCREENSIZE 2 and
//      BG_ATTR_PALETTEMODE 1 (src/region_map.c), and affine screen size 2 is
//      64x64 tiles at ONE BYTE per entry, which is exactly the 4096 bytes
//      map.bin decompresses to. So there are no 16-bit screen entries, no flip
//      bits and no palette-bank field: the tile id is simply the byte.
//
//   3. Those tile bytes are ABSOLUTE palette indices in the 112..143 range,
//      because the game loads the map's 32 colours at BG_PLTT_ID(7). Hence the
//      256-entry palette below with only that slice filled.
//
// What is deliberately NOT used: InitRegionMap() and LoadRegionMapGfx(). They
// drive BG layers, sprites and the task system on the top screen. The one piece
// of src/region_map.c this file does lean on is the player's position, and only
// because it is pure arithmetic -- see Ctr3dsGetRegionMapPlayerPos.

#include "global.h"
#include "decompress.h"
#include "region_map.h"
#include "landmark.h"
#include "constants/region_map_sections.h"

#include "../bridge.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"

// The drawn artwork's real extent inside the 64x64 tilemap: everything outside
// this is the blank tile. Measured from map.bin rather than assumed, because the
// MAPSEC grid (28x15 at offset 1,2) is smaller than the picture around it -- the
// northern coast, Dewford's islands and the eastern edge all sit outside it.
#define MAP_TW        31
#define MAP_TH        19
#define TILEMAP_STRIDE 64

// 31 tiles is 248px in a 320px panel, so it centres with 36px each side, and 19
// tiles is 152px, leaving exactly 40px for the caption. 2x would be 496x304 and
// does not fit in either dimension, so 1x is the only scale -- which is also the
// only pixel-perfect one.
#define MAP_PX        ((CTR_BOTTOM_WIDTH - MAP_TW * 8) / 2)
#define MAP_PY        0

// The caption takes the rest of the content area. Both bands are whole tiles so
// the window frame lands on tile boundaries: 19 + 5 = 24 tiles = 192px.
#define CAP_TY        MAP_TH
#define CAP_TH        ((UI_CONTENT_H / 8) - MAP_TH)
#define CAP_Y         (CAP_TY * 8)
#define CAP_TEXT_Y    (CAP_Y + 12)
#define CAP_MARGIN    10

#define MAP_TILE_COUNT 233
#define MAP_PAL_BASE   112
#define MAP_PAL_COUNT  32

// Decompressed once and kept. 19KB against a 150KB framebuffer in the same
// directory, and nothing in 3ds/ui/ ever touches the game's heap.
static u8    sTiles[MAP_TILE_COUNT * 64];
static u8    sTilemap[TILEMAP_STRIDE * TILEMAP_STRIDE];
static u16   sPal[256];
static u8    sIconGfx[0x80];        // 16x16 4bpp, 4 tiles, stored uncompressed
static u16   sIconPal[16];
static bool8 sLoaded;

// The tile the player last tapped, or -1 to follow the player instead.
static s8 sPickX = -1;
static s8 sPickY = -1;

// ---------------------------------------------------------------- loading ---
//
// The map never changes, so this is a one-shot load rather than a keyed cache.
// Both destinations are size-checked before either decompress: LZDecompressWram
// is bounded only by the size word in its own input, and an overrun here lands
// in the neighbouring statics of this file. That exact bug has been hit in
// ui_draw.c before -- see the comment above UiMonPic.
static void EnsureLoaded(void)
{
    const u32 *gfxLZ;
    const u32 *tilemapLZ;
    const u16 *gbaPal;
    const u8 *iconGfx;
    const u16 *iconPal;

    if (sLoaded)
        return;

    Ctr3dsGetRegionMapGfx(&gfxLZ, &tilemapLZ, &gbaPal);

    if (GetDecompressedDataSize(gfxLZ) > sizeof(sTiles))
        return;
    if (GetDecompressedDataSize(tilemapLZ) > sizeof(sTilemap))
        return;

    LZDecompressWram(gfxLZ, sTiles);
    LZDecompressWram(tilemapLZ, sTilemap);

    // Only the slice the art uses. Every other entry stays 0 and is never
    // indexed: no tile the tilemap references contains a byte outside this range.
    UiLoadPal(&sPal[MAP_PAL_BASE], gbaPal, MAP_PAL_COUNT);

    Ctr3dsGetRegionMapPlayerIcon(&iconGfx, &iconPal);
    for (u32 i = 0; i < sizeof(sIconGfx); i++)
        sIconGfx[i] = iconGfx[i];
    UiLoadPal(sIconPal, iconPal, 16);

    sLoaded = TRUE;
}

// --------------------------------------------------------------- helpers ----

// Whether a tap landed on something nameable. Takes the same absolute map-tile
// coordinates GetRegionMapSecIdAt does, which is what dividing a touch position
// by 8 gives directly.
static bool8 PickIsSet(void)
{
    return sPickX >= 0 && sPickY >= 0;
}

// The mapsec the caption is describing, and where its box sits. `mapSecId` is
// always written; the rest only when the player is what is being described.
static mapsec_u16_t CaptionMapSec(u8 *posWithinMapSec)
{
    u16 x, y;
    mapsec_u16_t mapSecId;
    bool8 inCave;

    if (PickIsSet())
    {
        *posWithinMapSec = 0;
        return GetRegionMapSecIdAt((u16)sPickX, (u16)sPickY);
    }

    Ctr3dsGetRegionMapPlayerPos(&x, &y, &mapSecId, posWithinMapSec, &inCave);
    return mapSecId;
}

// --------------------------------------------------------------- drawing ----

static void DrawMap(void)
{
    for (int ty = 0; ty < MAP_TH; ty++)
    {
        for (int tx = 0; tx < MAP_TW; tx++)
        {
            u32 tileId = sTilemap[ty * TILEMAP_STRIDE + tx];

            // Opaque: index 0 never appears in a tile this map references, so
            // there is nothing to see through and nothing to test per pixel.
            UiBlit8bppTile(MAP_PX + tx * 8, MAP_PY + ty * 8,
                           &sTiles[tileId * 64], sPal, FALSE);
        }
    }
}

// The 16x16 icon, positioned the way CreateRegionMapPlayerIcon positions its
// sprite: that sets the sprite's CENTRE to tile*8 + 4, so the top-left corner is
// tile*8 - 4.
static void DrawPlayerIcon(void)
{
    u16 x, y;
    mapsec_u16_t mapSecId;
    u8 posWithinMapSec;
    bool8 inCave;
    int px, py;

    // Birth Island, Faraway Island and Navel Rock are not on the Hoenn map at
    // all, and the game draws no icon there either.
    if (IsEventIslandMapSecId(gMapHeader.regionMapSectionId))
        return;

    Ctr3dsGetRegionMapPlayerPos(&x, &y, &mapSecId, &posWithinMapSec, &inCave);

    px = MAP_PX + (int)x * 8 - 4;
    py = MAP_PY + (int)y * 8 - 4;

    for (int t = 0; t < 4; t++)
        UiBlit4bppTile(px + (t % 2) * 8, py + (t / 2) * 8,
                       sIconGfx + t * 32, sIconPal, TRUE);
}

// Outlines the whole mapsec, not the tile that was tapped: a two-tile city reads
// as one place, and boxing half of it would look like a mis-hit.
static void DrawPick(void)
{
    mapsec_u16_t mapSecId;
    const struct RegionMapLocation *entry;

    if (!PickIsSet())
        return;

    mapSecId = GetRegionMapSecIdAt((u16)sPickX, (u16)sPickY);
    if (mapSecId >= MAPSEC_NONE)
        return;

    entry = &gRegionMapEntries[mapSecId];

    UiRect(MAP_PX + (entry->x + CTR_MAPCURSOR_X_MIN) * 8,
           MAP_PY + (entry->y + CTR_MAPCURSOR_Y_MIN) * 8,
           entry->width * 8, entry->height * 8, UI_COL_ACCENT);
}

static void DrawCaption(void)
{
    u8 posWithinMapSec = 0;
    mapsec_u16_t mapSecId = CaptionMapSec(&posWithinMapSec);
    const u8 *landmark;
    // Not MAP_NAME_LENGTH (16): the game's own struct RegionMap sizes this field
    // at 20, and GetMapName's empty-name fallback fills 18 plus a terminator.
    u8 name[32];

    UiWindowFrame(0, CAP_TY, CTR_BOTTOM_WIDTH / 8, CAP_TH);

    if (mapSecId >= MAPSEC_NONE)
    {
        u8 label[16];
        UiText(CAP_MARGIN, CAP_TEXT_Y, UiAscii(label, "Sea", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
        return;
    }

    // Already in the game's own encoding, so it goes straight to UiText.
    GetMapNameGeneric(name, mapSecId);
    UiText(CAP_MARGIN, CAP_TEXT_Y, name, UiThemeText(), UiThemeShadow());

    // Only for the player's own location: landmarks are keyed on which tile of a
    // multi-tile mapsec you are standing in, and a tapped tile has no such
    // position to offer.
    if (PickIsSet())
        return;

    landmark = GetLandmarkName((mapsec_u8_t)mapSecId, posWithinMapSec, 0);
    if (landmark != NULL)
        UiTextRight(CTR_BOTTOM_WIDTH - CAP_MARGIN, CAP_TEXT_Y, landmark,
                    UI_COL_DIM, UiThemeShadow());
}

void UiMapDraw(void)
{
    EnsureLoaded();

    if (!sLoaded)
    {
        u8 label[24];
        UiWindowFrame(0, 0, CTR_BOTTOM_WIDTH / 8, UI_CONTENT_H / 8);
        UiText(16, 16, UiAscii(label, "Map unavailable.", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
        return;
    }

    DrawMap();
    DrawPick();
    DrawPlayerIcon();
    DrawCaption();
}

// ----------------------------------------------------------- repaint key ----
//
// Without this the map would go stale while the player walks: nothing else in
// UiStateHash tracks where they are. It is naturally cheap and naturally coarse
// -- the cursor position only moves when the player crosses a band boundary
// within a mapsec, so walking the length of one stretch of route costs no
// repaints at all.
u32 UiMapStateKey(void)
{
    u16 x, y;
    mapsec_u16_t mapSecId;
    u8 posWithinMapSec;
    bool8 inCave;

    Ctr3dsGetRegionMapPlayerPos(&x, &y, &mapSecId, &posWithinMapSec, &inCave);

    return ((u32)x << 24) ^ ((u32)y << 16) ^ ((u32)mapSecId << 4)
         ^ (u32)posWithinMapSec ^ ((u32)inCave << 3);
}

// --------------------------------------------------------------- input -----

void UiMapTouch(const CtrTouchState *t)
{
    int tx, ty;

    if (!t->justReleased)
        return;

    // Anywhere off the map, the caption band included, drops the selection and
    // goes back to reporting where the player actually is.
    if (t->y < MAP_PY || t->y >= MAP_PY + MAP_TH * 8
     || t->x < MAP_PX || t->x >= MAP_PX + MAP_TW * 8)
    {
        if (PickIsSet())
        {
            sPickX = -1;
            sPickY = -1;
            UiMarkDirty();
        }
        return;
    }

    tx = (t->x - MAP_PX) / 8;
    ty = (t->y - MAP_PY) / 8;

    // Open sea has no mapsec. Treat it as a deselect rather than ignoring it, so
    // there is always an obvious way back to following the player.
    if (GetRegionMapSecIdAt((u16)tx, (u16)ty) >= MAPSEC_NONE)
    {
        sPickX = -1;
        sPickY = -1;
    }
    else
    {
        sPickX = (s8)tx;
        sPickY = (s8)ty;
    }

    UiMarkDirty();
}
