// EXTRA tab: port features that are not part of the original game.
//
// Everything here is a 3DS-side convenience rather than a change to Emerald, so
// nothing in this file touches game state. It talks to the host through
// bridge.h, the same way the RTC and the framebuffer do.
//
// Currently just fast-forward. The layout leaves the lower half free for the
// next toggle rather than centring one control on an empty screen.

#include "global.h"

#include "../bridge.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"

// Four buttons across the content area: 4 * 60 + 3 * 12 = 276, centred in 320
// leaves 22 either side, comfortably inside the 8px window frame.
#define SPEED_COUNT   (CTR_SPEED_MAX - CTR_SPEED_MIN + 1)
#define BTN_W         60
#define BTN_H         36
#define BTN_GAP       12
#define BTN_Y         48
#define BTN_X(i)      (22 + (i) * (BTN_W + BTN_GAP))

static void DrawSpeedButton(int index)
{
    int x = BTN_X(index);
    int speed = CTR_SPEED_MIN + index;
    int active = (speed == Ctr3dsGetSpeed());
    u8 label[8];
    char text[4];

    UiRect(x, BTN_Y, BTN_W, BTN_H, UI_COL_DIM);

    // The selected one gets a second, inset outline as well as accent text:
    // colour alone is easy to miss against the lighter window frames.
    if (active)
    {
        UiRect(x + 2, BTN_Y + 2, BTN_W - 4, BTN_H - 4, UI_COL_ACCENT);
        UiRect(x + 3, BTN_Y + 3, BTN_W - 6, BTN_H - 6, UI_COL_ACCENT);
    }

    text[0] = (char)('0' + speed);
    text[1] = 'x';
    text[2] = '\0';

    UiAscii(label, text, sizeof(label));
    UiText(x + (BTN_W - UiTextWidth(label)) / 2,
           BTN_Y + (BTN_H - UI_GLYPH_H) / 2,
           label, active ? UI_COL_ACCENT : UiThemeText(), UiThemeShadow());
}

void UiExtraDraw(void)
{
    u8 label[40];

    UiWindowFrame(0, 0, CTR_BOTTOM_WIDTH / 8, UI_CONTENT_H / 8);

    UiText(16, 16, UiAscii(label, "GAME SPEED", sizeof(label)),
           UiThemeText(), UiThemeShadow());

    for (int i = 0; i < SPEED_COUNT; i++)
        DrawSpeedButton(i);

    UiText(16, 100, UiAscii(label, "Runs the game faster. The picture", sizeof(label)),
           UI_COL_DIM, UiThemeShadow());
    UiText(16, 118, UiAscii(label, "and sound thin out while it does.", sizeof(label)),
           UI_COL_DIM, UiThemeShadow());
    UiText(16, 142, UiAscii(label, "Resets to 1x each time you start.", sizeof(label)),
           UI_COL_DIM, UiThemeShadow());
}

void UiExtraTouch(const CtrTouchState *t)
{
    if (!t->justReleased)
        return;

    for (int i = 0; i < SPEED_COUNT; i++)
    {
        if (!UiHit(t, BTN_X(i), BTN_Y, BTN_W, BTN_H))
            continue;

        Ctr3dsSetSpeed(CTR_SPEED_MIN + i);
        UiMarkDirty();
        return;
    }
}
