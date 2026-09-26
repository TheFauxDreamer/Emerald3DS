// DOWSING: the Itemfinder's range as a radar. See view_home.h.
//
// The Itemfinder (ItemfinderCheckForHiddenItems, src/item_use.c) looks 7
// metatiles to each side and 5 up and down from the player. This page shows
// the same 15x11 metatiles, with a dot for each hidden item that is not picked
// up. Ctr3dsHiddenItemAt asks the game for each place, and searches a
// connected map the game's way. The game's own search writes a task, so the
// loop over the range is here.

#include "global.h"
#include "field_player_avatar.h"      // PlayerGetDestCoords
#include "fieldmap.h"                 // gMapHeader
#include "item.h"                     // CheckBagHasItem
#include "item_use.h"                 // Ctr3dsHiddenItemAt
#include "constants/items.h"

#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "view_home.h"

#define RANGE_X      7
#define RANGE_Y      5
#define GRID_W       (2 * RANGE_X + 1)
#define GRID_H       (2 * RANGE_Y + 1)

// 12px metatiles: 180x132, centered, under the title line.
#define CELL         12
#define GRID_X       ((CTR_BOTTOM_WIDTH - GRID_W * CELL) / 2)
#define GRID_Y       (UI_PAGE_TOP + 2)
#define NOTE_Y       (GRID_Y + GRID_H * CELL + 4)

bool8 UiDowsingAvailable(void)
{
    return CheckBagHasItem(ITEM_ITEMFINDER, 1);
}

// TRUE when a map is loaded that the Itemfinder can search.
static bool8 MapLoaded(void)
{
    return gMapHeader.mapLayout != NULL && gMapHeader.events != NULL;
}

// One bit for each place in the range, row by row from the top left. Returns
// the number of items.
static u32 Scan(u32 *rows)
{
    s16 px, py;
    u32 n = 0;

    PlayerGetDestCoords(&px, &py);

    for (int dy = -RANGE_Y; dy <= RANGE_Y; dy++)
    {
        rows[dy + RANGE_Y] = 0;

        for (int dx = -RANGE_X; dx <= RANGE_X; dx++)
        {
            if (Ctr3dsHiddenItemAt(px + dx, py + dy))
            {
                rows[dy + RANGE_Y] |= 1u << (dx + RANGE_X);
                n++;
            }
        }
    }

    return n;
}

void UiDowsingPageDraw(void)
{
    u32 rows[GRID_H];
    u8 label[40];
    u32 n;
    int x;

    if (!UiDowsingAvailable())
    {
        UiText(UI_PAGE_LEFT, UI_PAGE_TOP, UiAscii(label, "You need the ITEMFINDER.", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
        return;
    }

    if (!MapLoaded())
        return;

    n = Scan(rows);

    // The range, a dim frame with a line for each metatile.
    UiFillRect(GRID_X, GRID_Y, GRID_W * CELL, GRID_H * CELL, UI_COL_BG);
    for (int i = 0; i <= GRID_W; i++)
        UiFillRect(GRID_X + i * CELL, GRID_Y, 1, GRID_H * CELL, UI_COL_DIM);
    for (int i = 0; i <= GRID_H; i++)
        UiFillRect(GRID_X, GRID_Y + i * CELL, GRID_W * CELL + 1, 1, UI_COL_DIM);

    // The player at the center, in the accent color.
    UiFillRect(GRID_X + RANGE_X * CELL + 3, GRID_Y + RANGE_Y * CELL + 3,
               CELL - 5, CELL - 5, UI_COL_ACCENT);

    // The items, in the gold of the shiny notice: something good to find.
    for (int r = 0; r < GRID_H; r++)
        for (int c = 0; c < GRID_W; c++)
            if (rows[r] & (1u << c))
            {
                UiFillRect(GRID_X + c * CELL + 2, GRID_Y + r * CELL + 2,
                           CELL - 3, CELL - 3, UI_COL_SHINY_EDGE);
                UiFillRect(GRID_X + c * CELL + 3, GRID_Y + r * CELL + 3,
                           CELL - 5, CELL - 5, UI_COL_SHINY);
            }

    x = UI_PAGE_LEFT;
    if (n == 0)
    {
        UiText(x, NOTE_Y, UiAscii(label, "No items in range.", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
    }
    else
    {
        x += UiNum(x, NOTE_Y, n, UiThemeText(), UiThemeShadow()) + 4;
        UiText(x, NOTE_Y, UiAscii(label, n == 1 ? "hidden item in range"
                                                : "hidden items in range", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
    }
}

u32 UiDowsingPageKey(void)
{
    u32 rows[GRID_H];
    u32 key;

    if (!UiDowsingAvailable())
        return 1;
    if (!MapLoaded())
        return 2;

    // The dots move with each step and go when an item is picked up. The scan
    // is the picture, so it is the key.
    key = Scan(rows);
    for (int r = 0; r < GRID_H; r++)
        key ^= rows[r] * (2654435761u + 2 * r);

    return key;
}
