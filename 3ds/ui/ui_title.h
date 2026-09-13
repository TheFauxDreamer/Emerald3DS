// TOUCH TO START: the title screen's PRESS START, copied on the bottom screen.
//
// Before the game starts, the bottom screen is blank and takes no touches. This
// is the one exception. While the PRESS START banner is up, the bottom screen
// shows TOUCH TO START in the banner's own letters. It blinks at half the
// banner's rate, and a tap anywhere is a press of START.
//
// Nothing here starts the game. A tap only sets a flag that
// Task_TitleScreenPhase3 (src/title_screen.c) reads next to its own START test.
// Thus the fade, the music and the change to the main menu are the title
// screen's own code.
//
// While the prompt is up, the bottom-right corner also shows the build id
// (Ctr3dsBuildId, ../bridge.h) in small dim text. It shows nowhere else.

#ifndef CTR_UI_TITLE_H
#define CTR_UI_TITLE_H

#include "global.h"
#include "../bridge.h"

// Paints TOUCH TO START in the lit half of its blink, and the build id in both
// halves. Paints nothing when the title is not up. For the shell's paint before
// the game, over a cleared screen.
void UiTitleDraw(void);

// A release anywhere on the screen, while the prompt is up, is START. It counts
// in the dark half of the blink too, as a START press does.
void UiTitleTouch(const CtrTouchState *t);

// A key for what the prompt shows, for the shell's repaint hash: zero when the
// title is not up, otherwise the half of the blink. It changes only when the
// prompt changes. Nothing else in the hash changes when the prompt blinks, so
// without this the prompt does not draw.
u32 UiTitleStateKey(void);

#endif // CTR_UI_TITLE_H
