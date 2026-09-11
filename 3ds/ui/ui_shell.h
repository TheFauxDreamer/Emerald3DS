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
// The shiny sparkle's gold, read out of
// graphics/battle_anims/sprites/gold_stars.png -- ANIM_TAG_GOLD_STARS, the
// sprite TryShinyAnimation (src/battle_anim_throw.c) spins around a shiny when
// the encounter opens. Taking the notice's colours from that exact art is the
// point: the panel is a second telling of a thing the game already said, so it
// should be the same gold rather than a yellow chosen to look like it.
//
// The art is a ramp, not one colour -- white core, cream highlight, gold body,
// orange edge -- and the notice uses three steps of it for the same reason the
// sprite does: a single flat yellow reads as a warning, a ramp reads as metal.
//
// Hardcoded from the art for exactly the reason the HP bar colours above are.
// The anim palette is only in VRAM while that battle animation is running, and
// this panel is drawn whenever the bottom screen repaints.
#define UI_COL_SHINY       0xFE24   // RGB(255,197, 32) gold body,  pal index 7
#define UI_COL_SHINY_PALE  0xFEF3   // RGB(255,222,156) highlight,  pal index 5
#define UI_COL_SHINY_EDGE  0xFB02   // RGB(255, 98, 16) orange edge, pal index 9

// Poke Ball marker. Fixed rather than themed: the ball is recognisable by its
// colours, and it carries its own dark outline on every window frame.
#define UI_COL_BALL_TOP    0xE104   // red
#define UI_COL_BALL_BOTTOM 0xFFFF   // white

// Any view calls this after changing something the screen depends on. The host
// only re-uploads the 320x240 texture when the screen is dirty.
void UiMarkDirty(void);

// TRUE on the frames animations are allowed to advance on, roughly five times a
// second. Every animated thing on this screen asks this rather than counting
// frames itself, so that two of them moving at once still cost one repaint per
// step instead of two -- and on this screen a repaint costs a whole VBlank, so
// that is the difference between about 55fps and about 51. See the note above
// UI_ANIM_STEP_FRAMES in bottom_screen.c.
bool8 UiAnimStepped(void);

// TRUE while a modal overlay (currently only the shiny notice) is covering the
// tab. A tab that defers part of its drawing to the shell's animated layer must
// draw a STILL version of that part itself while this is TRUE: the overlay owns
// that layer while it is up, so the deferred piece is otherwise not drawn at
// all -- and it cannot simply be drawn there anyway, because the animated layer
// paints over a snapshot that already contains the panel.
bool8 UiOverlayActive(void);

// The party slot the BAG tab will act on. Set by the party grid.
u8   UiSelectedMon(void);
void UiSetSelectedMon(u8 index);

// ---- per-tab entry points ----
void UiPartyDraw(void);
void UiPartyTouch(const CtrTouchState *t);

// Advances this tab's animations by one frame: the sliding HP bars and the mon
// icons' two-frame cycle. Returns TRUE on the frames the picture actually
// changed, which the shell turns into a repaint request -- the icons change
// once every six frames, so an idle party grid asks for ten repaints a second
// rather than sixty.
//
// Must be called once per frame, not once per redraw, or the animation stalls
// whenever the screen happens not to be repainting.
//
// `visible` is whether the PARTY tab is the one on screen. FALSE returns
// immediately without asking for anything, so no other tab pays for this; the
// bars adopt the party's real values on the frame the tab comes back rather
// than sliding for damage taken while it was hidden.
bool8 UiPartyTick(bool8 visible);

// TRUE when everything that moved lives on the animated layer -- the mon icons'
// frame and/or a sliding HP bar -- so the shell can put a handful of rects back
// instead of rebuilding all 76,800 pixels. FALSE only for the detail view,
// whose HP readout is not on that layer.
bool8 UiPartyAnimOnly(void);

// Redraw the moving pieces over the restored snapshot: the icons, and the HP
// bar and number of any slot still sliding. Only valid immediately after
// UiPartyAnimOnly() returned TRUE.
void UiPartyRedrawAnimated(void);

// Cheap identity of what the PARTY tab is showing: the live level cap behind
// the cheat tags, plus the selected mon's EV total while the IV/EV panel is up.
// Walks save data (badge flags), so the shell only asks while the tab is on
// screen and there is a save block to read.
u32 UiPartyStateKey(void);

void UiBagDraw(void);
void UiBagTouch(const CtrTouchState *t);

// TRUE while the target picker is up: the one BAG view that shows party HP and
// status, so the shell folds the party into its repaint hash for it. The flag
// survives a tab switch (the modal-flag bug in the cheatsheet), so ask only
// while BAG is the tab on screen.
bool8 UiBagPickerOpen(void);

void UiMapDraw(void);
void UiMapTouch(const CtrTouchState *t);

// Cheap identity of where the player is on the region map, for the shell's
// repaint hash. Nothing else in that hash tracks the player's location, so
// without this the map would go stale while they walk.
u32 UiMapStateKey(void);

void UiDexDraw(void);
void UiDexTouch(const CtrTouchState *t);

// Port features that are not part of the original game. Two pages: page 1 is
// host-side only and touches no game state, page 2 is the gameplay tweaks and
// deliberately does.
void UiExtraDraw(void);
void UiExtraTouch(const CtrTouchState *t);

// Cheap identity of the tweak toggles and the live level-cap readout, for the
// shell's repaint hash. The cap moves when a badge is earned, which happens
// nowhere near this tab.
u32 UiExtraStateKey(void);

// The tweak state on its own, without EXTRA's page number. The PARTY tab shows
// the same settings as cheat tags, so it needs the same key. Defined in
// tab_extra.c, next to the controls that set them.
u32 UiTweakStateKey(void);

// Cheap identity of the dex counts, for the shell's repaint hash. Walks the
// whole dex, so the shell only asks while the DEX tab is on screen.
u32 UiDexStateKey(void);

#endif // CTR_UI_SHELL_H
