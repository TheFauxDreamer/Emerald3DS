// The HOME pages that show game data, as the Poketch apps of the DS games do
// (game side). Each page is a tile of the HOME launcher (tab_extra.c) and has
// its own file:
//
// - TRAINER (view_trainer.c): the player's trainer card and records.
// - CLOCK (view_clock.c): the game's clock, play time, the daily events (the
//   Shoal Cave tide, the lottery, Mirage Island), steps, Repel and eggs.
// - DOWSING (view_dowsing.c): the Itemfinder's range as a radar.
// - BERRIES (view_berries.c): every planted berry tree.
// - DAY CARE (view_daycare.c): the Day Care's Pokemon and the old man's words.
// - FRIENDSHIP (view_friendship.c): each Pokemon's friendship, as hearts.
// - FRONTIER (view_frontier.c): the Battle Points, and each facility's
//   symbols and best record.
//
// All of them only read. Each reads save data, so the launcher opens them only
// when a save is loaded. Each key is for the shell's repaint hash, and changes
// when the page's picture changes with no touch.

#ifndef CTR_VIEW_HOME_H
#define CTR_VIEW_HOME_H

#include "global.h"
#include "../bridge.h"

// The first row under a page's title line, and the page's left edge.
#define UI_PAGE_TOP   30
#define UI_PAGE_LEFT  16

// TRAINER uses the whole content area, as the LINK page's card view does. It
// draws its own BACK, which pops the page.
void UiTrainerPageOpen(void);
void UiTrainerPageDraw(void);
void UiTrainerPageTouch(const CtrTouchState *t);
u32  UiTrainerPageKey(void);

void UiClockPageDraw(void);
u32  UiClockPageKey(void);

// FALSE until the bag holds the Itemfinder. The page then says so.
bool8 UiDowsingAvailable(void);
void  UiDowsingPageDraw(void);
u32   UiDowsingPageKey(void);

void UiBerriesPageOpen(void);
void UiBerriesPageDraw(void);
void UiBerriesPageTouch(const CtrTouchState *t);
u32  UiBerriesPageKey(void);

void UiDaycarePageDraw(void);
u32  UiDaycarePageKey(void);

void UiFriendshipPageDraw(void);
u32  UiFriendshipPageKey(void);

// FALSE until the player reaches the Battle Frontier. The tile is dim and does
// not open until then.
bool8 UiFrontierReached(void);
void  UiFrontierPageDraw(void);
u32   UiFrontierPageKey(void);

// The game's local time: the clock chip, less the offset the player set at the
// wall clock. It goes into `out`, so gLocalTime does not change. In
// view_clock.c.
struct Time;
void UiGameTime(struct Time *out);

#endif // CTR_VIEW_HOME_H
