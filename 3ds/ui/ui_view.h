// The views that open over a tab: which one is up, and how to get back (game
// side).
//
// A view is a screen that opens from a tab and has a way back: the PARTY
// detail, a DEX entry, BAG's target picker, MAP's encounter list, a HOME page
// and LINK's trainer cards. Each one was a flag in its own file, and a flag
// survived a tab switch: leave a detail view by tapping another tab, come back,
// and the detail was still open. Here they are one stack, and the shell empties
// it whenever the tab changes (UiViewReset), so that cannot happen.
//
// The tabs still draw and dispatch their own views. A tab asks
// UiViewIsOpen(id) where it used to test its flag, pushes where it used to set
// it, and pops where it used to clear it. The stack only owns the state.
//
// To add a view: one line in enum UiViewId, and push it from its tab. A view
// must take everything it needs from its `arg` or from the game when it draws,
// so nothing can outlive a pop.

#ifndef CTR_UI_VIEW_H
#define CTR_UI_VIEW_H

#include "global.h"

enum UiViewId
{
    UI_VIEW_NONE,
    UI_VIEW_PARTY_DETAIL,     // tab_party.c: one mon's stats and moves
    UI_VIEW_DEX_ENTRY,        // tab_dex.c: one species' entry
    UI_VIEW_BAG_PICK,         // tab_bag.c: arg is the item to use
    UI_VIEW_MAP_ENCOUNTERS,   // view_encounters.c
    UI_VIEW_HOME_PAGE,        // tab_extra.c: arg is the page
    UI_VIEW_LINK_CARDS,       // ui_link.c, over the LINK page
    UI_VIEW_COUNT,
};

// The deepest the stack goes. HOME, a page, and LINK's cards over the LINK
// page is two views; four leaves room.
#define UI_VIEW_DEPTH 4

// Both mark the screen dirty. A push past the depth does nothing, so a caller
// must not assume the view opened; UiViewIsOpen says. A pop of an empty stack
// does nothing.
void  UiViewPush(u8 id, u16 arg);
void  UiViewPop(void);

// Empties the stack. The shell calls it on every tab change, and on a tap on
// the tab that is already up, which returns that tab to its top.
void  UiViewReset(void);

// TRUE when `id` is anywhere on the stack. A tab tests its own views with this,
// so a view under another one still counts as open (LINK's page under its
// cards).
bool8 UiViewIsOpen(u8 id);

// The view on top, or UI_VIEW_NONE.
u8    UiViewTop(void);

// The arg that `id` was pushed with, or 0 if it is not open.
u16   UiViewArg(u8 id);

// The whole stack as one value, for the shell's repaint hash. Push and pop mark
// the screen dirty already; this is so the hash cannot disagree with what
// shows.
u32   UiViewKey(void);

#endif // CTR_UI_VIEW_H
