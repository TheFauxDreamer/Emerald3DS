// DEX tab: seen/caught counts, a browsable list, and the entry text.
//
// Reachable only once FLAG_SYS_POKEDEX_GET is set, mirroring the start menu
// (src/start_menu.c BuildNormalStartMenu). Everything shown comes from the
// game's own accessors, so it agrees with Emerald's own Pokedex by
// construction. Nothing here writes game state: GetSetPokedexFlag is only ever
// called with the FLAG_GET_* cases.

#include "global.h"
#include "pokedex.h"
#include "pokemon.h"
#include "data.h"
#include "event_data.h"
#include "constants/flags.h"
#include "constants/pokedex.h"
#include "constants/species.h"

#include "../bridge.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"

// Defined in the game's data; only ever declared extern (see
// src/international_string_util.c, which does the same).
extern const struct PokedexEntry gPokedexEntries[];

#define LIST_Y        30
#define ROW_H         26
#define VISIBLE_ROWS  5
#define SCROLL_W      28

static u16   sScroll;
static u16   sSelected;        // national dex number, 0 when the list is showing
static bool8 sDetailOpen;

static bool8 NationalMode(void)
{
    return gSaveBlock2Ptr->pokedex.mode == DEX_MODE_NATIONAL
        && FlagGet(FLAG_SYS_NATIONAL_DEX);
}

static u16 DexLength(void)
{
    return NationalMode() ? NATIONAL_DEX_COUNT : HOENN_DEX_COUNT;
}

// Row n of the list is dex entry n+1, in whichever order the player's Pokedex
// is currently set to. Returns the NATIONAL number, which is what every
// accessor below is keyed on.
static u16 RowToNationalNum(u16 row)
{
    u16 n = row + 1;

    return NationalMode() ? n : HoennToNationalOrder(n);
}

static void DrawHeader(void)
{
    u8 label[16];
    u16 seen, caught;

    if (NationalMode())
    {
        seen   = GetNationalPokedexCount(FLAG_GET_SEEN);
        caught = GetNationalPokedexCount(FLAG_GET_CAUGHT);
    }
    else
    {
        seen   = GetHoennPokedexCount(FLAG_GET_SEEN);
        caught = GetHoennPokedexCount(FLAG_GET_CAUGHT);
    }

    UiText(12, 6, UiAscii(label, "SEEN", sizeof(label)), UI_COL_DIM, UiThemeShadow());
    UiNum(60, 6, (s32)seen, UiThemeText(), UiThemeShadow());

    UiText(140, 6, UiAscii(label, "CAUGHT", sizeof(label)), UI_COL_DIM, UiThemeShadow());
    UiNum(206, 6, (s32)caught, UiThemeText(), UiThemeShadow());
}

static void DrawList(void)
{
    u16 len = DexLength();
    u8 label[16];

    for (u32 row = 0; row < VISIBLE_ROWS; row++)
    {
        u16 index = sScroll + (u16)row;
        int y = LIST_Y + (int)row * ROW_H;
        u16 national, species;
        bool8 seen, caught;

        if (index >= len)
            break;

        national = RowToNationalNum(index);
        seen     = GetSetPokedexFlag(national, FLAG_GET_SEEN) != 0;
        caught   = GetSetPokedexFlag(national, FLAG_GET_CAUGHT) != 0;

        // The displayed number follows the current mode, matching the game.
        UiNumRight(44, y, (s32)(index + 1), UI_COL_DIM, UiThemeShadow());

        if (!seen)
        {
            // An unseen entry stays anonymous, exactly as in the real Pokedex.
            UiText(56, y, UiAscii(label, "----------", sizeof(label)),
                   UI_COL_DIM, UiThemeShadow());
            continue;
        }

        species = NationalPokedexNumToSpecies(national);
        UiText(56, y, gSpeciesNames[species], UiThemeText(), UiThemeShadow());

        if (caught)
            UiFillRect(CTR_BOTTOM_WIDTH - SCROLL_W - 18, y + 3, 8, 8,
                       UI_COL_HP_HIGH);
    }

    if (sScroll > 0)
        UiText(CTR_BOTTOM_WIDTH - SCROLL_W, LIST_Y,
               UiAscii(label, "UP", sizeof(label)), UI_COL_ACCENT, UiThemeShadow());

    if (sScroll + VISIBLE_ROWS < len)
        UiText(CTR_BOTTOM_WIDTH - SCROLL_W, LIST_Y + (VISIBLE_ROWS - 1) * ROW_H,
               UiAscii(label, "DN", sizeof(label)), UI_COL_ACCENT, UiThemeShadow());
}

static void DrawDetail(void)
{
    u16 species = NationalPokedexNumToSpecies(sSelected);
    const struct PokedexEntry *entry = &gPokedexEntries[sSelected];
    u8 label[16];

    UiText(12, 6, gSpeciesNames[species], UiThemeText(), UiThemeShadow());
    UiText(12, 24, entry->categoryName, UI_COL_DIM, UiThemeShadow());

    // Stored in decimetres and hectograms; shown as the raw game values rather
    // than inventing a conversion the game never displays.
    UiText(12, 44, UiAscii(label, "HT", sizeof(label)), UI_COL_DIM, UiThemeShadow());
    UiNum(44, 44, (s32)entry->height, UiThemeText(), UiThemeShadow());
    UiText(110, 44, UiAscii(label, "WT", sizeof(label)), UI_COL_DIM, UiThemeShadow());
    UiNum(142, 44, (s32)entry->weight, UiThemeText(), UiThemeShadow());

    // The description carries its own line breaks, which UiText honours.
    if (entry->description != NULL)
        UiText(12, 70, entry->description, UiThemeText(), UiThemeShadow());

    UiRect(CTR_BOTTOM_WIDTH - 46, 6, 38, 22, UI_COL_DIM);
    UiText(CTR_BOTTOM_WIDTH - 40, 10, UiAscii(label, "BACK", sizeof(label)),
           UI_COL_ACCENT, UiThemeShadow());
}

// Seen/caught can change without the party changing (seeing a wild mon), so the
// shell needs this in its repaint hash. Counting walks the whole dex, so it is
// only worth doing while this tab is the one on screen.
u32 UiDexStateKey(void)
{
    if (NationalMode())
        return ((u32)GetNationalPokedexCount(FLAG_GET_SEEN) << 16)
             |  (u32)GetNationalPokedexCount(FLAG_GET_CAUGHT);

    return ((u32)GetHoennPokedexCount(FLAG_GET_SEEN) << 16)
         |  (u32)GetHoennPokedexCount(FLAG_GET_CAUGHT);
}

void UiDexDraw(void)
{
    UiWindowFrame(0, 0, CTR_BOTTOM_WIDTH / 8, UI_CONTENT_H / 8);

    if (sDetailOpen)
    {
        DrawDetail();
        return;
    }

    DrawHeader();
    DrawList();
}

void UiDexTouch(const CtrTouchState *t)
{
    u16 len;

    if (!t->justReleased)
        return;

    if (sDetailOpen)
    {
        if (UiHit(t, CTR_BOTTOM_WIDTH - 46, 6, 38, 22))
        {
            sDetailOpen = FALSE;
            UiMarkDirty();
        }
        return;
    }

    len = DexLength();

    if (t->x >= CTR_BOTTOM_WIDTH - SCROLL_W && t->y >= LIST_Y)
    {
        if (t->y < LIST_Y + ROW_H && sScroll > 0)
            sScroll--;
        else if (sScroll + VISIBLE_ROWS < len)
            sScroll++;
        else
            return;

        UiMarkDirty();
        return;
    }

    if (t->y >= LIST_Y && t->y < LIST_Y + VISIBLE_ROWS * ROW_H)
    {
        u16 index = sScroll + (u16)((t->y - LIST_Y) / ROW_H);
        u16 national;

        if (index >= len)
            return;

        national = RowToNationalNum(index);

        // Nothing to show for an entry the player has never seen.
        if (!GetSetPokedexFlag(national, FLAG_GET_SEEN))
            return;

        sSelected = national;
        sDetailOpen = TRUE;
        UiMarkDirty();
    }
}
