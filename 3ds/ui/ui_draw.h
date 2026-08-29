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

// A status badge (PSN, SLP, BRN ...): 32x8, the party menu's own art. Takes an
// AILMENT_* value, normally straight from GetMonAilment(). Draws nothing for
// AILMENT_NONE or AILMENT_PKRS, which is what the party menu does too.
void UiStatusIcon(int x, int y, u8 ailment);

// A solid triangle with a 1px outline, pointing up or down. Both dimensions are
// odd so the tip lands on a whole pixel.
#define UI_ARROW_W 11
#define UI_ARROW_H 7

void UiArrow(int x, int y, bool8 up, u16 fill);

// A two-tone HP bar, 8px tall, coloured by the game's own GetHPBarLevel so it
// changes colour at exactly the same points the battle bar does. `hp` is passed
// in rather than read from the mon: the party tab animates it, the BAG tab's
// target picker shows the real value.
void UiHpBar(int x, int y, int w, u32 hp, u32 maxHp);

int  UiHit(const CtrTouchState *t, int x, int y, int w, int h);

#endif // CTR_UI_DRAW_H
