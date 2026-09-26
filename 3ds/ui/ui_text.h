// Text for the bottom screen, in the game's own font (game side).
//
// This uses the game's glyph data and does not ship a font. The code in
// ui_text.c decodes the 2bpp glyphs itself, and does not touch the game's
// gCurGlyph.
//
// Strings here are game-encoded (charmap.txt) and end with EOS. They are not
// ASCII. GetSpeciesName(), GetItemName() and gRegionMapEntries[].name already
// return that encoding. Use UiAscii() for literals.

#ifndef CTR_UI_TEXT_H
#define CTR_UI_TEXT_H

#include "global.h"

#define UI_GLYPH_H 15   // gCurGlyph.height of the Normal Latin font
#define UI_LINE_H  16

// The maximum number of characters drawn from one string. Game name tables have
// a fixed size with EOS padding, but a bad string must not run away.
#define UI_TEXT_MAX 128

// Returns the advance in pixels. Give `shadow` equal to `fg` to turn it off.
int UiText(int x, int y, const u8 *str, u16 fg, u16 shadow);
int UiTextWidth(const u8 *str);

// The same glyphs at double size, nearest-neighbor. The ROM has no larger Latin
// font, so this scales the normal font.
//
// For headlines that the player must read. It costs four times the fill for
// each glyph, so do not use it for general text.
#define UI_GLYPH_BIG_SCALE 2
#define UI_GLYPH_BIG_H     (UI_GLYPH_H * UI_GLYPH_BIG_SCALE)   // 30

int UiTextBig(int x, int y, const u8 *str, u16 fg, u16 shadow);
int UiTextBigWidth(const u8 *str);

// The game's small font (FONT_SMALL, gFontSmallLatinGlyphs): letters 7px tall
// on a 5px advance. The normal font is 9px on 6px. Use it for minor text that
// must not compete with the screen, like the build id on the title screen. Text
// that the player must read stays in UiText.
#define UI_GLYPH_SMALL_H 13   // gCurGlyph.height of the Small Latin font

int UiTextSmall(int x, int y, const u8 *str, u16 fg, u16 shadow);
int UiTextSmallWidth(const u8 *str);

// Right-aligned, for HP and quantities that must line up.
int UiTextRight(int xRight, int y, const u8 *str, u16 fg, u16 shadow);

// ---- text that must fit ---------------------------------------------------
//
// For any text whose length the code does not control: nicknames, OT names,
// box names, a partner's name. Every other width on this screen is measured
// against the longest string the game has, which cannot be done for text a
// player typed.
//
// Both cut at whole characters and end a cut with the game's own ellipsis
// (CHAR_ELLIPSIS), never inside a glyph. A string that fits draws exactly as
// UiText draws it.

// One line in at most `maxW` pixels. Stops at the first newline, and a string
// that has more after it counts as cut. Returns the advance.
int UiTextClipped(int x, int y, int maxW, const u8 *str, u16 fg, u16 shadow);

// Up to `maxLines` lines of `maxW` pixels, UI_LINE_H apart. A line breaks at the
// last space that fits, or inside a word that is wider than a whole line. The
// string's own newlines break too. If the text needs more lines, the last one
// ends in the ellipsis. Returns the number of lines drawn.
int UiTextWrapped(int x, int y, int maxW, int maxLines, const u8 *str,
                  u16 fg, u16 shadow);

int UiNum(int x, int y, s32 value, u16 fg, u16 shadow);
int UiNumRight(int xRight, int y, s32 value, u16 fg, u16 shadow);

// The width that UiNum() uses, to center a label and a value as one block.
int UiNumWidth(s32 value);

// ASCII to game encoding, for fixed labels. Writes dstSize bytes at most, EOS
// included, and returns dst.
u8 *UiAscii(u8 *dst, const char *ascii, int dstSize);

#endif // CTR_UI_TEXT_H
