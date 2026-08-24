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

// Which of these are actually shown depends on what the player has unlocked;
// bottom_screen.c mirrors BuildNormalStartMenu() (src/start_menu.c).
enum UiTab
{
    UI_TAB_PARTY,
    UI_TAB_BAG,
    UI_TAB_MAP,
    UI_TAB_DEX,
    UI_TAB_EXTRA,
    UI_TAB_LINK,
    UI_TAB_COUNT
};

// Emerald's menu palette is loaded per-window; for our own chrome these are
// fixed RGB565 approximations of it.
#define UI_COL_TEXT     0xFFFF   // white
#define UI_COL_SHADOW   0x2124   // dark, for the font's shadow pixels
#define UI_COL_DIM      0x8410
#define UI_COL_ACCENT   0x07FF
#define UI_COL_BG       0x18C3
// The game's own HP bar colours, taken from graphics/battle_interface/hpbar.png
// (palette indices 10-15). Each state has a light and a dark shade and the real
// bar uses both, which is what gives it depth rather than looking like a flat
// block. Hardcoded from the art rather than read from the palette at runtime,
// because the healthbox palette is only loaded during battle and this bar is
// shown outside battle too.
#define UI_COL_HP_HIGH    0x5EB0   // RGB( 90,213,131)
#define UI_COL_HP_HIGH_L  0x77F5   // RGB(115,255,172)
#define UI_COL_HP_MID     0xCD61   // RGB(205,172,  8)
#define UI_COL_HP_MID_L   0xFF27   // RGB(255,230, 57)
#define UI_COL_HP_LOW     0xAA09   // RGB(172, 65, 74)
#define UI_COL_HP_LOW_L   0xFAC7   // RGB(255, 90, 57)
#define UI_COL_HP_BACK    0x1082
// Poke Ball marker. Fixed rather than themed: the ball is recognisable by its
// colours, and it carries its own dark outline on every window frame.
#define UI_COL_BALL_TOP    0xE104   // red
#define UI_COL_BALL_BOTTOM 0xFFFF   // white

// Any view calls this after changing something the screen depends on. The host
// only re-uploads the 320x240 texture when the screen is dirty.
void UiMarkDirty(void);

// The party slot the BAG tab will act on. Set by the party grid.
u8   UiSelectedMon(void);
void UiSetSelectedMon(u8 index);

// ---- per-tab entry points ----
void UiPartyDraw(void);
void UiPartyTouch(const CtrTouchState *t);

// Advances the HP bar animation by one frame. Returns TRUE while any bar is
// still moving, which the shell turns into a repaint request. Must be called
// once per frame, not once per redraw, or the animation stalls whenever the
// screen happens not to be repainting.
bool8 UiPartyTick(void);

void UiBagDraw(void);
void UiBagTouch(const CtrTouchState *t);

void UiMapDraw(void);
void UiMapTouch(const CtrTouchState *t);

void UiDexDraw(void);
void UiDexTouch(const CtrTouchState *t);

// Pairing for the Cable Club over local wireless. Pairing only: once connected,
// the game's own Cable Club does the rest.
void UiLinkDraw(void);
void UiLinkTouch(const CtrTouchState *t);

// Port features that are not part of the original game (fast-forward, and
// whatever follows it). Nothing here touches game state.
void UiExtraDraw(void);
void UiExtraTouch(const CtrTouchState *t);

// Cheap identity of the dex counts, for the shell's repaint hash. Walks the
// whole dex, so the shell only asks while the DEX tab is on screen.
u32 UiDexStateKey(void);

#endif // CTR_UI_SHELL_H
