// Bottom-screen shell (game side).
//
// This translation unit is compiled with the game's headers, NOT libctru's --
// see 3ds/bridge.h for why. That is the whole point of the split: Emerald's
// party data, item tables, fonts, mon icons and palettes are ordinary symbols
// here, so the UI reads real game state directly instead of scraping RAM.
//
// This file owns only the frame: the tab bar, which view is active, and when
// the screen needs repainting. Each tab draws its own content area
// (tab_party.c, tab_bag.c, tab_map.c) through the primitives in ui_draw.h and
// ui_text.h.
//
// Redraw policy matters. A repaint is 76,800 pixels of software fill, so the
// screen is only rebuilt when something it depends on actually changed: the
// party state hash, or an explicit UiMarkDirty() from a view.

#include "global.h"
#include "pokemon.h"
#include "constants/species.h"

#include "../bridge.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"

// Two distinct flags: sNeedsRepaint means the framebuffer contents are stale,
// sDirty means the host has not uploaded the current contents yet. Conflating
// them repaints every frame the host happens to be behind.
static int sNeedsRepaint = 1;
static int sDirty = 1;
static u32 sLastStateHash;
static u8  sTab = UI_TAB_PARTY;
static u8  sSelectedMon;

void UiMarkDirty(void)          { sNeedsRepaint = 1; }
u8   UiSelectedMon(void)        { return sSelectedMon; }
void UiSetSelectedMon(u8 index) { sSelectedMon = index; }

// Redrawing 320x240 every frame is pointless for a mostly-static UI, so
// everything the display depends on is hashed and compared instead. That is
// not just the party: the player can change the window border in Options at any
// time, and without it in here the screen would keep the old frame until
// something unrelated happened to dirty it.
static u32 UiStateHash(void)
{
    u32 hash = 2166136261u;   // FNV-1a

    hash ^= UiFrameId();
    hash *= 16777619u;

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

// Before the player has any Pokemon -- title screen, intro, the walk to Route
// 101 -- every tab has nothing to show, and six empty frames read as a broken
// menu rather than an idle one. Show a plain identity screen instead.
static int PartyIsEmpty(void)
{
    for (u32 i = 0; i < PARTY_SIZE; i++)
        if (GetMonData(&gPlayerParty[i], MON_DATA_SPECIES) != SPECIES_NONE)
            return 0;

    return 1;
}

static void DrawCentered(int y, const char *ascii, u16 color)
{
    u8 label[32];

    UiAscii(label, ascii, sizeof(label));
    UiText((CTR_BOTTOM_WIDTH - UiTextWidth(label)) / 2, y, label,
           color, UiThemeShadow());
}

static void DrawIdle(void)
{
    UiClear(UI_COL_BG);
    UiWindowFrame(2, 5, 36, 14);

    DrawCentered(72, "POKEMON EMERALD", UiThemeText());
    DrawCentered(96, "3DS", UI_COL_ACCENT);
    DrawCentered(140, "Your team appears here", UI_COL_DIM);
}

static const char *const sTabNames[UI_TAB_COUNT] = { "PARTY", "BAG", "MAP" };

static void DrawTabBar(void)
{
    const int tabW = CTR_BOTTOM_WIDTH / UI_TAB_COUNT;

    for (int i = 0; i < UI_TAB_COUNT; i++)
    {
        int x = i * tabW;
        int active = (i == sTab);
        u8 label[12];

        UiFillRect(x, UI_CONTENT_H, tabW, UI_TABBAR_H,
                   active ? UI_COL_BG : UI_COL_HP_BACK);
        UiRect(x, UI_CONTENT_H, tabW, UI_TABBAR_H, UI_COL_DIM);

        UiAscii(label, sTabNames[i], sizeof(label));
        UiText(x + (tabW - UiTextWidth(label)) / 2,
               UI_CONTENT_H + (UI_TABBAR_H - UI_GLYPH_H) / 2,
               label, active ? UI_COL_ACCENT : UI_COL_DIM, UI_COL_SHADOW);
    }
}

static void Redraw(void)
{
    if (PartyIsEmpty())
    {
        DrawIdle();
        sNeedsRepaint = 0;
        sDirty = 1;
        return;
    }

    UiClear(UI_COL_BG);

    switch (sTab)
    {
    case UI_TAB_PARTY: UiPartyDraw(); break;
    case UI_TAB_BAG:   UiBagDraw();   break;
    case UI_TAB_MAP:   UiMapDraw();   break;
    }

    DrawTabBar();

    sNeedsRepaint = 0;
    sDirty = 1;          // tell the host to re-upload
}

void CtrBottomInit(void)
{
    sTab = UI_TAB_PARTY;
    sSelectedMon = 0;
    sLastStateHash = 0;
    Redraw();
}

void CtrBottomUpdate(const CtrTouchState *touch)
{
    // Nothing on the idle screen is interactive.
    if (PartyIsEmpty())
        touch = NULL;


    // A tap on the tab bar switches views; anything above it belongs to the
    // active view. Acting on release rather than press means a touch that
    // slides off a tab does not trigger it.
    if (touch != NULL && touch->justReleased && touch->y >= UI_CONTENT_H)
    {
        int tab = touch->x / (CTR_BOTTOM_WIDTH / UI_TAB_COUNT);

        if (tab >= 0 && tab < UI_TAB_COUNT && tab != sTab)
        {
            sTab = (u8)tab;
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
        }
    }

    // This state can change without any touch at all -- taking damage, an
    // evolution, a level-up, or the player changing the border in Options -- so
    // it is polled rather than pushed.
    u32 hash = UiStateHash();
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
