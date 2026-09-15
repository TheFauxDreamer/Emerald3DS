// The wild encounter list for a place. See view_encounters.h.
//
// This file only reads. The species come from gWildMonHeaders, the same table
// as the encounter generator uses. Thus the list agrees with what the player
// meets.
//
// Three facts about that table. If you miss one, the list looks correct but is
// wrong:
// - The four categories have different slot counts: 12, 5, 5 and 10. The
//   original MapHasSpecies reads fishing with LAND_WILD_COUNT, which is an
//   out-of-bounds bug. This file does not copy it.
// - Altering Cave has nine headers in sequence for one map. Only the one that
//   VAR_ALTERING_CAVE_WILD_SET names is live. A plain scan finds the first one.
// - With the randomizer on, the species in the table is not the species that
//   the player meets: CreateWildMon maps it. Apply the mapping before the
//   dedupe, because two table entries can map to one mon.

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
// The full content area, 40x24 tiles. The frame's interior is x 8..311 and y
// 8..183. Everything below fits in those 176px:
//
//    8      header      one glyph row, 15px
//    25     grid        4 rows of 33, ending at 157
//    160    controls    22px, ending at 182
#define HDR_Y        8
#define HDR_MARGIN   10

#define GRID_Y       25
#define ROW_H        33
#define GRID_ROWS    4
#define GRID_COLS    2
#define COL_W        152
#define COL_X(c)     (8 + (c) * COL_W)

#define UI_ENC_PER_PAGE (GRID_ROWS * GRID_COLS)

// Inside one cell: the 32x32 icon, a 12px gutter for the caught marker, then
// the name with the type badges under it. The cell's right edge is at cx+114.
// Thus column 2 ends at 274, inside the 311 interior edge. Column 1 ends at
// 122, clear of column 2's icon at 160. The name has 104px, and the longest
// species name is about 60px.
//
// The ball's column is always there, with or without a ball, so the names do
// not move down a page. The dex list does the same (tab_dex.c).
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
// The same 42x22 BACK as the DEX entry screen, in the same corner.
#define BACK_X       (CTR_BOTTOM_WIDTH - 50)
#define BACK_W       42

// ------------------------------------------------------------------ state ---
//
// A hard limit that comes from the data. The worst single map is Safari Zone
// Southeast, with 15 species. The worst mapsec, with all its maps, is the
// Safari Zone with 38. A tap cannot reach that mapsec, because it is not in the
// region map's 28x15 grid. The limit is above it anyway, so nothing drops.
// There are six pages at most, so the page counter is one digit.
#define UI_ENC_MAX   48

static u16   sSpecies[UI_ENC_MAX];
static u8    sCount;

// The place that sSpecies holds. A repaint then costs one comparison, not a
// scan of 124 headers.
static bool8 sBuilt;
static u8    sBuiltSrc;
static u8    sBuiltGroup, sBuiltNum;
static mapsec_u16_t sBuiltMapSec;

// The mapsec whose name is the title of the panel. In PLAYER mode, it is the
// current map's own mapsec, not the one the panel opened with. Thus a cave
// shows its own name, not the outdoor name that the region map gives.
static mapsec_u16_t sTitleMapSec;

static bool8 sOpen;
static u8    sSrc;
static mapsec_u16_t sOpenMapSec;
static u8    sPage;

// ------------------------------------------------------------- gathering ----

// The dex's three states for one species: nothing, seen, or caught.
//
// Both flags in one call, because every caller needs both. The bounds guard and
// the conversion to the national number are the same for either flag.
#define DEX_SEEN    (1u << 0)
#define DEX_CAUGHT  (1u << 1)

static u32 DexState(u16 species)
{
    u16 national;
    u32 state = 0;

    // SpeciesToNationalPokedexNum reads [species - 1] from a table of
    // NUM_SPECIES - 1 entries, with no bounds check of its own (src/pokemon.c).
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

    // Apply the randomizer's mapping here, not at display time. The dedupe
    // below then sees the mons that the player meets, because two table entries
    // can map to one.
    //
    // Use Ctr3dsMapSpecies, not Ctr3dsMapWildSpecies. The second one checks if
    // the player stands in the Battle Pike or Pyramid (3ds/tweaks.c), which is
    // the wrong question for another map. That check protects pseudo-species in
    // gBattlePikeWildMonHeaders and gBattlePyramidWildMonHeaders, which this
    // file never reads.
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

// Every category of one header, in the order that the player meets them: grass,
// surfing, rock smash, then fishing. Nothing labels the categories, so the
// order groups the list.
static void AddHeader(const struct WildPokemonHeader *header)
{
    AddMonList(header->landMonsInfo,      LAND_WILD_COUNT);
    AddMonList(header->waterMonsInfo,     WATER_WILD_COUNT);
    AddMonList(header->rockSmashMonsInfo, ROCK_WILD_COUNT);
    AddMonList(header->fishingMonsInfo,   FISH_WILD_COUNT);
}

// TRUE when index `i` is one of Altering Cave's nine headers, and tells if it
// is the live one. This copies the test in GetCurrentMapWildMonHeaderId
// (src/wild_encounter.c), which is static there. Without it, the cave shows
// table 1 on all nine days.
static bool8 IsDeadAlteringCaveTable(u32 i, u8 mapGroup, u8 mapNum)
{
    u16 live;

    if (mapGroup != MAP_GROUP(MAP_ALTERING_CAVE) || mapNum != MAP_NUM(MAP_ALTERING_CAVE))
        return FALSE;

    live = VarGet(VAR_ALTERING_CAVE_WILD_SET);
    if (live >= NUM_ALTERING_CAVE_TABLES)
        live = 0;

    // The nine headers are in sequence, and a linear scan reaches the first
    // one. Thus the live one is `live` entries after it. Go back to the first
    // index; do not count matches, which depend on the scan order.
    for (u32 first = i; first > 0; first--)
    {
        if (gWildMonHeaders[first - 1].mapGroup != mapGroup
         || gWildMonHeaders[first - 1].mapNum != mapNum)
            return i != first + live;
    }

    return i != live;
}

// Build sSpecies again, unless it already holds this place.
static void Ensure(u8 source, mapsec_u16_t mapSecId)
{
    u8 group = 0, num = 0;

    // VarGet and the location below read the save block. Every entry point into
    // this file checks that a save block exists (sInGame for draw and touch,
    // SaveDataLive for the hash). This is only a second guard.
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

        // An indoor map with no mapsec of its own (MAPSEC_DYNAMIC, or a value
        // past the real ones) uses the caption's mapsec. That is the outdoor
        // place that the region map already found for the player.
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

    // A ball for caught, nothing for seen, as in the real dex list: the same
    // glyph in the same column (tab_dex.c). It is centered on the name's glyph
    // row, which is that list's `y + 4`.
    //
    // It comes before the seen branch, not inside it. Caught means seen, so
    // this always lands on a row with a name. But the three seen mirrors can
    // disagree, and a caught mark must not hide.
    if (state & DEX_CAUGHT)
        UiPokeball(cx + CELL_BALL_X, ry + (UI_GLYPH_H - UI_BALL_H) / 2);

    if (!(state & DEX_SEEN))
    {
        // Only the shape. It uses the frame's shadow color, not a fixed dark
        // color. The 20 window frames go from near white to near dark, and a
        // fixed color disappears on half of them.
        UiMonIconSilhouette(cx, ry, species, 0, UiThemeShadow());
        UiText(cx + CELL_TEXT_X, ry, UiAscii(label, "----------", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
        return;
    }

    UiMonIcon(cx, ry, species, 0);
    UiText(cx + CELL_TEXT_X, ry, gSpeciesNames[species],
           UiThemeText(), UiThemeShadow());

    UiTypeIcon(cx + CELL_TEXT_X, ry + CELL_TYPE_Y, gSpeciesInfo[species].types[0]);

    // A mon with one type has the same type in both slots. Test for that, so
    // the badge does not draw twice.
    if (gSpeciesInfo[species].types[1] != gSpeciesInfo[species].types[0])
        UiTypeIcon(cx + CELL_TYPE2_X, ry + CELL_TYPE_Y, gSpeciesInfo[species].types[1]);
}

static void DrawHeader(void)
{
    // Not MAP_NAME_LENGTH: GetMapNameGeneric's empty-name fallback writes 18
    // and a terminator. The MAP tab (tab_map.c) uses 32 for the same reason.
    u8 name[32];
    u32 pages = PageCount();

    if (sTitleMapSec < MAPSEC_NONE)
    {
        GetMapNameGeneric(name, sTitleMapSec);
        UiText(HDR_MARGIN, HDR_Y, name, UiThemeText(), UiThemeShadow());
    }

    // Only when there is more than one page, so a short route does not show
    // "1/1". One digit is enough: UI_ENC_MAX limits the list to six pages.
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

    // The list is built again on each draw. In PLAYER mode, it follows the
    // player from room to room. It can then become shorter than the current
    // page.
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

    // Both arrows use the same test as their drawing, so a control that does
    // not show cannot work.
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

    // The place that the panel describes. In PLAYER mode, this is the only
    // value in the hash that tracks the map. A walk from Granite Cave 1F to B1F
    // moves neither the region map cursor nor the mapsec. Without this, the
    // panel keeps 1F's list.
    if (sSrc == UI_ENC_SRC_PLAYER)
        key |= ((u32)sBuiltGroup << 4) | ((u32)sBuiltNum << 12);
    else
        key |= (u32)sOpenMapSec << 4;

    // What the page shows, and what the dex says about each mon. The bottom
    // screen is live during a battle, so both changes must show with nothing
    // else changing. A mon that the player meets changes from a silhouette to a
    // name. A mon that the player catches gets a ball. Thus both bits of
    // DexState go into the key.
    //
    // Eight species ids and their flags do not fit in the rest of the word, so
    // fold them with a multiply. The slot index goes into each value, so the
    // values of two slots cannot cancel (see the note in UiMapStateKey).
    //
    // This makes 16 GetSetPokedexFlag calls for eight rows. Do not use
    // GetNationalPokedexCount here. A key must cost O(what is on the screen).
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
