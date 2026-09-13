// The achievement toast: a strip across the top of the bottom screen that
// announces an unlock, over the current tab.
//
// It is an overlay (3ds/SECOND_SCREEN_CHEATSHEET.md, section 5), the third
// after the shiny notice and the quick-throw strip. It has the strip's shape:
// its own file, and the shell calls Active, Draw, Touch, StateKey and Tick.
//
// It follows the four overlay rules:
// - It paints after the tab.
// - It takes every touch in its rect.
// - It has a real control: VIEW, which opens the TROPHY tab.
// - It closes by itself.

#ifndef CTR_UI_ACHTOAST_H
#define CTR_UI_ACHTOAST_H

#include "global.h"
#include "../bridge.h"

// 40x5 tiles along the top of the content area, y 0..40.
//
// The shiny notice uses y 40..152 and the strip uses y 152..192. Thus the three
// touch exactly and fill the 192px content area, with no overlap. All three can
// be up at the same time, for example when the quick throw catches a shiny.
//
// UiWindowFrame takes tiles, so these values are in tiles: 5*8 is 40.
#define UI_AT_TX 0
#define UI_AT_TY 0
#define UI_AT_TW 40
#define UI_AT_TH 5

#define UI_AT_X  (UI_AT_TX * 8)     // 0
#define UI_AT_Y  (UI_AT_TY * 8)     // 0
#define UI_AT_W  (UI_AT_TW * 8)     // 320
#define UI_AT_H  (UI_AT_TH * 8)     // 40

bool8 UiAchToastActive(void);
void  UiAchToastDraw(void);

// Takes a touch inside the strip. Returns TRUE when VIEW was tapped. The shell
// then switches to the TROPHY tab. The toast is already closed.
bool8 UiAchToastTouch(const CtrTouchState *t);

// Called once each frame by the shell, while the game runs. It counts down the
// toast and starts the next one in the queue.
void  UiAchToastTick(void);

// A key for what the strip shows, for the shell's repaint hash. Zero while it
// is down, and different for each toast, so two toasts in a row both repaint.
u32   UiAchToastStateKey(void);

#endif // CTR_UI_ACHTOAST_H
