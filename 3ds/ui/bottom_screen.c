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
#include "data.h"              // gSpeciesNames
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
    // Not a game feature, so nothing to unlock: always available.
    [UI_TAB_EXTRA] = { "EXTRA", 0                    },
};

// Whether the game's save data exists to be read yet.
//
// gSaveBlock1Ptr and gSaveBlock2Ptr start as NULL (src/load_save.c:41-42) and
// are only assigned once a file is loaded or started. Every flag read below
// goes through gSaveBlock1Ptr, so before that point FlagGet() is a null
// dereference plus a field offset: flags[] sits at offset 0x1270, which puts
// the read at roughly 0x1300 with nothing mapped there.
//
// Azahar tolerated that for months of testing. A real ARM11 does not, and
// faulted on the first hardware boot with "data abort, translation - section,
// access type Read" at exactly that address. CtrBottomUpdate gates touch input
// and Redraw() on sInGame, but the state hash is polled unconditionally, so
// this was reached on literally the first frame.
//
// Deliberately not the same question as sInGame. That latches on reaching the
// overworld and stays true through battles and menus, which is what the screen
// wants; this asks only whether there is save data to read at all.
static bool8 SaveDataLive(void)
{
    return gSaveBlock1Ptr != NULL && gSaveBlock2Ptr != NULL;
}

static bool8 TabUnlocked(u32 tab)
{
    // The EXTRA tab's testing override. Deliberately checked before the flag
    // rather than folded into it, so the normal path is unchanged and the
    // override reads as the exception it is.
    if (Ctr3dsGetShowAllTabs())
        return TRUE;

    // Ordered so the two answers that need no save data come first, and the
    // flag read is only reached once there is a save block to read it from.
    if (sTabs[tab].flag == 0)
        return TRUE;

    if (!SaveDataLive())
        return FALSE;

    return FlagGet(sTabs[tab].flag);
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

// --------------------------------------------------------- shiny notice ----
//
// A shiny you can actually catch is the one thing on this screen worth
// interrupting the player for. Emerald says so twice already -- the sprite is
// recoloured and the encounter opens with a sparkle -- but both land in the
// first second of a battle whose transition the player may not have been
// watching, and neither of them survives being missed.
//
// It is a MODAL PANEL, centred, not a strip along an edge. A thin band at the
// top was the first attempt and it was easy to miss entirely: it sat where the
// eye is not, it was the height of one line, and on the PARTY tab it looked
// like part of the cheat tag strip. This takes the middle of the screen, states
// the species at double size, and has to be dismissed on purpose.
//
// Nothing is reserved for it. Every tab's layout is hand-fitted to a 192px
// content area, which 3ds/UI_SKIN_PLAN.md declares load bearing, so the panel
// is drawn over whatever is behind it and takes every touch inside its rect.
// UiWindowFrame's centre tiles are opaque, so it genuinely covers rather than
// floating over a readable background.
//
// 30x14 tiles is 240x112, which centres exactly in 320x192 on whole 8px
// boundaries -- (320-240)/2 and (192-112)/2 are both 40 -- and UiWindowFrame
// takes tiles, so that is not a coincidence to be broken casually.
#define NOTICE_TX     5
#define NOTICE_TY     5
#define NOTICE_TW     30
#define NOTICE_TH     14

#define NOTICE_X      (NOTICE_TX * 8)     // 40
#define NOTICE_Y      (NOTICE_TY * 8)     // 40
#define NOTICE_W      (NOTICE_TW * 8)     // 240
#define NOTICE_H      (NOTICE_TH * 8)     // 112

// The interior, inside the frame's 8px border: x 48..272, y 48..144.
#define NOTICE_IN_X   (NOTICE_X + 8)
#define NOTICE_IN_W   (NOTICE_W - 16)
#define NOTICE_IN_Y   (NOTICE_Y + 8)
#define NOTICE_IN_H   (NOTICE_H - 16)

// Three rows. The headline pairs the mon's icon with SHINY! at double size, the
// species gets a line of its own also at double size, and the dismiss button
// closes it. 52+30, 84+30, 118+22 ends at 140 inside the 144px floor.
#define NOTICE_HEAD_Y  (NOTICE_IN_Y + 4)                  // 52
#define NOTICE_NAME_Y  (NOTICE_HEAD_Y + UI_GLYPH_BIG_H + 2)   // 84
#define NOTICE_ICON_W  32
#define NOTICE_ICON_GAP 10

#define NOTICE_BTN_W  100
#define NOTICE_BTN_H  22
#define NOTICE_BTN_X  (NOTICE_IN_X + (NOTICE_IN_W - NOTICE_BTN_W) / 2)
#define NOTICE_BTN_Y  (NOTICE_NAME_Y + UI_GLYPH_BIG_H + 4)    // 118

// Which encounter the player has already dismissed the notice for. Keyed on the
// mon rather than on a bare flag, so the next shiny still gets its own notice.
// The separate "is set" flag is not redundant: a personality of 0 is legal, and
// without it that mon's notice could never be shown.
//
// Nothing has to clear this when the battle ends. UiShinyOpponent goes FALSE on
// gBattleOutcome, so the panel dismisses itself on a catch or a faint or a run
// whether or not the player ever touched it, and the next encounter carries a
// different identity anyway.
static u32   sNoticeDismissed;
static bool8 sNoticeDismissedSet;

static bool8 NoticeActive(u16 *species, u32 *identity)
{
    u32 id;

    if (!UiShinyOpponent(species, &id))
        return FALSE;

    if (identity != NULL)
        *identity = id;

    return !(sNoticeDismissedSet && id == sNoticeDismissed);
}

// ------------------------------------------------------- notice animation --
//
// The only thing on this screen that moves on its own, and the only reason it
// is affordable: NoticeTick returns FALSE whenever the panel is down, so not a
// single extra repaint is asked for on any frame without a shiny on screen.
// This screen is static by design -- a repaint is 76,800 pixels of software
// fill plus a blocking texture upload on the host -- and an ambient animation
// would pay that on every frame of the game rather than on the handful of
// seconds a shiny is being announced.
//
// It follows UiPartyTick's shape (tab_party.c): file statics, one step per
// call, TRUE while it still wants frames. The counter counts CALLS, not
// milliseconds, which is the same idiom UiHold uses -- CtrBottomUpdate runs
// once per DISPLAYED frame, so a call is a 60th of a second even under
// fast-forward, and the twinkle does not speed up with the game.

// A power of two on purpose. sNoticeAnim is a u16 and wraps after eighteen
// minutes of an undismissed panel; 65536 divides by 64 exactly, so the wrap
// lands on a cycle boundary instead of jumping the twinkle mid-step.
#define NOTICE_CYCLE      64
#define NOTICE_CORNERS    4
#define NOTICE_SWEEP_END  24   // frames the opening glint lasts

// How far in from the interior edge a corner sparkle is CENTRED. The big frame
// reaches 8px left and right of its axis, so 12 clears the 2px gold rule.
#define NOTICE_SPK_IN_X   12
#define NOTICE_SPK_IN_T   10
#define NOTICE_SPK_IN_B   12

#define SWEEP_W           4
#define SWEEP_SLANT       24

static u16   sNoticeAnim;     // frames this panel has been up
static u32   sNoticeAnimId;   // the encounter those frames belong to
static bool8 sNoticeAnimSet;

static bool8 NoticeTick(void)
{
    u32 id;

    if (!NoticeActive(NULL, &id))
    {
        sNoticeAnimSet = FALSE;
        return FALSE;
    }

    // A different encounter is a different panel. Restarting on the same
    // identity the dismiss logic keys on means the second shiny of a session
    // gets its own opening sweep instead of inheriting the first one's phase.
    if (!sNoticeAnimSet || id != sNoticeAnimId)
    {
        sNoticeAnimId = id;
        sNoticeAnimSet = TRUE;
        sNoticeAnim = 0;
    }
    else
    {
        sNoticeAnim++;
    }

    return TRUE;
}

// Which frame of the art a corner shows at `phase`, or -1 for nothing.
//
// Thresholds rather than arithmetic because the three frames are 3, 6 and 16px
// wide: an even hold on each reads as a jump into the big one, so the holds are
// tuned against that. Dark for most of the cycle, which is what makes this a
// twinkle rather than a pulse.
static int TwinkleSize(u16 phase)
{
    if (phase < 4)  return 0;
    if (phase < 8)  return 1;
    if (phase < 14) return 2;
    if (phase < 18) return 1;
    if (phase < 22) return 0;

    return -1;
}

// The glint that crosses the panel as it opens, once. Drawn on the bare ground
// before the rule and the text, so it passes BEHIND the headline the way light
// crosses glass rather than washing over the words.
//
// There is no alpha anywhere in this drawing layer -- UiFillRect writes solid
// colour -- so a soft glow is not on offer. A narrow hard band moving quickly
// is, and carrying the orange edge either side of the gold core is the same
// trick the headline uses to keep a flat fill from reading as a flat bar.
static void DrawSweep(u16 phase)
{
    // The leading edge travels the interior plus the slant plus its own width,
    // so the band starts fully off the left and finishes fully off the right
    // rather than appearing and vanishing inside the panel.
    int travel = NOTICE_IN_W + SWEEP_SLANT + SWEEP_W;
    int lead = -(SWEEP_SLANT + SWEEP_W) + (int)phase * travel / NOTICE_SWEEP_END;

    for (int r = 0; r < NOTICE_IN_H; r++)
    {
        // Lower rows lag, which is the whole of the tilt.
        int x = NOTICE_IN_X + lead
              + (NOTICE_IN_H - 1 - r) * SWEEP_SLANT / NOTICE_IN_H;
        int w = SWEEP_W;

        // UiFillRect clamps to the SCREEN, and this layer has no clip
        // rectangle at all, so the band has to be cut to the interior by hand
        // or it paints straight out over the window frame.
        if (x < NOTICE_IN_X)
        {
            w += x - NOTICE_IN_X;
            x = NOTICE_IN_X;
        }
        if (x + w > NOTICE_IN_X + NOTICE_IN_W)
            w = NOTICE_IN_X + NOTICE_IN_W - x;
        if (w <= 0)
            continue;

        UiFillRect(x, NOTICE_IN_Y + r, w, 1, UI_COL_SHINY_EDGE);
        if (w > 2)
            UiFillRect(x + 1, NOTICE_IN_Y + r, w - 2, 1, UI_COL_SHINY);
    }
}

// One sparkle per interior corner, each a quarter cycle behind the last and
// ordered around the panel rather than in reading order, so the twinkle travels
// round it instead of hopping across it.
//
// All four corners are bare ground to draw on: the headline block, the species
// line and the 100px DISMISS button are every one of them centred, which leaves
// the ends of the top and bottom rows empty.
static void DrawCornerSparkles(void)
{
    static const struct { s16 dx, dy; } sCorners[NOTICE_CORNERS] =
    {
        { NOTICE_SPK_IN_X,               NOTICE_SPK_IN_T },
        { NOTICE_IN_W - NOTICE_SPK_IN_X, NOTICE_SPK_IN_T },
        { NOTICE_IN_W - NOTICE_SPK_IN_X, NOTICE_IN_H - NOTICE_SPK_IN_B },
        { NOTICE_SPK_IN_X,               NOTICE_IN_H - NOTICE_SPK_IN_B },
    };

    for (u32 i = 0; i < NOTICE_CORNERS; i++)
    {
        u16 phase = (u16)((sNoticeAnim + i * (NOTICE_CYCLE / NOTICE_CORNERS))
                          % NOTICE_CYCLE);
        int size = TwinkleSize(phase);

        if (size >= 0)
            UiSparkle(NOTICE_IN_X + sCorners[i].dx,
                      NOTICE_IN_Y + sCorners[i].dy, (u8)size);
    }
}

static void DrawNotice(u16 species, u32 personality)
{
    u8  label[16];
    int w, x;

    UiWindowFrame(NOTICE_TX, NOTICE_TY, NOTICE_TW, NOTICE_TH);

    // The panel paints its own ground instead of sitting on the frame's.
    //
    // This is what makes a fixed gold safe. Everything else on this screen uses
    // UiThemeText/UiThemeShadow precisely because the 20 frames run from
    // near-white to near-black, and the sparkle gold is one colour that cannot
    // follow them: on the light half of that range it would be a pale mark on a
    // pale field, which is the "easy to miss" failure the modal panel exists to
    // fix. A dark ground makes it read identically on all 20. The frame still
    // draws the border, so the panel is still visibly the player's.
    UiFillRect(NOTICE_IN_X, NOTICE_IN_Y, NOTICE_IN_W, NOTICE_IN_H, UI_COL_SHADOW);

    // On the bare ground and under everything else, so the opening glint passes
    // behind the headline rather than over it. Once per encounter: NoticeTick
    // restarts the counter for each new shiny, and this is the only thing that
    // reads the low end of it.
    if (sNoticeAnim < NOTICE_SWEEP_END)
        DrawSweep(sNoticeAnim);

    // Gold rule just inside the frame, two passes for a 2px line -- the same
    // idiom the selected move row and the EXTRA toggles use for emphasis.
    UiRect(NOTICE_IN_X, NOTICE_IN_Y, NOTICE_IN_W, NOTICE_IN_H, UI_COL_SHINY);
    UiRect(NOTICE_IN_X + 1, NOTICE_IN_Y + 1, NOTICE_IN_W - 2, NOTICE_IN_H - 2,
           UI_COL_SHINY_EDGE);

    // Icon and headline as one centred block, so the pair stays balanced rather
    // than the icon hanging off a fixed left margin.
    //
    // The icon is the party icon, not the Pokedex front sprite. Neither has a
    // shiny palette in Gen 3 -- the shiny colours are a battle-sprite palette
    // the dex art never loads -- and a 64x64 portrait in ordinary colours on a
    // panel shouting SHINY would read as a contradiction. An icon is small
    // enough to be taken as a label for the species rather than a picture of
    // this individual.
    UiAscii(label, "SHINY!", sizeof(label));
    w = NOTICE_ICON_W + NOTICE_ICON_GAP + UiTextBigWidth(label);
    x = NOTICE_IN_X + (NOTICE_IN_W - w) / 2;

    UiMonIcon(x, NOTICE_HEAD_Y + (UI_GLYPH_BIG_H - NOTICE_ICON_W) / 2,
              species, personality);

    // Gold body over an orange shadow, which is the sprite's own ramp rather
    // than a generic drop shadow: the star art shades from gold into orange at
    // its edges, so the headline picks up depth the same way it does.
    UiTextBig(x + NOTICE_ICON_W + NOTICE_ICON_GAP, NOTICE_HEAD_Y, label,
              UI_COL_SHINY, UI_COL_SHINY_EDGE);

    // The species on its own line, also doubled: it is the half of the message
    // the player actually has to act on.
    //
    // The pale step of the ramp, not UiThemeText(): the theme colours track the
    // frame, and on a light frame they are dark ink meant for a light field,
    // which on this panel's own dark ground would be near-invisible. Cream also
    // keeps the hierarchy right, sitting a step under the gold headline instead
    // of competing with it.
    UiTextBig(NOTICE_IN_X + (NOTICE_IN_W - UiTextBigWidth(gSpeciesNames[species])) / 2,
              NOTICE_NAME_Y, gSpeciesNames[species],
              UI_COL_SHINY_PALE, UI_COL_SHADOW);

    // A real control rather than "tap anywhere". Only this rect dismisses, so
    // a stray touch on a panel the player is still reading does not throw it
    // away; the rest of the panel absorbs touches without acting on them.
    UiRect(NOTICE_BTN_X, NOTICE_BTN_Y, NOTICE_BTN_W, NOTICE_BTN_H, UI_COL_SHINY);
    UiAscii(label, "DISMISS", sizeof(label));
    UiText(NOTICE_BTN_X + (NOTICE_BTN_W - UiTextWidth(label)) / 2,
           NOTICE_BTN_Y + (NOTICE_BTN_H - UI_GLYPH_H) / 2,
           label, UI_COL_SHINY_PALE, UI_COL_SHADOW);

    // Last, so a sparkle is never half-hidden behind the text it is decorating.
    // The corners are clear of every element above, but "clear" is a property
    // of the current layout and drawing order is a property of this function.
    DrawCornerSparkles();
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

    u32 top[6];
    top[0] = UiFrameId();
    top[1] = sInGame;
    // The override is host-side and always safe to read; the three flags are
    // not, so they are only folded in once there is a save block behind them.
    // Before that the hash simply reports "no tabs unlocked", which is both
    // true and what the blank pre-game screen already shows.
    top[2] = (u32)(Ctr3dsGetShowAllTabs() != 0) << 3;

    if (SaveDataLive())
        top[2] |= (u32)FlagGet(FLAG_SYS_POKEMON_GET)
               |  ((u32)FlagGet(FLAG_SYS_POKENAV_GET) << 1)
               |  ((u32)FlagGet(FLAG_SYS_POKEDEX_GET) << 2);
    // The matchup badges depend on who we are facing, so the opponent has to be
    // in here or they would go stale when it switches.
    top[3] = UiMatchupOpponentKey();

    // Tab-conditional state, in a slot of its own rather than XORed onto the
    // always-live matchup key above: two keys sharing a slot can cancel. Only
    // one of these can ever be live, and each is asked for only while its tab is
    // up, because both walk data the other tabs have no reason to touch --
    // counting the dex means every entry, and the map position means the whole
    // of InitMapBasedOnPlayerLocation.
    // Every one of these walks save data: the dex counts live in
    // gSaveBlock2Ptr->pokedex, the map position reads gSaveBlock1Ptr, and the
    // level cap behind the party's cheat tags reads badge flags.
    top[4] = 0;
    if (SaveDataLive())
    {
        if (sTab == UI_TAB_DEX)
            top[4] = UiDexStateKey();
        else if (sTab == UI_TAB_MAP)
            top[4] = UiMapStateKey();
        else if (sTab == UI_TAB_EXTRA)
            top[4] = UiExtraStateKey();
        // The party grid's cheat tags print the live level cap, which steps
        // up the moment a badge is earned, and the detail view's IV/EV panel
        // prints EVs, which move after a battle without necessarily moving
        // anything else in this hash. Neither touches another slot.
        else if (sTab == UI_TAB_PARTY)
            top[4] = UiPartyStateKey();
    }

    // The shiny notice, which nothing else here covers: it appears when a
    // catchable shiny does, disappears when the player dismisses it, and
    // disappears again when the battle ends. Safe before there is a save block,
    // because everything behind it is a plain global gated on gMain.inBattle.
    top[5] = NoticeActive(NULL, NULL);

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
    u16 noticeSpecies = SPECIES_NONE;
    u32 noticePersonality = 0;
    // A repaint is 76,800 pixels of software fill plus the active tab's own
    // drawing, and it is what every touch handler asks for through
    // UiMarkDirty(). That makes it the first thing to rule in or out when a tap
    // costs the player a visible pause. CtrLogSlow reports nothing on a healthy
    // frame, so this costs one clock read per repaint. See 3ds/bridge.h.
    unsigned int t0 = CtrTimeNowMs();

    // Before the game proper is running there is nothing meaningful to show,
    // and a menu floating under the title screen looks broken.
    if (!sInGame)
    {
        UiClear(0);
        sNeedsRepaint = 0;
        sDirty = 1;
        CtrLogSlow("redraw", t0);
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
    }

    // Last, and over the top of whichever tab just drew: it is an alert, and an
    // alert a view can paint over is not one.
    if (NoticeActive(&noticeSpecies, &noticePersonality))
        DrawNotice(noticeSpecies, noticePersonality);

    DrawTabBar(vis, n);

    sNeedsRepaint = 0;
    sDirty = 1;          // tell the host to re-upload

    CtrLogSlow("redraw", t0);
}

void CtrBottomInit(void)
{
    sTab = UI_TAB_PARTY;
    sSelectedMon = 0;
    sLastStateHash = 0;
    sInGame = FALSE;
    sNoticeDismissed = 0;
    sNoticeDismissedSet = FALSE;
    Redraw();
}

void CtrBottomUpdate(const CtrTouchState *touch)
{
    u32 hash;
    u32 noticeIdentity = 0;
    // The whole of the touch response, so the log can separate it from the
    // repaint it usually ends in: `update` slow with `redraw` fast means the
    // cost is in a touch handler or in UiStateHash, not in the painting.
    unsigned int t0 = CtrTimeNowMs();

    UpdateInGameLatch();

    // Nothing is interactive before the game starts.
    if (!sInGame)
        touch = NULL;

    // The panel is modal, so it takes every touch inside its rect before the
    // tabs see it -- a press that never becomes a release included, or a drag
    // begun on the panel would carry on into whatever it is covering. Only the
    // DISMISS button does anything; the rest of the panel absorbs and ignores.
    //
    // The tab bar is deliberately still live. The panel sits entirely in the
    // content area, so a player who wants to check their party before throwing
    // a ball can still switch tabs, and the panel follows them there.
    if (touch != NULL
        && UiHit(touch, NOTICE_X, NOTICE_Y, NOTICE_W, NOTICE_H)
        && NoticeActive(NULL, &noticeIdentity))
    {
        if (touch->justReleased
            && UiHit(touch, NOTICE_BTN_X, NOTICE_BTN_Y,
                     NOTICE_BTN_W, NOTICE_BTN_H))
        {
            sNoticeDismissed = noticeIdentity;
            sNoticeDismissedSet = TRUE;
            sNeedsRepaint = 1;
        }
    }
    // A tap on the tab bar switches views; anything above it belongs to the
    // active view. Acting on release rather than press means a touch that
    // slides off a tab does not trigger it.
    else if (touch != NULL && touch->justReleased && touch->y >= UI_CONTENT_H)
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
        }
    }

    // A moving HP bar needs a repaint every frame until it settles, and then
    // must stop: this screen is otherwise static and full repaints are not free.
    if (sInGame && UiPartyTick())
        sNeedsRepaint = 1;

    // The shiny panel's sparkles, on the same terms. Both ticks live here
    // rather than inside Redraw because a tick that only ran when the screen
    // happened to repaint would stall exactly when it is the thing that ought
    // to be causing the repaint.
    if (sInGame && NoticeTick())
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

    CtrLogSlow("bottom.update", t0);
}

int CtrBottomIsDirty(void)            { return sDirty; }
void CtrBottomClearDirty(void)        { sDirty = 0; }
const u16 *CtrBottomFramebuffer(void) { return UiFb(); }
