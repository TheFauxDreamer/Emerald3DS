// The achievement toast. See ui_achtoast.h for what it is and why it sits
// where it does.
//
// It reads through AchActive() (3ds/achievements.h), so it announces whatever
// the provider says was unlocked and knows nothing about how.
//
// It dresses like the shiny notice (DrawNotice in bottom_screen.c): its own
// dark ground inside the player's frame, the gold rule, a sparkle. That is not
// decoration. Both panels say "something good just happened", and saying it in
// the same gold means the player reads the second one without learning it. The
// dark ground is also what makes a fixed gold legible on all twenty frames.
//
// Static: nothing on it moves. It costs one repaint to appear and one to go.
// What it can cost beyond that is the PARTY tab's: while any overlay is up the
// shell turns each party animation step into a full repaint on the second-core
// path, so the grid keeps moving under the strip (CtrBottomUpdate). That was
// measured at 59a0ba6 as about 4.1 ms of paint plus 1.6 ms of upload, about
// 1 ms past the join, with no missed VBlank, and here it lasts four seconds.

#include "global.h"

#include "../bridge.h"
#include "../achievements.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "ui_achtoast.h"

// The interior, inside the frame's 8px border: x 8..312, y 8..32.
#define AT_IN_X    (UI_AT_X + 8)
#define AT_IN_W    (UI_AT_W - 16)
#define AT_IN_Y    (UI_AT_Y + 8)
#define AT_IN_H    (UI_AT_H - 16)

// One row, everything centred on the 24px interior's own height.
#define AT_TEXT_Y  (AT_IN_Y + (AT_IN_H - UI_GLYPH_H) / 2)

// The big sparkle frame is 16x14 around its axis, which UiSparkle centres on,
// so 12px in from the rule clears it on the left and the text starts after.
#define AT_STAR_CX (AT_IN_X + 12)
#define AT_STAR_CY (AT_IN_Y + AT_IN_H / 2)
#define AT_TEXT_X  (AT_IN_X + 26)

// VIEW takes the right-hand end, the quick-throw strip's THROW position.
#define AT_BTN_W   52
#define AT_BTN_H   18
#define AT_BTN_X   (AT_IN_X + AT_IN_W - AT_BTN_W - 4)
#define AT_BTN_Y   (AT_IN_Y + (AT_IN_H - AT_BTN_H) / 2)

// Where the text has to stop, with or without the button beside it.
#define AT_TEXT_END_VIEW  (AT_BTN_X - 8)
#define AT_TEXT_END       (AT_IN_X + AT_IN_W - 8)

// Four seconds, counted in calls rather than milliseconds: the shell ticks once
// per DISPLAYED frame, so a call is a 60th of a second even under
// fast-forward, the idiom UiHold and the shiny notice use. Long enough to read
// a title, short enough that a toast is gone before it is in the way.
#define AT_FRAMES  240

static bool8 sActive;
static u16   sIndex;
static u16   sBatch;
static u16   sFrames;
static u16   sSeq;          // one per toast shown, the state key's identity

// VIEW would open the tab the player is already on, so it is not drawn there,
// and a control that is not drawn is not tappable either.
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

    // Straight on to the next one if there is one, so a queue of toasts is one
    // repaint each rather than a down-and-up pair.
    NextToast();
}

void UiAchToastDraw(void)
{
    u8 text[64];
    int end = ViewShown() ? AT_TEXT_END_VIEW : AT_TEXT_END;
    struct AchView v;
    const struct UiRamp *ramp;

    // One achievement wears its category's colours. A batch stays gold, the
    // colour every achievement had before categories: it can hold several
    // kinds at once, and no one of them should speak for the rest.
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

    // The notice's own ground and 2px rule, so a fixed colour reads the same on
    // every frame the player can choose. The ground is what makes the pale step
    // of any ramp safe to print text in here.
    UiFillRect(AT_IN_X, AT_IN_Y, AT_IN_W, AT_IN_H, UI_COL_SHADOW);
    UiRect(AT_IN_X, AT_IN_Y, AT_IN_W, AT_IN_H, ramp->body);
    UiRect(AT_IN_X + 1, AT_IN_Y + 1, AT_IN_W - 2, AT_IN_H - 2, ramp->edge);

    UiSparkleRamp(AT_STAR_CX, AT_STAR_CY, UI_SPARKLE_SIZES - 1,
                  ramp->pale, ramp->body, ramp->edge);

    if (sBatch > 1)
    {
        // Several at once: a first load catching up with the save, or a moment
        // that finished more than one. The list says which, one tap away.
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

        // "Unlocked" in the pale step of the ramp, then the title in its body
        // colour -- the notice's hierarchy turned sideways. The label goes first
        // when it fits, and gives way to the title when it does not.
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
    // On release, like every control on this screen. The shell has already
    // given this the whole rect, so a press anywhere else on the strip is
    // absorbed rather than reaching the tab underneath.
    if (!t->justReleased || !ViewShown())
        return FALSE;

    if (!UiHit(t, AT_BTN_X, AT_BTN_Y, AT_BTN_W, AT_BTN_H))
        return FALSE;

    // VIEW has done the toast's job, so it goes; the list takes over.
    sActive = FALSE;
    return TRUE;
}
