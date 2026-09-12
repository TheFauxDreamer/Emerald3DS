// TOUCH TO START. See ui_title.h for what this is and why it starts nothing
// itself.

#include "global.h"
#include "graphics.h"               // gTitleScreenPressStartPal
#include "title_screen.h"

#include "../bridge.h"
#include "ui_draw.h"
#include "ui_title.h"

// The lettering of graphics/title_screen/press_start.png, the banner's own art:
// 2px strokes, a 1px black outline round every stroke, and each row a step of
// one ramp, white over lavender over grey. The characters are indices into the
// banner's palette and a space is transparent.
//
// START is copied from the art verbatim (x 83..124, rows 1..7), and TOUCH and TO
// reuse its T. The art has no O, U, C or H, so those four are drawn to its
// rule: every pixel touching a stroke is outline, a counter the stroke closes
// is outline too, and anything further out is transparent. The gap between
// words is the 7 columns the art leaves between PRESS and START.
//
// Drawn in the art's own style rather than in the game's text font, because it
// is the same prompt as the one on the top screen and ought to look it.
#define TITLE_ART_W  116
#define TITLE_ART_H  7

static const char sArt[TITLE_ART_H][TITLE_ART_W + 1] =
{
    "111111111111111111111 111111111111111 1111       111111111111111111       111111111111111111  11  1111111 1111111111",
    "155555555155555551551 155155555551551 1551       155555555155555551       155555551555555551 1551 155555511555555551",
    "111155111155111551551 15515511111155111551       111155111155111551       155111111111551111155551155111551111551111",
    "   1441  144111441441 1441441    144444441          1441  144111441       144444441  1441  1441144144111441  1441   ",
    "   1331  133111331331113313311111133111331          1331  133111331       111111331  1331 13311113333133311  1331   ",
    "   1331  1333333313333333133333331331 1331          1331  133333331       133333331  1331 13333311333113331  1331   ",
    "   1111  1111111111111111111111111111 1111          1111  111111111       111111111  1111 11111111111111111  1111   ",
};

// Doubled, because the top screen shows the banner at 1.5x by default and the
// bottom one has no fractional scale to match it with: 1x would be half its
// size, and 2x is 232x14, centred on the whole screen. There is no tab bar
// before the game, so the content area's floor does not apply.
#define TITLE_SCALE  2
#define TITLE_X      ((UI_W - TITLE_ART_W * TITLE_SCALE) / 2)   // 44
#define TITLE_Y      ((UI_H - TITLE_ART_H * TITLE_SCALE) / 2)   // 113

// The palette's first six entries are the only ones the art uses: transparent,
// the black outline and the four steps of the ramp. Converted once, on first
// draw, from the game's own palette rather than hardcoded, so the prompt is the
// banner's colours by construction.
#define TITLE_PAL_COUNT  6

static u16   sPal[TITLE_PAL_COUNT];
static bool8 sPalLoaded;

void UiTitleDraw(void)
{
    if (Ctr3dsTitlePromptState() != CTR3DS_TITLE_PROMPT_LIT)
        return;

    if (!sPalLoaded)
    {
        UiLoadPal(sPal, gTitleScreenPressStartPal, TITLE_PAL_COUNT);
        sPalLoaded = TRUE;
    }

    // 622 pixels at 2x2, once every 16 frames and only on the title screen.
    // TITLE_X is even, so every one of these is a single paired store per row.
    for (int row = 0; row < TITLE_ART_H; row++)
    {
        for (int col = 0; col < TITLE_ART_W; col++)
        {
            char c = sArt[row][col];

            if (c < '1' || c >= '0' + TITLE_PAL_COUNT)
                continue;

            UiFillRect(TITLE_X + col * TITLE_SCALE, TITLE_Y + row * TITLE_SCALE,
                       TITLE_SCALE, TITLE_SCALE, sPal[c - '0']);
        }
    }
}

// Acting on release, as every control on this screen does. The whole screen is
// the control here, which the cheatsheet's "tap anywhere" warning does not
// cover: that is about a panel read mid-game, where a stray touch loses
// something. A stray touch on the title only does what START does.
void UiTitleTouch(const CtrTouchState *t)
{
    if (t == NULL || !t->justReleased)
        return;

    if (Ctr3dsTitlePromptState() != CTR3DS_TITLE_PROMPT_NONE)
        Ctr3dsTitleTouchStart();
}

u32 UiTitleStateKey(void)
{
    return Ctr3dsTitlePromptState();
}
