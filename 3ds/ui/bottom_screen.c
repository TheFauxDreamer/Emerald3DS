// Bottom-screen shell (game side).
//
// This file compiles with the game's headers, not libctru's (see 3ds/bridge.h).
// Thus the UI reads the party, the items, the fonts and the icons directly.
//
// This file owns the frame: the tabs, the active view and the repaint policy.
// Each tab draws its own content area with ui_draw.h and ui_text.h.
//
// A repaint fills 76,800 pixels in software. Thus the screen repaints only when
// an input changes.

#include "global.h"
#include "main.h"
#include "pokemon.h"
#include "data.h"              // gSpeciesNames
#include "event_data.h"
#include "overworld.h"
#include "constants/flags.h"
#include "constants/species.h"

#include "../bridge.h"
#include "../achievements.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "matchup.h"
#include "status_tags.h"
#include "ui_quickball.h"
#include "ui_achtoast.h"
#include "ui_title.h"
#include "ui_team.h"

// Two flags. sNeedsRepaint: the framebuffer is stale. sDirty: the host has not
// uploaded the framebuffer yet. Do not merge them.
static int sNeedsRepaint = 1;
static int sDirty = 1;
static u32 sLastStateHash;
static u8  sTab = UI_TAB_PARTY;
static u8  sSelectedMon;
static bool8 sInGame;

void UiMarkDirty(void)          { sNeedsRepaint = 1; }
u8   UiActiveTab(void)          { return sTab; }
u8   UiSelectedMon(void)        { return sSelectedMon; }
void UiSetSelectedMon(u8 index) { sSelectedMon = index; }

// ---------------------------------------------------------------- tabs -----
//
// The tabs follow BuildNormalStartMenu() (src/start_menu.c). Do not show a tab
// before the player has its item. A zero flag means "always available", as the
// start menu does for the bag.
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
    // Always available. The list has no start-menu entry to follow.
    [UI_TAB_TROPHY] = { "TROPHY", 0                  },
    // Not a game feature, so it is always available.
    [UI_TAB_EXTRA] = { "EXTRA", 0                    },
};

// TRUE when there is save data to read.
//
// gSaveBlock1Ptr and gSaveBlock2Ptr are NULL until a file loads
// (src/load_save.c). FlagGet() reads through gSaveBlock1Ptr, so an earlier call
// reads near address 0x1300. A real ARM11 faults there. The state hash polls
// every frame, so gate every save read on this function.
//
// This is not the same test as sInGame. sInGame stays TRUE through battles and
// menus. This function only says if save data exists.
static bool8 SaveDataLive(void)
{
    return gSaveBlock1Ptr != NULL && gSaveBlock2Ptr != NULL;
}

static bool8 TabUnlocked(u32 tab)
{
    // The EXTRA tab's test override. It comes before the flag so that the
    // normal path stays the same.
    if (Ctr3dsGetShowAllTabs())
        return TRUE;

    // The two answers that need no save data come first. The flag read occurs
    // only when a save block exists.
    if (sTabs[tab].flag == 0)
        return TRUE;

    if (!SaveDataLive())
        return FALSE;

    return FlagGet(sTabs[tab].flag);
}

// Fill `out` with the visible tab ids and return their count. The bag has no
// flag, so the count is never zero.
static u32 VisibleTabs(u8 *out)
{
    u32 n = 0;

    for (u32 i = 0; i < UI_TAB_COUNT; i++)
        if (TabUnlocked(i))
            out[n++] = (u8)i;

    return n;
}

// Flags never clear, so the active tab cannot usually disappear. This guard
// stops an index into a hidden tab.
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
// A catchable shiny is the one event on this screen that interrupts the player.
// The game shows it only in the first second of the battle, and the player can
// miss it.
//
// The notice is a modal panel in the center of the content area. The player
// must dismiss it. The tabs keep their layouts, and the panel covers them.
// UiWindowFrame fills its center tiles, so the panel is opaque.
//
// 30x14 tiles is 240x112. It centers exactly in 320x192 on 8px boundaries, 40px
// from each side. Keep these numbers in whole tiles.
#define NOTICE_TX     5
#define NOTICE_TY     5
#define NOTICE_TW     30
#define NOTICE_TH     14

#define NOTICE_X      (NOTICE_TX * 8)     // 40
#define NOTICE_Y      (NOTICE_TY * 8)     // 40
#define NOTICE_W      (NOTICE_TW * 8)     // 240
#define NOTICE_H      (NOTICE_TH * 8)     // 112

// The interior, inside the 8px border: x 48..272, y 48..144.
#define NOTICE_IN_X   (NOTICE_X + 8)
#define NOTICE_IN_W   (NOTICE_W - 16)
#define NOTICE_IN_Y   (NOTICE_Y + 8)
#define NOTICE_IN_H   (NOTICE_H - 16)

// Three rows: the icon with SHINY! at double size, the species at double size,
// and the DISMISS button. The last row ends at y 140, inside the 144px floor.
#define NOTICE_HEAD_Y  (NOTICE_IN_Y + 4)                  // 52
#define NOTICE_NAME_Y  (NOTICE_HEAD_Y + UI_GLYPH_BIG_H + 2)   // 84
#define NOTICE_ICON_W  32
#define NOTICE_ICON_GAP 10

#define NOTICE_BTN_W  100
#define NOTICE_BTN_H  22
#define NOTICE_BTN_X  (NOTICE_IN_X + (NOTICE_IN_W - NOTICE_BTN_W) / 2)
#define NOTICE_BTN_Y  (NOTICE_NAME_Y + UI_GLYPH_BIG_H + 4)    // 118

// The encounter that the player dismissed the notice for. The key is the mon,
// not a flag, so the next shiny gets its own notice. The "is set" flag is
// necessary because a personality of 0 is valid.
//
// Nothing clears this when the battle ends. UiShinyOpponent is FALSE when
// gBattleOutcome is set, so the panel closes by itself.
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

// -------------------------------------------------------- animation clock --
//
// The step clock for the party icons. On the single-core path, every animation
// uses it. Ctr3dsRasteriserOnOwnCore() selects the period.
//
// On one core, each repaint costs a full VBlank: fps = 3600 / (60 + repaints
// per second). Two animations on different periods cost two repaints. On one
// shared clock, their repaints occur together. The step is 12 frames. The host
// uploads a repaint in five frames (3ds/host/video.c), so the step must be
// longer than five.
//
// With a second core, a repaint costs the frame nothing. The step is 6 frames,
// the pace of the game's own party menu (sAnim_0, src/pokemon_icon.c). The
// shiny notice uses its own frame count (NoticeTick).
#define UI_ANIM_STEP_FRAMES (Ctr3dsRasteriserOnOwnCore() ? 6 : 12)

static u8    sAnimSub;
static bool8 sAnimStepped;

// TRUE on the frames when animations can advance. Ticks use this and do not
// count frames, so they stay in step.
bool8 UiAnimStepped(void)
{
    return sAnimStepped;
}

// TRUE while an overlay covers the tab.
//
// While an overlay is up, it owns the animated layer. That layer paints over a
// snapshot that contains the overlay. Thus a tab must draw its animated pieces
// into its own paint while this is TRUE (see DrawCell in tab_party.c). On the
// second-core path, the shell then repaints fully when a piece moves. On the
// single-core path, the piece stays still.
//
// The achievement toast counts too: it covers the top row of party icons.
bool8 UiOverlayActive(void)
{
    return NoticeActive(NULL, NULL) || UiQuickBallActive() || UiAchToastActive();
}

// ------------------------------------------------------- notice animation --
//
// NoticeTick returns FALSE while the panel is down, so the animation asks for
// no repaints at other times. It counts calls, not milliseconds. A call is one
// displayed frame, so the twinkle keeps its speed under fast-forward.
//
// Ctr3dsRasteriserOnOwnCore() selects one of two tunings:
// - Second core: the counter counts frames. The corners twinkle on a 64-frame
//   cycle, and a gold glint crosses the panel when it opens.
// - One core: the counter counts steps of the shared clock. The twinkle is 8
//   steps long, and a two-step burst replaces the glint.

// The twinkle length on each path: 64 frames, or 8 steps.
//
// Both are powers of two. sNoticeTime is a u16, and 65536 divides by both, so
// the wrap occurs on a cycle boundary.
#define NOTICE_FRAME_CYCLE  64
#define NOTICE_STEP_CYCLE   8
#define NOTICE_CORNERS      4

// The opening glint on the second-core path. A SWEEP_W band, slanted by
// SWEEP_SLANT pixels, crosses the panel in NOTICE_SWEEP_END frames. See
// DrawSweep.
#define NOTICE_SWEEP_END    24
#define SWEEP_W             4
#define SWEEP_SLANT         24

// The opening burst on the single-core path, in steps. All corners show the
// largest frame before the twinkle starts.
#define NOTICE_BURST        2

// The distance from the interior edge to a corner sparkle's center. The big
// frame extends 8px each side, so 12 clears the 2px gold rule.
#define NOTICE_SPK_IN_X   12
#define NOTICE_SPK_IN_T   10
#define NOTICE_SPK_IN_B   12

static u16   sNoticeTime;     // frames or steps since it opened
static u32   sNoticeAnimId;   // the encounter that owns the count
static bool8 sNoticeAnimSet;
static bool8 sNoticeFull;     // this tick needs the full panel

// The art frame that a corner shows at `phase`, or -1 for nothing. There is one
// table for each path.
//
// The three frames are 3, 6 and 16px wide. Equal holds look like a jump to the
// big frame, so the thresholds are tuned. The frame table shows the sizes for
// 4, 4, 6, 4 and 4 frames, and is dark for the other 42.
static int TwinkleSizeFrames(u16 phase)
{
    if (phase < 4)  return 0;
    if (phase < 8)  return 1;
    if (phase < 14) return 2;
    if (phase < 18) return 1;
    if (phase < 22) return 0;

    return -1;
}

// The step table has one step for each size. Each corner is lit for five steps
// and dark for three.
static int TwinkleSizeSteps(u16 phase)
{
    if (phase < 1) return 0;
    if (phase < 2) return 1;
    if (phase < 3) return 2;
    if (phase < 4) return 1;
    if (phase < 5) return 0;

    return -1;
}

// The frame that corner `i` shows at count `t`, or -1 for nothing. Each corner
// is a quarter cycle behind the previous one.
static int CornerSize(u32 i, u16 t)
{
    if (Ctr3dsRasteriserOnOwnCore())
        return TwinkleSizeFrames((u16)((t + i * (NOTICE_FRAME_CYCLE / NOTICE_CORNERS))
                                       % NOTICE_FRAME_CYCLE));

    if (t < NOTICE_BURST)
        return UI_SPARKLE_SIZES - 1;

    return TwinkleSizeSteps((u16)((t + i * (NOTICE_STEP_CYCLE / NOTICE_CORNERS))
                                  % NOTICE_STEP_CYCLE));
}

static bool8 NoticeTick(void)
{
    u32 id;

    if (!NoticeActive(NULL, &id))
    {
        sNoticeAnimSet = FALSE;
        return FALSE;
    }

    // A different encounter is a different panel. Restart the count, so that
    // the next shiny gets its own glint or burst.
    if (!sNoticeAnimSet || id != sNoticeAnimId)
    {
        sNoticeAnimId = id;
        sNoticeAnimSet = TRUE;
        sNoticeTime = 0;
        sNoticeFull = TRUE;     // the full panel,
        return TRUE;            // not only the sparkles
    }

    sNoticeFull = FALSE;

    if (Ctr3dsRasteriserOnOwnCore())
    {
        sNoticeTime++;

        // DrawNotice draws the glint into the panel, so each glint frame is a
        // full repaint. So is the frame after the last one. Without it, the
        // snapshot keeps the band, and each later sparkle step puts it back.
        if (sNoticeTime <= NOTICE_SWEEP_END)
        {
            sNoticeFull = TRUE;
            return TRUE;
        }

        // After the glint, repaint only when a corner changes size: 24 of the
        // 64 frames in a cycle.
        for (u32 i = 0; i < NOTICE_CORNERS; i++)
            if (CornerSize(i, sNoticeTime) != CornerSize(i, (u16)(sNoticeTime - 1)))
                return TRUE;

        return FALSE;
    }

    // On one core, repaint only on a step frame. More repaints lower the frame
    // rate.
    if (!UiAnimStepped())
        return FALSE;

    sNoticeTime++;

    return TRUE;
}

// The glint that crosses the panel when it opens, on the second-core path only.
// It goes on the bare ground, before the rule and the text, so it passes behind
// the headline.
//
// The drawing layer has no alpha, so the glint is a narrow hard band with an
// orange edge on each side of a gold core.
//
// The single-core path has no glint. At five steps a second, the band cannot
// move smoothly, so that path uses the burst (CornerSize).
static void DrawSweep(u16 phase)
{
    // The leading edge crosses the interior, the slant and the band width. Thus
    // the band starts and ends fully outside the panel.
    int travel = NOTICE_IN_W + SWEEP_SLANT + SWEEP_W;
    int lead = -(SWEEP_SLANT + SWEEP_W) + (int)phase * travel / NOTICE_SWEEP_END;

    for (int r = 0; r < NOTICE_IN_H; r++)
    {
        // Lower rows lag, which gives the tilt.
        int x = NOTICE_IN_X + lead
              + (NOTICE_IN_H - 1 - r) * SWEEP_SLANT / NOTICE_IN_H;
        int w = SWEEP_W;

        // UiFillRect clips only to the screen. Clip the band to the interior
        // here, or it paints over the window frame.
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

// One sparkle for each interior corner. The text rows are centered, so the four
// corners are empty.
//
// The four corner rects fit the largest frame of the art: 16x14 around its
// axis, 8px to the left and 5px above the center.
#define NOTICE_SPK_W  16
#define NOTICE_SPK_H  14
#define NOTICE_SPK_AX 8
#define NOTICE_SPK_AY 5

// In order around the panel, so the twinkle travels around it. The full paint
// and the sparkle step both use this.
static const struct { s16 dx, dy; } sCorners[NOTICE_CORNERS] =
{
    { NOTICE_SPK_IN_X,               NOTICE_SPK_IN_T },
    { NOTICE_IN_W - NOTICE_SPK_IN_X, NOTICE_SPK_IN_T },
    { NOTICE_IN_W - NOTICE_SPK_IN_X, NOTICE_IN_H - NOTICE_SPK_IN_B },
    { NOTICE_SPK_IN_X,               NOTICE_IN_H - NOTICE_SPK_IN_B },
};

static int sCornerDx(u32 i) { return sCorners[i].dx; }
static int sCornerDy(u32 i) { return sCorners[i].dy; }

static void DrawCornerSparkles(void)
{
    for (u32 i = 0; i < NOTICE_CORNERS; i++)
    {
        int size = CornerSize(i, sNoticeTime);

        if (size >= 0)
            UiSparkle(NOTICE_IN_X + sCorners[i].dx,
                      NOTICE_IN_Y + sCorners[i].dy, (u8)size);
    }
}

// The four sparkles over the snapshot, not over a new panel. When the notice is
// up, this is all that an animation step draws.
static void RedrawNoticeSparkles(void)
{
    for (u32 i = 0; i < NOTICE_CORNERS; i++)
        UiRestoreRect(NOTICE_IN_X + sCornerDx(i) - NOTICE_SPK_AX,
                      NOTICE_IN_Y + sCornerDy(i) - NOTICE_SPK_AY,
                      NOTICE_SPK_W, NOTICE_SPK_H);

    DrawCornerSparkles();
}

static void DrawNotice(u16 species, u32 personality)
{
    u8  label[16];
    int w, x;

    UiWindowFrame(NOTICE_TX, NOTICE_TY, NOTICE_TW, NOTICE_TH);

    // The panel paints its own dark ground.
    //
    // A fixed gold is not legible on the light frames. A dark ground makes it
    // legible on all 20. The frame still draws the player's border.
    UiFillRect(NOTICE_IN_X, NOTICE_IN_Y, NOTICE_IN_W, NOTICE_IN_H, UI_COL_SHADOW);

    // The glint goes on the bare ground, under everything else. Second-core
    // path only, once for each encounter. NoticeTick repaints fully for each
    // glint frame and one frame after, which removes the band from the
    // snapshot.
    if (Ctr3dsRasteriserOnOwnCore() && sNoticeTime < NOTICE_SWEEP_END)
        DrawSweep(sNoticeTime);

    // A gold rule inside the frame. Two passes make a 2px line, as for the
    // selected move row and the EXTRA toggles.
    UiRect(NOTICE_IN_X, NOTICE_IN_Y, NOTICE_IN_W, NOTICE_IN_H, UI_COL_SHINY);
    UiRect(NOTICE_IN_X + 1, NOTICE_IN_Y + 1, NOTICE_IN_W - 2, NOTICE_IN_H - 2,
           UI_COL_SHINY_EDGE);

    // The icon and the headline are one centered block.
    //
    // The icon is the party icon, not the Pokedex picture. In Gen 3, neither
    // has a shiny palette. A small icon identifies the species. A large picture
    // in normal colors on a SHINY panel is confusing.
    UiAscii(label, "SHINY!", sizeof(label));
    w = NOTICE_ICON_W + NOTICE_ICON_GAP + UiTextBigWidth(label);
    x = NOTICE_IN_X + (NOTICE_IN_W - w) / 2;

    UiMonIcon(x, NOTICE_HEAD_Y + (UI_GLYPH_BIG_H - NOTICE_ICON_W) / 2,
              species, personality);

    // Gold over an orange shadow: the ramp of the star sprite.
    UiTextBig(x + NOTICE_ICON_W + NOTICE_ICON_GAP, NOTICE_HEAD_Y, label,
              UI_COL_SHINY, UI_COL_SHINY_EDGE);

    // The species, on its own line at double size. The player acts on this.
    //
    // Use the pale step of the ramp, not UiThemeText(). The theme colors are
    // for a light ground, and this ground is dark.
    UiTextBig(NOTICE_IN_X + (NOTICE_IN_W - UiTextBigWidth(gSpeciesNames[species])) / 2,
              NOTICE_NAME_Y, gSpeciesNames[species],
              UI_COL_SHINY_PALE, UI_COL_SHADOW);

    // A real control. Only this rect dismisses the panel, so a stray touch does
    // not close it. The rest of the panel absorbs touches.
    UiRect(NOTICE_BTN_X, NOTICE_BTN_Y, NOTICE_BTN_W, NOTICE_BTN_H, UI_COL_SHINY);
    UiAscii(label, "DISMISS", sizeof(label));
    UiText(NOTICE_BTN_X + (NOTICE_BTN_W - UiTextWidth(label)) / 2,
           NOTICE_BTN_Y + (NOTICE_BTN_H - UI_GLYPH_H) / 2,
           label, UI_COL_SHINY_PALE, UI_COL_SHADOW);

    // Do not draw the sparkles here. They are on the animated layer, which
    // paints after the snapshot (see DrawAnimatedLayer). A sparkle in the
    // snapshot stays behind when a smaller frame replaces it.
}

// ------------------------------------------------------------- lifecycle ---
//
// The screen is blank during the intro. On the title screen, it shows only
// TOUCH TO START (ui_title.c). After the game starts, it stays up in battles
// and menus. A live overworld test would hide it in each battle, so latch the
// first arrival at the overworld.
static void UpdateInGameLatch(void)
{
    if (gMain.callback2 == CB2_Overworld)
        sInGame = TRUE;
}

// --------------------------------------------------------------- redraw ----
//
// Hash each input that the display uses, and repaint only when the hash
// changes. The inputs include the window border and the unlock flags, not only
// the party.
static u32 UiStateHash(void)
{
    u32 hash = 2166136261u;   // FNV-1a

    u32 top[10];
    top[0] = UiFrameId();
    top[1] = sInGame;
    // The override is host side and always safe to read. Fold the three flags
    // only when a save block exists. Before that, the hash shows "no tabs",
    // which is correct.
    top[2] = (u32)(Ctr3dsGetShowAllTabs() != 0) << 3;

    if (SaveDataLive())
        top[2] |= (u32)FlagGet(FLAG_SYS_POKEMON_GET)
               |  ((u32)FlagGet(FLAG_SYS_POKENAV_GET) << 1)
               |  ((u32)FlagGet(FLAG_SYS_POKEDEX_GET) << 2);
    // The matchup badges depend on the opponent. Without it here, the badges
    // are stale after the opponent switches.
    top[3] = UiMatchupOpponentKey();

    // Tab state, in its own slot. Two keys in one slot can cancel.
    //
    // Only one of these keys is live at a time. Each walks data that the other
    // tabs do not use, so each runs only while its tab is up. All of them read
    // save data.
    top[4] = 0;
    if (SaveDataLive())
    {
        if (sTab == UI_TAB_DEX)
            top[4] = UiDexStateKey();
        else if (sTab == UI_TAB_MAP)
            top[4] = UiMapStateKey();
        else if (sTab == UI_TAB_EXTRA)
            top[4] = UiExtraStateKey();
        // The cheat tags show the live level cap, which changes when the player
        // gets a badge. The IV/EV panel shows EVs, which change after a battle.
        // Neither is in another slot.
        else if (sTab == UI_TAB_PARTY)
            top[4] = UiPartyStateKey();
        // The list counters. A catch or a battle changes them without a touch.
        else if (sTab == UI_TAB_TROPHY)
            top[4] = UiTrophyStateKey();
    }

    // The shiny notice. It opens with a catchable shiny and closes on DISMISS
    // or at the end of the battle. It reads only globals, so it is safe before
    // a save exists.
    top[5] = NoticeActive(NULL, NULL);

    // The quick-throw strip, in its own slot. Nothing else in the hash changes
    // when action selection opens or closes. Without this key, the strip does
    // not appear. Zero while it is down.
    top[6] = UiQuickBallStateKey();

    // TOUCH TO START on the title screen, in its own slot. Nothing else changes
    // when the prompt blinks. Zero after the game starts. It reads only gMain,
    // the tasks and the sprites, so it is safe before a save exists.
    top[7] = sInGame ? 0 : UiTitleStateKey();

    // The unlocked count, and whether any achievement is unseen. On every tab,
    // because the tab bar dot uses it. It reads only the provider's bits, so it
    // is safe before a save exists.
    top[8] = AchActive()->stateKey();

    // The achievement toast, in its own slot. Nothing else changes when it
    // opens or closes. Zero while it is down.
    top[9] = UiAchToastStateKey();

    for (u32 i = 0; i < ARRAY_COUNT(top); i++)
    {
        hash ^= top[i];
        hash *= 16777619u;
    }

    // The party, only where it shows: the PARTY tab and the BAG target picker.
    // On other tabs, a hit in battle must not cause a repaint.
    //
    // MAP's fly row also uses the party, and UiMapStateKey folds that itself. A
    // switch to PARTY repaints anyway, and UiPartyTick takes the real HP on
    // that frame.
    if (sTab == UI_TAB_PARTY || (sTab == UI_TAB_BAG && UiBagPickerOpen()))
    {
        // Read through UiPartyMon, as the views do. The game's party menu
        // reorders gPlayerParty while it is up. That is not a change on this
        // screen.
        for (u32 i = 0; i < PARTY_SIZE; i++)
        {
            struct Pokemon *mon = UiPartyMon((u8)i);
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

        // Confusion. It is not in MON_DATA_STATUS (see status_tags.h), so fold
        // it here. Fold the badge phase only for the picker, which has no
        // animated layer. On the grid, the phase would make each flip a full
        // repaint.
        hash ^= UiStatusTagsKey(sTab == UI_TAB_BAG);
        hash *= 16777619u;

        // The slots that belong to a battle partner (ui_team.h). Their Pokemon
        // are in the party before the battle starts, so nothing above changes
        // when the tint must appear or go.
        hash ^= UiTeamKey();
        hash *= 16777619u;
    }

    return hash;
}

static void DrawTabBar(const u8 *vis, u32 n)
{
    const int tabW = CTR_BOTTOM_WIDTH / (int)n;

    for (u32 i = 0; i < n; i++)
    {
        int x = (int)i * tabW;
        // The last tab takes the remainder, so the bar fills the width.
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

        // A gold dot on the TROPHY tab when an unlock is unseen. It is what
        // stays after a toast that nobody read, so it uses the toast's gold.
        // Never on the active tab, which marks all rows as seen.
        if (vis[i] == UI_TAB_TROPHY && !active && AchActive()->anyUnseen())
        {
            UiFillRect(x + w - 12, UI_CONTENT_H + 6, 7, 7, UI_COL_SHINY_EDGE);
            UiFillRect(x + w - 11, UI_CONTENT_H + 7, 5, 5, UI_COL_SHINY);
        }
    }
}

// Defined below, next to the animation-step path.
static void DrawAnimatedLayer(void);

static void Redraw(void)
{
    u8 vis[UI_TAB_COUNT];
    u32 n;
    u16 noticeSpecies = SPECIES_NONE;
    u32 noticePersonality = 0;
    // A repaint fills 76,800 pixels and draws the active tab. Each touch
    // handler asks for one through UiMarkDirty(), so check it first when a tap
    // causes a pause. CtrLogSlow reports nothing on a normal frame. See
    // 3ds/bridge.h.
    unsigned int t0 = CtrTimeNowMs();
    // The same interval in ticks, for CtrProfile. The millisecond clock is too
    // coarse for the paint. See 3ds/bridge.h.
    unsigned long long tp = CtrTicksNow();

    // Before the game starts, show only the title's TOUCH TO START, in the lit
    // half of its blink. While it is up, also show the build id in the corner.
    if (!sInGame)
    {
        UiClear(0);
        UiTitleDraw();
        sNeedsRepaint = 0;
        sDirty = 1;
        CtrLogSlow("redraw", t0);
        CtrProfile("paint.blank", tp);
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
    case UI_TAB_TROPHY: UiTrophyDraw(); break;
    case UI_TAB_EXTRA: UiExtraDraw(); break;
    }

    // The overlays go over the tab. The strip and the toast come first and the
    // notice last. If the geometry changes and they overlap, the alert stays on
    // top. At present they touch at y 40 and y 152 and do not overlap.
    if (UiQuickBallActive())
        UiQuickBallDraw();

    if (UiAchToastActive())
        UiAchToastDraw();

    if (NoticeActive(&noticeSpecies, &noticePersonality))
        DrawNotice(noticeSpecies, noticePersonality);

    DrawTabBar(vis, n);

    sNeedsRepaint = 0;
    sDirty = 1;          // tell the host to upload again

    // Keep the finished screen, so that an animation step can restore a rect
    // and not repaint everything. The snapshot contains the tab, the overlay
    // and the bar.
    UiSnapshot();

    // The moving parts come after the snapshot, so a step never restores them.
    DrawAnimatedLayer();

    CtrLogSlow("redraw", t0);
    CtrProfile("paint", tp);
}

// Each moving part draws over the snapshot, not into it. Each one restores its
// rects first.
//
// While an overlay is up, it owns this layer, because the snapshot already
// contains the panel. Thus this is an "else". The tab draws its moving pieces
// into its own paint while UiOverlayActive() is TRUE.
static void DrawAnimatedLayer(void)
{
    if (NoticeActive(NULL, NULL))
        RedrawNoticeSparkles();
    // Not while the quick-throw strip is up. The snapshot contains the strip,
    // and the bottom row of party icons is under it. A redraw here puts the
    // icons on top of the strip. The achievement toast has the same problem
    // with the top row.
    else if (!UiQuickBallActive() && !UiAchToastActive() && sTab == UI_TAB_PARTY)
        UiPartyRedrawAnimated();
}

// An animation step: restore a few rects from the snapshot and draw the moving
// pieces again. This is much cheaper than a full repaint, which matters most on
// the single-core path.
static void RedrawAnimated(void)
{
    unsigned long long tp = CtrTicksNow();

    DrawAnimatedLayer();
    sDirty = 1;

    CtrProfile("paint.anim", tp);
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
    int animParty = 0, animNotice = 0;
    // The full touch response. If `update` is slow and `redraw` is fast, the
    // cost is in a touch handler or in UiStateHash.
    unsigned int t0 = CtrTimeNowMs();

    UpdateInGameLatch();

    // Achievements first, so the list, the toast and the hash see this frame's
    // unlocks. Each displayed frame, not gated on sInGame. It checks for a save
    // itself, and adopts a playthrough only on a CB2_Overworld frame (see Adopt
    // in achievements.c).
    AchTick();

    // Advance the step clock before a tick reads it. See UI_ANIM_STEP_FRAMES.
    sAnimStepped = (++sAnimSub >= UI_ANIM_STEP_FRAMES);
    if (sAnimStepped)
        sAnimSub = 0;

    // The confused party mons, and which tag a two-tag mon shows. After the
    // step clock, because a tag flips only on a step. Before any badge draw or
    // hash. On every tab, because it must see the first action selection of the
    // battle.
    UiStatusTagsTick();

    // Before the game starts, nothing is interactive. The one exception: a tap
    // on the title screen is START (ui_title.c).
    if (!sInGame)
    {
        UiTitleTouch(touch);
        touch = NULL;
    }

    // The panel is modal. It takes every touch in its rect before the tabs, a
    // press without a release too. Only DISMISS acts.
    //
    // The tab bar stays live. The panel is in the content area, so the player
    // can check the party before a throw, and the panel follows them.
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
    // The achievement toast, on the same terms: the strip absorbs all touches,
    // and only VIEW acts. It opens the TROPHY tab.
    else if (touch != NULL
             && UiHit(touch, UI_AT_X, UI_AT_Y, UI_AT_W, UI_AT_H)
             && UiAchToastActive())
    {
        if (UiAchToastTouch(touch))
        {
            sTab = UI_TAB_TROPHY;
            sNeedsRepaint = 1;
        }
    }
    // The quick-throw strip, on the same terms. It takes every touch in its
    // rect, so a drag that starts on it does not reach the tab.
    else if (touch != NULL
             && UiHit(touch, UI_QB_X, UI_QB_Y, UI_QB_W, UI_QB_H)
             && UiQuickBallActive())
    {
        UiQuickBallTouch(touch);
    }
    // A tap on the tab bar switches the view. Touches above it go to the active
    // view. Act on release, so a touch that slides off a tab does not trigger
    // it.
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
        case UI_TAB_TROPHY: UiTrophyTouch(touch); break;
        case UI_TAB_EXTRA: UiExtraTouch(touch); break;
        }
    }

    // The HP bars and icons of the party grid, only while the grid shows. The
    // shell gives the visibility, so other tabs pay nothing.
    //
    // A step that moves only its own rects uses the cheap path. Other changes
    // need a full repaint. If you are not sure, ask for the full repaint.
    if (sInGame && UiPartyTick(sTab == UI_TAB_PARTY))
    {
        if (!UiPartyAnimOnly())
        {
            sNeedsRepaint = 1;
        }
        // While an overlay is up, DrawCell paints the party's moving parts into
        // the tab. On the second-core path, each step is then a full repaint,
        // and the grid keeps moving under the panel.
        else if (UiOverlayActive() && Ctr3dsRasteriserOnOwnCore())
        {
            sNeedsRepaint = 1;
        }
        // On the single-core path, the party stays still under an overlay. Thus
        // there is no cheap redraw while the strip or the toast is up: it has
        // nothing to draw. The tick still runs, so the icons continue from the
        // same phase when the overlay closes.
        else if (!UiQuickBallActive() && !UiAchToastActive())
        {
            animParty = 1;
        }
    }

    // Each frame, not gated on sInGame or on the strip. It clears state after
    // the battle, when the strip is already down. It asks for no repaint.
    UiQuickBallTick();

    // The sparkles of the shiny panel. The ticks run here, not in Redraw,
    // because a tick must run even when there is no repaint.
    if (sInGame && NoticeTick())
    {
        // The panel opens, or a glint frame: repaint the full panel.
        if (sNoticeFull)
            sNeedsRepaint = 1;
        else
            animNotice = 1;
    }

    // The toast countdown and the next toast. Only after the game starts,
    // because nothing shows before that.
    if (sInGame)
        UiAchToastTick();

    // The NEW tags of the achievements list, taken when the tab opens and
    // dropped when it closes. Before the hash, so the first paint knows which
    // rows are new.
    UiTrophyTick(sInGame && sTab == UI_TAB_TROPHY);

    // This state changes without a touch (damage, evolution, a level-up, a new
    // border, a new Pokedex), so poll it.
    hash = UiStateHash();
    if (hash != sLastStateHash)
    {
        sLastStateHash = hash;
        sNeedsRepaint = 1;
    }

    // A full repaint includes everything that the cheap path draws, and
    // refreshes the snapshot.
    if (sNeedsRepaint)
        Redraw();
    else if (animParty || animNotice)
    {
        // No snapshot yet (the first paint, or just after the title screen):
        // make one.
        if (UiHasSnapshot())
            RedrawAnimated();
        else
            Redraw();
    }

    CtrLogSlow("bottom.update", t0);
}

int CtrBottomIsDirty(void)            { return sDirty; }
void CtrBottomClearDirty(void)        { sDirty = 0; }
const u16 *CtrBottomFramebuffer(void) { return UiFb(); }
