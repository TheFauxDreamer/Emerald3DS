// Quick throw: the last ball that the player used, offered on the bottom
// screen.
//
// The NDS games kept the last ball in reach on the touch screen. The game has
// no such value: gLastUsedItem is also used for held items, Thief and the AI.
// Thus the stored ball is a byte of settings.bin. HandleAction_UseItem()
// (src/battle_util.c) writes it, and this file reads it.
//
// It is an overlay (3ds/SECOND_SCREEN_CHEATSHEET.md, section 5). It draws over
// the current tab and takes every touch in its rect first.
//
// It starts small: a box in the bottom right corner with the ball and its
// count, so it does not cover the tab. A tap on the box opens the full strip.
// The strip's HIDE button makes it small again. It is small again at the next
// encounter.

#ifndef CTR_UI_QUICKBALL_H
#define CTR_UI_QUICKBALL_H

#include "global.h"
#include "../bridge.h"

// A strip along the bottom of the content area, 40x5 tiles.
//
// The shiny notice uses y 40..152 (NOTICE_* in bottom_screen.c). This strip
// starts at 152, so the two touch exactly with no overlap. Both are up for a
// catchable shiny, which is the main case for this feature.
//
// UiWindowFrame takes tiles, so these values are in tiles: 19*8 is 152.
#define UI_QB_TX 0
#define UI_QB_TY 19
#define UI_QB_TW 40
#define UI_QB_TH 5

#define UI_QB_X  (UI_QB_TX * 8)     // 0
#define UI_QB_Y  (UI_QB_TY * 8)     // 152
#define UI_QB_W  (UI_QB_TW * 8)     // 320
#define UI_QB_H  (UI_QB_TH * 8)     // 40

// The small box, 7x5 tiles, at the right end of the strip's row.
#define UI_QB_MINI_TW 7
#define UI_QB_MINI_TX (UI_QB_TX + UI_QB_TW - UI_QB_MINI_TW)
#define UI_QB_MINI_X  (UI_QB_MINI_TX * 8)     // 264
#define UI_QB_MINI_W  (UI_QB_MINI_TW * 8)     // 56

// TRUE while the strip is up. It asks four questions, in this order:
//
//  1. Is the EXTRA tab's QUICK BALL switch on? (Cheapest, so a player who
//     turned it off pays for nothing else.)
//  2. Can the player throw a ball at the opponent? (UiCatchableOpponent)
//  3. Is the player's controller in action selection, so a throw is legal?
//  4. Is there a ball to offer?
//
// Question 3 keeps the overlay small in time. It shows only while the top
// screen asks "What will X do?", and closes when the player answers. The button
// is thus always live.
bool8 UiQuickBallActive(void);

void UiQuickBallDraw(void);

// TRUE when the touch is on the overlay as it shows now: the small box, or the
// full strip. The shell gives the touch to UiQuickBallTouch only then, so the
// rest of the strip's row stays with the tab while the box is small.
bool8 UiQuickBallHit(const CtrTouchState *t);
void UiQuickBallTouch(const CtrTouchState *t);

// Clears the state of one encounter (the cycled-to ball and any refusal
// message) after the battle. Call it every frame, not every repaint. The strip
// is down for most of a battle, so it cannot do this itself. An old message
// would show on the next encounter.
void UiQuickBallTick(void);

// A key for what the strip shows, for the shell's repaint hash. Zero while it
// is down. Nothing else in the hash changes when action selection opens, so
// without this the strip does not appear.
u32 UiQuickBallStateKey(void);

#endif // CTR_UI_QUICKBALL_H
