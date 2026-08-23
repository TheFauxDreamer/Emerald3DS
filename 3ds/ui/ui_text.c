// Emerald-font text for the bottom screen. See text.h.

#include "global.h"
#include "fonts.h"
#include "string_util.h"              // GetExtCtrlCodeLength
#include "constants/characters.h"

#include "ui_draw.h"
#include "ui_text.h"

// Font decoding, done here rather than through the game's DecompressGlyphTile().
//
// That function looks pure but is not: it expands 2bpp through
// sFontHalfRowLookupTable, a static table the text engine REGENERATES at
// runtime from whatever fg/bg/shadow palette indices it is currently printing
// with (GenerateFontHalfRowLookupTable, src/text.c:363). Calling it from here
// produced whatever indices the game last happened to set -- frequently 0,
// which reads as transparent, i.e. invisible text.
//
// The underlying format is simple enough to read directly, so we do, and share
// no mutable state with the text engine at all.
//
// Layout, from DecompressGlyph_Normal (src/text.c:1853), in u16 units:
//     0x00..0x07  top-left tile        0x08..0x0F  top-right
//     0x10..0x17  bottom-left          0x18..0x1F  bottom-right
// One u16 per row = 8 pixels at 2bpp, pixel 0 in the MOST significant bits.
// (Derived from sFontHalfRowOffsets packing pixel 0 into the high nibble.)
// Values: 0 = background, 1 = foreground, 2 = shadow, 3 aliases to background.
static u32 GlyphPixel(const u16 *glyph, int x, int y)
{
    int idx = ((y >= 8) ? 0x10 : 0x00) + ((x >= 8) ? 0x08 : 0x00) + (y & 7);
    u32 v = (glyph[idx] >> (14 - 2 * (x & 7))) & 3;
    return (v == 3) ? 0 : v;
}

// u16, not u8: CHAR_EXTRA_SYMBOL selects glyph `operand | 0x100` out of the same
// tables (src/text.c:1455). latin_normal.png is 256x512, so all 512 slots exist.
static void BlitGlyph(int x, int y, u16 glyphId, u16 fg, u16 shadow)
{
    const u16 *glyph = gFontNormalLatinGlyphs + (0x20 * glyphId);
    int width = gFontNormalLatinGlyphWidths[glyphId];

    if (width > 16)
        width = 16;

    for (int row = 0; row < UI_GLYPH_H; row++)
    {
        int py = y + row;
        if (py < 0 || py >= UI_H)
            continue;

        u16 *dst = &UiFb()[py * UI_W];

        for (int col = 0; col < width; col++)
        {
            int px = x + col;
            if (px < 0 || px >= UI_W)
                continue;

            u32 v = GlyphPixel(glyph, col, row);
            if (v == 0)
                continue;                       // background: leave it alone

            dst[px] = (v == 2) ? shadow : fg;
        }
    }
}

// How far past `str` an EXT_CTRL_CODE_BEGIN sequence runs, matching
// SkipExtCtrlCode() (src/string_util.c:694): the 0xFC byte, then the length the
// game reports for the code, which already counts the code byte itself.
// GetExtCtrlCodeLength returns 0 for a code it does not know, which would stall
// the walk, so floor it at 1.
static int CtrlCodeSpan(const u8 *str)
{
    u8 len = GetExtCtrlCodeLength(str[1]);

    return 1 + (len ? len : 1);
}

// A two-byte sequence whose second byte is the terminator is a malformed
// string, and stepping over it would walk past the EOS. The iteration guard
// would eventually stop that, but only after drawing rubbish.
static bool8 Truncated(const u8 *str)
{
    return str[1] == EOS;
}

// Emerald's strings are not plain byte streams. Three control codes move the
// pen without printing (src/text.c:1063-1085), and CHAR_EXTRA_SYMBOL escapes
// into the upper half of the glyph table. Without these the Pokedex prints
// "{NO}" and "??'??" as garbage, and every other game string is one control
// code away from doing the same.
//
// UiTextWidth below MUST stay in step with this: centring compares the two.
int UiText(int x, int y, const u8 *str, u16 fg, u16 shadow)
{
    int startX = x;
    int guard = UI_TEXT_MAX;

    if (str == NULL)
        return 0;

    // Bounded: these strings come from game tables, and a missing EOS would
    // otherwise walk off the end of one and draw whatever follows it.
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
            case EXT_CTRL_CODE_CLEAR:    x += arg;                            break;
            case EXT_CTRL_CODE_SKIP:     x  = startX + arg;                   break;
            case EXT_CTRL_CODE_CLEAR_TO: if (startX + arg > x) x = startX + arg; break;
            default: break;   // colours, pauses, sounds: nothing to draw here
            }

            str += CtrlCodeSpan(str);
            continue;
        }

        // Two bytes, and the second is the payload. The keypad icons live in
        // their own sheet we do not load, so that one is skipped rather than
        // drawn as a wrong glyph.
        if (*str == CHAR_EXTRA_SYMBOL || *str == CHAR_KEYPAD_ICON)
        {
            if (Truncated(str))
                break;

            if (*str == CHAR_EXTRA_SYMBOL)
            {
                u16 glyph = (u16)(str[1] | 0x100);

                BlitGlyph(x, y, glyph, fg, shadow);
                x += gFontNormalLatinGlyphWidths[glyph];
            }

            str += 2;
            continue;
        }

        if (*str == CHAR_NEWLINE)
        {
            x = startX;
            y += UI_LINE_H;
            str++;
            continue;
        }

        BlitGlyph(x, y, *str, fg, shadow);
        x += gFontNormalLatinGlyphWidths[*str];
        str++;
    }

    return x - startX;
}

// Mirrors UiText, and mirrors GetStringWidth's handling of the three pen codes
// (src/text.c:1425-1435): CLEAR adds, SKIP assigns, CLEAR_TO takes the maximum.
int UiTextWidth(const u8 *str)
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
                w += gFontNormalLatinGlyphWidths[str[1] | 0x100];

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

        w += gFontNormalLatinGlyphWidths[*str];
        str++;
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
        // Feet and inches, for the Pokedex height readout.
        else if (c == '\'')            out = CHAR_SGL_QUOTE_RIGHT;
        else if (c == '"')             out = CHAR_DBL_QUOTE_RIGHT;
        else if (c == '\n')            out = CHAR_NEWLINE;
        else                           out = CHAR_SPACE;

        dst[i] = out;
    }

    dst[i] = EOS;
    return dst;
}
