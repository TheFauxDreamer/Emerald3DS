// The achievement toast: a strip across the top of the bottom screen that says
// something was just unlocked, over whichever tab is up.
//
// This is an OVERLAY in the sense section 5 of 3ds/SECOND_SCREEN_CHEATSHEET.md
// means it, the third after the shiny notice and the quick-throw strip, and it
// has the quick-throw strip's shape: its own file, and the shell calls Active,
// Draw, Touch, StateKey and Tick.
//
// It follows the four overlay rules: painted after the tab, takes every touch
// inside its rect, has a real control (VIEW, which opens the TROPHY tab), and
// expires on its own.

#ifndef CTR_UI_ACHTOAST_H
#define CTR_UI_ACHTOAST_H

#include "global.h"
#include "../bridge.h"

// 40x5 tiles along the top of the content area, y 0..40.
//
// The Y is the whole reason for the size, the same argument ui_quickball.h
// makes from the other end. The shiny notice occupies y 40..152 and the strip
// y 152..192, so ending at 40 makes all three ABUT EXACTLY and tile the 192px
// content area between them: no overlap, no torn border, and all three can be
// up together -- a catchable shiny caught by the quick throw, say, which is
// precisely when a shiny-catch achievement unlocks.
//
// UiWindowFrame takes TILES, so these are tiles, and 5*8 == 40 is not a
// coincidence to be broken casually.
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

// Takes a touch inside the strip. Returns TRUE when VIEW was tapped, which the
// shell answers by switching to the TROPHY tab; the toast has already gone.
bool8 UiAchToastTouch(const CtrTouchState *t);

// Once a frame, from the shell, while the game is running: counts the toast
// down and brings on the next one waiting.
void  UiAchToastTick(void);

// Cheap identity of what the strip shows, for the shell's repaint hash. Zero
// while it is down, and different for every toast, so two in a row still
// repaint between them.
u32   UiAchToastStateKey(void);

#endif // CTR_UI_ACHTOAST_H
