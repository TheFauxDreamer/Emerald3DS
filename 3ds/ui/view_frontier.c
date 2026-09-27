// FRONTIER: the Battle Frontier at a glance, as the Frontier Pass shows it.
// See view_home.h.
//
// The Battle Points, then each facility with its silver and gold symbols and
// its best record over all its modes. All of them are save values. The
// game's own records windows (src/frontier_util.c) print to a window, so they
// are not called.

#include "global.h"
#include "event_data.h"               // FlagGet
#include "constants/battle_frontier.h"
#include "constants/flags.h"

#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "view_home.h"

// The BP line, then one row for each facility.
#define BP_Y         UI_PAGE_TOP
#define ROW_Y(i)     (UI_PAGE_TOP + 24 + (i) * 18)
#define NAME_X       UI_PAGE_LEFT
#define DOT_X        110
#define DOT          8
#define NUM_R        230
#define UNIT_X       236

static const char *const sFacilityName[NUM_FRONTIER_FACILITIES] =
{
    [FRONTIER_FACILITY_TOWER]   = "BATTLE TOWER",
    [FRONTIER_FACILITY_DOME]    = "BATTLE DOME",
    [FRONTIER_FACILITY_PALACE]  = "BATTLE PALACE",
    [FRONTIER_FACILITY_ARENA]   = "BATTLE ARENA",
    [FRONTIER_FACILITY_FACTORY] = "BATTLE FACTORY",
    [FRONTIER_FACILITY_PIKE]    = "BATTLE PIKE",
    [FRONTIER_FACILITY_PYRAMID] = "BATTLE PYRAMID",
};

bool8 UiFrontierReached(void)
{
    return FlagGet(FLAG_LANDMARK_BATTLE_FRONTIER);
}

// The largest of `n` records, as the game caps them for its records windows.
static u16 Best(const u16 *records, u32 n)
{
    u16 best = 0;

    for (u32 i = 0; i < n; i++)
        if (records[i] > best)
            best = records[i];

    return best > MAX_STREAK ? MAX_STREAK : best;
}

// A facility's best record over all its modes, from its record fields.
static u16 FacilityRecord(u8 facility)
{
    const struct BattleFrontier *f = &gSaveBlock2Ptr->frontier;

    switch (facility)
    {
    case FRONTIER_FACILITY_TOWER:
        return Best(&f->towerRecordWinStreaks[0][0], ARRAY_COUNT(f->towerRecordWinStreaks) * FRONTIER_LVL_MODE_COUNT);
    case FRONTIER_FACILITY_DOME:
        return Best(&f->domeRecordWinStreaks[0][0], ARRAY_COUNT(f->domeRecordWinStreaks) * FRONTIER_LVL_MODE_COUNT);
    case FRONTIER_FACILITY_PALACE:
        return Best(&f->palaceRecordWinStreaks[0][0], ARRAY_COUNT(f->palaceRecordWinStreaks) * FRONTIER_LVL_MODE_COUNT);
    case FRONTIER_FACILITY_ARENA:
        return Best(f->arenaRecordStreaks, FRONTIER_LVL_MODE_COUNT);
    case FRONTIER_FACILITY_FACTORY:
        return Best(&f->factoryRecordWinStreaks[0][0], ARRAY_COUNT(f->factoryRecordWinStreaks) * FRONTIER_LVL_MODE_COUNT);
    case FRONTIER_FACILITY_PIKE:
        return Best(f->pikeRecordStreaks, FRONTIER_LVL_MODE_COUNT);
    default:
        return Best(f->pyramidRecordStreaks, FRONTIER_LVL_MODE_COUNT);
    }
}

// The symbol flags are in facility order, silver then gold.
static bool8 HasSymbol(u8 facility, bool8 gold)
{
    return FlagGet(FLAG_SYS_TOWER_SILVER + 2 * facility + (gold ? 1 : 0));
}

static void DrawDot(int x, int y, bool8 on, u16 body, u16 edge)
{
    UiFillRect(x, y, DOT, DOT, on ? edge : UI_COL_DIM);
    UiFillRect(x + 1, y + 1, DOT - 2, DOT - 2, on ? body : UI_COL_BG);
}

void UiFrontierPageDraw(void)
{
    u8 label[20];
    int x;

    x = UI_PAGE_LEFT;
    x += UiText(x, BP_Y, UiAscii(label, "BATTLE POINTS", sizeof(label)),
                UI_COL_DIM, UiThemeShadow()) + 8;
    UiNum(x, BP_Y, gSaveBlock2Ptr->frontier.battlePoints, UiThemeText(), UiThemeShadow());

    for (u8 i = 0; i < NUM_FRONTIER_FACILITIES; i++)
    {
        int y = ROW_Y(i);
        int dotY = y + (UI_GLYPH_H - DOT) / 2;
        const char *unit = i == FRONTIER_FACILITY_PIKE    ? "rooms"
                         : i == FRONTIER_FACILITY_PYRAMID ? "floors"
                                                          : "wins";

        UiText(NAME_X, y, UiAscii(label, sFacilityName[i], sizeof(label)),
               UiThemeText(), UiThemeShadow());

        // The two symbols in the colors of the game's art: silver, then gold.
        DrawDot(DOT_X, dotY, HasSymbol(i, FALSE), UI_COL_TEXT, UI_COL_DIM);
        DrawDot(DOT_X + DOT + 3, dotY, HasSymbol(i, TRUE), UI_COL_SHINY, UI_COL_SHINY_EDGE);

        UiText(DOT_X + 2 * DOT + 12, y, UiAscii(label, "best", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
        UiNumRight(NUM_R, y, FacilityRecord(i), UiThemeText(), UiThemeShadow());
        UiText(UNIT_X, y, UiAscii(label, unit, sizeof(label)), UI_COL_DIM, UiThemeShadow());
    }
}

u32 UiFrontierPageKey(void)
{
    // A challenge changes all of these, with no touch here.
    u32 key = gSaveBlock2Ptr->frontier.battlePoints;

    for (u8 i = 0; i < NUM_FRONTIER_FACILITIES; i++)
        key ^= ((u32)FacilityRecord(i) | ((u32)HasSymbol(i, FALSE) << 14)
                | ((u32)HasSymbol(i, TRUE) << 15)) * (2654435761u + 2 * i);

    return key;
}
