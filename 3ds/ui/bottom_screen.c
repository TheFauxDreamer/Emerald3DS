// Bottom-screen shell (game side).
//
// This translation unit is compiled with the game's headers, NOT libctru's --
// see 3ds/bridge.h for why. That is the whole point of the split: Emerald's
// party data, item tables, fonts, mon icons and palettes are ordinary symbols
// here, so the UI reads real game state directly instead of scraping RAM.
//
// This file owns only the frame: which tabs exist, which view is active, and
// when the screen needs repainting. Each tab draws its own content area
// (tab_party.c, tab_bag.c, tab_map.c, tab_dex.c) through the primitives in
// ui_draw.h and ui_text.h.
//
// Redraw policy matters. A repaint is 76,800 pixels of software fill, so the
// screen is only rebuilt when something it depends on actually changed.

#include "global.h"
#include "main.h"
#include "pokemon.h"
#include "event_data.h"
#include "overworld.h"
#include "constants/flags.h"
#include "constants/species.h"

#include "../bridge.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "matchup.h"

// Two distinct flags: sNeedsRepaint means the framebuffer contents are stale,
// sDirty means the host has not uploaded the current contents yet. Conflating
// them repaints every frame the host happens to be behind.
static int sNeedsRepaint = 1;
static int sDirty = 1;
static u32 sLastStateHash;
static u8  sTab = UI_TAB_PARTY;
static u8  sSelectedMon;
static bool8 sInGame;

void UiMarkDirty(void)          { sNeedsRepaint = 1; }
u8   UiSelectedMon(void)        { return sSelectedMon; }
void UiSetSelectedMon(u8 index) { sSelectedMon = index; }

// ---------------------------------------------------------------- tabs -----
//
// Which tabs exist mirrors BuildNormalStartMenu() (src/start_menu.c): the
// second screen must not offer the Pokedex or the map before the player has
// been given them. A zero flag means always available, which is what the start
// menu does for the bag.
struct UiTabDef
{
    const char *name;
    u16 flag;
};

static const struct UiTabDef sTabs[UI_TAB_COUNT] =
{
    [UI_TAB_PARTY] = { "PARTY", FLAG_SYS_POKEMON_GET },
    [UI_TAB_BAG]   = { "BAG",   0                    },
    [UI_TAB_MAP]   = { "MAP",   FLAG_SYS_POKENAV_GET },
    [UI_TAB_DEX]   = { "DEX",   FLAG_SYS_POKEDEX_GET },
    // Not game features, so nothing to unlock: always available.
    [UI_TAB_EXTRA] = { "EXTRA", 0                    },
    [UI_TAB_LINK]  = { "LINK",  0                    },
};

static bool8 TabUnlocked(u32 tab)
{
    return sTabs[tab].flag == 0 || FlagGet(sTabs[tab].flag);
}

// Fills `out` with the visible tab ids in order and returns how many. The bag
// has no flag, so this can never return zero.
static u32 VisibleTabs(u8 *out)
{
    u32 n = 0;

    for (u32 i = 0; i < UI_TAB_COUNT; i++)
        if (TabUnlocked(i))
            out[n++] = (u8)i;

    return n;
}

// Flags only ever get set, so an active tab cannot normally vanish. Guard it
// anyway rather than indexing a hidden tab.
static void EnsureTabVisible(void)
{
    u8 vis[UI_TAB_COUNT];
    u32 n = VisibleTabs(vis);

    for (u32 i = 0; i < n; i++)
        if (vis[i] == sTab)
            return;

    sTab = vis[0];
}

// ------------------------------------------------------------- lifecycle ---
//
// The screen must stay blank on the title screen and through the intro, but
// stay up during battles and menus once the game proper is running. A live
// "are we in the overworld" test would blink it out on every battle, so latch
// on having reached the overworld once instead.
static void UpdateInGameLatch(void)
{
    if (gMain.callback2 == CB2_Overworld)
        sInGame = TRUE;
}

// --------------------------------------------------------------- redraw ----
//
// Everything the display depends on is hashed and compared, so the screen is
// rebuilt only when one of those inputs actually moved. That is not just the
// party: the window border and the unlock flags can change at any time, and
// without them here the screen would keep the stale version until something
// unrelated happened to dirty it.
static u32 UiStateHash(void)
{
    u32 hash = 2166136261u;   // FNV-1a

    u32 top[4];
    top[0] = UiFrameId();
    top[1] = sInGame;
    top[2] = (u32)FlagGet(FLAG_SYS_POKEMON_GET)
           | ((u32)FlagGet(FLAG_SYS_POKENAV_GET) << 1)
           | ((u32)FlagGet(FLAG_SYS_POKEDEX_GET) << 2);
    // The matchup badges depend on who we are facing, so the opponent has to be
    // in here or they would go stale when it switches.
    top[3] = UiMatchupOpponentKey();
    // Only while that tab is up: counting walks every dex entry.
    if (sTab == UI_TAB_DEX)
        top[3] ^= UiDexStateKey();

    for (u32 i = 0; i < ARRAY_COUNT(top); i++)
    {
        hash ^= top[i];
        hash *= 16777619u;
    }

    for (u32 i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *mon = &gPlayerParty[i];
        u32 fields[5];

        fields[0] = GetMonData(mon, MON_DATA_SPECIES);
        fields[1] = GetMonData(mon, MON_DATA_HP);
        fields[2] = GetMonData(mon, MON_DATA_MAX_HP);
        fields[3] = GetMonData(mon, MON_DATA_LEVEL);
        fields[4] = GetMonData(mon, MON_DATA_STATUS);

        for (u32 f = 0; f < ARRAY_COUNT(fields); f++)
        {
            hash ^= fields[f];
            hash *= 16777619u;
        }
    }

    return hash;
}

static void DrawTabBar(const u8 *vis, u32 n)
{
    const int tabW = CTR_BOTTOM_WIDTH / (int)n;

    for (u32 i = 0; i < n; i++)
    {
        int x = (int)i * tabW;
        // The last tab absorbs the rounding remainder so the bar fills the width.
        int w = (i == n - 1) ? (CTR_BOTTOM_WIDTH - x) : tabW;
        int active = (vis[i] == sTab);
        u8 label[12];

        UiFillRect(x, UI_CONTENT_H, w, UI_TABBAR_H,
                   active ? UI_COL_BG : UI_COL_HP_BACK);
        UiRect(x, UI_CONTENT_H, w, UI_TABBAR_H, UI_COL_DIM);

        UiAscii(label, sTabs[vis[i]].name, sizeof(label));
        UiText(x + (w - UiTextWidth(label)) / 2,
               UI_CONTENT_H + (UI_TABBAR_H - UI_GLYPH_H) / 2,
               label, active ? UI_COL_ACCENT : UI_COL_DIM, UI_COL_SHADOW);
    }
}

static void Redraw(void)
{
    u8 vis[UI_TAB_COUNT];
    u32 n;

    // Before the game proper is running there is nothing meaningful to show,
    // and a menu floating under the title screen looks broken.
    if (!sInGame)
    {
        UiClear(0);
        sNeedsRepaint = 0;
        sDirty = 1;
        return;
    }

    EnsureTabVisible();
    n = VisibleTabs(vis);

    UiClear(UI_COL_BG);

    switch (sTab)
    {
    case UI_TAB_PARTY: UiPartyDraw(); break;
    case UI_TAB_BAG:   UiBagDraw();   break;
    case UI_TAB_MAP:   UiMapDraw();   break;
    case UI_TAB_DEX:   UiDexDraw();   break;
    case UI_TAB_EXTRA: UiExtraDraw(); break;
    case UI_TAB_LINK:  UiLinkDraw();  break;
    }

    DrawTabBar(vis, n);

    sNeedsRepaint = 0;
    sDirty = 1;          // tell the host to re-upload
}

void CtrBottomInit(void)
{
    sTab = UI_TAB_PARTY;
    sSelectedMon = 0;
    sLastStateHash = 0;
    sInGame = FALSE;
    Redraw();
}

void CtrBottomUpdate(const CtrTouchState *touch)
{
    u32 hash;

    UpdateInGameLatch();

    // Nothing is interactive before the game starts.
    if (!sInGame)
        touch = NULL;

    // A tap on the tab bar switches views; anything above it belongs to the
    // active view. Acting on release rather than press means a touch that
    // slides off a tab does not trigger it.
    if (touch != NULL && touch->justReleased && touch->y >= UI_CONTENT_H)
    {
        u8 vis[UI_TAB_COUNT];
        u32 n = VisibleTabs(vis);
        u32 i = (u32)touch->x * n / CTR_BOTTOM_WIDTH;

        if (i < n && vis[i] != sTab)
        {
            sTab = vis[i];
            sNeedsRepaint = 1;
        }
    }
    else if (touch != NULL && touch->y < UI_CONTENT_H)
    {
        switch (sTab)
        {
        case UI_TAB_PARTY: UiPartyTouch(touch); break;
        case UI_TAB_BAG:   UiBagTouch(touch);   break;
        case UI_TAB_MAP:   UiMapTouch(touch);   break;
        case UI_TAB_DEX:   UiDexTouch(touch);   break;
        case UI_TAB_EXTRA: UiExtraTouch(touch); break;
        case UI_TAB_LINK:  UiLinkTouch(touch);  break;
        }
    }

    // A moving HP bar needs a repaint every frame until it settles, and then
    // must stop: this screen is otherwise static and full repaints are not free.
    if (sInGame && UiPartyTick())
        sNeedsRepaint = 1;

    // This state can change without any touch at all -- taking damage, an
    // evolution, a level-up, the player changing the border in Options, or
    // being handed the Pokedex -- so it is polled rather than pushed.
    hash = UiStateHash();
    if (hash != sLastStateHash)
    {
        sLastStateHash = hash;
        sNeedsRepaint = 1;
    }

    if (sNeedsRepaint)
        Redraw();
}

int CtrBottomIsDirty(void)            { return sDirty; }
void CtrBottomClearDirty(void)        { sDirty = 0; }
const u16 *CtrBottomFramebuffer(void) { return UiFb(); }
