// The achievement toast. See ui_achtoast.h for what it is and why it is here.
//
// It reads through AchActive() (3ds/achievements.h), so it shows what the
// provider unlocked and does not know how.
//
// It looks like the shiny notice (DrawNotice in bottom_screen.c): a dark ground
// inside the player's frame, the gold rule and a sparkle. Both panels tell good
// news, so the same gold makes the second one easy to read. The dark ground
// makes the gold legible on all twenty frames.
//
// It is static. It costs one repaint to open and one to close. While an overlay
// is up on the second-core path, each party animation step is a full repaint
// (CtrBottomUpdate). The toast lasts four seconds.

#include "global.h"

#include "../bridge.h"
#include "../achievements.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "ui_achtoast.h"

// The interior, inside the 8px border: x 8..312, y 8..32.
#define AT_IN_X    (UI_AT_X + 8)
#define AT_IN_W    (UI_AT_W - 16)
#define AT_IN_Y    (UI_AT_Y + 8)
#define AT_IN_H    (UI_AT_H - 16)

// One row. Everything is centered on the 24px interior height.
#define AT_TEXT_Y  (AT_IN_Y + (AT_IN_H - UI_GLYPH_H) / 2)

// The big sparkle frame is 16x14 around its axis, which UiSparkle centers on. A
// position 12px from the rule clears it on the left. The text starts after it.
#define AT_STAR_CX (AT_IN_X + 12)
#define AT_STAR_CY (AT_IN_Y + AT_IN_H / 2)
#define AT_TEXT_X  (AT_IN_X + 26)

// VIEW uses the right end, at the THROW position of the quick-throw strip.
#define AT_BTN_W   52
#define AT_BTN_H   18
#define AT_BTN_X   (AT_IN_X + AT_IN_W - AT_BTN_W - 4)
#define AT_BTN_Y   (AT_IN_Y + (AT_IN_H - AT_BTN_H) / 2)

// Where the text must stop, with or without the button.
#define AT_TEXT_END_VIEW  (AT_BTN_X - 8)
#define AT_TEXT_END       (AT_IN_X + AT_IN_W - 8)

// Four seconds, counted in calls, not milliseconds. The shell ticks once for
// each displayed frame, so a call is 1/60 s even under fast-forward. That is
// long enough to read a title.
#define AT_FRAMES  240

static bool8 sActive;
static u16   sIndex;
static u16   sBatch;
static u16   sFrames;
static u16   sSeq;          // one for each toast, the state key

// On the TROPHY tab, VIEW would open the same tab, so it does not show there. A
// control that does not show does not work.
static bool8 ViewShown(void)
{
    return UiActiveTab() != UI_TAB_TROPHY;
}

static bool8 NextToast(void)
{
    if (!AchActive()->popToast(&sIndex, &sBatch))
        return FALSE;

    sActive = TRUE;
    sFrames = 0;
    sSeq++;
    return TRUE;
}

bool8 UiAchToastActive(void)
{
    return sActive;
}

u32 UiAchToastStateKey(void)
{
    return sActive ? (0x10000u | sSeq) : 0;
}

void UiAchToastTick(void)
{
    if (sActive)
    {
        if (++sFrames < AT_FRAMES)
            return;

        sActive = FALSE;
    }

    // Go directly to the next toast, if there is one. A queue of toasts then
    // costs one repaint for each.
    NextToast();
}

void UiAchToastDraw(void)
{
    u8 text[64];
    int end = ViewShown() ? AT_TEXT_END_VIEW : AT_TEXT_END;
    struct AchView v;
    const struct UiRamp *ramp;

    // One achievement uses its category's colors. A batch stays gold, because
    // it can hold several categories.
    if (sBatch > 1)
    {
        ramp = UiAchCategoryRamp(ACH_CAT_STORY);
    }
    else
    {
        AchActive()->get(sIndex, &v);
        ramp = UiAchCategoryRamp(v.category);
    }

    UiWindowFrame(UI_AT_TX, UI_AT_TY, UI_AT_TW, UI_AT_TH);

    // The notice's own ground and 2px rule, so a fixed color looks the same on
    // every frame. The dark ground makes the pale step of any ramp safe for
    // text.
    UiFillRect(AT_IN_X, AT_IN_Y, AT_IN_W, AT_IN_H, UI_COL_SHADOW);
    UiRect(AT_IN_X, AT_IN_Y, AT_IN_W, AT_IN_H, ramp->body);
    UiRect(AT_IN_X + 1, AT_IN_Y + 1, AT_IN_W - 2, AT_IN_H - 2, ramp->edge);

    UiSparkleRamp(AT_STAR_CX, AT_STAR_CY, UI_SPARKLE_SIZES - 1,
                  ramp->pale, ramp->body, ramp->edge);

    if (sBatch > 1)
    {
        // Several at once: a first load that catches up with the save, or one
        // moment that completed more than one. The list shows which, one tap
        // away.
        int x = AT_TEXT_X;

        x += UiNum(x, AT_TEXT_Y, sBatch, ramp->body, UI_COL_SHADOW);
        UiText(x, AT_TEXT_Y, UiAscii(text, " achievements unlocked", sizeof(text)),
               ramp->pale, UI_COL_SHADOW);
    }
    else
    {
        u8 title[64];
        int titleW, x = AT_TEXT_X;

        UiAscii(title, v.title, sizeof(title));
        titleW = UiTextWidth(title);

        // "Unlocked" in the pale step of the ramp, then the title in the body
        // color. The label goes first when it fits, and gives way to the title
        // when it does not.
        UiAscii(text, "Unlocked ", sizeof(text));
        if (x + UiTextWidth(text) + titleW <= end)
            x += UiText(x, AT_TEXT_Y, text, ramp->pale, UI_COL_SHADOW);

        UiText(x, AT_TEXT_Y, title, ramp->body, UI_COL_SHADOW);
    }

    if (ViewShown())
    {
        UiRect(AT_BTN_X, AT_BTN_Y, AT_BTN_W, AT_BTN_H, ramp->body);
        UiAscii(text, "VIEW", sizeof(text));
        UiText(AT_BTN_X + (AT_BTN_W - UiTextWidth(text)) / 2,
               AT_BTN_Y + (AT_BTN_H - UI_GLYPH_H) / 2,
               text, ramp->pale, UI_COL_SHADOW);
    }
}

bool8 UiAchToastTouch(const CtrTouchState *t)
{
    // On release, like every control on this screen. The shell already gave
    // this function the full rect, so a press elsewhere on the strip does not
    // reach the tab.
    if (!t->justReleased || !ViewShown())
        return FALSE;

    if (!UiHit(t, AT_BTN_X, AT_BTN_Y, AT_BTN_W, AT_BTN_H))
        return FALSE;

    // VIEW did the toast's job, so the toast closes and the list shows.
    sActive = FALSE;
    return TRUE;
}
