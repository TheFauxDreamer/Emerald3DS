// EXTRA tab: port features that are not part of the original game.
//
// Everything here is a 3DS-side convenience rather than a change to Emerald, so
// nothing in this file touches game state. It talks to the host through
// bridge.h, the same way the RTC and the framebuffer do.
//
// Two sections so far. Both button rows deliberately span the same 22..298, so
// the block reads as one control panel rather than two unrelated widgets.

#include "global.h"

#include "../bridge.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"

// Doubling steps rather than 1/2/3/4: past 2x the interesting question is
// "much faster", and 3x sits too close to 2x to be worth a button.
static const u8 sSpeeds[] = { 1, 2, 4, 8 };
#define SPEED_COUNT   ARRAY_COUNT(sSpeeds)

// Indexed by button, never derived from the index: the modes are an enum, not a
// sequence, so arithmetic on the index would be a lie waiting to break.
static const u8 sScales[] = {
    CTR_TOP_SCALE_1X, CTR_TOP_SCALE_1_5X, CTR_TOP_SCALE_FILL
};
static const char *const sScaleNames[] = { "1x", "1.5x", "FILL" };
#define SCALE_COUNT   ARRAY_COUNT(sScales)

// What a button can be bound to, in tap order. MOD sits in the same cycle as
// the speeds rather than in a separate control, which is what makes the two
// mutually exclusive: a button holds one value, so binding a speed necessarily
// stops it being the modifier and there is no way to ask for both.
//
// Tapping wraps back to unbound, so every state is reachable with one finger.
static const u8 sTurboSteps[] = { CTR_BIND_OFF, 2, 4, 8, CTR_BIND_MOD };
static const char *const sTurboNames[CTR_TURBO_COUNT] = { "X", "Y", "ZL", "ZR" };

// All three rows total 276px wide (4*60 + 3*12, and 3*84 + 2*12), centred in
// 320, which leaves 22 either side and clears the 8px window frame comfortably.
#define BTN_H         26
#define BTN_GAP       12

#define SPD_W         60
#define SPD_Y         28
#define SPD_X(i)      (22 + (i) * (SPD_W + BTN_GAP))

#define SCL_W         84
#define SCL_Y         80
#define SCL_X(i)      (22 + (i) * (SCL_W + BTN_GAP))

#define TRB_W         60
#define TRB_Y         132
#define TRB_X(i)      (22 + (i) * (TRB_W + BTN_GAP))

// The selected button gets a doubled inset outline as well as accent text.
// Colour alone is easy to miss against the lighter window frames.
static void DrawButton(int x, int y, int w, const u8 *label, int active)
{
    UiRect(x, y, w, BTN_H, UI_COL_DIM);

    if (active)
    {
        UiRect(x + 2, y + 2, w - 4, BTN_H - 4, UI_COL_ACCENT);
        UiRect(x + 3, y + 3, w - 6, BTN_H - 6, UI_COL_ACCENT);
    }

    UiText(x + (w - UiTextWidth(label)) / 2, y + (BTN_H - UI_GLYPH_H) / 2,
           label, active ? UI_COL_ACCENT : UiThemeText(), UiThemeShadow());
}

void UiExtraDraw(void)
{
    u8 label[40];

    UiWindowFrame(0, 0, CTR_BOTTOM_WIDTH / 8, UI_CONTENT_H / 8);

    UiText(16, 10, UiAscii(label, "GAME SPEED", sizeof(label)),
           UiThemeText(), UiThemeShadow());

    for (u32 i = 0; i < SPEED_COUNT; i++)
    {
        char text[4];

        // Every offered speed is a single digit, so no wider label is needed.
        text[0] = (char)('0' + sSpeeds[i]);
        text[1] = 'x';
        text[2] = '\0';

        DrawButton(SPD_X((int)i), SPD_Y, SPD_W,
                   UiAscii(label, text, sizeof(label)),
                   sSpeeds[i] == Ctr3dsGetSpeed());
    }

    UiText(16, 62, UiAscii(label, "SCREEN SIZE", sizeof(label)),
           UiThemeText(), UiThemeShadow());

    for (u32 i = 0; i < SCALE_COUNT; i++)
        DrawButton(SCL_X((int)i), SCL_Y, SCL_W,
                   UiAscii(label, sScaleNames[i], sizeof(label)),
                   sScales[i] == Ctr3dsGetTopScale());

    UiText(16, 114, UiAscii(label, "BUTTON HOLD", sizeof(label)),
           UiThemeText(), UiThemeShadow());

    for (u32 i = 0; i < CTR_TURBO_COUNT; i++)
    {
        int bind = Ctr3dsGetTurboBind((int)i);
        char text[10];
        int n = 0;

        // "X 4x", "X MOD", or "X -" when nothing is bound.
        text[n++] = sTurboNames[i][0];
        if (sTurboNames[i][1] != '\0')
            text[n++] = sTurboNames[i][1];
        text[n++] = ' ';

        if (bind == CTR_BIND_OFF)
        {
            text[n++] = '-';
        }
        else if (bind == CTR_BIND_MOD)
        {
            text[n++] = 'M';
            text[n++] = 'O';
            text[n++] = 'D';
        }
        else
        {
            text[n++] = (char)('0' + bind);
            text[n++] = 'x';
        }
        text[n] = '\0';

        DrawButton(TRB_X((int)i), TRB_Y, TRB_W,
                   UiAscii(label, text, sizeof(label)), bind != CTR_BIND_OFF);
    }

    // MOD needs saying: it is not obvious that one of these buttons is what
    // makes the Pokedex arrows jump. ZL and ZR do not exist on an Old 3DS, so
    // a binding there would otherwise look broken rather than unsupported.
    UiText(16, 164, UiAscii(label, "MOD jumps lists. ZL/ZR: New 3DS.", sizeof(label)),
           UI_COL_DIM, UiThemeShadow());
}

void UiExtraTouch(const CtrTouchState *t)
{
    if (!t->justReleased)
        return;

    for (u32 i = 0; i < SPEED_COUNT; i++)
    {
        if (UiHit(t, SPD_X((int)i), SPD_Y, SPD_W, BTN_H))
        {
            Ctr3dsSetSpeed(sSpeeds[i]);
            UiMarkDirty();
            return;
        }
    }

    for (u32 i = 0; i < SCALE_COUNT; i++)
    {
        if (UiHit(t, SCL_X((int)i), SCL_Y, SCL_W, BTN_H))
        {
            Ctr3dsSetTopScale(sScales[i]);
            UiMarkDirty();
            return;
        }
    }

    for (u32 i = 0; i < CTR_TURBO_COUNT; i++)
    {
        if (UiHit(t, TRB_X((int)i), TRB_Y, TRB_W, BTN_H))
        {
            // Cycle to the next step, wrapping through 0. Searching for the
            // current value rather than storing an index keeps the button and
            // the host in agreement even if one is changed elsewhere.
            int cur = Ctr3dsGetTurboBind((int)i);
            u32 step = 0;

            for (u32 k = 0; k < ARRAY_COUNT(sTurboSteps); k++)
                if (sTurboSteps[k] == cur)
                    step = k;

            step = (step + 1) % ARRAY_COUNT(sTurboSteps);
            Ctr3dsSetTurboBind((int)i, sTurboSteps[step]);
            UiMarkDirty();
            return;
        }
    }
}
