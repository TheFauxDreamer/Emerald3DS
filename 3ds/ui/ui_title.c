// TOUCH TO START. See ui_title.h for what it is and why it does not start the
// game itself.

#include "global.h"
#include "graphics.h"               // gTitleScreenPressStartPal
#include "title_screen.h"

#include "../bridge.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"               // UI_COL_DIM, UI_COL_SHADOW
#include "ui_title.h"

// The letters of graphics/title_screen/press_start.png, the banner's own art.
// It has 2px strokes and a 1px black outline around each stroke. Each row is
// one step of a ramp (white, lavender, gray). The characters are indices into
// the banner's palette. A space is transparent.
//
// START comes from the art exactly (x 83..124, rows 1..7), and TOUCH and TO use
// its T. The art has no O, U, C or H. Those four follow its rule: a pixel next
// to a stroke is outline, a closed counter is outline, and pixels further out
// are transparent. The gap between words is 7 columns, as between PRESS and
// START.
//
// It uses the art's style, not the game's font, because it is the same prompt
// as on the top screen.
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

// Doubled. The top screen shows the banner at 1.5x by default, and the bottom
// screen has no fractional scale. At 2x the art is 232x14, centered on the full
// screen. There is no tab bar before the game, so the content area does not
// apply.
#define TITLE_SCALE  2
#define TITLE_X      ((UI_W - TITLE_ART_W * TITLE_SCALE) / 2)   // 44
#define TITLE_Y      ((UI_H - TITLE_ART_H * TITLE_SCALE) / 2)   // 113

// The art uses only the first six palette entries: transparent, the black
// outline and the four ramp steps. They are converted once from the game's own
// palette, so the prompt always has the banner's colors.
#define TITLE_PAL_COUNT  6

static u16   sPal[TITLE_PAL_COUNT];
static bool8 sPalLoaded;

// The banner's own blink, from SpriteCB_PressStartCopyrightBanner: it is lit
// while bit 4 of its frame count is set, so 16 frames lit and 16 dark.
#define BANNER_BLINK_FRAMES  16

// This prompt blinks at half that rate: 32 frames lit and 32 dark. It is larger
// and closer to the eye, where the banner's pace looks busy. It uses the
// banner's frame count, not its own, and a power of two, because it tests one
// bit of that count.
//
// The offset makes it light on a frame when the banner lights. The banner first
// lights at count 16. Without the offset, this would light at 32, half a second
// after the top screen.
#define TITLE_BLINK_FRAMES   32
#define TITLE_BLINK_OFFSET   (TITLE_BLINK_FRAMES - BANNER_BLINK_FRAMES)

enum { PROMPT_NONE, PROMPT_LIT, PROMPT_DARK };

static u32 PromptState(void)
{
    s32 clock = Ctr3dsTitlePromptClock();

    if (clock < 0)
        return PROMPT_NONE;

    return ((clock + TITLE_BLINK_OFFSET) & TITLE_BLINK_FRAMES) ? PROMPT_LIT : PROMPT_DARK;
}

// The build id (Ctr3dsBuildId, ../bridge.h) in the bottom-right corner, so a
// player can check which build is installed. It shows only on the title screen.
//
// It uses the game's small font and the UI's dim gray, so it does not compete
// with the prompt. It shows in both halves of the blink, because it is not part
// of the prompt and blinking text is harder to read. The letters are about 5px
// from each edge (the small font's ink is in rows 4-11 of its 13).
#define BUILD_ID_MARGIN 4

static void DrawBuildId(void)
{
    u8 id[40];

    UiAscii(id, Ctr3dsBuildId(), sizeof(id));
    UiTextSmall(UI_W - BUILD_ID_MARGIN - UiTextSmallWidth(id),
                UI_H - BUILD_ID_MARGIN - UI_GLYPH_SMALL_H,
                id, UI_COL_DIM, UI_COL_SHADOW);
}

void UiTitleDraw(void)
{
    u32 state = PromptState();

    if (state == PROMPT_NONE)
        return;

    DrawBuildId();

    if (state != PROMPT_LIT)
        return;

    if (!sPalLoaded)
    {
        UiLoadPal(sPal, gTitleScreenPressStartPal, TITLE_PAL_COUNT);
        sPalLoaded = TRUE;
    }

    // The logo is 622 pixels at 2x2, drawn once every 32 frames, only on the
    // title screen. TITLE_X is even, so each rect is one paired store for each
    // row.
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

// Act on release, like every control on this screen. Here the full screen is
// the control. A stray touch on the title only does what START does, so there
// is nothing to lose.
void UiTitleTouch(const CtrTouchState *t)
{
    if (t == NULL || !t->justReleased)
        return;

    if (PromptState() != PROMPT_NONE)
        Ctr3dsTitleTouchStart();
}

u32 UiTitleStateKey(void)
{
    return PromptState();
}
