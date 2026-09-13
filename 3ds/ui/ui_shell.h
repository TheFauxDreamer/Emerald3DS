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
//
// Six is the cap. The bar divides 320px between the visible tabs, and six is
// 53px each, about the floor for a fingertip (SECOND_SCREEN_CHEATSHEET.md,
// "Adding a tab"). TROPHY sits before EXTRA so the port's settings stay at the
// right-hand end.
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

// The achievement categories (ACH_CAT_* in 3ds/achievements.h), one ramp each
// in the gold one's shape: a pale core, a mid-tone body and a dark edge. That
// shape is what makes them safe on a window frame. The sparkle art takes any of
// them (UiSparkleRamp), and a title in the body colour with the edge as its
// shadow reads on the lightest frame (the edge outlines it) and the darkest (the
// body carries it), the way the shiny notice's gold headline does on its own
// dark ground. Story is that gold itself, so it has no constants of its own.
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

// The ramp for an ACH_CAT_* value, and gold for anything it does not know, which
// is what every achievement looked like before categories. Defined in
// tab_trophy.c; the achievement toast shares it.
const struct UiRamp *UiAchCategoryRamp(u8 category);

// Poke Ball marker. Fixed rather than themed: the ball is recognisable by its
// colours, and it carries its own dark outline on every window frame.
#define UI_COL_BALL_TOP    0xE104   // red
#define UI_COL_BALL_BOTTOM 0xFFFF   // white

// Any view calls this after changing something the screen depends on. The host
// only re-uploads the 320x240 texture when the screen is dirty.
void UiMarkDirty(void);

// TRUE on the frames animations are allowed to advance on: every sixth frame
// with the rasteriser on its own core, every twelfth without one.
//
// On the single-core path every animated thing on this screen asks this rather
// than counting frames itself, so that two of them moving at once still cost
// one repaint per step instead of two -- and there a repaint costs a whole
// VBlank, so that is the difference between about 55fps and about 51. With a
// second core a repaint costs the frame nothing, so the party's icons use this
// at the game's own pace and the shiny notice counts frames of its own. See
// the note above UI_ANIM_STEP_FRAMES in bottom_screen.c.
bool8 UiAnimStepped(void);

// TRUE while an overlay (the shiny notice, the quick-throw strip or the
// achievement toast) is covering the tab. A tab that defers part of its
// drawing to the shell's animated layer must draw that part into its own
// paint while this is TRUE: the overlay owns that layer while it is up, so
// the deferred piece is otherwise not drawn at all -- and it cannot simply be
// drawn there anyway, because the animated layer paints over a snapshot that
// already contains the panel. On the second-core path the shell repaints
// fully for every step while an overlay is up, so what the tab draws there may
// move; on the single-core path it does not, so draw a still.
bool8 UiOverlayActive(void);

// The tab on screen, one of enum UiTab. For an overlay that has to know, the
// way the achievement toast drops its VIEW button on the TROPHY tab itself.
u8 UiActiveTab(void);

// The party slot the BAG tab will act on. Set by the party grid.
u8   UiSelectedMon(void);
void UiSetSelectedMon(u8 index);

// ---- per-tab entry points ----
void UiPartyDraw(void);
void UiPartyTouch(const CtrTouchState *t);

// Advances this tab's animations by one frame: the sliding HP bars, the mon
// icons' two-frame cycle, and a status badge alternating between two tags
// (status_tags.h). Returns TRUE on the frames the picture actually
// changed, which the shell turns into a repaint request -- the icons change
// only on UiAnimStepped() frames, so an idle party grid asks for ten repaints a
// second (five on the single-core path) rather than sixty. A badge flip lands
// on one of those same frames. With BATTLE ANIM off in battle, the flip is the
// only thing that still asks.
//
// Must be called once per frame, not once per redraw, or the animation stalls
// whenever the screen happens not to be repainting.
//
// `visible` is whether the PARTY tab is the one on screen. FALSE returns
// immediately without asking for anything, so no other tab pays for this; the
// bars adopt the party's real values on the frame the tab comes back rather
// than sliding for damage taken while it was hidden.
bool8 UiPartyTick(bool8 visible);

// TRUE when everything that moved lives on the animated layer (the mon icons'
// frame, a sliding HP bar, a status badge changing tag), so the shell can put
// a handful of rects back instead of rebuilding all 76,800 pixels. FALSE only
// for an HP slide in the detail view, whose HP readout is not on that layer.
bool8 UiPartyAnimOnly(void);

// Redraw the moving pieces over the restored snapshot: on the grid, every
// slot's icon, HP bar, HP number and status badge, moving or not, since a full
// paint leaves all of them out of the snapshot; in the detail view, its one
// icon and its badge. Only valid immediately after UiPartyAnimOnly() returned
// TRUE, or as the last step of a full paint.
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

// The achievements list (tab_trophy.c). It reads everything through
// AchActive() in 3ds/achievements.h, so it works the same for any provider.
void UiTrophyDraw(void);
void UiTrophyTouch(const CtrTouchState *t);

// Per frame, on UiPartyTick's terms: `visible` is whether TROPHY is the tab on
// screen. On the frame it comes on screen this copies what is unseen into the
// tab's own NEW tags and marks it all seen; on the frame it goes, the tags go.
// Must run every frame, before the state hash, or a tab switch would paint the
// list before it knew which rows are new.
void UiTrophyTick(bool8 visible);

// Cheap identity of the visible rows' counters, for the shell's repaint hash.
// They move with no touch on this tab: a catch, a hatch, a trainer battle.
u32 UiTrophyStateKey(void);

// How many achievements have a title or a description too wide for the list,
// for the debug page's check. Nothing on this screen clips text.
u16 UiTrophyTooWide(void);

#endif // CTR_UI_SHELL_H
