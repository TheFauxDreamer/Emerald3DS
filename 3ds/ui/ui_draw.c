// Bottom-screen drawing primitives. See draw.h for why these exist.

#include "global.h"
#include "text_window.h"
#include "pokemon_icon.h"

#include "ui_draw.h"

static u16 sFb[UI_W * UI_H];

u16 *UiFb(void) { return sFb; }

// GBA palettes are BGR555 with the high bit unused; the 3DS texture is RGB565.
// Green gains a bit, so replicate the top bit rather than leaving it dark.
u16 UiBgr555ToRgb565(u16 c)
{
    u32 r = (c      ) & 0x1F;
    u32 g = (c >>  5) & 0x1F;
    u32 b = (c >> 10) & 0x1F;
    return (u16)((r << 11) | ((g << 1) | (g >> 4)) << 5 | b);
}

void UiLoadPal(u16 *dst, const u16 *src, int count)
{
    for (int i = 0; i < count; i++)
        dst[i] = UiBgr555ToRgb565(src[i]);
}

void UiClear(u16 color)
{
    for (int i = 0; i < UI_W * UI_H; i++)
        sFb[i] = color;
}

void UiFillRect(int x, int y, int w, int h, u16 color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > UI_W) w = UI_W - x;
    if (y + h > UI_H) h = UI_H - y;
    if (w <= 0 || h <= 0)
        return;

    for (int row = 0; row < h; row++)
    {
        u16 *dst = &sFb[(y + row) * UI_W + x];
        for (int col = 0; col < w; col++)
            dst[col] = color;
    }
}

void UiRect(int x, int y, int w, int h, u16 color)
{
    if (w <= 0 || h <= 0)
        return;
    UiFillRect(x, y, w, 1, color);
    UiFillRect(x, y + h - 1, w, 1, color);
    UiFillRect(x, y, 1, h, color);
    UiFillRect(x + w - 1, y, 1, h, color);
}

void UiBlit4bppTile(int x, int y, const u8 *tile, const u16 *pal, int transparent0)
{
    for (int row = 0; row < 8; row++)
    {
        int py = y + row;
        if (py < 0 || py >= UI_H)
            continue;

        const u8 *src = tile + row * 4;   // 8 pixels, 2 per byte
        u16 *dst = &sFb[py * UI_W];

        for (int col = 0; col < 8; col++)
        {
            int px = x + col;
            if (px < 0 || px >= UI_W)
                continue;

            // Low nibble is the left pixel of each byte.
            u32 idx = (col & 1) ? (src[col >> 1] >> 4) : (src[col >> 1] & 0xF);
            if (idx == 0 && transparent0)
                continue;

            dst[px] = pal[idx];
        }
    }
}

// graphics/text_window/1.png is 24x24 -- a 3x3 nine-slice, tiles in row-major
// order. Corners are drawn once, edges and centre repeat.
void UiWindowFrame(int tx, int ty, int wTiles, int hTiles)
{
    static u16 pal[16];
    static int palReady;

    if (!palReady)
    {
        UiLoadPal(pal, gTextWindowFrame1_Pal, 16);
        palReady = 1;
    }

    if (wTiles < 2 || hTiles < 2)
        return;

    for (int row = 0; row < hTiles; row++)
    {
        int sy = (row == 0) ? 0 : (row == hTiles - 1 ? 2 : 1);

        for (int col = 0; col < wTiles; col++)
        {
            int sx = (col == 0) ? 0 : (col == wTiles - 1 ? 2 : 1);
            const u8 *tile = gTextWindowFrame1_Gfx + (sy * 3 + sx) * 32;

            // Opaque: the frame is the background, nothing shows through it.
            UiBlit4bppTile((tx + col) * 8, (ty + row) * 8, tile, pal, FALSE);
        }
    }
}

void UiMonIcon(int x, int y, u16 species, u32 personality)
{
    const u8 *gfx = GetMonIconPtr(species, personality, FALSE);
    const u16 *gbaPal = GetValidMonIconPalettePtr(species);
    u16 pal[16];

    if (gfx == NULL || gbaPal == NULL)
        return;

    UiLoadPal(pal, gbaPal, 16);

    // 32x32 sprite, 1D mapping: 16 consecutive tiles, four per row.
    for (int t = 0; t < 16; t++)
        UiBlit4bppTile(x + (t % 4) * 8, y + (t / 4) * 8, gfx + t * 32, pal, TRUE);
}

int UiHit(const CtrTouchState *t, int x, int y, int w, int h)
{
    return t->x >= x && t->x < x + w && t->y >= y && t->y < y + h;
}
