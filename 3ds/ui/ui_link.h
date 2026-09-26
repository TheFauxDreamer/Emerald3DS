// LINK: pairing for the Cable Club over 3DS local wireless.
//
// This is the LINK tile of the HOME tab (tab_extra.c), not a tab of its own.
// The tab bar holds six tabs at 53px each, which is the practical floor for a
// finger (3ds/SECOND_SCREEN_CHEATSHEET.md, section 5). A seventh tab does not
// fit.
//
// The file is ui_link.c, not link.c. 3ds/build_objs.sh globs 3ds/ui/*.c into
// the same object directory as src/*.c, so link.c here would overwrite
// src/link.o and remove the game's link code from the archive, with no error at
// any stage. See cheatsheet section 12.

#ifndef CTR_UI_LINK_H
#define CTR_UI_LINK_H

#include "global.h"
#include "../bridge.h"

// Draws in the content area of the HOME page. The caller draws the frame and
// the title line, as it does for every other page -- unless UiLinkPageFullBleed()
// says otherwise.
void UiLinkPageDraw(void);

// TRUE while the trainer card view is up. A card is 240x160, a whole GBA
// screen, and the window frame plus the title line do not leave room for one, so the
// page takes the whole content area and draws its own way back.
int UiLinkPageFullBleed(void);
void UiLinkPageTouch(const CtrTouchState *t);

// The panel shows the wireless state, which changes with no touch: a peer joins
// or drops on its own schedule. Without a state key the panel repaints only on
// a touch and shows a stale player count.
//
// The result is already in position for UiExtraStateKey(), which owns the other
// bits of that value. This uses bits 19-23 and 28-31.
u32 UiLinkPageStateKey(void);

#endif // CTR_UI_LINK_H
