// Wild encounter list for a place. See view_encounters.h for what it is for.
//
// Everything here reads. The species come from gWildMonHeaders, which is the
// same table the encounter generator rolls against, so the list agrees with what
// the player will actually meet by construction rather than by being kept in
// step.
//
// Three things about that table are not obvious and each produces a
// plausible-looking wrong list if missed:
//
//   1. The four categories have DIFFERENT slot counts -- 12, 5, 5, 10. Reading
//      fishing with LAND_WILD_COUNT is the vanilla out-of-bounds bug in
//      MapHasSpecies (src/pokedex_area_screen.c); it is not copied here.
//
//   2. Altering Cave has NINE consecutive headers for one map and only the one
//      VAR_ALTERING_CAVE_WILD_SET names is live. A plain scan finds the first
//      and is wrong eight days out of nine.
//
//   3. With the randomiser on, the species in the table is not the species the
//      player meets: CreateWildMon remaps it (src/wild_encounter.c:384). The
//      mapping is applied here BEFORE deduping, because two table entries can
//      map onto one mon.

#include "global.h"
#include "pokemon.h"
#include "pokedex.h"
#include "data.h"
#include "event_data.h"
#include "overworld.h"
#include "region_map.h"
#include "wild_encounter.h"
#include "constants/maps.h"
#include "constants/species.h"
#include "constants/pokemon.h"
#include "constants/region_map_sections.h"
#include "constants/wild_encounter.h"
#include "constants/vars.h"

#include "../bridge.h"
#include "../tweaks.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "view_encounters.h"

// ---------------------------------------------------------------- layout ---
//
// The whole content area, 40x24 tiles, so the frame's interior is x 8..311 and
// y 8..183. Everything below is fitted inside that 176px of height:
//
//   8      header      one glyph row, 15px
//   25     grid        4 rows of 33, ending at 157
//   160    controls    22px, ending at 182
#define HDR_Y        8
#define HDR_MARGIN   10

#define GRID_Y       25
#define ROW_H        33
#define GRID_ROWS    4
#define GRID_COLS    2
#define COL_W        152
#define COL_X(c)     (8 + (c) * COL_W)

#define UI_ENC_PER_PAGE (GRID_ROWS * GRID_COLS)

// Inside one cell. The icon is 32x32, then a 12px gutter for the caught marker,
// then the name with the type badges under it. That puts the cell's right edge
// at cx+114 -- column 2 therefore ends at 274, clear of the 311 interior edge,
// and column 1 ends at 122, clear of column 2's icon at 160. The name gets the
// remaining 104px of its column, against about 60px for the longest
// ten-character species name.
//
// The ball's column is reserved whether or not a given cell draws one, which is
// what stops names shuffling sideways down a page -- the same reason the dex
// list reserves its own (tab_dex.c:62).
#define CELL_ICON_W  32
#define CELL_BALL_X  36
#define CELL_TEXT_X  48
#define CELL_TYPE_Y  16
#define CELL_TYPE2_X (CELL_TEXT_X + UI_TYPE_ICON_W + 2)

#define BTN_Y        160
#define BTN_H        22
#define PAGE_W       52
#define PAGE_UP_X    10
#define PAGE_DN_X    70
// The same 42x22 the DEX tab's entry screen uses, in the same place, so BACK
// means the same thing and is in the same corner on both.
#define BACK_X       (CTR_BOTTOM_WIDTH - 50)
#define BACK_W       42

// ------------------------------------------------------------------ state ---
//
// A hard stop rather than an assumption, sized off the data: the worst single
// map is Safari Zone Southeast at 15 unique species, and the worst MAPSEC, once
// its several maps are aggregated, is the Safari Zone at 38. That one is not
// reachable by tapping -- only mapsecs in the region map's 28x15 grid are, and
// the Safari Zone is not one of them -- but the cap is set past it anyway, so
// nothing the table can hold gets silently dropped. Six pages at most, which is
// what lets the page counter stay a single digit.
#define UI_ENC_MAX   48

static u16   sSpecies[UI_ENC_MAX];
static u8    sCount;

// What sSpecies was built for, so a repaint costs a comparison rather than a
// 124-header scan. A repaint is already ~4.9ms of a ~5.7ms budget.
static bool8 sBuilt;
static u8    sBuiltSrc;
static u8    sBuiltGroup, sBuiltNum;
static mapsec_u16_t sBuiltMapSec;

// The mapsec whose name titles the panel. Not always the one it was opened with:
// in PLAYER mode it is the current map's own mapsec, which is how a cave gets
// its own name instead of the outdoor one the region map resolves it to.
static mapsec_u16_t sTitleMapSec;

static bool8 sOpen;
static u8    sSrc;
static mapsec_u16_t sOpenMapSec;
static u8    sPage;

// ------------------------------------------------------------- gathering ----

// The dex's own three-state readout for one species: nothing, seen, or caught.
//
// Both flags in one call because every caller wants both, and the bounds guard
// and the species -> national conversion are the same work for either.
#define DEX_SEEN    (1u << 0)
#define DEX_CAUGHT  (1u << 1)

static u32 DexState(u16 species)
{
    u16 national;
    u32 state = 0;

    // SpeciesToNationalPokedexNum indexes [species - 1] into a table of
    // NUM_SPECIES - 1 entries with no bound of its own (src/pokemon.c:5690).
    if (species == SPECIES_NONE || species >= NUM_SPECIES)
        return 0;

    national = SpeciesToNationalPokedexNum(species);
    if (national == 0)
        return 0;

    if (GetSetPokedexFlag(national, FLAG_GET_SEEN))
        state |= DEX_SEEN;
    if (GetSetPokedexFlag(national, FLAG_GET_CAUGHT))
        state |= DEX_CAUGHT;

    return state;
}

static void AddSpecies(u16 species)
{
    if (sCount >= UI_ENC_MAX)
        return;

    if (species == SPECIES_NONE || species >= NUM_SPECIES)
        return;

    // The randomiser's mapping, applied here rather than at display time so the
    // dedupe below sees the mons the player will actually meet: two table
    // entries can map onto one.
    //
    // Ctr3dsMapSpecies and not Ctr3dsMapWildSpecies. The latter's extra guard
    // asks whether the PLAYER is standing in the Battle Pike or Pyramid
    // (3ds/tweaks.c:312), which is the wrong question about some other map, and
    // it is unnecessary here anyway: the index-encoded pseudo-species it exists
    // to protect live in gBattlePikeWildMonHeaders and
    // gBattlePyramidWildMonHeaders, separate arrays this file never reads.
    species = Ctr3dsMapSpecies(species);

    if (species == SPECIES_NONE || species >= NUM_SPECIES)
        return;

    for (u32 i = 0; i < sCount; i++)
        if (sSpecies[i] == species)
            return;

    sSpecies[sCount++] = species;
}

static void AddMonList(const struct WildPokemonInfo *info, u32 slots)
{
    if (info == NULL || info->wildPokemon == NULL)
        return;

    for (u32 i = 0; i < slots; i++)
        AddSpecies(info->wildPokemon[i].species);
}

// Every category of one header, in the order the player meets them: grass, then
// surfing, then rock smash, then fishing. Nothing labels the categories, so the
// order is what groups the list.
static void AddHeader(const struct WildPokemonHeader *header)
{
    AddMonList(header->landMonsInfo,      LAND_WILD_COUNT);
    AddMonList(header->waterMonsInfo,     WATER_WILD_COUNT);
    AddMonList(header->rockSmashMonsInfo, ROCK_WILD_COUNT);
    AddMonList(header->fishingMonsInfo,   FISH_WILD_COUNT);
}

// Whether index `i` is one of Altering Cave's nine headers, and if so whether it
// is the live one. This is GetCurrentMapWildMonHeaderId's own test
// (src/wild_encounter.c:310), which is static there, so it is reproduced rather
// than called. Without it the cave shows table 1 on all nine days.
static bool8 IsDeadAlteringCaveTable(u32 i, u8 mapGroup, u8 mapNum)
{
    u16 live;

    if (mapGroup != MAP_GROUP(MAP_ALTERING_CAVE) || mapNum != MAP_NUM(MAP_ALTERING_CAVE))
        return FALSE;

    live = VarGet(VAR_ALTERING_CAVE_WILD_SET);
    if (live >= NUM_ALTERING_CAVE_TABLES)
        live = 0;

    // The nine are consecutive and the first is the one a linear scan reaches,
    // so the live one is `live` entries past it. Walk back to that first index
    // rather than counting matches, which would depend on scan order.
    for (u32 first = i; first > 0; first--)
    {
        if (gWildMonHeaders[first - 1].mapGroup != mapGroup
         || gWildMonHeaders[first - 1].mapNum != mapNum)
            return i != first + live;
    }

    return i != live;
}

// Rebuilds sSpecies unless it already holds this exact place.
static void Ensure(u8 source, mapsec_u16_t mapSecId)
{
    u8 group = 0, num = 0;

    // VarGet and the location below both read the save block. Every entry point
    // into this file is gated on there being one (bottom_screen.c: sInGame for
    // draw and touch, SaveDataLive for the hash), so this is the belt to that
    // brace rather than an expected path.
    if (gSaveBlock1Ptr == NULL)
    {
        sCount = 0;
        sBuilt = FALSE;
        return;
    }

    if (source == UI_ENC_SRC_PLAYER)
    {
        group = gSaveBlock1Ptr->location.mapGroup;
        num   = gSaveBlock1Ptr->location.mapNum;
    }

    if (sBuilt
     && sBuiltSrc == source
     && sBuiltGroup == group
     && sBuiltNum == num
     && sBuiltMapSec == mapSecId)
        return;

    sCount = 0;
    sBuilt = TRUE;
    sBuiltSrc = source;
    sBuiltGroup = group;
    sBuiltNum = num;
    sBuiltMapSec = mapSecId;

    if (source == UI_ENC_SRC_PLAYER)
    {
        sTitleMapSec = gMapHeader.regionMapSectionId;

        // An indoor map with no mapsec of its own (MAPSEC_DYNAMIC, and anything
        // past the real ones) borrows the caption's, which is the outdoor place
        // the region map already resolved the player to.
        if (sTitleMapSec >= MAPSEC_NONE)
            sTitleMapSec = mapSecId;
    }
    else
    {
        sTitleMapSec = mapSecId;
    }

    for (u32 i = 0; gWildMonHeaders[i].mapGroup != MAP_GROUP(MAP_UNDEFINED); i++)
    {
        const struct WildPokemonHeader *header = &gWildMonHeaders[i];

        if (source == UI_ENC_SRC_PLAYER)
        {
            if (header->mapGroup != group || header->mapNum != num)
                continue;
        }
        else
        {
            if (mapSecId >= MAPSEC_NONE)
                break;

            if (Overworld_GetMapHeaderByGroupAndId(header->mapGroup,
                                                   header->mapNum)->regionMapSectionId
                != mapSecId)
                continue;
        }

        if (IsDeadAlteringCaveTable(i, header->mapGroup, header->mapNum))
            continue;

        AddHeader(header);
    }
}

// ---------------------------------------------------------------- drawing ---

static u32 PageCount(void)
{
    if (sCount == 0)
        return 1;

    return ((u32)sCount + UI_ENC_PER_PAGE - 1) / UI_ENC_PER_PAGE;
}

static void DrawCell(int cx, int ry, u16 species)
{
    u8 label[16];
    u32 state = DexState(species);

    // A ball only for caught, nothing for merely seen: the same three-state
    // readout the real dex list gives, in the same glyph and the same column
    // (tab_dex.c:326). Centred on the name's glyph row, which is what that
    // list's `y + 4` is.
    //
    // Ahead of the seen branch rather than inside it. Caught implies seen, so
    // this can only ever land on a row that also draws a name -- but
    // GetSetPokedexFlag exists precisely because the three seen mirrors can
    // disagree, and a readout that hides a caught mark to keep its own layout
    // tidy is the wrong way round.
    if (state & DEX_CAUGHT)
        UiPokeball(cx + CELL_BALL_X, ry + (UI_GLYPH_H - UI_BALL_H) / 2);

    if (!(state & DEX_SEEN))
    {
        // The shape and nothing else. Drawn in the frame's own shadow colour
        // rather than a fixed dark one, because the 20 window frames run
        // near-white to near-dark and a fixed silhouette vanishes against half
        // of them.
        UiMonIconSilhouette(cx, ry, species, 0, UiThemeShadow());
        UiText(cx + CELL_TEXT_X, ry, UiAscii(label, "----------", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
        return;
    }

    UiMonIcon(cx, ry, species, 0);
    UiText(cx + CELL_TEXT_X, ry, gSpeciesNames[species],
           UiThemeText(), UiThemeShadow());

    UiTypeIcon(cx + CELL_TEXT_X, ry + CELL_TYPE_Y, gSpeciesInfo[species].types[0]);

    // A single-typed mon carries the same type in both slots, so testing them
    // is what keeps it from drawing the same badge twice.
    if (gSpeciesInfo[species].types[1] != gSpeciesInfo[species].types[0])
        UiTypeIcon(cx + CELL_TYPE2_X, ry + CELL_TYPE_Y, gSpeciesInfo[species].types[1]);
}

static void DrawHeader(void)
{
    // Not MAP_NAME_LENGTH: GetMapNameGeneric's empty-name fallback fills 18
    // plus a terminator, which is the same reason tab_map.c sizes its own at 32.
    u8 name[32];
    u32 pages = PageCount();

    if (sTitleMapSec < MAPSEC_NONE)
    {
        GetMapNameGeneric(name, sTitleMapSec);
        UiText(HDR_MARGIN, HDR_Y, name, UiThemeText(), UiThemeShadow());
    }

    // Only when there is more than one, so a route that fits on a page says
    // nothing rather than "1/1". Single digits are enough: UI_ENC_MAX caps the
    // list at six pages.
    if (pages > 1)
    {
        char ascii[4];
        u8 label[8];

        ascii[0] = (char)('0' + sPage + 1);
        ascii[1] = '/';
        ascii[2] = (char)('0' + pages);
        ascii[3] = '\0';

        UiTextRight(CTR_BOTTOM_WIDTH - HDR_MARGIN, HDR_Y,
                    UiAscii(label, ascii, sizeof(label)),
                    UI_COL_DIM, UiThemeShadow());
    }
}

static void DrawControls(void)
{
    u8 label[8];
    u32 pages = PageCount();

    if (sPage > 0)
    {
        UiRect(PAGE_UP_X, BTN_Y, PAGE_W, BTN_H, UI_COL_DIM);
        UiArrow(PAGE_UP_X + (PAGE_W - UI_ARROW_W) / 2,
                BTN_Y + (BTN_H - UI_ARROW_H) / 2, TRUE, UI_COL_ACCENT);
    }

    if (sPage + 1 < pages)
    {
        UiRect(PAGE_DN_X, BTN_Y, PAGE_W, BTN_H, UI_COL_DIM);
        UiArrow(PAGE_DN_X + (PAGE_W - UI_ARROW_W) / 2,
                BTN_Y + (BTN_H - UI_ARROW_H) / 2, FALSE, UI_COL_ACCENT);
    }

    UiRect(BACK_X, BTN_Y, BACK_W, BTN_H, UI_COL_DIM);
    UiAscii(label, "BACK", sizeof(label));
    UiText(BACK_X + (BACK_W - UiTextWidth(label)) / 2,
           BTN_Y + (BTN_H - UI_GLYPH_H) / 2, label,
           UI_COL_ACCENT, UiThemeShadow());
}

void UiEncountersDraw(void)
{
    u32 first;

    Ensure(sSrc, sOpenMapSec);

    // The list is re-derived on every draw, so in PLAYER mode it follows the
    // player from room to room -- and can shrink under a page that was valid
    // when it was turned to.
    if (sPage >= PageCount())
        sPage = (u8)(PageCount() - 1);

    UiWindowFrame(0, 0, CTR_BOTTOM_WIDTH / 8, UI_CONTENT_H / 8);

    DrawHeader();

    if (sCount == 0)
    {
        u8 label[32];

        UiText(HDR_MARGIN, GRID_Y + ROW_H,
               UiAscii(label, "Nothing lives here.", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
        DrawControls();
        return;
    }

    first = (u32)sPage * UI_ENC_PER_PAGE;

    for (u32 i = 0; i < UI_ENC_PER_PAGE; i++)
    {
        if (first + i >= sCount)
            break;

        DrawCell(COL_X(i % GRID_COLS), GRID_Y + (int)(i / GRID_COLS) * ROW_H,
                 sSpecies[first + i]);
    }

    DrawControls();
}

// ------------------------------------------------------------------ shell ---

bool8 UiEncountersAvailable(u8 source, mapsec_u16_t mapSecId)
{
    Ensure(source, mapSecId);
    return sCount > 0;
}

void UiEncountersOpen(u8 source, mapsec_u16_t mapSecId)
{
    sOpen = TRUE;
    sSrc = source;
    sOpenMapSec = mapSecId;
    sPage = 0;
    UiMarkDirty();
}

void UiEncountersClose(void)
{
    if (!sOpen)
        return;

    sOpen = FALSE;
    UiMarkDirty();
}

bool8 UiEncountersIsOpen(void)
{
    return sOpen;
}

void UiEncountersTouch(const CtrTouchState *t)
{
    u32 pages;

    if (!t->justReleased)
        return;

    if (UiHit(t, BACK_X, BTN_Y, BACK_W, BTN_H))
    {
        UiEncountersClose();
        return;
    }

    pages = PageCount();

    // Both arrows carry the same test their drawing does, so a control that is
    // not on the screen cannot be tapped.
    if (sPage > 0 && UiHit(t, PAGE_UP_X, BTN_Y, PAGE_W, BTN_H))
    {
        sPage--;
        UiMarkDirty();
        return;
    }

    if (sPage + 1 < pages && UiHit(t, PAGE_DN_X, BTN_Y, PAGE_W, BTN_H))
    {
        sPage++;
        UiMarkDirty();
    }
}

// ----------------------------------------------------------- repaint key ----

u32 UiEncountersStateKey(void)
{
    u32 key;
    u32 first;

    if (!sOpen)
        return 0;

    Ensure(sSrc, sOpenMapSec);

    key = 1u | ((u32)sPage << 1);

    // Which place is being described. In PLAYER mode this is the only thing in
    // the whole hash that tracks the map itself: walking from Granite Cave 1F to
    // B1F moves neither the region map cursor nor the mapsec, so without this
    // the panel would keep showing 1F's list.
    if (sSrc == UI_ENC_SRC_PLAYER)
        key |= ((u32)sBuiltGroup << 4) | ((u32)sBuiltNum << 12);
    else
        key |= (u32)sOpenMapSec << 4;

    // What is actually on the page, and what the dex says about each of it. The
    // bottom screen is live during battle, so both transitions have to land with
    // nothing else moving: a mon MET while this panel is up turns from a
    // silhouette into a name, and one CAUGHT while it is up grows a ball. Both
    // bits of DexState are folded for that reason -- seen alone would leave the
    // ball a repaint behind.
    //
    // Eight species ids and their flags do not fit in what is left of the word,
    // so they are folded through a multiply instead of placed in bits. The slot
    // index goes into the value being folded: that is what makes each slot's
    // contribution distinct, and two contributions that cancel are the one
    // failure this hash exists to prevent (the note in UiMapStateKey).
    //
    // Sixteen GetSetPokedexFlag calls a frame at eight rows, and deliberately
    // not GetNationalPokedexCount, which the DEX tab can afford because counting
    // is what it shows. A key must be O(what is on screen).
    first = (u32)sPage * UI_ENC_PER_PAGE;

    for (u32 i = 0; i < UI_ENC_PER_PAGE && first + i < sCount; i++)
    {
        u32 v = (u32)sSpecies[first + i]
              | (DexState(sSpecies[first + i]) << 16)
              | ((u32)i << 18);

        key ^= v * 2654435761u;
    }

    return key;
}
