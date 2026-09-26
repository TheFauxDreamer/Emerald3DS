// Text in the game's font for the bottom screen. See ui_text.h.

#include "global.h"
#include "fonts.h"
#include "string_util.h"              // GetExtCtrlCodeLength
#include "constants/characters.h"

#include "ui_draw.h"
#include "ui_text.h"

// This file decodes the font itself. Do not use the game's
// DecompressGlyphTile().
//
// That function expands 2bpp through sFontHalfRowLookupTable. The text engine
// rebuilds that table from the colors that it prints with at the time
// (GenerateFontHalfRowLookupTable, src/text.c). A call from here uses those
// colors, often 0, which is transparent: the text is invisible.
//
// The format is simple to read directly, so this file does that and shares no
// state with the text engine.
//
// The layout, from DecompressGlyph_Normal (src/text.c), in u16 units:
//     0x00..0x07  top-left tile        0x08..0x0F  top-right
//     0x10..0x17  bottom-left          0x18..0x1F  bottom-right
// One u16 is one row of 8 pixels at 2bpp, pixel 0 in the most significant bits.
// Values: 0 background, 1 foreground, 2 shadow, 3 the same as background.
static u32 GlyphPixel(const u16 *glyph, int x, int y)
{
    int idx = ((y >= 8) ? 0x10 : 0x00) + ((x >= 8) ? 0x08 : 0x00) + (y & 7);
    u32 v = (glyph[idx] >> (14 - 2 * (x & 7))) & 3;
    return (v == 3) ? 0 : v;
}

// The two Latin fonts of this screen: the game's normal font for almost all
// text, and its small font (FONT_SMALL) for minor text. Both use the glyph
// format and 16x16 cell layout above (DecompressGlyph_Small reads the same four
// tiles). Only the tables and the height are different. Both width tables have
// 512 slots.
struct UiFont
{
    const u16 *glyphs;
    const u8  *widths;
    u8 height;          // rows drawn for each glyph
    u8 lineH;           // the advance for a new line
};

static const struct UiFont sFontNormal = {
    gFontNormalLatinGlyphs, gFontNormalLatinGlyphWidths, UI_GLYPH_H, UI_LINE_H,
};
static const struct UiFont sFontSmall = {
    gFontSmallLatinGlyphs, gFontSmallLatinGlyphWidths, UI_GLYPH_SMALL_H, UI_GLYPH_SMALL_H + 1,
};

// The id is a u16: CHAR_EXTRA_SYMBOL selects glyph `operand | 0x100` from the
// same tables. The glyph sheets have 512 slots. `scale` turns one source pixel
// into a scale x scale block. There is no larger Latin font in the ROM, so this
// scales the normal font.
//
// Scale 1 is every caller but the title, and it gets its own loop.
//
// The general loop below tests each destination pixel against both edges,
// inside the innermost loop, and calls GlyphPixel for each source pixel, which
// recomputes the glyph index and reloads the row. Text is thousands of these
// per repaint, so the scale-1 path clips its column span once per glyph and
// loads the row's two words once per row.
static void BlitGlyph(const struct UiFont *font, int x, int y, u16 glyphId,
                      u16 fg, u16 shadow, int scale)
{
    const u16 *glyph = font->glyphs + (0x20 * glyphId);
    int width = font->widths[glyphId];

    if (width > 16)
        width = 16;

    UiTouchRows(y, font->height * scale);

    if (scale == 1)
    {
        int col0 = (x < gUiClip.x0) ? gUiClip.x0 - x : 0;
        int col1 = (x + width > gUiClip.x1) ? gUiClip.x1 - x : width;

        for (int row = 0; row < font->height && col0 < col1; row++)
        {
            int py = y + row;
            const u16 *w;
            u16 *dst;
            u32 wLo, wHi;

            if (py < gUiClip.y0 || py >= gUiClip.y1)
                continue;

            // The glyph is four 8x8 tiles. A row's left half and right half are
            // eight words apart; see GlyphPixel for the same arithmetic.
            w = glyph + ((row >= 8) ? 0x10 : 0x00) + (row & 7);
            wLo = w[0];
            wHi = w[8];
            dst = &UiFb()[py * UI_STRIDE];

            for (int col = col0; col < col1; col++)
            {
                u32 bits = (col < 8) ? wLo : wHi;
                u32 v = (bits >> (14 - 2 * (col & 7))) & 3;

                // 0 is the background and 3 reads as background too.
                if (v == 0 || v == 3)
                    continue;

                dst[x + col] = (v == 2) ? shadow : fg;
            }
        }

        return;
    }

    for (int row = 0; row < font->height; row++)
    {
        for (int sy = 0; sy < scale; sy++)
        {
            int py = y + row * scale + sy;
            u16 *dst;

            if (py < gUiClip.y0 || py >= gUiClip.y1)
                continue;

            dst = &UiFb()[py * UI_STRIDE];

            for (int col = 0; col < width; col++)
            {
                u32 v = GlyphPixel(glyph, col, row);
                u16 colour;

                if (v == 0)
                    continue;                   // background: do not change it

                colour = (v == 2) ? shadow : fg;

                for (int sx = 0; sx < scale; sx++)
                {
                    int px = x + col * scale + sx;

                    if (px < gUiClip.x0 || px >= gUiClip.x1)
                        continue;

                    dst[px] = colour;
                }
            }
        }
    }
}

// The length of an EXT_CTRL_CODE_BEGIN sequence, as in SkipExtCtrlCode()
// (src/string_util.c). It is the 0xFC byte, then the length that the game gives
// for the code, which includes the code byte. GetExtCtrlCodeLength returns 0
// for an unknown code, which would stop the walk, so the minimum is 1.
static int CtrlCodeSpan(const u8 *str)
{
    u8 len = GetExtCtrlCodeLength(str[1]);

    return 1 + (len ? len : 1);
}

// A two-byte sequence whose second byte is the terminator is a bad string. A
// step over it would go past the EOS.
static bool8 Truncated(const u8 *str)
{
    return str[1] == EOS;
}

// The game's strings are not plain bytes. Three control codes move the pen with
// no print, and CHAR_EXTRA_SYMBOL selects the upper half of the glyph table.
// Without them, the Pokedex prints garbage for "{NO}" and "??'??".
//
// TextWidth below must agree with this function, because centering uses both.
//
// Every pen movement scales with the glyphs, so a scaled string has the same
// shape at a larger size.
static int DrawText(const struct UiFont *font, int x, int y, const u8 *str,
                    u16 fg, u16 shadow, int scale)
{
    int startX = x;
    int guard = UI_TEXT_MAX;

    if (str == NULL)
        return 0;

    // Limited: these strings come from game tables. A missing EOS would
    // otherwise walk past the end of the string.
    while (*str != EOS && guard-- > 0)
    {
        if (*str == EXT_CTRL_CODE_BEGIN)
        {
            u8 code, arg;

            if (Truncated(str))
                break;

            code = str[1];
            arg  = str[2];

            switch (code)
            {
            case EXT_CTRL_CODE_CLEAR:    x += arg * scale;                    break;
            case EXT_CTRL_CODE_SKIP:     x  = startX + arg * scale;           break;
            case EXT_CTRL_CODE_CLEAR_TO: if (startX + arg * scale > x) x = startX + arg * scale; break;
            default: break;   // colors, pauses, sounds: nothing to draw
            }

            str += CtrlCodeSpan(str);
            continue;
        }

        // Two bytes, and the second is the value. The keypad icons are in a
        // sheet that this file does not load, so skip that one. Do not draw a
        // wrong glyph.
        if (*str == CHAR_EXTRA_SYMBOL || *str == CHAR_KEYPAD_ICON)
        {
            if (Truncated(str))
                break;

            if (*str == CHAR_EXTRA_SYMBOL)
            {
                u16 glyph = (u16)(str[1] | 0x100);

                BlitGlyph(font, x, y, glyph, fg, shadow, scale);
                x += font->widths[glyph] * scale;
            }

            str += 2;
            continue;
        }

        if (*str == CHAR_NEWLINE)
        {
            x = startX;
            y += font->lineH * scale;
            str++;
            continue;
        }

        BlitGlyph(font, x, y, *str, fg, shadow, scale);
        x += font->widths[*str] * scale;
        str++;
    }

    return x - startX;
}

static int TextWidth(const struct UiFont *font, const u8 *str);

int UiText(int x, int y, const u8 *str, u16 fg, u16 shadow)
{
    return DrawText(&sFontNormal, x, y, str, fg, shadow, 1);
}

int UiTextBig(int x, int y, const u8 *str, u16 fg, u16 shadow)
{
    return DrawText(&sFontNormal, x, y, str, fg, shadow, UI_GLYPH_BIG_SCALE);
}

int UiTextBigWidth(const u8 *str)
{
    return UiTextWidth(str) * UI_GLYPH_BIG_SCALE;
}

int UiTextSmall(int x, int y, const u8 *str, u16 fg, u16 shadow)
{
    return DrawText(&sFontSmall, x, y, str, fg, shadow, 1);
}

int UiTextSmallWidth(const u8 *str)
{
    return TextWidth(&sFontSmall, str);
}

int UiTextWidth(const u8 *str)
{
    return TextWidth(&sFontNormal, str);
}

// The same walk as DrawText, and the same handling of the three pen codes as
// GetStringWidth (src/text.c): CLEAR adds, SKIP sets, CLEAR_TO takes the
// maximum.
static int TextWidth(const struct UiFont *font, const u8 *str)
{
    int w = 0, best = 0;
    int guard = UI_TEXT_MAX;

    if (str == NULL)
        return 0;

    while (*str != EOS && guard-- > 0)
    {
        if (*str == EXT_CTRL_CODE_BEGIN)
        {
            u8 code, arg;

            if (Truncated(str))
                break;

            code = str[1];
            arg  = str[2];

            switch (code)
            {
            case EXT_CTRL_CODE_CLEAR:    w += arg;                break;
            case EXT_CTRL_CODE_SKIP:     w  = arg;                break;
            case EXT_CTRL_CODE_CLEAR_TO: if (arg > w) w = arg;    break;
            default: break;
            }

            str += CtrlCodeSpan(str);
            continue;
        }

        if (*str == CHAR_EXTRA_SYMBOL || *str == CHAR_KEYPAD_ICON)
        {
            if (Truncated(str))
                break;

            if (*str == CHAR_EXTRA_SYMBOL)
                w += font->widths[str[1] | 0x100];

            str += 2;
            continue;
        }

        if (*str == CHAR_NEWLINE)
        {
            if (w > best) best = w;
            w = 0;
            str++;
            continue;
        }

        w += font->widths[*str];
        str++;
    }

    return (w > best) ? w : best;
}

int UiTextRight(int xRight, int y, const u8 *str, u16 fg, u16 shadow)
{
    return UiText(xRight - UiTextWidth(str), y, str, fg, shadow);
}

// ------------------------------------------------------ text that must fit --
//
// A string is walked in units: one glyph byte, a two-byte CHAR_EXTRA_SYMBOL or
// CHAR_KEYPAD_ICON, or a control code with its arguments. A cut is only ever
// between two units, so no code or glyph is split.

// The bytes in the unit at `str`, or 0 for a bad string that stops the walk.
static int UnitLen(const u8 *str)
{
    if (*str == EXT_CTRL_CODE_BEGIN)
        return Truncated(str) ? 0 : CtrlCodeSpan(str);
    if (*str == CHAR_EXTRA_SYMBOL || *str == CHAR_KEYPAD_ICON)
        return Truncated(str) ? 0 : 2;
    return 1;
}

// The pen after the unit at `str`, from `w`. The same rules as TextWidth.
static int UnitPen(const struct UiFont *font, const u8 *str, int w)
{
    if (*str == EXT_CTRL_CODE_BEGIN)
    {
        u8 arg = str[2];

        switch (str[1])
        {
        case EXT_CTRL_CODE_CLEAR:    return w + arg;
        case EXT_CTRL_CODE_SKIP:     return arg;
        case EXT_CTRL_CODE_CLEAR_TO: return (arg > w) ? arg : w;
        default:                     return w;
        }
    }

    if (*str == CHAR_EXTRA_SYMBOL)
        return w + font->widths[str[1] | 0x100];
    if (*str == CHAR_KEYPAD_ICON)
        return w;
    return w + font->widths[*str];
}

// One line of `str`, which ends at a newline or EOS, measured against `maxW`.
struct LineFit
{
    int len;        // bytes to the end of the line
    int fit;        // bytes of the longest start that fits
    int wrap;       // bytes before the last space that fits, or -1
    bool8 over;     // the whole line does not fit
};

static void FitLine(const struct UiFont *font, const u8 *str, int maxW,
                    struct LineFit *out)
{
    int i = 0, w = 0;
    int guard = UI_TEXT_MAX;

    out->fit = 0;
    out->wrap = -1;
    out->over = FALSE;

    while (str[i] != EOS && str[i] != CHAR_NEWLINE && guard-- > 0)
    {
        int n = UnitLen(&str[i]);
        int next;

        if (n == 0)
            break;

        // A break goes before the space, and the space itself is dropped, so
        // the text before it is what must fit.
        if (str[i] == CHAR_SPACE && !out->over)
            out->wrap = i;

        next = UnitPen(font, &str[i], w);
        if (next > maxW)
            out->over = TRUE;
        if (!out->over)
            out->fit = i + n;

        w = next;
        i += n;
    }

    out->len = i;
}

// Draw `n` bytes of `str`, and the ellipsis after them if `ellipsis`. Trailing
// spaces before an ellipsis go, so it reads "SEA…" and not "SEA …".
static int DrawCut(const struct UiFont *font, int x, int y, const u8 *str,
                   int n, bool8 ellipsis, u16 fg, u16 shadow)
{
    u8 buf[UI_TEXT_MAX + 2];

    if (n > UI_TEXT_MAX)
        n = UI_TEXT_MAX;

    memcpy(buf, str, (size_t)n);

    if (ellipsis)
    {
        while (n > 0 && buf[n - 1] == CHAR_SPACE)
            n--;
        buf[n++] = CHAR_ELLIPSIS;
    }

    buf[n] = EOS;
    return DrawText(font, x, y, buf, fg, shadow, 1);
}

// One line with an ellipsis, in `maxW`: the start that fits beside the
// ellipsis. If even the ellipsis does not fit, nothing draws.
static int DrawEllipsized(const struct UiFont *font, int x, int y, int maxW,
                          const u8 *str, u16 fg, u16 shadow)
{
    struct LineFit fit;
    int room = maxW - font->widths[CHAR_ELLIPSIS];

    if (room < 0)
        return 0;

    FitLine(font, str, room, &fit);
    return DrawCut(font, x, y, str, fit.fit, TRUE, fg, shadow);
}

int UiTextClipped(int x, int y, int maxW, const u8 *str, u16 fg, u16 shadow)
{
    struct LineFit fit;

    if (str == NULL)
        return 0;

    FitLine(&sFontNormal, str, maxW, &fit);

    if (!fit.over && str[fit.len] == EOS)
        return UiText(x, y, str, fg, shadow);

    return DrawEllipsized(&sFontNormal, x, y, maxW, str, fg, shadow);
}

int UiTextWrapped(int x, int y, int maxW, int maxLines, const u8 *str,
                  u16 fg, u16 shadow)
{
    int lines = 0;
    int guard = UI_TEXT_MAX;

    if (str == NULL)
        return 0;

    while (*str != EOS && lines < maxLines && guard-- > 0)
    {
        struct LineFit fit;
        bool8 last = (lines == maxLines - 1);
        int n;

        FitLine(&sFontNormal, str, maxW, &fit);

        if (!fit.over)
        {
            const u8 *rest = str + fit.len;

            if (*rest == CHAR_NEWLINE)
                rest++;

            // The last line, with text still to come: say so.
            if (last && *rest != EOS)
            {
                DrawEllipsized(&sFontNormal, x, y, maxW, str, fg, shadow);
                return lines + 1;
            }

            DrawCut(&sFontNormal, x, y, str, fit.len, FALSE, fg, shadow);
            str = rest;
        }
        else if (last)
        {
            DrawEllipsized(&sFontNormal, x, y, maxW, str, fg, shadow);
            return lines + 1;
        }
        else
        {
            // At the last space that fits, or inside the word when one word is
            // wider than the line. At least one unit, so the walk always moves.
            n = (fit.wrap > 0) ? fit.wrap : fit.fit;
            if (n == 0)
                n = UnitLen(str) ? UnitLen(str) : 1;

            DrawCut(&sFontNormal, x, y, str, n, FALSE, fg, shadow);
            str += n;

            // The space that the break replaced, and any after it.
            while (*str == CHAR_SPACE)
                str++;
        }

        y += UI_LINE_H;
        lines++;
    }

    return lines;
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

// The width that UiNum() draws for this value. It uses the same NumToStr as the
// draw calls, so it always agrees with the screen.
int UiNumWidth(s32 value)
{
    u8 buf[16];
    NumToStr(buf, value);
    return UiTextWidth(buf);
}

u8 *UiAscii(u8 *dst, const char *ascii, int dstSize)
{
    // Separate source and destination positions, because the e-acute below is
    // two bytes of input and one glyph of output.
    int i = 0, o = 0;

    if (dstSize <= 0)
        return dst;

    for (; ascii[i] != '\0' && o < dstSize - 1; i++)
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
        else if (c == '!')             out = CHAR_EXCL_MARK;
        // Feet and inches, for the Pokedex height.
        else if (c == '\'')            out = CHAR_SGL_QUOTE_RIGHT;
        else if (c == '"')             out = CHAR_DBL_QUOTE_RIGHT;
        else if (c == '\n')            out = CHAR_NEWLINE;
        // The UTF-8 e-acute (C3 A9), so a literal can spell POKéMON as the game
        // does.
        else if ((u8)c == 0xC3 && (u8)ascii[i + 1] == 0xA9)
        {
            out = CHAR_e_ACUTE;
            i++;
        }
        else                           out = CHAR_SPACE;

        dst[o++] = out;
    }

    dst[o] = EOS;
    return dst;
}
