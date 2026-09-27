// CLOCK: the game's time, what it changes each day, and the counters that go
// down as the player walks. See view_home.h.
//
// The time is the game's own local time: the clock chip, less the offset the
// player set at the wall clock. It is the time that berries, tides and the
// daily events use. RtcCalcTimeDifference (src/rtc.c) gives it into a local
// struct, so gLocalTime does not change.
//
// One page has all of it, so the player does not go between two:
//
//    30   the time at double size | play time, time to the next day
//    66   STEPS | REPEL
//    82   LOTTERY | MIRAGE
//    98   the Shoal Cave tide
//    120  the eggs, two columns of three

#include "global.h"
#include "event_data.h"               // VarGet
#include "egg_hatch.h"                // GetEggCyclesToSubtract
#include "overworld.h"                // GetGameStat
#include "pokemon.h"
#include "rtc.h"
#include "time_events.h"              // IsMirageIslandPresent
#include "constants/characters.h"   // CHAR_0, EOS
#include "constants/flags.h"
#include "constants/game_stat.h"
#include "constants/vars.h"

#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "view_home.h"

#define TIME_Y       (UI_PAGE_TOP + 2)
#define ROW_H        16

// Right of the time: two rows, the value after the label.
#define SIDE_X       120
#define SIDE_VALUE_X 210

// Two columns of label and value under the time.
#define GRID_Y0      (TIME_Y + UI_GLYPH_BIG_H + 6)
#define COL_X(c)     (UI_PAGE_LEFT + (c) * 148)
#define COL_VALUE_DX 62
#define TIDE_VALUE_X (UI_PAGE_LEFT + 90)

// The eggs: two columns of three, under the rest.
#define EGG_Y0       (GRID_Y0 + 3 * ROW_H + 6)
#define EGG_COL_W    144

// The hours of high tide in Shoal Cave. This is a copy of the table in
// UpdateShoalTideFlag (src/time_events.c): it is local to that function, and
// the function writes FLAG_SYS_SHOAL_TIDE. A 1 is high tide, because the
// Shoal Cave scripts read the set flag as high tide.
static const u8 sHighTide[24] =
{
    1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1,
    1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1,
};

void UiGameTime(struct Time *out)
{
    struct SiiRtcInfo rtc;

    RtcGetInfo(&rtc);
    RtcCalcTimeDifference(&rtc, out, &gSaveBlock2Ptr->localTimeOffset);
}

// Two digits, with a leading zero.
static int Num2(int x, int y, u32 value, bool8 big)
{
    u8 text[4];

    text[0] = CHAR_0 + (value / 10) % 10;
    text[1] = CHAR_0 + value % 10;
    text[2] = EOS;

    return big ? UiTextBig(x, y, text, UiThemeText(), UiThemeShadow())
               : UiText(x, y, text, UiThemeText(), UiThemeShadow());
}

static void Label(int x, int y, const char *text)
{
    u8 label[24];

    UiText(x, y, UiAscii(label, text, sizeof(label)), UI_COL_DIM, UiThemeShadow());
}

// A word value. `on` puts it in the accent: something the player can still do
// today.
static int Value(int x, int y, const char *text, bool8 on)
{
    u8 label[32];

    return UiText(x, y, UiAscii(label, text, sizeof(label)),
                  on ? UI_COL_ACCENT : UiThemeText(), UiThemeShadow());
}

// "Hh Mm" to the game's midnight, when it clears the daily flags.
static void DrawNewDay(int x, int y, const struct Time *now)
{
    u32 left = (23 - now->hours) * 60 + (60 - now->minutes);

    if (left >= 60)
    {
        x += UiNum(x, y, left / 60, UiThemeText(), UiThemeShadow());
        x += Value(x, y, "h", FALSE) + 4;
    }
    x += UiNum(x, y, left % 60, UiThemeText(), UiThemeShadow());
    Value(x, y, "m", FALSE);
}

static void DrawTide(int y, u32 hour)
{
    u8 colon[2] = { CHAR_COLON, EOS };
    u8 high = sHighTide[hour % 24];
    u32 next = hour;
    int x;

    // The next hour with the other tide. Every day has both, so this ends.
    do
        next = (next + 1) % 24;
    while (sHighTide[next] == high);

    Label(UI_PAGE_LEFT, y, "SHOAL CAVE");
    x = TIDE_VALUE_X + Value(TIDE_VALUE_X, y, high ? "high tide until" : "low tide until", !high) + 4;
    x += Num2(x, y, next, FALSE);
    x += UiText(x, y, colon, UiThemeText(), UiThemeShadow());
    Num2(x, y, 0, FALSE);
}

// The steps until the egg in party slot `slot` hatches, or -1 for a bad egg.
//
// TryProduceOrHatchEgg (src/daycare.c) counts the day care's stepCounter up
// to 255, then takes GetEggCyclesToSubtract() cycles from each egg. An egg
// with no cycles left hatches at that time. The counter is a u8: after 255 it
// goes to 0, so each later count is 256 steps. That function writes the save,
// so this does the sum and does not call it.
static s32 EggStepsLeft(u8 slot)
{
    struct Pokemon *mon = &gPlayerParty[slot];
    u32 cycles = GetMonData(mon, MON_DATA_FRIENDSHIP);
    u32 toSub = GetEggCyclesToSubtract();
    u32 counts = 0;

    if (GetMonData(mon, MON_DATA_SANITY_IS_BAD_EGG))
        return -1;

    // One count for each subtraction, as the game makes it, then the one that
    // hatches.
    while (cycles != 0)
    {
        cycles -= (cycles >= toSub) ? toSub : 1;
        counts++;
    }

    return (s32)(((254u - gSaveBlock1Ptr->daycare.stepCounter) & 0xFF) + 1 + counts * 256);
}

static void DrawEggs(void)
{
    u8 label[24];
    u32 n = 0;

    for (u8 i = 0; i < gPlayerPartyCount && i < PARTY_SIZE; i++)
    {
        int x, y;
        s32 steps;

        if (!GetMonData(&gPlayerParty[i], MON_DATA_IS_EGG))
            continue;

        steps = EggStepsLeft(i);
        if (steps < 0)
            continue;

        x = UI_PAGE_LEFT + (n / 3) * EGG_COL_W;
        y = EGG_Y0 + (n % 3) * ROW_H;
        n++;

        x += UiText(x, y, UiAscii(label, "EGG", sizeof(label)),
                    UI_COL_DIM, UiThemeShadow()) + 4;
        x += UiNum(x, y, i + 1, UI_COL_DIM, UiThemeShadow()) + 8;
        x += UiText(x, y, UiAscii(label, "about", sizeof(label)),
                    UI_COL_DIM, UiThemeShadow()) + 4;
        x += UiNum(x, y, steps, UiThemeText(), UiThemeShadow()) + 4;
        UiText(x, y, UiAscii(label, "steps", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
    }

    if (n == 0)
        UiText(UI_PAGE_LEFT, EGG_Y0, UiAscii(label, "No EGGS in the party.", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
}

void UiClockPageDraw(void)
{
    struct Time now;
    u8 colon[2] = { CHAR_COLON, EOS };
    int x, y;
    u16 repel = VarGet(VAR_REPEL_STEP_COUNT);
    bool8 lottery = FlagGet(FLAG_DAILY_PICKED_LOTO_TICKET);
    bool8 mirage = IsMirageIslandPresent();

    UiGameTime(&now);

    // The time at double size, then the play time and the new day beside it.
    x = UI_PAGE_LEFT;
    x += Num2(x, TIME_Y, now.hours, TRUE);
    x += UiTextBig(x, TIME_Y, colon, UiThemeText(), UiThemeShadow());
    Num2(x, TIME_Y, now.minutes, TRUE);

    Label(SIDE_X, TIME_Y, "PLAY TIME");
    x = SIDE_VALUE_X + UiNum(SIDE_VALUE_X, TIME_Y, gSaveBlock2Ptr->playTimeHours,
                             UiThemeText(), UiThemeShadow());
    x += UiText(x, TIME_Y, colon, UiThemeText(), UiThemeShadow());
    Num2(x, TIME_Y, gSaveBlock2Ptr->playTimeMinutes, FALSE);

    Label(SIDE_X, TIME_Y + ROW_H, "NEW DAY IN");
    DrawNewDay(SIDE_VALUE_X, TIME_Y + ROW_H, &now);

    y = GRID_Y0;
    Label(COL_X(0), y, "STEPS");
    UiNum(COL_X(0) + COL_VALUE_DX, y, GetGameStat(GAME_STAT_STEPS), UiThemeText(), UiThemeShadow());

    Label(COL_X(1), y, "REPEL");
    if (repel != 0)
    {
        x = COL_X(1) + COL_VALUE_DX;
        x += UiNum(x, y, repel, UiThemeText(), UiThemeShadow()) + 4;
        Label(x, y, "left");
    }
    else
    {
        Label(COL_X(1) + COL_VALUE_DX, y, "none");
    }

    y += ROW_H;
    Label(COL_X(0), y, "LOTTERY");
    Value(COL_X(0) + COL_VALUE_DX, y, lottery ? "drawn" : "not drawn", !lottery);
    Label(COL_X(1), y, "MIRAGE");
    Value(COL_X(1) + COL_VALUE_DX, y, mirage ? "visible today" : "not today", mirage);

    y += ROW_H;
    DrawTide(y, now.hours);

    DrawEggs();
}

u32 UiClockPageKey(void)
{
    struct Time now;
    u32 key;

    UiGameTime(&now);

    // The minute moves the time, the tide and the new day.
    key = (u32)now.hours * 60 + now.minutes;
    key ^= (u32)gSaveBlock2Ptr->playTimeMinutes << 11;
    key ^= GetGameStat(GAME_STAT_STEPS) << 17;
    key ^= (u32)VarGet(VAR_REPEL_STEP_COUNT) * 2654435761u;
    key ^= (u32)gSaveBlock1Ptr->daycare.stepCounter << 24;
    key ^= ((u32)FlagGet(FLAG_DAILY_PICKED_LOTO_TICKET)
            | ((u32)IsMirageIslandPresent() << 1)) * 0xC2B2AE35u;

    // An egg loses a cycle, hatches or joins the party with no touch here.
    for (u8 i = 0; i < gPlayerPartyCount && i < PARTY_SIZE; i++)
        if (GetMonData(&gPlayerParty[i], MON_DATA_IS_EGG))
            key ^= (GetMonData(&gPlayerParty[i], MON_DATA_FRIENDSHIP) + 1) * (0x9E3779B1u + i);

    return key;
}
