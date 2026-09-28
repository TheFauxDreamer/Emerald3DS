// The nav bar. See ui_navbar.h.
//
// The config is 16 bits from settings.bin. Bits 0-11 are the four picks, 3
// bits each, left to right. Each holds the tab id + 1, and 0 means the default
// for that pick. Bit 12 turns the labels off. Thus 0 is the default bar with
// labels, which is what a settings file older than v15 has.

#include "global.h"

#include "../bridge.h"
#include "../achievements.h"          // AchActive, for the TROPHY dot
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "ui_navbar.h"

#define PICK_BITS    3
#define PICK_MASK    0x7
#define LABELS_OFF   (1 << 12)
#define PICKS_MASK   0xFFF

// The slot widths: HOME 80, the others 60. 4 x 60 + 80 = 320. Both are over
// the 53px floor for a finger (SECOND_SCREEN_CHEATSHEET.md, "Adding a tab").
#define SLOT_W       60
#define HOME_W       80

// With labels: the icon, then the label in the small font under it.
#define ICON_DY      6
#define HOME_ICON_DY 4
#define LABEL_DY     27

static const u8 sDefaultPicks[UI_NAV_PICKS] =
{
    UI_TAB_PARTY, UI_TAB_BAG, UI_TAB_MAP, UI_TAB_TROPHY,
};

static const char *const sLabel[UI_TAB_COUNT] =
{
    [UI_TAB_PARTY]  = "PARTY",
    [UI_TAB_BAG]    = "BAG",
    [UI_TAB_MAP]    = "MAP",
    [UI_TAB_DEX]    = "DEX",
    [UI_TAB_TROPHY] = "TROPHY",
    [UI_TAB_EXTRA]  = "HOME",
};

const char *UiNavTabName(u8 tab)
{
    return tab < UI_TAB_COUNT ? sLabel[tab] : "";
}

// ------------------------------------------------------------------ icons ---
//
// One style for all: a 1px line drawing in one color, so the active tab reads
// by its color alone. A '#' is ink.

static const char *const sIconParty[UI_NAV_ICON] =
{
    ".....######.....",
    "...##########...",
    "..############..",
    ".##############.",
    ".##############.",
    "######....######",
    "#####.####.#####",
    "#....#.##.#....#",
    "#....#.##.#....#",
    "#.....####.....#",
    "#..............#",
    ".#............#.",
    ".#............#.",
    "..#..........#..",
    "...##......##...",
    ".....######.....",
};

static const char *const sIconBag[UI_NAV_ICON] =
{
    "......####......",
    ".....#....#.....",
    ".....#....#.....",
    "..############..",
    ".#............#.",
    ".#............#.",
    ".##############.",
    ".#....####....#.",
    ".#....#..#....#.",
    ".#....####....#.",
    ".#............#.",
    ".#............#.",
    ".#............#.",
    ".#............#.",
    ".##############.",
    "................",
};

static const char *const sIconMap[UI_NAV_ICON] =
{
    ".....######.....",
    "....########....",
    "...###....###...",
    "..###......###..",
    "..###......###..",
    "..###......###..",
    "...###....###...",
    "....########....",
    ".....######.....",
    "......####......",
    "......####......",
    ".......##.......",
    ".......##.......",
    "................",
    "...##########...",
    "................",
};

static const char *const sIconDex[UI_NAV_ICON] =
{
    ".##############.",
    ".#............#.",
    ".#.####.......#.",
    ".#.#..#..###..#.",
    ".#.#..#.......#.",
    ".#.####..###..#.",
    ".#............#.",
    ".##############.",
    ".#............#.",
    ".#.##########.#.",
    ".#.#........#.#.",
    ".#.#........#.#.",
    ".#.##########.#.",
    ".#............#.",
    ".##############.",
    "................",
};

static const char *const sIconTrophy[UI_NAV_ICON] =
{
    "..############..",
    "###..........###",
    "#.#..........#.#",
    "#.#..........#.#",
    ".##..........##.",
    "..#..........#..",
    "...#........#...",
    "....##....##....",
    "......####......",
    ".......##.......",
    ".......##.......",
    ".....######.....",
    "....########....",
    "....########....",
    "................",
    "................",
};

static const char *const sIconHome[UI_NAV_ICON_HOME] =
{
    ".........##.........",
    "........####........",
    ".......##..##.......",
    "......##....##......",
    ".....##......##.....",
    "....##........##....",
    "...##..........##...",
    "..##............##..",
    ".##..............##.",
    "####............####",
    "...#............#...",
    "...#............#...",
    "...#....####....#...",
    "...#....#..#....#...",
    "...#....#..#....#...",
    "...#....#..#....#...",
    "...#....#..#....#...",
    "...##############...",
    "....................",
    "....................",
};

void UiNavDrawIcon(int x, int y, u8 tab, u16 color)
{
    const char *const *rows;
    int size = UI_NAV_ICON;

    switch (tab)
    {
    case UI_TAB_PARTY:  rows = sIconParty;  break;
    case UI_TAB_BAG:    rows = sIconBag;    break;
    case UI_TAB_MAP:    rows = sIconMap;    break;
    case UI_TAB_DEX:    rows = sIconDex;    break;
    case UI_TAB_TROPHY: rows = sIconTrophy; break;
    default:            rows = sIconHome; size = UI_NAV_ICON_HOME; break;
    }

    // One rect for each run of ink, not one for each pixel.
    for (int r = 0; r < size; r++)
    {
        for (int c = 0; c < size;)
        {
            int start;

            if (rows[r][c] != '#')
            {
                c++;
                continue;
            }

            start = c;
            while (c < size && rows[r][c] == '#')
                c++;
            UiFillRect(x + start, y + r, c - start, 1, color);
        }
    }
}

// ----------------------------------------------------------------- config ---

static bool8 Pickable(u32 tab)
{
    return tab < UI_TAB_COUNT && tab != UI_TAB_EXTRA;
}

// The four picks, left to right. A pick that is not valid, or a tab in two
// picks, gives the default for all four.
static void Picks(u8 out[UI_NAV_PICKS])
{
    u32 config = (u32)Ctr3dsGetNavConfig();
    u32 seen = 0;

    for (u32 i = 0; i < UI_NAV_PICKS; i++)
    {
        u32 v = (config >> (i * PICK_BITS)) & PICK_MASK;
        u32 tab = (v == 0) ? sDefaultPicks[i] : v - 1;

        if (!Pickable(tab) || (seen & (1u << tab)))
        {
            memcpy(out, sDefaultPicks, UI_NAV_PICKS);
            return;
        }

        seen |= 1u << tab;
        out[i] = (u8)tab;
    }
}

static void SetPicks(const u8 picks[UI_NAV_PICKS])
{
    u32 config = (u32)Ctr3dsGetNavConfig() & ~PICKS_MASK;

    for (u32 i = 0; i < UI_NAV_PICKS; i++)
        config |= ((u32)picks[i] + 1) << (i * PICK_BITS);

    Ctr3dsSetNavConfig((int)config);
}

void UiNavSlots(u8 out[UI_NAV_SLOTS])
{
    u8 picks[UI_NAV_PICKS];

    Picks(picks);
    out[0] = picks[0];
    out[1] = picks[1];
    out[UI_NAV_HOME_SLOT] = UI_TAB_EXTRA;
    out[3] = picks[2];
    out[4] = picks[3];
}

u8 UiNavSpareTab(void)
{
    u8 picks[UI_NAV_PICKS];
    u32 used = 0;

    Picks(picks);
    for (u32 i = 0; i < UI_NAV_PICKS; i++)
        used |= 1u << picks[i];

    for (u8 tab = 0; tab < UI_TAB_COUNT; tab++)
        if (Pickable(tab) && !(used & (1u << tab)))
            return tab;

    return UI_TAB_DEX;
}

bool8 UiNavLabels(void)
{
    return !(Ctr3dsGetNavConfig() & LABELS_OFF);
}

void UiNavPut(u8 pick, u8 tab)
{
    u8 picks[UI_NAV_PICKS];

    if (pick >= UI_NAV_PICKS || !Pickable(tab))
        return;

    Picks(picks);

    for (u32 i = 0; i < UI_NAV_PICKS; i++)
        if (picks[i] == tab)
            picks[i] = picks[pick];

    picks[pick] = tab;
    SetPicks(picks);
}

void UiNavSetLabels(bool8 on)
{
    u32 config = (u32)Ctr3dsGetNavConfig();

    Ctr3dsSetNavConfig((int)(on ? config & ~LABELS_OFF : config | LABELS_OFF));
}

void UiNavReset(void)
{
    Ctr3dsSetNavConfig(Ctr3dsGetNavConfig() & ~PICKS_MASK);
}

// -------------------------------------------------------------------- bar ---

#if CTR_UI_RAIL
// The rail: five slots of 48px, top to bottom, all as wide as the rail.
#define RAIL_SLOT_H  (CTR_BOTTOM_HEIGHT / UI_NAV_SLOTS)

static int SlotX(u32 slot) { (void)slot; return UI_CONTENT_W; }
static int SlotW(u32 slot) { (void)slot; return UI_RAIL_W; }
static int SlotY(u32 slot) { return (int)slot * RAIL_SLOT_H; }
static int SlotH(u32 slot) { (void)slot; return RAIL_SLOT_H; }
#else
static int SlotX(u32 slot)
{
    return (int)slot * SLOT_W + (slot > UI_NAV_HOME_SLOT ? HOME_W - SLOT_W : 0);
}

static int SlotW(u32 slot)
{
    return slot == UI_NAV_HOME_SLOT ? HOME_W : SLOT_W;
}

static int SlotY(u32 slot) { (void)slot; return UI_CONTENT_H; }
static int SlotH(u32 slot) { (void)slot; return UI_TABBAR_H; }
#endif

void UiNavDraw(u8 activeTab)
{
    u8 slots[UI_NAV_SLOTS];
    bool8 labels = UiNavLabels();
    u8 spare = UiNavSpareTab();

    UiNavSlots(slots);

    for (u32 i = 0; i < UI_NAV_SLOTS; i++)
    {
        int x = SlotX(i), w = SlotW(i);
        int y = SlotY(i), h = SlotH(i);
        bool8 home = (i == UI_NAV_HOME_SLOT);
        int size = home ? UI_NAV_ICON_HOME : UI_NAV_ICON;
        bool8 active = slots[i] == activeTab || (home && activeTab == spare);
        bool8 live = UiTabUnlocked(slots[i]);
        u16 color = active ? UI_COL_ACCENT : live ? UiThemeText() : UI_COL_DIM;
        int iconY;

        UiFillRect(x, y, w, h, active ? UI_COL_BG : UI_COL_HP_BACK);
        UiRect(x, y, w, h, UI_COL_DIM);

        if (labels)
            iconY = y + (home ? HOME_ICON_DY : ICON_DY);
        else
            iconY = y + (h - size) / 2;

        UiNavDrawIcon(x + (w - size) / 2, iconY, slots[i], color);

        if (labels)
        {
            u8 label[8];

            UiAscii(label, sLabel[slots[i]], sizeof(label));
            UiTextSmall(x + (w - UiTextSmallWidth(label)) / 2, y + LABEL_DY,
                        label, color, UI_COL_SHADOW);
        }

        // A gold dot on TROPHY when an unlock is unseen. It is what stays
        // after a toast that nobody read, so it uses the toast's gold. Never
        // on the active tab, which marks all rows as seen.
        if (slots[i] == UI_TAB_TROPHY && !active && AchActive()->anyUnseen())
        {
            UiFillRect(x + w - 12, y + 6, 7, 7, UI_COL_SHINY_EDGE);
            UiFillRect(x + w - 11, y + 7, 5, 5, UI_COL_SHINY);
        }
    }
}

u8 UiNavHit(const CtrTouchState *t)
{
    u8 slots[UI_NAV_SLOTS];

    UiNavSlots(slots);

    for (u32 i = 0; i < UI_NAV_SLOTS; i++)
    {
        if (!UiHit(t, SlotX(i), SlotY(i), SlotW(i), SlotH(i)))
            continue;

        return UiTabUnlocked(slots[i]) ? slots[i] : UI_TAB_COUNT;
    }

    return UI_TAB_COUNT;
}
