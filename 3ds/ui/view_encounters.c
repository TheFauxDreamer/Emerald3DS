// The wild encounter list for a place. See view_encounters.h.
//
// This file only reads. The species come from gWildMonHeaders, the same table
// as the encounter generator uses. Thus the list agrees with what the player
// meets. There is one list for each way to meet a mon (grass, surfing, Rock
// Smash, each rod), with the level range and the chance of each mon.
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
//   merge, because two table entries can map to one mon.

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
#include "ui_view.h"
#include "view_encounters.h"

// ---------------------------------------------------------------- layout ---
//
// The full content area, 40x24 tiles. The frame's interior is x 8..311 and y
// 8..183. Everything below fits in those 176px:
//
//    8      header      one glyph row, 15px: the place, the method, the page
//    25     chips       one per method this place has, 17px, ending at 42
//    46     grid        3 rows of 37, ending at 157
//    160    controls    22px, ending at 182
#define HDR_Y        8
#define HDR_MARGIN   10

// The method chips. Six fit: 6 * 48 + 5 * 2 = 298 of the 304px interior. The
// labels are short ("SMASH" is the widest, 30px) because the header names the
// method in full.
#define CHIP_Y       25
#define CHIP_H       17
#define CHIP_W       48
#define CHIP_GAP     2

#define GRID_Y       46
#define ROW_H        37
#define GRID_ROWS    3
#define GRID_COLS    2
#define COL_W        152
#define COL_X(c)     (8 + (c) * COL_W)

#define UI_ENC_PER_PAGE (GRID_ROWS * GRID_COLS)

// Inside one cell: the 32x32 icon, a 12px gutter for the caught marker, then
// two lines. Line 1 is the name, with the level right-aligned. Line 2 is the
// type badges, with the chance right-aligned. The right edge is cx+146, so
// column 2 ends at 306, inside the 311 interior edge, and column 1 ends at 154,
// clear of column 2's icon at 160.
//
// The name can meet the level: 10 letters and "Lv12-15" leave no gap. The name
// is cut to fit (UiTextClipped), so the level always shows whole.
//
// The ball's column is always there, with or without a ball, so the names do
// not move down a page. The dex list does the same (tab_dex.c).
#define CELL_ICON_W  32
#define CELL_BALL_X  36
#define CELL_TEXT_X  48
#define CELL_TYPE_Y  16
#define CELL_TYPE2_X (CELL_TEXT_X + UI_TYPE_ICON_W + 2)
#define CELL_RIGHT   146
#define CELL_NAME_GAP 4

#define BTN_Y        160
#define BTN_H        22
#define PAGE_W       52
#define PAGE_UP_X    10
#define PAGE_DN_X    70
// The same 42x22 BACK as the DEX entry screen, in the same corner.
#define BACK_X       (CTR_BOTTOM_WIDTH - 50)
#define BACK_W       42

// ---------------------------------------------------------------- methods ---
//
// How the player meets a mon. The game has four tables per map, and fishing is
// three rods in one table: slots 0-1 are the OLD ROD, 2-4 the GOOD ROD and 5-9
// the SUPER ROD (ChooseWildMonIndex_Fishing, src/wild_encounter.c).
enum
{
    ENC_LAND,
    ENC_SURF,
    ENC_ROCK,
    ENC_OLD_ROD,
    ENC_GOOD_ROD,
    ENC_SUPER_ROD,
    ENC_METHOD_COUNT,
};

static const char *const sChipLabel[ENC_METHOD_COUNT] = {
    "LAND", "SURF", "SMASH", "OLD", "GOOD", "SUPER",
};

static const char *const sMethodName[ENC_METHOD_COUNT] = {
    "LAND", "SURFING", "ROCK SMASH", "OLD ROD", "GOOD ROD", "SUPER ROD",
};

// The chance of each slot, in percent. These are "encounter_rates" in
// src/data/wild_encounters.json. The build turns them into the
// ENCOUNTER_CHANCE_* macros, but only inside src/wild_encounter.c, so they are
// copied here. Within one rod the fishing slots add to 100, as each table does.
static const u8 sLandRate[LAND_WILD_COUNT]  = { 20, 20, 10, 10, 10, 10, 5, 5, 4, 4, 1, 1 };
static const u8 sWaterRate[WATER_WILD_COUNT] = { 60, 30, 5, 4, 1 };
static const u8 sRockRate[ROCK_WILD_COUNT]  = { 60, 30, 5, 4, 1 };
static const u8 sFishRate[FISH_WILD_COUNT]  = { 70, 30, 60, 20, 20, 40, 40, 15, 4, 1 };

// ------------------------------------------------------------------ state ---
//
// A limit for each method. One map has at most 12 species for a method (the
// land table). A tapped mapsec merges its maps, and none comes near this. A
// full method has six pages, so the page counter is one digit.
#define UI_ENC_MAX   32

struct EncEntry
{
    u16 species;
    u8  minLevel;
    u8  maxLevel;
    u16 rate;       // percent; more than 100 only when maps are merged
};

static struct EncEntry sList[ENC_METHOD_COUNT][UI_ENC_MAX];
static u8    sCount[ENC_METHOD_COUNT];

// How many headers gave each method entries. A mapsec with two maps that both
// have grass gives two, and the chances of two maps do not add up to anything
// the player meets. The chance then does not show.
static u8    sHeaders[ENC_METHOD_COUNT];

// The place that sList holds. A repaint then costs one comparison, not a scan
// of 124 headers.
static bool8 sBuilt;
static u8    sBuiltSrc;
static u8    sBuiltGroup, sBuiltNum;
static mapsec_u16_t sBuiltMapSec;

// The mapsec whose name is the title of the panel. In PLAYER mode, it is the
// current map's own mapsec, not the one the panel opened with. Thus a cave
// shows its own name, not the outdoor name that the region map gives.
static mapsec_u16_t sTitleMapSec;

static u8    sSrc;
static mapsec_u16_t sOpenMapSec;
static u8    sMethod;
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

// One slot of one table: merge it into the method's list.
static void AddSlot(u8 method, const struct WildPokemon *mon, u8 rate)
{
    struct EncEntry *list = sList[method];
    u16 species = mon->species;

    if (species == SPECIES_NONE || species >= NUM_SPECIES)
        return;

    // Apply the randomizer's mapping here, not at display time. The merge
    // below then sees the mons that the player meets, because two table
    // entries can map to one.
    //
    // Use Ctr3dsMapSpecies, not Ctr3dsMapWildSpecies. The second one checks if
    // the player stands in the Battle Pike or Pyramid (3ds/tweaks.c), which is
    // the wrong question for another map. That check protects pseudo-species in
    // gBattlePikeWildMonHeaders and gBattlePyramidWildMonHeaders, which this
    // file never reads.
    species = Ctr3dsMapSpecies(species);

    if (species == SPECIES_NONE || species >= NUM_SPECIES)
        return;

    for (u32 i = 0; i < sCount[method]; i++)
    {
        if (list[i].species != species)
            continue;

        if (mon->minLevel < list[i].minLevel)
            list[i].minLevel = mon->minLevel;
        if (mon->maxLevel > list[i].maxLevel)
            list[i].maxLevel = mon->maxLevel;
        list[i].rate += rate;
        return;
    }

    if (sCount[method] >= UI_ENC_MAX)
        return;

    list[sCount[method]].species  = species;
    list[sCount[method]].minLevel = mon->minLevel;
    list[sCount[method]].maxLevel = mon->maxLevel;
    list[sCount[method]].rate     = rate;
    sCount[method]++;
}

// Slots [first, first + n) of one table into one method.
static void AddSlots(u8 method, const struct WildPokemonInfo *info,
                     u32 first, u32 n, const u8 *rates)
{
    if (info == NULL || info->wildPokemon == NULL)
        return;

    for (u32 i = first; i < first + n; i++)
        AddSlot(method, &info->wildPokemon[i], rates[i]);

    sHeaders[method]++;
}

// Every table of one header. The four categories have different slot counts:
// 12, 5, 5 and 10. The original MapHasSpecies reads fishing with
// LAND_WILD_COUNT, which is an out-of-bounds bug. This file does not copy it.
static void AddHeader(const struct WildPokemonHeader *header)
{
    AddSlots(ENC_LAND, header->landMonsInfo,      0, LAND_WILD_COUNT,  sLandRate);
    AddSlots(ENC_SURF, header->waterMonsInfo,     0, WATER_WILD_COUNT, sWaterRate);
    AddSlots(ENC_ROCK, header->rockSmashMonsInfo, 0, ROCK_WILD_COUNT,  sRockRate);
    AddSlots(ENC_OLD_ROD,   header->fishingMonsInfo, 0, 2, sFishRate);
    AddSlots(ENC_GOOD_ROD,  header->fishingMonsInfo, 2, 3, sFishRate);
    AddSlots(ENC_SUPER_ROD, header->fishingMonsInfo, 5, 5, sFishRate);
}

// Most common first, as the player meets them. Insertion sort: the lists are
// short, and it keeps the table order for equal chances.
static void SortByRate(u8 method)
{
    struct EncEntry *list = sList[method];

    for (u32 i = 1; i < sCount[method]; i++)
    {
        struct EncEntry e = list[i];
        u32 j = i;

        while (j > 0 && list[j - 1].rate < e.rate)
        {
            list[j] = list[j - 1];
            j--;
        }
        list[j] = e;
    }
}

static u32 TotalCount(void)
{
    u32 n = 0;

    for (u32 m = 0; m < ENC_METHOD_COUNT; m++)
        n += sCount[m];
    return n;
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

// Build the lists again, unless they already hold this place.
static void Ensure(u8 source, mapsec_u16_t mapSecId)
{
    u8 group = 0, num = 0;

    // VarGet and the location below read the save block. Every entry point into
    // this file checks that a save block exists (sInGame for draw and touch,
    // SaveDataLive for the hash). This is only a second guard.
    if (gSaveBlock1Ptr == NULL)
    {
        for (u32 m = 0; m < ENC_METHOD_COUNT; m++)
            sCount[m] = 0;
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

    for (u32 m = 0; m < ENC_METHOD_COUNT; m++)
    {
        sCount[m] = 0;
        sHeaders[m] = 0;
    }
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

    for (u32 m = 0; m < ENC_METHOD_COUNT; m++)
        SortByRate((u8)m);
}

// ---------------------------------------------------------------- drawing ---

// The first method that has entries, or ENC_METHOD_COUNT if none has.
static u8 FirstMethod(void)
{
    for (u32 m = 0; m < ENC_METHOD_COUNT; m++)
        if (sCount[m] > 0)
            return (u8)m;
    return ENC_METHOD_COUNT;
}

// The list is built again when the place changes. In PLAYER mode it follows
// the player from room to room, and the new room may lack the chosen method or
// have fewer pages. Fix both before anything reads them.
static void KeepSelectionValid(void)
{
    if (sMethod >= ENC_METHOD_COUNT || sCount[sMethod] == 0)
    {
        sMethod = FirstMethod();
        sPage = 0;
    }
}

static u32 PageCount(void)
{
    if (sMethod >= ENC_METHOD_COUNT || sCount[sMethod] == 0)
        return 1;

    return ((u32)sCount[sMethod] + UI_ENC_PER_PAGE - 1) / UI_ENC_PER_PAGE;
}

// ASCII digits of `v` at `p`. Returns the end.
static char *PutNum(char *p, u32 v)
{
    char tmp[6];
    int n = 0;

    do
    {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0 && n < (int)sizeof(tmp));

    while (n > 0)
        *p++ = tmp[--n];
    return p;
}

// The x of each present method's chip, left to right, centred in the interior.
// The draw and the hit test both use this. Returns how many chips there are.
static u32 ChipLayout(u8 *methods, int *xs)
{
    u32 n = 0;
    int total, x;

    for (u32 m = 0; m < ENC_METHOD_COUNT; m++)
        if (sCount[m] > 0)
            methods[n++] = (u8)m;

    if (n == 0)
        return 0;

    total = (int)n * CHIP_W + ((int)n - 1) * CHIP_GAP;
    x = 8 + (CTR_BOTTOM_WIDTH - 16 - total) / 2;

    for (u32 i = 0; i < n; i++)
        xs[i] = x + (int)i * (CHIP_W + CHIP_GAP);

    return n;
}

// The same shape as the EXTRA and MAP buttons: a 1px border, and a doubled
// accent inset on the one that is chosen.
static void DrawChips(void)
{
    u8 methods[ENC_METHOD_COUNT];
    int xs[ENC_METHOD_COUNT];
    u32 n = ChipLayout(methods, xs);
    u8 label[8];

    for (u32 i = 0; i < n; i++)
    {
        bool8 on = (methods[i] == sMethod);

        UiRect(xs[i], CHIP_Y, CHIP_W, CHIP_H, UI_COL_DIM);
        if (on)
        {
            UiRect(xs[i] + 2, CHIP_Y + 2, CHIP_W - 4, CHIP_H - 4, UI_COL_ACCENT);
            UiRect(xs[i] + 3, CHIP_Y + 3, CHIP_W - 6, CHIP_H - 6, UI_COL_ACCENT);
        }

        UiAscii(label, sChipLabel[methods[i]], sizeof(label));
        UiText(xs[i] + (CHIP_W - UiTextWidth(label)) / 2,
               CHIP_Y + (CHIP_H - UI_GLYPH_H) / 2 + 1, label,
               on ? UI_COL_ACCENT : UiThemeText(), UiThemeShadow());
    }
}

static void DrawCell(int cx, int ry, const struct EncEntry *e, bool8 showRate)
{
    u8 label[16];
    char ascii[12];
    char *p;
    u16 species = e->species;
    u32 state = DexState(species);
    int levelW, nameW;

    // The level: "Lv12-15", or "Lv5" when the table fixes it.
    p = ascii;
    *p++ = 'L';
    *p++ = 'v';
    p = PutNum(p, e->minLevel);
    if (e->maxLevel != e->minLevel)
    {
        *p++ = '-';
        p = PutNum(p, e->maxLevel);
    }
    *p = '\0';
    UiAscii(label, ascii, sizeof(label));
    levelW = UiTextRight(cx + CELL_RIGHT, ry, label, UI_COL_DIM, UiThemeShadow());
    nameW = CELL_RIGHT - levelW - CELL_NAME_GAP - CELL_TEXT_X;

    // The chance. It does not spoil anything, so an unseen mon shows it too.
    if (showRate)
    {
        p = PutNum(ascii, e->rate);
        *p++ = '%';
        *p = '\0';
        UiTextRight(cx + CELL_RIGHT, ry + CELL_TYPE_Y, UiAscii(label, ascii, sizeof(label)),
                    UI_COL_DIM, UiThemeShadow());
    }

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
        UiTextClipped(cx + CELL_TEXT_X, ry, nameW,
                      UiAscii(label, "----------", sizeof(label)),
                      UI_COL_DIM, UiThemeShadow());
        return;
    }

    UiMonIcon(cx, ry, species, 0);
    UiTextClipped(cx + CELL_TEXT_X, ry, nameW, gSpeciesNames[species],
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
    u8 label[24];
    char ascii[24];
    char *p = ascii;
    u32 pages = PageCount();
    int right;

    // The method in full, then the page when there is more than one. A short
    // route does not show "1/1". One digit is enough: UI_ENC_MAX limits a
    // method to six pages.
    if (sMethod < ENC_METHOD_COUNT)
    {
        for (const char *m = sMethodName[sMethod]; *m != '\0'; m++)
            *p++ = *m;

        if (pages > 1)
        {
            *p++ = ' ';
            *p++ = ' ';
            *p++ = (char)('0' + sPage + 1);
            *p++ = '/';
            *p++ = (char)('0' + pages);
        }
    }
    *p = '\0';

    right = UiTextRight(CTR_BOTTOM_WIDTH - HDR_MARGIN, HDR_Y,
                        UiAscii(label, ascii, sizeof(label)),
                        UI_COL_DIM, UiThemeShadow());

    // The place name, cut before the method if both are long.
    if (sTitleMapSec < MAPSEC_NONE)
    {
        GetMapNameGeneric(name, sTitleMapSec);
        UiTextClipped(HDR_MARGIN, HDR_Y,
                      CTR_BOTTOM_WIDTH - 2 * HDR_MARGIN - right - 8,
                      name, UiThemeText(), UiThemeShadow());
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
    KeepSelectionValid();

    if (sPage >= PageCount())
        sPage = (u8)(PageCount() - 1);

    UiWindowFrame(0, 0, CTR_BOTTOM_WIDTH / 8, UI_CONTENT_H / 8);

    DrawHeader();

    if (sMethod >= ENC_METHOD_COUNT)
    {
        u8 label[32];

        UiText(HDR_MARGIN, GRID_Y + ROW_H,
               UiAscii(label, "Nothing lives here.", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
        DrawControls();
        return;
    }

    DrawChips();

    first = (u32)sPage * UI_ENC_PER_PAGE;

    for (u32 i = 0; i < UI_ENC_PER_PAGE; i++)
    {
        if (first + i >= sCount[sMethod])
            break;

        DrawCell(COL_X(i % GRID_COLS), GRID_Y + (int)(i / GRID_COLS) * ROW_H,
                 &sList[sMethod][first + i], sHeaders[sMethod] == 1);
    }

    DrawControls();
}

// ------------------------------------------------------------------ shell ---

bool8 UiEncountersAvailable(u8 source, mapsec_u16_t mapSecId)
{
    Ensure(source, mapSecId);
    return TotalCount() > 0;
}

// Open is UI_VIEW_MAP_ENCOUNTERS on the view stack, so a tab switch closes the
// list (ui_view.h). The place and the method stay here: the stack arg has no
// room for both, and they are set again on every open.
void UiEncountersOpen(u8 source, mapsec_u16_t mapSecId)
{
    sSrc = source;
    sOpenMapSec = mapSecId;
    sPage = 0;

    // The first method this place has. Ensure fills the lists for it now, so
    // the choice is made against the place that opens.
    Ensure(source, mapSecId);
    sMethod = FirstMethod();
    UiViewPush(UI_VIEW_MAP_ENCOUNTERS, 0);
}

void UiEncountersClose(void)
{
    if (UiViewTop() == UI_VIEW_MAP_ENCOUNTERS)
        UiViewPop();
}

bool8 UiEncountersIsOpen(void)
{
    return UiViewIsOpen(UI_VIEW_MAP_ENCOUNTERS);
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

    Ensure(sSrc, sOpenMapSec);
    KeepSelectionValid();

    // The chips. The same layout as the draw, so a chip that does not show
    // cannot work.
    {
        u8 methods[ENC_METHOD_COUNT];
        int xs[ENC_METHOD_COUNT];
        u32 n = ChipLayout(methods, xs);

        for (u32 i = 0; i < n; i++)
        {
            if (!UiHit(t, xs[i], CHIP_Y, CHIP_W, CHIP_H))
                continue;

            if (methods[i] != sMethod)
            {
                sMethod = methods[i];
                sPage = 0;
                UiMarkDirty();
            }
            return;
        }
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

    if (!UiEncountersIsOpen())
        return 0;

    Ensure(sSrc, sOpenMapSec);
    KeepSelectionValid();

    // The page uses bits 1-3 (six pages at most) and the method bits 20-22,
    // clear of the place below.
    key = 1u | ((u32)sPage << 1) | ((u32)sMethod << 20);

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
    // Six species ids and their flags do not fit in the rest of the word, so
    // fold them with a multiply. The slot index goes into each value, so the
    // values of two slots cannot cancel (see the note in UiMapStateKey).
    //
    // This makes 12 GetSetPokedexFlag calls for six rows. Do not use
    // GetNationalPokedexCount here. A key must cost O(what is on the screen).
    if (sMethod >= ENC_METHOD_COUNT)
        return key;

    first = (u32)sPage * UI_ENC_PER_PAGE;

    for (u32 i = 0; i < UI_ENC_PER_PAGE && first + i < sCount[sMethod]; i++)
    {
        u16 species = sList[sMethod][first + i].species;
        u32 v = (u32)species
              | (DexState(species) << 16)
              | ((u32)i << 18);

        key ^= v * 2654435761u;
    }

    return key;
}
