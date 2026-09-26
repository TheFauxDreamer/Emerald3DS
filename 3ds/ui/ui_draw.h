// Bottom-screen drawing primitives (game side).
//
// Everything here draws into one 320x240 RGB565 framebuffer that this module
// owns. It is on the game side, so the window frames, fonts and icons are the
// game's own data.
//
// The GBA stores graphics as 4bpp tiles with BGR555 palettes. The bottom screen
// uses RGB565, so every path goes through the tile blitters and
// UiBgr555ToRgb565().

#ifndef CTR_UI_DRAW_H
#define CTR_UI_DRAW_H

#include "global.h"
#include "../bridge.h"

// UI_W is what is visible and what everything clips against. UI_STRIDE is how
// far apart two rows are. They differ because the UI paints straight into the
// host's linear staging buffer, whose width the GPU forces to a power of two.
// Address a row with UI_STRIDE; test a coordinate against UI_W.
#define UI_W      CTR_BOTTOM_WIDTH
#define UI_H      CTR_BOTTOM_HEIGHT
#define UI_STRIDE CTR_BOTTOM_STRIDE

// The framebuffer that the host gets each frame. Its address does not change
// once the host has set it.
u16 *UiFb(void);

// The host's buffer, set once before anything paints.
void UiSetFb(u16 *fb);

// ---- the dirty band -------------------------------------------------------
//
// Which rows have been drawn into since the host last took the picture, so a
// step that moves two icons does not upload all 240 rows. Every primitive that
// writes the framebuffer calls UiTouchRows, so no drawing site can forget.
// Empty means clean: top >= bot.
void UiTouchRows(int y, int h);
void UiDirtyRows(int *top, int *bot);
void UiClearDirtyRows(void);

// ---- the clip rect --------------------------------------------------------
//
// Every primitive that writes the framebuffer clips against this rect, and not
// only against the screen: the fills, the rect, the tile and row blits, and the
// text. The rect is the full screen unless a caller pushes a smaller one, so
// code that never pushes draws as before.
//
// Push around anything whose size is not known in advance: a row of a list, a
// cell of a grid, a panel that holds player text. A push is intersected with
// the rect that is already there, so a nested push can only make it smaller.
// Pop each push in the same function.
//
// UiClear ignores the clip: it is the whole-screen reset. UiRestoreRect
// ignores it too, because it puts back pixels that were already clipped when
// they were drawn.
//
// The shell resets it before each paint (UiClipReset), so a missing pop costs
// one paint, not every paint after it.
#define UI_CLIP_DEPTH 4

// x1 and y1 are exclusive. Read it; change it only through the calls below.
struct UiClipRect
{
    s16 x0, y0, x1, y1;
};
extern struct UiClipRect gUiClip;

void UiClipPush(int x, int y, int w, int h);
void UiClipPop(void);
void UiClipReset(void);

u16  UiBgr555ToRgb565(u16 bgr555);

// Convert a GBA 16-color palette once. The per-pixel work is then a table
// lookup.
void UiLoadPal(u16 *dst565, const u16 *srcGbaPal, int count);

void UiClear(u16 color);
void UiFillRect(int x, int y, int w, int h, u16 color);
void UiRect(int x, int y, int w, int h, u16 color);   // 1px outline

// One 8x8 4bpp tile. The GBA packs two pixels in each byte, the low nibble
// first. Index 0 is transparent in every GBA palette, so it is skipped when
// transparent0 is set.
void UiBlit4bppTile(int x, int y, const u8 *tile, const u16 *pal565,
                    int transparent0);

// A run of pixels the caller has already converted to RGB565, clipped to the
// screen. For anything that composes its own colours instead of blitting GBA
// art.
void UiBlitRow(int x, int y, const u16 *src, int w);

// The same, with a tilemap entry's flip bits. A GBA background map flips tiles
// rather than storing four copies of each corner, so anything drawn from a real
// tilemap needs this. No flip costs nothing: it calls the plain blit above.
void UiBlit4bppTileFlip(int x, int y, const u8 *tile, const u16 *pal565,
                        int transparent0, int hflip, int vflip);

// One 8x8 8bpp tile, 64 bytes. `pal565` must have 256 entries, not 16. An 8bpp
// GBA background has no palette bank, so each byte is an absolute index into
// the BG palette. See the note above the definition.
void UiBlit8bppTile(int x, int y, const u8 *tile, const u16 *pal565,
                    int transparent0);

// A 3x3 nine-slice window frame, in the border that the player selected in
// Options -> Frame. Position and size are in 8px tiles.
void UiWindowFrame(int tx, int ty, int wTiles, int hTiles);

// The player's current frame choice. Put it into every redraw trigger, or the
// screen keeps the old border.
u8 UiFrameId(void);

// Text colors for anything on a frame: the same colors as the game's menus, so
// they are legible on all 20 borders. Do not use a fixed color there, because
// the frames go from light to dark.
u16 UiThemeText(void);
u16 UiThemeShadow(void);

// A party or box mon icon: 32x32, 4bpp, 4x4 tiles in 1D sprite order.
void UiMonIcon(int x, int y, u16 species, u32 personality);

// The same icon, at one of its two frames.
//
// Every graphics/pokemon/*/icon.png is 32x64: two 32x32 frames. The game cycles
// the two frames on every icon (sAnim_0, src/pokemon_icon.c). All 387 sheets
// have both frames, so only bit 0 of `frame` is read.
//
// UiMonIcon above is this at frame 0, for a still icon. For the animation, the
// caller advances the frame and gives it here.
void UiMonIconFrame(int x, int y, u16 species, u32 personality, u8 frame);

// The same icon in one flat color, with its outline: what an unknown mon looks
// like. On a window frame, give UiThemeShadow(). The 20 frames go from near
// white to near dark, so a fixed color is not visible on half of them.
void UiMonIconSilhouette(int x, int y, u16 species, u32 personality, u16 color);

// A bag item icon: 32x32, made the same way as the game's item sprites.
void UiItemIcon(int x, int y, u16 itemId);

// A Pokedex front sprite: 64x64, the game's own art. Cached on species, so a
// cursor that moves through a list costs one decompress for each mon.
void UiMonPic(int x, int y, u16 species);

// A trainer's front sprite, 64x64, the game's own art. `picId` is a
// TRAINER_PIC_* constant, not a facility class. Cached on the id.
#define UI_TRAINER_PIC_W 64
#define UI_TRAINER_PIC_H 64

void UiTrainerPic(int x, int y, u16 picId);

// The Pokedex "caught" marker, 7x7.
#define UI_BALL_W 7
#define UI_BALL_H 7

void UiPokeball(int x, int y);

// One specific kind of ball, 16x16, from the game's throw art, so a Master Ball
// looks like a Master Ball. It takes an ITEM_*_BALL id, not a BALL_* id.
// Anything that is not a ball draws as a Poke Ball. Cached on the ball kind.
//
// UiPokeball above is the generic marker. The dex marker means "caught", not
// "caught in this ball". Use this function when the kind of ball matters.
#define UI_BALL_ICON_W 16
#define UI_BALL_ICON_H 16

void UiBallIcon(int x, int y, u16 itemId);

// A species footprint: 16x16, 1bpp, in `color`.
void UiFootprint(int x, int y, u16 species, u16 color);

// A move or species type badge, 32x16, from the game's icon sheet. It takes a
// TYPE_* value, and draws nothing for a value outside the sheet.
#define UI_TYPE_ICON_W 32
#define UI_TYPE_ICON_H 16

void UiTypeIcon(int x, int y, u8 type);

// A status badge (PSN, SLP, BRN ...): 32x8, the party menu's own art. It takes
// an AILMENT_* value or UI_STATUS_CNF, usually from UiStatusTag()
// (status_tags.h). It draws nothing for AILMENT_NONE or AILMENT_PKRS, as in the
// party menu.
//
// The game has no art for UI_STATUS_CNF. Confusion is not a stored status, so
// it has no AILMENT_* value. The value is after the sheet's range (AILMENT_FNT
// is 7, and index 7 of the sheet is blank). The badge is drawn by hand in the
// sheet's style.
#define UI_STATUS_CNF 8

void UiStatusIcon(int x, int y, u8 ailment);

// A solid triangle with a 1px outline, up or down. Both dimensions are odd, so
// the tip is on a whole pixel.
#define UI_ARROW_W 11
#define UI_ARROW_H 7

void UiArrow(int x, int y, bool8 up, u16 fill);

// The cursor that the game puts next to the selected battle menu entry, in the
// player's menu colors. It is only the ink, with no blank rows or columns, so
// (x, y) is the glyph and centering it is exact.
#define UI_CHEVRON_W 6
#define UI_CHEVRON_H 10

void UiChevron(int x, int y);

// A checkbox for an on/off setting. The box has a 1px dim outline, as a button
// frame has. A checked box also has a tick in the accent color with a theme
// shadow, so the shape shows the state, not the color alone. (x, y) is the top
// left corner of the box.
#define UI_CHECKBOX_SIZE 14

void UiCheckBox(int x, int y, bool8 checked);

// A gold sparkle, at the three sizes of the game's shiny animation.
//
// The file gold_stars.png has six 8x8 tiles: tiles 0-3 are a 16x16 star, tile 4
// an 8x8 star and tile 5 a small twinkle. Thus `size` 0 to 2 selects a frame of
// the art, not a scale. There is no scaler.
//
// It is centered on (cx, cy) at the star's bright horizontal axis, not at its
// bounding box. The three frames have different shapes, and a box center makes
// the twinkle move up as it grows.
#define UI_SPARKLE_SIZES 3

void UiSparkle(int cx, int cy, u8 size);

// The same art in a different three-step ramp, with the same roles: the pale
// core, the body and the dark edge. The achievement categories use this
// (UiAchCategoryRamp in ui_shell.h). UiSparkle is this in gold.
void UiSparkleRamp(int cx, int cy, u8 size, u16 pale, u16 body, u16 edge);

// A two-tone HP bar, 8px tall. The game's own GetHPBarLevel sets its color, so
// it changes color at the same points as the battle bar. The caller gives `hp`:
// the party tab gives an animated value, and the BAG picker gives the real
// value.
void UiHpBar(int x, int y, int w, u32 hp, u32 maxHp);

// ---- partial repaint ------------------------------------------------------
//
// A full repaint fills all 320x240 pixels. That is expensive to move two icons.
// On the single-core path, it costs the game a frame.
//
// Thus keep a copy of the last full paint. An animation restores the part that
// it will draw again, and the screen does not repaint. A restore copies a few
// thousand pixels.
//
// The copy is the full composited screen: the tab, the overlay and the bar.
// Thus a restore is correct for anything on top, and a redrawn element lands in
// the same place.
void UiSnapshot(void);
int  UiHasSnapshot(void);
void UiRestoreRect(int x, int y, int w, int h);

int  UiHit(const CtrTouchState *t, int x, int y, int w, int h);

// Press-and-hold auto-repeat for a control that steps something. A player can
// then hold an arrow to cross a list.
//
// One UiHold lives next to the state of its control. It is a frame counter, not
// a widget. Both list tabs use it (see UiDexTouch).
//
// It counts calls. CtrBottomUpdate runs the active tab's Touch once for each
// displayed frame, touched or not. Thus a call is 1/60 s, even under
// fast-forward.
typedef struct
{
    u16 frames;     // frames inside the rect, 0 when idle
    u16 next;       // the frame count for the next repeat
    u8  repeated;   // this press has repeated, so its release must not act
} UiHold;

// The delay is long enough that a slow tap is still one step. Then there are
// two rates. The national dex has 386 rows, and one rate is too fast at the top
// or too slow at the bottom.
#define UI_HOLD_DELAY    18   // frames held before the first repeat
#define UI_HOLD_PERIOD   6    // frames between repeats at first
#define UI_HOLD_FAST_AT  60   // frames held before the faster rate
#define UI_HOLD_FAST     3    // frames between repeats after that

// TRUE on each frame when the control must act. Call it once for each frame,
// before the handler's `if (!t->justReleased) return;` guard. A hold must act
// on frames with no release. It still acts once on the release of a plain tap,
// so a converted control keeps its tap behavior. A press that repeated does not
// act again on release.
bool8 UiHoldRepeat(UiHold *h, const CtrTouchState *t,
                   int x, int y, int w, int hgt);

#endif // CTR_UI_DRAW_H
