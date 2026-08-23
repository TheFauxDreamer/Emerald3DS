// Shared state and layout for the bottom-screen views (game side).
//
// bottom_screen.c owns the tab bar, the view stack and the redraw policy; each
// tab_*.c owns its own content area and touch handling.

#ifndef CTR_UI_SHELL_H
#define CTR_UI_SHELL_H

#include "global.h"
#include "../bridge.h"

// The tab bar sits along the bottom; everything above it is the content area.
// Kept to whole 8px tiles so the window frames land on tile boundaries.
#define UI_TABBAR_H   48
#define UI_CONTENT_H  (CTR_BOTTOM_HEIGHT - UI_TABBAR_H)   // 192 = 24 tiles

enum UiTab
{
    UI_TAB_PARTY,
    UI_TAB_BAG,
    UI_TAB_MAP,
    UI_TAB_COUNT
};

// Emerald's menu palette is loaded per-window; for our own chrome these are
// fixed RGB565 approximations of it.
#define UI_COL_TEXT     0xFFFF   // white
#define UI_COL_SHADOW   0x2124   // dark, for the font's shadow pixels
#define UI_COL_DIM      0x8410
#define UI_COL_ACCENT   0x07FF
#define UI_COL_BG       0x18C3
#define UI_COL_HP_HIGH  0x2648
#define UI_COL_HP_MID   0xE604
#define UI_COL_HP_LOW   0xE1C7
#define UI_COL_HP_BACK  0x1082

// Any view calls this after changing something the screen depends on. The host
// only re-uploads the 320x240 texture when the screen is dirty.
void UiMarkDirty(void);

// The party slot the BAG tab will act on. Set by the party grid.
u8   UiSelectedMon(void);
void UiSetSelectedMon(u8 index);

// ---- per-tab entry points ----
void UiPartyDraw(void);
void UiPartyTouch(const CtrTouchState *t);

void UiBagDraw(void);
void UiBagTouch(const CtrTouchState *t);

void UiMapDraw(void);
void UiMapTouch(const CtrTouchState *t);

#endif // CTR_UI_SHELL_H
