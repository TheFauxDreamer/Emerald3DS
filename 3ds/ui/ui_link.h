// LINK: pairing for the Cable Club over 3DS local wireless.
//
// This is page 5 of the EXTRA tab, not a tab of its own. The tab bar holds six
// tabs at 53px each, which is the practical floor for a finger
// (3ds/SECOND_SCREEN_CHEATSHEET.md, section 5). A seventh tab does not fit.
// When the HOME launcher of 3ds/SECOND_SCREEN_PLAN.md lands, this panel becomes
// a tile of it. Only the three entry points below must move.
//
// The file is ui_link.c, not link.c. 3ds/build_objs.sh globs 3ds/ui/*.c into
// the same object directory as src/*.c, so link.c here would overwrite
// src/link.o and remove the game's link code from the archive, with no error at
// any stage. See cheatsheet section 12.

#ifndef CTR_UI_LINK_H
#define CTR_UI_LINK_H

#include "global.h"
#include "../bridge.h"

// Draws in the content area of the EXTRA window. The caller draws the frame and
// the pager, as it does for every other page.
void UiLinkPageDraw(void);
void UiLinkPageTouch(const CtrTouchState *t);

// The panel shows the wireless state, which changes with no touch: a peer joins
// or drops on its own schedule. Without a state key the panel repaints only on
// a touch and shows a stale player count.
//
// The result is already in position for UiExtraStateKey(), which owns the other
// bits of that value. This uses bits 19-23 and 28-31.
u32 UiLinkPageStateKey(void);

#endif // CTR_UI_LINK_H
