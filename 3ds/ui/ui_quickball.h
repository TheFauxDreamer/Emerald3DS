// Quick throw: the ball you last used, offered back on the bottom screen.
//
// The NDS games remembered which ball you threw and kept it in reach on the
// touch screen. Emerald has no such concept -- gLastUsedItem is reused for held
// items, Thief and the AI's own item use, so it cannot stand in -- so the
// memory is a byte of settings.bin, written by HandleAction_UseItem()
// (src/battle_util.c) and read back here.
//
// This is an OVERLAY, in the sense section 5 of 3ds/SECOND_SCREEN_CHEATSHEET.md
// means it: drawn over whichever tab just painted, and claiming its whole rect
// of touches before that tab sees any of them.

#ifndef CTR_UI_QUICKBALL_H
#define CTR_UI_QUICKBALL_H

#include "global.h"
#include "../bridge.h"

// A strip along the bottom of the content area, 40x5 tiles.
//
// The Y is the whole reason for the size. The shiny notice occupies y 40..152
// (NOTICE_* in bottom_screen.c), so starting at 152 makes the two ABUT
// EXACTLY: no overlap in either direction, no argument about which paints
// last, and no torn border in the one case where both are up at once -- which
// is a catchable shiny, i.e. precisely the case the whole feature is for.
//
// UiWindowFrame takes TILES, so these are tiles and 19*8 == 152 is not a
// coincidence to be broken casually.
#define UI_QB_TX 0
#define UI_QB_TY 19
#define UI_QB_TW 40
#define UI_QB_TH 5

#define UI_QB_X  (UI_QB_TX * 8)     // 0
#define UI_QB_Y  (UI_QB_TY * 8)     // 152
#define UI_QB_W  (UI_QB_TW * 8)     // 320
#define UI_QB_H  (UI_QB_TH * 8)     // 40

// TRUE while the strip is up, which is four questions in this order:
//
//   1. the EXTRA tab's QUICK BALL switch is on   -- cheapest, and a player who
//      turned it off should pay for none of the rest
//   2. the opponent is one you may throw a ball at (UiCatchableOpponent)
//   3. the player's controller is in action selection, so a throw is legal
//   4. there is a ball to offer
//
// Question 3 is what keeps this honest as an overlay. Every tab's layout is
// hand-fitted to a 192px content area, so a panel up for the whole of every
// wild battle would be a real cost; bound to action selection it appears
// exactly while the top screen is already asking "What will X do?" and goes
// again the moment the player answers. It also means the button is never dead.
bool8 UiQuickBallActive(void);

void UiQuickBallDraw(void);
void UiQuickBallTouch(const CtrTouchState *t);

// Clears the per-encounter state -- the cycled-to ball and any refusal message
// -- once the battle is over. Must be called every frame, not every repaint:
// the strip is DOWN for most of a battle, so it cannot do this on its own way
// out, and a message left standing would reappear over the next encounter.
void UiQuickBallTick(void);

// Cheap identity of what the strip is showing, for the shell's repaint hash.
// Zero while it is down. Nothing else in that hash moves when action selection
// opens, so without this the strip would never appear.
u32 UiQuickBallStateKey(void);

#endif // CTR_UI_QUICKBALL_H
