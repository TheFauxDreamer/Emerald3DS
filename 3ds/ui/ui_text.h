// Text for the bottom screen, drawn with Emerald's own font (game side).
//
// This reuses the game's glyph data and DecompressGlyphTile() rather than
// shipping a font: gFontNormalLatinGlyphs is 2bpp, and DecompressGlyphTile
// expands one 8x8 tile to 4bpp as eight u32 rows, low nibble = leftmost pixel.
// The game's own gCurGlyph is deliberately untouched -- the text engine owns
// it, and it decodes into whatever buffer you hand it.
//
// IMPORTANT: strings here are GAME-ENCODED (charmap.txt), EOS-terminated, not
// ASCII. GetSpeciesName(), GetItemName() and gRegionMapEntries[].name already
// return that encoding. Use UiAscii() for literals.

#ifndef CTR_UI_TEXT_H
#define CTR_UI_TEXT_H

#include "global.h"

#define UI_GLYPH_H 15   // matches gCurGlyph.height for the Normal Latin font
#define UI_LINE_H  16

// Upper bound on characters drawn from one string. Game name tables are fixed
// size and EOS-padded, but a corrupt or unterminated one must not run away.
#define UI_TEXT_MAX 128

// Returns the advance in pixels. `shadow` may equal `fg` to disable it.
int UiText(int x, int y, const u8 *str, u16 fg, u16 shadow);
int UiTextWidth(const u8 *str);

// Right-aligned variant, for HP and quantities that should line up.
int UiTextRight(int xRight, int y, const u8 *str, u16 fg, u16 shadow);

int UiNum(int x, int y, s32 value, u16 fg, u16 shadow);
int UiNumRight(int xRight, int y, s32 value, u16 fg, u16 shadow);

// Width UiNum() would use, for centring a label and value as one block.
int UiNumWidth(s32 value);

// ASCII -> game encoding for static labels. Writes at most dstSize bytes
// including EOS, and returns dst.
u8 *UiAscii(u8 *dst, const char *ascii, int dstSize);

#endif // CTR_UI_TEXT_H
