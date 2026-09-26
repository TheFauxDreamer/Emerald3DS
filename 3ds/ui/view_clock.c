// CLOCK: the game's time and the counters that go down as the player walks.
// See view_home.h.
//
// The time is the game's own local time: the clock chip, less the offset the
// player set at the wall clock. It is the time that berries, tides and the
// daily events use. RtcCalcTimeDifference (src/rtc.c) gives it into a local
// struct, so gLocalTime does not change.

#include "global.h"
#include "event_data.h"               // VarGet
#include "egg_hatch.h"                // GetEggCyclesToSubtract
#include "overworld.h"                // GetGameStat
#include "pokemon.h"
#include "rtc.h"
#include "constants/characters.h"   // CHAR_0, EOS
#include "constants/game_stat.h"
#include "constants/vars.h"

#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "view_home.h"

#define TIME_Y       (UI_PAGE_TOP + 2)
#define ROW_Y0       (TIME_Y + UI_GLYPH_BIG_H + 8)
#define ROW_H        17
#define VALUE_X      120

// The eggs: two columns of three, under the counters.
#define EGG_Y0       (ROW_Y0 + 3 * ROW_H + 4)
#define EGG_COL_W    144

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

static void Label(int y, const char *text)
{
    u8 label[24];

    UiText(UI_PAGE_LEFT, y, UiAscii(label, text, sizeof(label)),
           UI_COL_DIM, UiThemeShadow());
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
    u8 label[24];
    int x, y;
    u16 repel = VarGet(VAR_REPEL_STEP_COUNT);

    UiGameTime(&now);

    // The time at double size, centered.
    x = (CTR_BOTTOM_WIDTH - (2 * UiTextBigWidth(UiAscii(label, "00", sizeof(label)))
                             + UiTextBigWidth(colon))) / 2;
    x += Num2(x, TIME_Y, now.hours, TRUE);
    x += UiTextBig(x, TIME_Y, colon, UiThemeText(), UiThemeShadow());
    Num2(x, TIME_Y, now.minutes, TRUE);

    y = ROW_Y0;
    Label(y, "PLAY TIME");
    x = VALUE_X + UiNum(VALUE_X, y, gSaveBlock2Ptr->playTimeHours, UiThemeText(), UiThemeShadow());
    x += UiText(x, y, colon, UiThemeText(), UiThemeShadow());
    Num2(x, y, gSaveBlock2Ptr->playTimeMinutes, FALSE);

    y += ROW_H;
    Label(y, "STEPS");
    UiNum(VALUE_X, y, GetGameStat(GAME_STAT_STEPS), UiThemeText(), UiThemeShadow());

    y += ROW_H;
    Label(y, "REPEL");
    if (repel != 0)
    {
        x = VALUE_X + UiNum(VALUE_X, y, repel, UiThemeText(), UiThemeShadow()) + 4;
        UiText(x, y, UiAscii(label, "steps left", sizeof(label)), UI_COL_DIM, UiThemeShadow());
    }
    else
    {
        UiText(VALUE_X, y, UiAscii(label, "none", sizeof(label)), UI_COL_DIM, UiThemeShadow());
    }

    DrawEggs();
}

u32 UiClockPageKey(void)
{
    struct Time now;
    u32 key;

    UiGameTime(&now);

    key = (u32)now.hours * 60 + now.minutes;
    key ^= (u32)gSaveBlock2Ptr->playTimeMinutes << 11;
    key ^= GetGameStat(GAME_STAT_STEPS) << 17;
    key ^= (u32)VarGet(VAR_REPEL_STEP_COUNT) * 2654435761u;
    key ^= (u32)gSaveBlock1Ptr->daycare.stepCounter << 24;

    // An egg loses a cycle, hatches or joins the party with no touch here.
    for (u8 i = 0; i < gPlayerPartyCount && i < PARTY_SIZE; i++)
        if (GetMonData(&gPlayerParty[i], MON_DATA_IS_EGG))
            key ^= (GetMonData(&gPlayerParty[i], MON_DATA_FRIENDSHIP) + 1) * (0x9E3779B1u + i);

    return key;
}
