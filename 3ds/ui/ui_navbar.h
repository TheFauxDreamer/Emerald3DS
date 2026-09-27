// The nav bar along the bottom of the touch screen (game side).
//
// Five slots with an icon and a small label: HOME in the center, wider than
// the others, and four tabs that the player picks from PARTY, BAG, MAP, DEX
// and TROPHY. The default is PARTY, BAG, HOME, MAP, TROPHY. The one tab that
// is not picked is a tile on HOME (tab_extra.c).
//
// A slot keeps its place when its tab is not unlocked yet. It shows a dim
// icon and does nothing, so HOME stays in the center and the bar never moves.
//
// The choice is a console setting in settings.bin (Ctr3dsGetNavConfig in
// bridge.h). This file gives the bits their meaning and checks them. The NAV
// BAR page of SETTINGS changes them.

#ifndef CTR_UI_NAVBAR_H
#define CTR_UI_NAVBAR_H

#include "global.h"
#include "../bridge.h"

#define UI_NAV_SLOTS     5
#define UI_NAV_HOME_SLOT 2
// The slots that the player picks, in bar order: 0, 1, 3 and 4.
#define UI_NAV_PICKS     4

// The tab in each slot, left to right. Slot UI_NAV_HOME_SLOT is always
// UI_TAB_EXTRA. A config that is not valid gives the default bar.
void UiNavSlots(u8 out[UI_NAV_SLOTS]);

// The one tab of PARTY, BAG, MAP, DEX and TROPHY that is not on the bar.
u8 UiNavSpareTab(void);

// TRUE when the slots show their labels under the icons.
bool8 UiNavLabels(void);

// Draws the bar in y UI_CONTENT_H..CTR_BOTTOM_HEIGHT. `activeTab` is the tab
// on the screen. When it is the spare tab, HOME shows as active: the player
// came from there.
void UiNavDraw(u8 activeTab);

// The tab of the slot under the touch, or UI_TAB_COUNT for none or for a slot
// whose tab is not unlocked.
u8 UiNavHit(const CtrTouchState *t);

// A tab's name as the bar shows it, in ASCII.
const char *UiNavTabName(u8 tab);

// A tab's icon, 16x16 (HOME 20x20), with its top left at (x, y).
#define UI_NAV_ICON      16
#define UI_NAV_ICON_HOME 20
void UiNavDrawIcon(int x, int y, u8 tab, u16 color);

// For the NAV BAR page. UiNavPut puts `tab` in pick `pick` (0..3, left to
// right). When the tab is already in another pick, the two change places.
void UiNavPut(u8 pick, u8 tab);
void UiNavSetLabels(bool8 on);
void UiNavReset(void);

#endif // CTR_UI_NAVBAR_H
