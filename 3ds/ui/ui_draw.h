// Bottom-screen drawing primitives (game side).
//
// Everything here works on one 320x240 RGB565 framebuffer that this module
// owns. It is game-side, so the GBA's own graphics are ordinary symbols: the
// window frames, fonts and mon icons blitted here are Emerald's, not
// reimplementations.
//
// The GBA stores graphics as 4bpp tiles and palettes as BGR555, while the
// bottom screen wants RGB565, so every path funnels through Blit4bppTile() and
// Bgr555ToRgb565().

#ifndef CTR_UI_DRAW_H
#define CTR_UI_DRAW_H

#include "global.h"
#include "../bridge.h"

#define UI_W CTR_BOTTOM_WIDTH
#define UI_H CTR_BOTTOM_HEIGHT

// The framebuffer handed to the host each frame. Stable for the process.
u16 *UiFb(void);

u16  UiBgr555ToRgb565(u16 bgr555);

// Convert a GBA 16-colour palette once, so per-pixel work is a table lookup.
void UiLoadPal(u16 *dst565, const u16 *srcGbaPal, int count);

void UiClear(u16 color);
void UiFillRect(int x, int y, int w, int h, u16 color);
void UiRect(int x, int y, int w, int h, u16 color);   // 1px outline

// One 8x8 4bpp tile. GBA packs two pixels per byte, LOW nibble first. Index 0
// is the transparent slot in every GBA palette, so it is skipped when
// transparent0 is set.
void UiBlit4bppTile(int x, int y, const u8 *tile, const u16 *pal565,
                    int transparent0);

// One 8x8 8bpp tile, 64 bytes. `pal565` must have 256 entries, not 16: an 8bpp
// GBA background has no palette-bank field, so each byte is an absolute index
// into the whole BG palette. See the note above the definition.
void UiBlit8bppTile(int x, int y, const u8 *tile, const u16 *pal565,
                    int transparent0);

// A 3x3 nine-slice window frame in whichever of the 20 borders the player chose
// in Options -> Frame. Coordinates and size are in 8px tiles.
void UiWindowFrame(int tx, int ty, int wTiles, int hTiles);

// The player's current frame choice. Fold this into any redraw trigger, or the
// screen keeps the old border until something else happens to dirty it.
u8 UiFrameId(void);

// Text colours for anything drawn ON a frame -- the same ones Emerald's own
// menus print with, so they stay legible across all 20 borders. Do not use a
// fixed colour there: the frames run from light to dark.
u16 UiThemeText(void);
u16 UiThemeShadow(void);

// A party/box mon icon: 32x32, 4bpp, 4x4 tiles in 1D sprite order.
void UiMonIcon(int x, int y, u16 species, u32 personality);

// The same icon, at one of its TWO frames.
//
// A mon icon in this ROM is not one picture: every graphics/pokemon/*/icon.png
// is 32x64, two 32x32 frames, and the game cycles the pair on every icon it
// draws (sAnim_0, src/pokemon_icon.c, six game frames each). All 387 sheets
// have both, so `frame` never needs a per-species check; only bit 0 is read.
//
// UiMonIcon above is this at frame 0, which is what a still icon wants. A
// caller that wants the animation advances a frame itself and passes it here.
void UiMonIconFrame(int x, int y, u16 species, u32 personality, u8 frame);

// A bag item icon: 32x32, drawn the way the game builds its own item sprites.
void UiItemIcon(int x, int y, u16 itemId);

// A Pokedex front sprite: 64x64, the game's own art. Cached on species, so
// moving a cursor through a list costs one decompress per mon, not per repaint.
void UiMonPic(int x, int y, u16 species);

// The Pokedex "caught" marker, 7x7.
#define UI_BALL_W 7
#define UI_BALL_H 7

void UiPokeball(int x, int y);

// A species footprint: 16x16, 1bpp, drawn in `color`.
void UiFootprint(int x, int y, u16 species, u16 color);

// A move or species type badge, 32x16, drawn from the game's own icon sheet.
// Takes a TYPE_* value. Draws nothing for anything outside the sheet.
#define UI_TYPE_ICON_W 32
#define UI_TYPE_ICON_H 16

void UiTypeIcon(int x, int y, u8 type);

// A status badge (PSN, SLP, BRN ...): 32x8, the party menu's own art. Takes an
// AILMENT_* value, normally straight from GetMonAilment(). Draws nothing for
// AILMENT_NONE or AILMENT_PKRS, which is what the party menu does too.
void UiStatusIcon(int x, int y, u8 ailment);

// A solid triangle with a 1px outline, pointing up or down. Both dimensions are
// odd so the tip lands on a whole pixel.
#define UI_ARROW_W 11
#define UI_ARROW_H 7

void UiArrow(int x, int y, bool8 up, u16 fill);

// The cursor Emerald puts beside the selected battle menu entry, in the player's
// own menu colours. Ink only -- the blank rows and columns around it in the
// source tiles are dropped -- so (x, y) is the glyph itself and centring it on a
// row or a cell is exact.
#define UI_CHEVRON_W 6
#define UI_CHEVRON_H 10

void UiChevron(int x, int y);

// A gold sparkle, at the three sizes the game's own shiny animation has.
//
// gold_stars.png is six 8x8 tiles: tiles 0-3 are one 16x16 star, tile 4 an 8x8
// one and tile 5 a small twinkle (gWishStarSpriteTemplate and
// gMiniTwinklingStarSpriteTemplate, src/battle_anim_effects_3.c). So `size` 0
// to 2 selects a FRAME OF THE ART, not a scale factor -- there is no scaler
// here because the sheet already drew all three.
//
// Centred on (cx, cy), and centred on the star's own bright horizontal axis
// rather than on its bounding box, because the three frames are not the same
// shape: box-centring makes a twinkle appear to slide up the screen as it grows.
#define UI_SPARKLE_SIZES 3

void UiSparkle(int cx, int cy, u8 size);

// A two-tone HP bar, 8px tall, coloured by the game's own GetHPBarLevel so it
// changes colour at exactly the same points the battle bar does. `hp` is passed
// in rather than read from the mon: the party tab animates it, the BAG tab's
// target picker shows the real value.
void UiHpBar(int x, int y, int w, u32 hp, u32 maxHp);

int  UiHit(const CtrTouchState *t, int x, int y, int w, int h);

// Press-and-hold auto-repeat for a control that steps something, so a list can
// be crossed by holding an arrow instead of tapping it 300 times.
//
// One UiHold lives beside the state its control drives -- it is a frame
// counter, not a widget. Both list tabs page with it; see UiDexTouch.
//
// Frames, counted in calls: CtrBottomUpdate runs the active tab's Touch once
// per DISPLAYED frame, whether or not anything is being touched, so counting
// calls is counting 60ths of a second even under fast-forward.
typedef struct
{
    u16 frames;     // frames this press has spent inside the rect, 0 when idle
    u16 next;       // frame count the next repeat fires on
    u8  repeated;   // this press has already repeated, so its release must not
} UiHold;

// Long enough that a slow tap is still one step, then two rates: the second one
// exists because the national dex is 386 rows and one fixed rate makes crossing
// it either twitchy at the top or a chore at the bottom.
#define UI_HOLD_DELAY    18   // frames held before the first repeat
#define UI_HOLD_PERIOD   6    // frames between repeats to begin with
#define UI_HOLD_FAST_AT  60   // frames held before the rate steps up
#define UI_HOLD_FAST     3    // frames between repeats after that

// TRUE on each frame the control should act. Call it once per frame, BEFORE the
// handler's `if (!t->justReleased) return;` guard -- a hold has to act on frames
// where nothing has been released. It still fires once on the release of a
// plain tap, so a control converted to this keeps its old tap behaviour; a press
// that got as far as repeating does not act again when it is lifted.
bool8 UiHoldRepeat(UiHold *h, const CtrTouchState *t,
                   int x, int y, int w, int hgt);

#endif // CTR_UI_DRAW_H
