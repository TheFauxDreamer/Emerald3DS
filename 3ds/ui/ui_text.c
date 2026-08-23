// Emerald-font text for the bottom screen. See text.h.

#include "global.h"
#include "text.h"        // the GAME's: DecompressGlyphTile()
#include "fonts.h"
#include "constants/characters.h"

#include "ui_draw.h"
#include "ui_text.h"

// One decoded glyph: two 8-row halves, each up to two tiles wide. Matches the
// layout DecompressGlyphTile writes and GLYPH_COPY reads (src/text.c).
struct DecodedGlyph
{
    u32 top[16];
    u32 bottom[16];
    u8  width;
};

static void DecodeGlyph(u8 glyphId, struct DecodedGlyph *g)
{
    const u16 *glyphs = gFontNormalLatinGlyphs + (0x20 * glyphId);

    g->width = gFontNormalLatinGlyphWidths[glyphId];

    // Mirrors DecompressGlyph_Normal (src/text.c): glyphs wider than one tile
    // occupy four tiles rather than two.
    if (g->width <= 8)
    {
        DecompressGlyphTile(glyphs, g->top);
        DecompressGlyphTile(glyphs + 0x10, g->bottom);
    }
    else
    {
        DecompressGlyphTile(glyphs, g->top);
        DecompressGlyphTile(glyphs + 0x8, g->top + 8);
        DecompressGlyphTile(glyphs + 0x10, g->bottom);
        DecompressGlyphTile(glyphs + 0x18, g->bottom + 8);
    }
}

// Pixel (col,row) of a decoded half. Rows are one u32 each; the low nibble is
// the leftmost pixel. Index 0 is transparent.
static u32 GlyphPixel(const u32 *half, int col, int row)
{
    const u32 *word = (col < 8) ? &half[row] : &half[8 + row];
    return (*word >> (4 * (col & 7))) & 0xF;
}

static void BlitGlyph(int x, int y, const struct DecodedGlyph *g,
                      u16 fg, u16 shadow)
{
    for (int row = 0; row < UI_GLYPH_H; row++)
    {
        int py = y + row;
        if (py < 0 || py >= UI_H)
            continue;

        const u32 *half = (row < 8) ? g->top : g->bottom;
        int localRow = (row < 8) ? row : row - 8;
        u16 *dst = &UiFb()[py * UI_W];

        for (int col = 0; col < g->width; col++)
        {
            int px = x + col;
            if (px < 0 || px >= UI_W)
                continue;

            u32 idx = GlyphPixel(half, col, localRow);
            if (idx == 0)
                continue;   // transparent

            // Emerald's fonts encode 1 = foreground, 2 = shadow. Anything else
            // is treated as foreground rather than dropped.
            dst[px] = (idx == 2) ? shadow : fg;
        }
    }
}

int UiText(int x, int y, const u8 *str, u16 fg, u16 shadow)
{
    struct DecodedGlyph g;
    int startX = x;
    int guard = UI_TEXT_MAX;

    if (str == NULL)
        return 0;

    // Bounded: these strings come from game tables, and a missing EOS would
    // otherwise walk off the end of one and draw whatever follows it.
    for (; *str != EOS && guard-- > 0; str++)
    {
        if (*str == CHAR_NEWLINE)
        {
            x = startX;
            y += UI_LINE_H;
            continue;
        }

        DecodeGlyph(*str, &g);
        BlitGlyph(x, y, &g, fg, shadow);
        x += g.width;
    }

    return x - startX;
}

int UiTextWidth(const u8 *str)
{
    int w = 0, best = 0;
    int guard = UI_TEXT_MAX;

    if (str == NULL)
        return 0;

    for (; *str != EOS && guard-- > 0; str++)
    {
        if (*str == CHAR_NEWLINE)
        {
            if (w > best) best = w;
            w = 0;
            continue;
        }
        w += gFontNormalLatinGlyphWidths[*str];
    }

    return (w > best) ? w : best;
}

int UiTextRight(int xRight, int y, const u8 *str, u16 fg, u16 shadow)
{
    return UiText(xRight - UiTextWidth(str), y, str, fg, shadow);
}

static void NumToStr(u8 *dst, s32 value)
{
    u8 tmp[12];
    int n = 0;
    u32 v;

    if (value < 0)
    {
        *dst++ = CHAR_HYPHEN;
        v = (u32)-value;
    }
    else
    {
        v = (u32)value;
    }

    do {
        tmp[n++] = CHAR_0 + (v % 10);
        v /= 10;
    } while (v != 0 && n < (int)sizeof(tmp));

    while (n > 0)
        *dst++ = tmp[--n];

    *dst = EOS;
}

int UiNum(int x, int y, s32 value, u16 fg, u16 shadow)
{
    u8 buf[16];
    NumToStr(buf, value);
    return UiText(x, y, buf, fg, shadow);
}

int UiNumRight(int xRight, int y, s32 value, u16 fg, u16 shadow)
{
    u8 buf[16];
    NumToStr(buf, value);
    return UiTextRight(xRight, y, buf, fg, shadow);
}

u8 *UiAscii(u8 *dst, const char *ascii, int dstSize)
{
    int i = 0;

    if (dstSize <= 0)
        return dst;

    for (; ascii[i] != '\0' && i < dstSize - 1; i++)
    {
        char c = ascii[i];
        u8 out;

        if      (c >= '0' && c <= '9') out = CHAR_0 + (c - '0');
        else if (c >= 'A' && c <= 'Z') out = CHAR_A + (c - 'A');
        else if (c >= 'a' && c <= 'z') out = CHAR_a + (c - 'a');
        else if (c == ' ')             out = CHAR_SPACE;
        else if (c == '-')             out = CHAR_HYPHEN;
        else if (c == '/')             out = CHAR_SLASH;
        else if (c == ':')             out = CHAR_COLON;
        else if (c == '.')             out = CHAR_PERIOD;
        else if (c == ',')             out = CHAR_COMMA;
        else if (c == '?')             out = CHAR_QUESTION_MARK;
        else if (c == '%')             out = CHAR_PERCENT;
        else if (c == '+')             out = CHAR_PLUS;
        else if (c == '\n')            out = CHAR_NEWLINE;
        else                           out = CHAR_SPACE;

        dst[i] = out;
    }

    dst[i] = EOS;
    return dst;
}
