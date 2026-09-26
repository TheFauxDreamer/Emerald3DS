// DAILY: what changes with the game's clock, and the Battle Frontier. See
// view_home.h.
//
// The top part: the Shoal Cave tide, the Lilycove lottery, Mirage Island and
// the time to the next day, when the game clears its daily flags. The bottom
// part, once the player has reached the Battle Frontier: the Battle Points,
// and each facility's symbols and best record. Everything is a save value or
// the game's local time (UiGameTime).

#include "global.h"
#include "event_data.h"               // FlagGet
#include "rtc.h"                      // struct Time
#include "time_events.h"              // IsMirageIslandPresent
#include "constants/battle_frontier.h"
#include "constants/characters.h"   // CHAR_0, EOS
#include "constants/flags.h"

#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "view_home.h"

#define ROW_H        16
#define VALUE_X      112

// The Battle Frontier: a head line, then two columns of four facilities.
#define BF_HEAD_Y    (UI_PAGE_TOP + 4 * ROW_H + 6)
#define BF_ROW_Y(i)  (BF_HEAD_Y + 18 + ((i) % 4) * ROW_H)
#define BF_COL_X(i)  (UI_PAGE_LEFT + ((i) / 4) * 148)
#define BF_DOT_DX    58
#define BF_DOT       6
#define BF_NUM_R     112
#define BF_UNIT_DX   114

// The hours of high tide in Shoal Cave. This is a copy of the table in
// UpdateShoalTideFlag (src/time_events.c): it is local to that function, and
// the function writes FLAG_SYS_SHOAL_TIDE. A 1 is high tide, because the
// Shoal Cave scripts read the set flag as high tide.
static const u8 sHighTide[24] =
{
    1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1,
    1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1,
};

static const char *const sFacilityName[NUM_FRONTIER_FACILITIES] =
{
    [FRONTIER_FACILITY_TOWER]   = "TOWER",
    [FRONTIER_FACILITY_DOME]    = "DOME",
    [FRONTIER_FACILITY_PALACE]  = "PALACE",
    [FRONTIER_FACILITY_ARENA]   = "ARENA",
    [FRONTIER_FACILITY_FACTORY] = "FACTORY",
    [FRONTIER_FACILITY_PIKE]    = "PIKE",
    [FRONTIER_FACILITY_PYRAMID] = "PYRAMID",
};

static void Label(int y, const char *text)
{
    u8 label[24];

    UiText(UI_PAGE_LEFT, y, UiAscii(label, text, sizeof(label)), UI_COL_DIM, UiThemeShadow());
}

static int Value(int x, int y, const char *text, bool8 on)
{
    u8 label[32];

    return UiText(x, y, UiAscii(label, text, sizeof(label)),
                  on ? UI_COL_ACCENT : UiThemeText(), UiThemeShadow());
}

// "HH:00", with a leading zero.
static void DrawHour(int x, int y, u32 hour)
{
    u8 text[6];

    text[0] = CHAR_0 + hour / 10;
    text[1] = CHAR_0 + hour % 10;
    text[2] = CHAR_COLON;
    text[3] = CHAR_0;
    text[4] = CHAR_0;
    text[5] = EOS;
    UiText(x, y, text, UiThemeText(), UiThemeShadow());
}

static void DrawTide(int y, u32 hour)
{
    u8 high = sHighTide[hour % 24];
    u32 next = hour;
    int x;

    // The next hour with the other tide. Every day has both, so this ends.
    do
        next = (next + 1) % 24;
    while (sHighTide[next] == high);

    Label(y, "SHOAL CAVE");
    x = VALUE_X + Value(VALUE_X, y, high ? "high tide until" : "low tide until", !high) + 4;
    DrawHour(x, y, next);
}

static void DrawNewDay(int y, const struct Time *now)
{
    u32 left = (23 - now->hours) * 60 + (60 - now->minutes);
    int x = VALUE_X;

    Label(y, "NEW DAY IN");
    if (left >= 60)
    {
        x += UiNum(x, y, left / 60, UiThemeText(), UiThemeShadow());
        x += Value(x, y, "h", FALSE) + 4;
    }
    x += UiNum(x, y, left % 60, UiThemeText(), UiThemeShadow());
    Value(x, y, "m", FALSE);
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
    UiFillRect(x, y, BF_DOT, BF_DOT, on ? edge : UI_COL_DIM);
    UiFillRect(x + 1, y + 1, BF_DOT - 2, BF_DOT - 2, on ? body : UI_COL_BG);
}

static void DrawFrontier(void)
{
    u8 label[16];
    int x;

    Value(UI_PAGE_LEFT, BF_HEAD_Y, "BATTLE FRONTIER", FALSE);
    x = CTR_BOTTOM_WIDTH - UI_PAGE_LEFT;
    x -= UiNumRight(x, BF_HEAD_Y, gSaveBlock2Ptr->frontier.battlePoints,
                    UiThemeText(), UiThemeShadow()) + 4;
    UiTextRight(x, BF_HEAD_Y, UiAscii(label, "BP", sizeof(label)), UI_COL_DIM, UiThemeShadow());

    for (u8 i = 0; i < NUM_FRONTIER_FACILITIES; i++)
    {
        int fx = BF_COL_X(i), fy = BF_ROW_Y(i);
        int dotY = fy + (UI_GLYPH_H - BF_DOT) / 2;
        const char *unit = i == FRONTIER_FACILITY_PIKE    ? "rooms"
                         : i == FRONTIER_FACILITY_PYRAMID ? "floors"
                                                          : "wins";

        UiText(fx, fy, UiAscii(label, sFacilityName[i], sizeof(label)),
               UiThemeText(), UiThemeShadow());
        DrawDot(fx + BF_DOT_DX, dotY, HasSymbol(i, FALSE), UI_COL_TEXT, UI_COL_DIM);
        DrawDot(fx + BF_DOT_DX + BF_DOT + 2, dotY, HasSymbol(i, TRUE),
                UI_COL_SHINY, UI_COL_SHINY_EDGE);
        UiNumRight(fx + BF_NUM_R, fy, FacilityRecord(i), UiThemeText(), UiThemeShadow());
        UiTextSmall(fx + BF_UNIT_DX, fy + 1, UiAscii(label, unit, sizeof(label)),
                    UI_COL_DIM, UiThemeShadow());
    }
}

void UiDailyPageDraw(void)
{
    struct Time now;
    int y = UI_PAGE_TOP;

    UiGameTime(&now);

    DrawTide(y, now.hours);

    y += ROW_H;
    Label(y, "LOTTERY");
    Value(VALUE_X, y, FlagGet(FLAG_DAILY_PICKED_LOTO_TICKET) ? "drawn today" : "not drawn yet",
          !FlagGet(FLAG_DAILY_PICKED_LOTO_TICKET));

    y += ROW_H;
    Label(y, "MIRAGE ISLAND");
    Value(VALUE_X, y, IsMirageIslandPresent() ? "visible today" : "not today",
          IsMirageIslandPresent());

    y += ROW_H;
    DrawNewDay(y, &now);

    if (FlagGet(FLAG_LANDMARK_BATTLE_FRONTIER))
        DrawFrontier();
}

u32 UiDailyPageKey(void)
{
    struct Time now;
    u32 key;

    UiGameTime(&now);

    // The minute moves the tide and the new day. The rest changes as the
    // player plays, with no touch here.
    key = (u32)now.hours * 60 + now.minutes;
    key ^= (u32)FlagGet(FLAG_DAILY_PICKED_LOTO_TICKET) << 11;
    key ^= (u32)IsMirageIslandPresent() << 12;
    key ^= (u32)FlagGet(FLAG_LANDMARK_BATTLE_FRONTIER) << 13;
    key ^= (u32)gSaveBlock2Ptr->frontier.battlePoints << 16;

    for (u8 i = 0; i < NUM_FRONTIER_FACILITIES; i++)
        key ^= ((u32)FacilityRecord(i) | ((u32)HasSymbol(i, FALSE) << 14)
                | ((u32)HasSymbol(i, TRUE) << 15)) * (2654435761u + 2 * i);

    return key;
}
