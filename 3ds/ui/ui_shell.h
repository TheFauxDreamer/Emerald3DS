// Shared state and layout for the bottom-screen views (game side).
//
// The shell (bottom_screen.c) owns the tab bar, the view stack and the redraw
// policy. Each tab_*.c owns its content area and its touch handling.

#ifndef CTR_UI_SHELL_H
#define CTR_UI_SHELL_H

#include "global.h"
#include "../bridge.h"

// The tab bar is along the bottom. Everything above it is the content area.
// Both are whole 8px tiles, so the window frames land on tile boundaries.
#define UI_TABBAR_H   48
#define UI_CONTENT_H  (CTR_BOTTOM_HEIGHT - UI_TABBAR_H)   // 192 = 24 tiles

// The tabs that show depend on what the player has unlocked. The shell
// (bottom_screen.c) follows BuildNormalStartMenu() (src/start_menu.c).
//
// Six tabs is the maximum. The bar divides 320px between the visible tabs, and
// 53px is about the minimum for a finger (SECOND_SCREEN_CHEATSHEET.md, "Adding
// a tab"). TROPHY comes before EXTRA, so the port's settings stay at the right
// end.
enum UiTab
{
    UI_TAB_PARTY,
    UI_TAB_BAG,
    UI_TAB_MAP,
    UI_TAB_DEX,
    UI_TAB_TROPHY,
    UI_TAB_EXTRA,
    UI_TAB_COUNT
};

// The game loads its menu palette for each window. For the port's own UI, these
// are fixed RGB565 values near it.
#define UI_COL_TEXT     0xFFFF   // white
#define UI_COL_SHADOW   0x2124   // dark, for the shadow pixels of the font
#define UI_COL_DIM      0x8410
#define UI_COL_ACCENT   0x07FF
#define UI_COL_BG       0x18C3
// The game's HP bar colors, from graphics/battle_interface/hpbar.png (palette
// indices 10-15). Each state has a light and a dark shade, and the bar uses
// both. The values are fixed, because the healthbox palette loads only in
// battle and this bar also shows outside battle.
#define UI_COL_HP_HIGH    0x5EB0   // RGB( 90,213,131)
#define UI_COL_HP_HIGH_L  0x77F5   // RGB(115,255,172)
#define UI_COL_HP_MID     0xCD61   // RGB(205,172,  8)
#define UI_COL_HP_MID_L   0xFF27   // RGB(255,230, 57)
#define UI_COL_HP_LOW     0xAA09   // RGB(172, 65, 74)
#define UI_COL_HP_LOW_L   0xFAC7   // RGB(255, 90, 57)
#define UI_COL_HP_BACK    0x1082
// The gold of the shiny sparkle, from
// graphics/battle_anims/sprites/gold_stars.png (ANIM_TAG_GOLD_STARS, which
// TryShinyAnimation shows around a shiny). The notice tells the same news as
// the game, so it uses the same gold.
//
// The art is a ramp, not one color: a white core, a cream highlight, a gold
// body and an orange edge. The notice uses three steps of it. A flat yellow
// looks like a warning. A ramp looks like metal.
//
// The values are fixed, for the same reason as the HP bar colors. The animation
// palette is in VRAM only while that animation runs.
#define UI_COL_SHINY       0xFE24   // RGB(255,197, 32) gold body,  pal index 7
#define UI_COL_SHINY_PALE  0xFEF3   // RGB(255,222,156) highlight,  pal index 5
#define UI_COL_SHINY_EDGE  0xFB02   // RGB(255, 98, 16) orange edge, pal index 9

// The achievement categories (ACH_CAT_* in 3ds/achievements.h). Each has a ramp
// with the shape of the gold one: a pale core, a middle body and a dark edge.
// That shape makes them safe on a window frame. The sparkle art takes any of
// them (UiSparkleRamp). A title in the body color with the edge as its shadow
// is legible on the lightest and the darkest frame. Story uses the gold itself.
#define UI_COL_ACH_GREEN_PALE   0xBFF7   // RGB(190,255,190)  Legendary
#define UI_COL_ACH_GREEN        0x468C   // RGB( 64,208, 96)
#define UI_COL_ACH_GREEN_EDGE   0x1386   // RGB( 16,112, 48)
#define UI_COL_ACH_RED_PALE     0xFDF6   // RGB(255,190,180)  Pokemon
#define UI_COL_ACH_RED          0xEA07   // RGB(232, 64, 56)
#define UI_COL_ACH_RED_EDGE     0x90C3   // RGB(144, 24, 24)
#define UI_COL_ACH_PURPLE_PALE  0xEE5F   // RGB(232,200,255)  Battle
#define UI_COL_ACH_PURPLE       0xAB1D   // RGB(168, 96,232)
#define UI_COL_ACH_PURPLE_EDGE  0x5953   // RGB( 88, 40,152)
#define UI_COL_ACH_BLUE_PALE    0xBF1F   // RGB(184,224,255)  Extras
#define UI_COL_ACH_BLUE         0x3C9F   // RGB( 56,144,248)
#define UI_COL_ACH_BLUE_EDGE    0x1255   // RGB( 16, 72,168)
#define UI_COL_ACH_PINK_PALE    0xFE9D   // RGB(255,208,236)  Contests
#define UI_COL_ACH_PINK         0xF397   // RGB(240,112,184)
#define UI_COL_ACH_PINK_EDGE    0x994D   // RGB(152, 40,104)

struct UiRamp
{
    u16 pale, body, edge;
};

// The ramp for an ACH_CAT_* value. Gold for any value that it does not know.
// Defined in tab_trophy.c. The achievement toast uses it too.
const struct UiRamp *UiAchCategoryRamp(u8 category);

// The colors of a battle partner's Pokemon (Steven at the Space Center, the
// Battle Frontier's multi partners). They come from the game's party menu: the
// PARTY_PAL_MULTI_ALT boxes, graphics/party_menu/bg.png indices 68-70. The
// values are fixed, as for the HP bar. The ground is index 68, the box fill, at
// 45% over white. All 20 window frames have a white interior, so the dark theme
// text is legible on it. See ui_team.h.
#define UI_COL_ALLY        0xC523   // RGB(197,164, 24) pal index 69
#define UI_COL_ALLY_EDGE   0x9CE4   // RGB(156,156, 32) pal index 70
#define UI_COL_ALLY_GROUND 0xEF36   // RGB(236,229,181) index 68 over white

// The Poke Ball marker. The colors are fixed, not themed: a ball is known by
// its colors, and it has its own dark outline on every window frame.
#define UI_COL_BALL_TOP    0xE104   // red
#define UI_COL_BALL_BOTTOM 0xFFFF   // white

// Each view calls this after it changes something that the screen shows. The
// host uploads the 320x240 texture only when the screen is dirty.
void UiMarkDirty(void);

// TRUE on the frames when animations can advance: every sixth frame with a
// second core, every twelfth without.
//
// On the single-core path, every animation on this screen uses this and does
// not count frames itself. Two moving things then cost one repaint for each
// step, not two. There, each repaint costs a VBlank. With a second core, a
// repaint costs the frame nothing. The party icons then use this at the game's
// pace, and the shiny notice counts its own frames. See UI_ANIM_STEP_FRAMES in
// bottom_screen.c.
bool8 UiAnimStepped(void);

// TRUE while an overlay (the shiny notice, the quick-throw strip or the
// achievement toast) covers the tab.
//
// A tab that puts parts of its drawing on the shell's animated layer must draw
// those parts into its own paint while this is TRUE. The overlay owns that
// layer, which paints over a snapshot that contains the panel. On the
// second-core path, the shell repaints fully for each step, so those parts can
// move. On the single-core path, draw them still.
bool8 UiOverlayActive(void);

// The tab on the screen, one of enum UiTab. For an overlay that must know: the
// achievement toast has no VIEW button on the TROPHY tab.
u8 UiActiveTab(void);

// The party slot that the BAG tab acts on. The party grid sets it.
u8   UiSelectedMon(void);
void UiSetSelectedMon(u8 index);

// ---- per-tab entry points ----
void UiPartyDraw(void);
void UiPartyTouch(const CtrTouchState *t);

// Advances this tab's animations by one frame: the sliding HP bars, the
// two-frame icon cycle, and a status badge that alternates between two tags
// (status_tags.h). Returns TRUE on the frames where the picture changed. The
// shell then asks for a repaint. The icons change only on UiAnimStepped()
// frames. A badge flip occurs on one of those frames. With BATTLE ANIM off in
// battle, only the flip asks for a repaint.
//
// Call it once each frame, not once for each redraw. Otherwise the animation
// stops when the screen does not repaint.
//
// `visible` tells if the PARTY tab is on the screen. FALSE returns at once, so
// no other tab pays for this. When the tab comes back, the bars take the real
// values and do not slide.
bool8 UiPartyTick(bool8 visible);

// TRUE when everything that moved is on the animated layer (the icon frame, a
// sliding HP bar, a badge flip). The shell then restores a few rects and does
// not repaint all 76,800 pixels. FALSE only for an HP slide in the detail view,
// where the HP readout is not on that layer.
bool8 UiPartyAnimOnly(void);

// Draw the moving parts again over the restored snapshot. On the grid, these
// are the icon, HP bar, HP number and status badge of every slot. A full paint
// leaves all of them out of the snapshot. In the detail view, they are its icon
// and its badge. Valid only just after UiPartyAnimOnly() returned TRUE, or as
// the last step of a full paint.
void UiPartyRedrawAnimated(void);

// A key for what the PARTY tab shows. It holds the live level cap of the cheat
// tags, and the selected mon's EV total while the IV/EV panel is up. It reads
// save data (badge flags), so the shell asks only while the tab shows and a
// save exists.
u32 UiPartyStateKey(void);

void UiBagDraw(void);
void UiBagTouch(const CtrTouchState *t);

// TRUE while the target picker is up. It is the one BAG view that shows party
// HP and status, so the shell folds the party into its hash for it. The flag
// stays after a tab switch (the modal-flag bug in the cheatsheet), so ask only
// while BAG is on the screen.
bool8 UiBagPickerOpen(void);

void UiMapDraw(void);
void UiMapTouch(const CtrTouchState *t);
// Clears the FLY and ESCAPE confirms. The shell calls it when the MAP tab is
// left, so a YES never waits on a screen the player went away from.
void UiMapLeave(void);

// A key for the player's position on the region map, for the shell's repaint
// hash. Nothing else in the hash tracks the position, so without this the map
// goes stale while the player walks.
u32 UiMapStateKey(void);

void UiDexDraw(void);
void UiDexTouch(const CtrTouchState *t);

// Port features that are not part of the original game. Page 1 is host side
// only and changes no game state. Page 2 has the gameplay tweaks, which do.
void UiExtraDraw(void);
void UiExtraTouch(const CtrTouchState *t);

// A key for the tweak toggles and the live level-cap value, for the shell's
// repaint hash. The cap changes when the player gets a badge, away from this
// tab.
u32 UiExtraStateKey(void);

// The tweak state without EXTRA's page number. The PARTY tab shows the same
// settings as cheat tags, so it needs the same key. Defined in tab_extra.c,
// next to the controls.
u32 UiTweakStateKey(void);

// A key for the dex counts, for the shell's repaint hash. It walks the full
// dex, so the shell asks only while the DEX tab shows.
u32 UiDexStateKey(void);

// The achievements list (tab_trophy.c). It reads everything through AchActive()
// in 3ds/achievements.h, so it works the same for any provider.
void UiTrophyDraw(void);
void UiTrophyTouch(const CtrTouchState *t);

// Call each frame, like UiPartyTick. `visible` tells if TROPHY is on the
// screen. When the tab comes on the screen, this copies the unseen rows into
// the tab's NEW tags and marks them seen. When the tab goes, the tags go. It
// must run before the state hash, or a tab switch paints the list before it
// knows the new rows.
void UiTrophyTick(bool8 visible);

// A key for the counters of the visible rows, for the shell's repaint hash.
// They change without a touch on this tab: a catch, a hatch, a trainer battle.
u32 UiTrophyStateKey(void);

// The number of achievements with a title or a description that is too wide for
// the list, for the debug page. Nothing on this screen clips text.
u16 UiTrophyTooWide(void);

#endif // CTR_UI_SHELL_H
