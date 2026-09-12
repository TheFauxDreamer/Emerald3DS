// TOUCH TO START: the title screen's PRESS START, mirrored on the bottom screen.
//
// Before the game proper is running the bottom screen is blank and takes no
// touches. This is the one exception. While the title's PRESS START banner is
// up, the bottom screen shows TOUCH TO START in the banner's own lettering,
// blinking on the banner's clock at half its rate, and a tap anywhere counts as
// a press of START.
//
// Nothing about starting the game happens here. A tap only raises a flag that
// Task_TitleScreenPhase3 (src/title_screen.c) reads beside its own START test,
// so the fade, the music and the hand-off to the main menu are the title
// screen's own code rather than a second copy of it.

#ifndef CTR_UI_TITLE_H
#define CTR_UI_TITLE_H

#include "global.h"
#include "../bridge.h"

// Paints TOUCH TO START in the lit half of its blink, and nothing otherwise, so
// the dark half is simply the blank screen. For the shell's pre-game paint,
// over a cleared screen.
void UiTitleDraw(void);

// A release anywhere on the screen, while the prompt is up, is START. It still
// counts in the dark half of the blink, as a press of START does.
void UiTitleTouch(const CtrTouchState *t);

// Cheap identity of what the prompt is showing, for the shell's repaint hash:
// zero off the title, otherwise which half of the blink it is in. It changes
// only when the prompt does, never on the frames between. Nothing else in that
// hash moves when the prompt blinks, so without this it would never be drawn.
u32 UiTitleStateKey(void);

#endif // CTR_UI_TITLE_H
