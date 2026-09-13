// Bottom-screen shell (game side).
//
// This translation unit is compiled with the game's headers, NOT libctru's --
// see 3ds/bridge.h for why. That is the whole point of the split: Emerald's
// party data, item tables, fonts, mon icons and palettes are ordinary symbols
// here, so the UI reads real game state directly instead of scraping RAM.
//
// This file owns only the frame: which tabs exist, which view is active, and
// when the screen needs repainting. Each tab draws its own content area
// (tab_party.c, tab_bag.c, tab_map.c, tab_dex.c, tab_trophy.c, tab_extra.c)
// through the primitives in ui_draw.h and ui_text.h.
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
#include "../achievements.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "matchup.h"
#include "status_tags.h"
#include "ui_quickball.h"
#include "ui_achtoast.h"
#include "ui_title.h"

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
u8   UiActiveTab(void)          { return sTab; }
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
    // Always available. A locked list is a list of goals, and there is no
    // start-menu entry for it to mirror.
    [UI_TAB_TROPHY] = { "TROPHY", 0                  },
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

// -------------------------------------------------------- animation clock --
//
// The step clock the party grid's icons advance on, and on the single-core path
// the one clock for every animation on this screen. Which path the port is on
// (Ctr3dsRasteriserOnOwnCore) picks the period, because the two have different
// budgets.
//
// ON ONE CORE (no second core could be had, or a CTR_PPU_THREAD=0 build), a
// repaint costs the game an entire extra VBlank. Measured, not assumed: at 60
// repaints a second the game ran at 30fps and at 10 it ran at 53, which is
// fps = 3600 / (60 + repaints per second) to within the reading error. So the
// frame rate is set by HOW OFTEN this screen repaints and barely at all by what
// it draws, and the reason there is only one clock there is arithmetic rather
// than tidiness.
//
// Every animation asks for its repaint through the same sNeedsRepaint flag, so
// two of them coalesce into one repaint only when they land on the SAME frames.
// On separate periods a shiny panel over an animating party grid would ask
// twice as often and cost twice as much. Sharing one step clock makes the frame
// rate a property of this screen rather than of how many things happen to be
// moving on it.
//
// 12 frames is 5 steps a second. On that path it also has to stay clear of the
// host's upload: 3ds/host/video.c hands a repaint to the GPU 48 rows at a time,
// five frames per repaint, so that the top screen keeps its 60fps. A step
// period shorter than five frames would ask for the next picture before the
// last one had finished arriving, and the bottom screen would be uploading on
// every frame -- which is the cost this is all avoiding. 12 leaves seven idle
// frames between runs.
//
// WITH A SECOND CORE the rasteriser runs there while this paints, and the host
// uploads the whole screen inside that same overlap, before it collects the
// render. A step then costs the frame nothing and reaches the panel on the
// frame it was painted, so there is nothing for a slow clock to save. It runs
// at 6, which is Emerald's own party menu pace (sAnim_0, src/pokemon_icon.c),
// and the shiny notice leaves it for a frame count of its own (NoticeTick).
#define UI_ANIM_STEP_FRAMES (Ctr3dsRasteriserOnOwnCore() ? 6 : 12)

static u8    sAnimSub;
static bool8 sAnimStepped;

// TRUE on the frames the animations are allowed to advance on. Ticks call this
// instead of counting frames themselves, which is what keeps them in step.
bool8 UiAnimStepped(void)
{
    return sAnimStepped;
}

// TRUE while a modal overlay is covering the tab.
//
// A tab asks this to know that the animated layer will not be run for it this
// frame, because the overlay has that layer to itself -- anything drawn there
// while the panel is up would land ON TOP of the panel, the overlay being part
// of the snapshot the layer paints over. A tab with a piece deferred to that
// layer has to draw it into its own paint instead, or the piece is simply
// missing for as long as the panel is up. See DrawCell in tab_party.c, which is
// the case that found this. On the second-core path the shell then asks for a
// full repaint on every frame that piece moves (CtrBottomUpdate), so it keeps
// moving under the panel; on the single-core path it is drawn still.
//
// The achievement toast counts too: the party grid's top row of icons sits
// inside its y 0..40, and the cheat tag strip with them.
bool8 UiOverlayActive(void)
{
    return NoticeActive(NULL, NULL) || UiQuickBallActive() || UiAchToastActive();
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
//
// Two tunings, and Ctr3dsRasteriserOnOwnCore() picks between them:
//
//   With a second core, the animation as it was first written. The counter
//   counts frames, the corners twinkle on a 64-frame cycle, and a slanted gold
//   glint crosses the panel as it opens. A repaint costs the frame nothing on
//   that path, so there is nothing to be saved by waiting for a step.
//
//   On one core, what that path can afford. The counter counts steps of the
//   shared clock (UI_ANIM_STEP_FRAMES), the twinkle is 8 steps long, and a
//   two-step burst stands in for the glint.

// The twinkle's length on each path, in whatever the counter counts: 64 frames
// is about 1.1 seconds, and 8 steps of 12 frames is 96 frames, about 1.6.
//
// Powers of two on purpose. sNoticeTime is a u16, and wraps after eighteen
// minutes of an undismissed panel counting frames, or three and a half hours
// counting steps. 65536 divides by both, so the wrap lands on a cycle boundary
// instead of jumping the twinkle mid-cycle. On the frame path it also plays
// the glint again, which is harmless.
#define NOTICE_FRAME_CYCLE  64
#define NOTICE_STEP_CYCLE   8
#define NOTICE_CORNERS      4

// The opening glint on the second-core path: NOTICE_SWEEP_END frames to cross
// the panel, as a SWEEP_W band slanted SWEEP_SLANT pixels over the interior's
// height. See DrawSweep.
#define NOTICE_SWEEP_END    24
#define SWEEP_W             4
#define SWEEP_SLANT         24

// The opening flourish on the single-core path, in steps: every corner at the
// largest frame at once, before the staggered twinkle takes over.
#define NOTICE_BURST        2

// How far in from the interior edge a corner sparkle is CENTRED. The big frame
// reaches 8px left and right of its axis, so 12 clears the 2px gold rule.
#define NOTICE_SPK_IN_X   12
#define NOTICE_SPK_IN_T   10
#define NOTICE_SPK_IN_B   12

static u16   sNoticeTime;     // frames (own core) or steps (one core) it has been up
static u32   sNoticeAnimId;   // the encounter that count belongs to
static bool8 sNoticeAnimSet;
static bool8 sNoticeFull;     // this tick needs the panel painted, not just its sparkles

// Which frame of the art a corner shows at `phase`, or -1 for nothing. One table
// per path.
//
// Thresholds rather than arithmetic because the three frames are 3, 6 and 16px
// wide: an even hold on each reads as a jump into the big one, so the holds are
// tuned against that.
//
// The frame table holds the sizes for 4, 4, 6, 4 and 4 frames, then goes dark
// for the other 42. Dark for most of the cycle, which is what makes this a
// twinkle rather than a pulse.
static int TwinkleSizeFrames(u16 phase)
{
    if (phase < 4)  return 0;
    if (phase < 8)  return 1;
    if (phase < 14) return 2;
    if (phase < 18) return 1;
    if (phase < 22) return 0;

    return -1;
}

// The step table has one step per size, so each corner is lit for five steps
// of the eight and dark for three. The stagger round the panel is what keeps
// that reading as a twinkle at this rate.
static int TwinkleSizeSteps(u16 phase)
{
    if (phase < 1) return 0;
    if (phase < 2) return 1;
    if (phase < 3) return 2;
    if (phase < 4) return 1;
    if (phase < 5) return 0;

    return -1;
}

// What corner `i` shows at count `t`, on whichever path this is, or -1 for
// nothing. Each corner is a quarter cycle behind the last.
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

    // A different encounter is a different panel. Restarting on the same
    // identity the dismiss logic keys on means the second shiny of a session
    // gets its own opening glint or burst instead of inheriting the first one's
    // phase.
    if (!sNoticeAnimSet || id != sNoticeAnimId)
    {
        sNoticeAnimId = id;
        sNoticeAnimSet = TRUE;
        sNoticeTime = 0;
        sNoticeFull = TRUE;     // the panel itself has to be painted, not just
        return TRUE;            // its sparkles
    }

    sNoticeFull = FALSE;

    if (Ctr3dsRasteriserOnOwnCore())
    {
        sNoticeTime++;

        // The glint is drawn into the panel itself (DrawNotice), so every one
        // of its frames is a full repaint. So is the frame after its last,
        // which paints the panel without it: skip that one and the band's last
        // position stays in the snapshot, where every sparkle step after it
        // would put it back.
        if (sNoticeTime <= NOTICE_SWEEP_END)
        {
            sNoticeFull = TRUE;
            return TRUE;
        }

        // After that only on a frame where some corner actually changes size,
        // which is 24 of the 64 in a cycle. The other 40 would restore and
        // redraw exactly what is already there, and upload an unchanged screen.
        for (u32 i = 0; i < NOTICE_CORNERS; i++)
            if (CornerSize(i, sNoticeTime) != CornerSize(i, (u16)(sNoticeTime - 1)))
                return TRUE;

        return FALSE;
    }

    // On one core, only on a step frame. Returning TRUE on the other eleven
    // would repaint the screen to draw exactly what is already on it, which is
    // the whole of what took the game to 30fps.
    if (!UiAnimStepped())
        return FALSE;

    sNoticeTime++;

    return TRUE;
}

// The glint that crosses the panel as it opens, once, on the second-core path.
// Drawn on the bare ground before the rule and the text, so it passes BEHIND
// the headline the way light crosses glass rather than washing over the words.
//
// There is no alpha anywhere in this drawing layer -- UiFillRect writes solid
// colour -- so a soft glow is not on offer. A narrow hard band moving quickly
// is, and carrying the orange edge either side of the gold core is the same
// trick the headline uses to keep a flat fill from reading as a flat bar.
//
// Not on the single-core path. There it could only move on the step clock, and
// a 4px band crossing 224px needs roughly a position every 4px to read as
// movement, which at five steps a second would take eleven seconds. Five
// positions is not a sweep, it is four gold bars flashing in sequence, so that
// path opens with the burst instead (CornerSize): a state CHANGE rather than
// motion, which reads the same at any rate that clock can be set to.
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
// The four corner rects, big enough for the largest frame of the art: it is
// 16x14 around its axis, which sits 8 left and 5 above the centre.
#define NOTICE_SPK_W  16
#define NOTICE_SPK_H  14
#define NOTICE_SPK_AX 8
#define NOTICE_SPK_AY 5

// Ordered around the panel rather than in reading order, so the twinkle travels
// round it. Shared by the full paint and the sparkles-only step below.
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

// The same four sparkles, over the snapshot rather than over a fresh panel.
// This is the whole of an animation step's drawing when the notice is up: four
// 16x14 restores and four glyphs, against a rebuild of the whole screen for
// them, which was 4.9 ms on the single-core path.
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
    // behind the headline rather than over it. Second-core path only, and once
    // per encounter: NoticeTick restarts the count for each new shiny, and asks
    // for a full repaint on every glint frame plus one after, which is what
    // takes the band back out of the snapshot the sparkles restore from.
    if (Ctr3dsRasteriserOnOwnCore() && sNoticeTime < NOTICE_SWEEP_END)
        DrawSweep(sNoticeTime);

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

    // The sparkles are NOT drawn here. They are the animated layer, painted
    // after the shell snapshots this panel, so the snapshot is the still panel
    // beneath them -- see DrawAnimatedLayer. Baking a sparkle into the
    // background would leave it behind when a smaller frame is drawn over it.
}

// ------------------------------------------------------------- lifecycle ---
//
// The screen must stay blank through the intro, show nothing but TOUCH TO START
// on the title screen (ui_title.c), and stay up during battles and menus once
// the game proper is running. A live "are we in the overworld" test would blink
// it out on every battle, so latch on having reached the overworld once instead.
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

    u32 top[10];
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
        // The list's counters, which a catch or a battle moves with no touch.
        else if (sTab == UI_TAB_TROPHY)
            top[4] = UiTrophyStateKey();
    }

    // The shiny notice, which nothing else here covers: it appears when a
    // catchable shiny does, disappears when the player dismisses it, and
    // disappears again when the battle ends. Safe before there is a save block,
    // because everything behind it is a plain global gated on gMain.inBattle.
    top[5] = NoticeActive(NULL, NULL);

    // The quick-throw strip, in a slot of its own for the same reason the
    // tab-conditional key above has one: two keys sharing a slot can cancel.
    // Nothing else in this hash moves when action selection opens or closes,
    // which is exactly when the strip appears and disappears, so without this
    // it would never be drawn at all. Zero while it is down.
    top[6] = UiQuickBallStateKey();

    // The title screen's TOUCH TO START, which blinks on the PRESS START
    // banner's clock. A slot of its own for the same reason as the two above,
    // and just as necessary: nothing else here moves when the prompt blinks, so
    // without it the prompt would never appear. Zero once the game runs. Safe
    // before there is a save block, because it reads only gMain, the tasks and
    // the sprites.
    top[7] = sInGame ? 0 : UiTitleStateKey();

    // Achievements: how many are unlocked and whether any is unseen. On every
    // tab, not just TROPHY, because the tab bar's dot depends on it and an
    // unlock can land on any of them. Reads only the provider's own bits, so it
    // is safe before there is a save block.
    top[8] = AchActive()->stateKey();

    // The achievement toast, in a slot of its own for the reason the strip and
    // the notice have theirs: nothing else here moves when it comes up or goes,
    // so without this it would never be drawn. Zero while it is down.
    top[9] = UiAchToastStateKey();

    for (u32 i = 0; i < ARRAY_COUNT(top); i++)
    {
        hash ^= top[i];
        hash *= 16777619u;
    }

    // The party, but only where it is drawn: the PARTY tab, and BAG's target
    // picker. This used to be folded in on every tab, so in a battle each hit,
    // status change and level-up repainted BAG, MAP, DEX and EXTRA as well: a
    // full repaint apiece for a screen that shows none of it. It is also 30
    // GetMonData calls a frame, six of them decrypting, that the other tabs no
    // longer pay.
    //
    // Nothing else goes stale for it. MAP's fly row does depend on the party,
    // and UiMapStateKey folds exactly that itself. Switching to PARTY or
    // opening the picker repaints on its own, and UiPartyTick adopts the real
    // HP on the frame the tab comes back.
    if (sTab == UI_TAB_PARTY || (sTab == UI_TAB_BAG && UiBagPickerOpen()))
    {
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

        // Confusion, which is not in MON_DATA_STATUS or anywhere else on the
        // mon (status_tags.h), so gaining or losing CNF would otherwise leave
        // the badge stale. Which tag a two-tag mon is showing is folded only
        // for the picker: it has no animated layer, so a repaint is its only
        // way to flip. The grid flips on its animated layer, and folding the
        // phase there would turn every flip into a full repaint.
        hash ^= UiStatusTagsKey(sTab == UI_TAB_BAG);
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

        // Something unlocked that the player has not looked at yet: a gold dot
        // in the TROPHY tab's corner, until the tab is opened. It is what is
        // left of a toast that expired unread, so it is the toast's gold. Never
        // on the active tab, which marks everything seen as it shows.
        if (vis[i] == UI_TAB_TROPHY && !active && AchActive()->anyUnseen())
        {
            UiFillRect(x + w - 12, UI_CONTENT_H + 6, 7, 7, UI_COL_SHINY_EDGE);
            UiFillRect(x + w - 11, UI_CONTENT_H + 7, 5, 5, UI_COL_SHINY);
        }
    }
}

// Defined below, next to the animation-step path that shares it.
static void DrawAnimatedLayer(void);

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
    // And the same bracket in ticks, because the millisecond clock above cannot
    // resolve this at all: the paint is the OTHER half of the repaint cost, and
    // the whole question is how it compares with the upload the host does after
    // it. See CtrProfile in 3ds/bridge.h.
    unsigned long long tp = CtrTicksNow();

    // Before the game proper is running there is nothing meaningful to show,
    // and a menu floating under the title screen looks broken. The one thing
    // drawn here is the title's TOUCH TO START, and only in the lit half of its
    // blink.
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

    // The overlays, over the top of whichever tab just drew. The strip and the
    // achievement toast first and the notice last, so that if the geometry is
    // ever changed such that they do overlap, the ALERT is the one that
    // survives -- an alert a view can paint over is not one. As they stand the
    // three abut exactly (y 40 and y 152) and none touches another.
    if (UiQuickBallActive())
        UiQuickBallDraw();

    if (UiAchToastActive())
        UiAchToastDraw();

    if (NoticeActive(&noticeSpecies, &noticePersonality))
        DrawNotice(noticeSpecies, noticePersonality);

    DrawTabBar(vis, n);

    sNeedsRepaint = 0;
    sDirty = 1;          // tell the host to re-upload

    // Remember the finished screen, so the next animation step can put back
    // what it covers rather than rebuilding all of this. Composited -- tab,
    // overlay and bar -- which is what makes restoring a rect correct whatever
    // happened to be on top of it.
    UiSnapshot();

    // ...and only now the moving parts, so they are never inside the thing an
    // animation step restores from.
    DrawAnimatedLayer();

    CtrLogSlow("redraw", t0);
    CtrProfile("paint", tp);
}

// Everything on this screen that moves, drawn OVER the snapshot rather than
// into it. Each of these restores its own rects first, which is a no-op right
// after a full paint and is the whole trick on an animation step.
//
// The overlay has this layer to itself while it is up, and it has to: the
// snapshot this paints over already includes the panel, so a tab drawing here
// would draw on top of it.
//
// That is why it is an else rather than both. What it does NOT mean is that the
// tab's deferred pieces stop existing -- the panel is 240x112 in the middle of a
// 320x192 area, and only one of the party's six icons is fully behind it. A tab
// with a piece deferred to this layer draws it into its own paint while
// UiOverlayActive(), which is where the other five come from.
static void DrawAnimatedLayer(void)
{
    if (NoticeActive(NULL, NULL))
        RedrawNoticeSparkles();
    // Not while the quick-throw strip is up. It has no animation of its own, so
    // there is nothing to draw here for it -- but the snapshot this layer
    // paints over now CONTAINS the strip, and the party grid's bottom row of
    // icons sits under it, so redrawing them here would punch them straight
    // through the panel. UiOverlayActive() is TRUE for the strip precisely so
    // that DrawCell paints the icons into the tab instead. The achievement
    // toast is the same case at the other end: the top row sits under it.
    else if (!UiQuickBallActive() && !UiAchToastActive() && sTab == UI_TAB_PARTY)
        UiPartyRedrawAnimated();
}

// An animation step, and nothing else: a few rects put back from the snapshot
// and the moving pieces drawn again over them.
//
// This exists because of one measurement, taken on the single-core path, where
// it still holds: a full repaint was 4.9 ms and the frame had 5.7 ms of slack,
// so rebuilding the screen five times a second to step some icons spent nearly
// all of it and the game dropped to 55fps. The same step through here is a few
// thousand pixels. On the second-core path a full repaint is mostly hidden
// behind the rasteriser, but a step through here still leaves core 0 more of
// that overlap for the whole-screen upload that follows it.
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
    // The whole of the touch response, so the log can separate it from the
    // repaint it usually ends in: `update` slow with `redraw` fast means the
    // cost is in a touch handler or in UiStateHash, not in the painting.
    unsigned int t0 = CtrTimeNowMs();

    UpdateInGameLatch();

    // Achievements first, so everything below -- the list, the toast, the
    // hash -- sees this frame's unlocks. Per displayed frame and never gated on
    // sInGame: it gates itself on there being a save to read, and adopts a
    // playthrough only on a CB2_Overworld frame (see Adopt in achievements.c).
    AchTick();

    // The step clock, advanced before any tick reads it. See
    // UI_ANIM_STEP_FRAMES: on the single-core path this is what stops two
    // animations costing twice the frame rate of one.
    sAnimStepped = (++sAnimSub >= UI_ANIM_STEP_FRAMES);
    if (sAnimStepped)
        sAnimSub = 0;

    // Which party mons are confused, and which of a mon's two tags is showing.
    // After the step clock, because a tag only flips on a step, and before
    // anything that draws a badge or hashes one. Every tab, not just PARTY:
    // it has to see the battle's first action selection whichever tab is up,
    // and BAG's target picker shows the same badges.
    UiStatusTagsTick();

    // Nothing is interactive before the game starts, except that a tap anywhere
    // on the title screen counts as START (ui_title.c).
    if (!sInGame)
    {
        UiTitleTouch(touch);
        touch = NULL;
    }

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
    // The achievement toast, on the same terms: the whole strip absorbs, and
    // only VIEW acts, by opening the TROPHY tab. The notice and the toast abut
    // at y 40 rather than overlap, so this order only decides a tie that the
    // geometry does not allow.
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
    // The quick-throw strip, on the same terms and for the same reasons: it
    // takes every touch inside its rect, release or not, so a drag begun on it
    // cannot carry through into the tab it is covering. Below the notice in
    // this chain because the two can be up together and the notice is the
    // alert, though as drawn they do not overlap.
    else if (touch != NULL
             && UiHit(touch, UI_QB_X, UI_QB_Y, UI_QB_W, UI_QB_H)
             && UiQuickBallActive())
    {
        UiQuickBallTouch(touch);
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
        case UI_TAB_TROPHY: UiTrophyTouch(touch); break;
        case UI_TAB_EXTRA: UiExtraTouch(touch); break;
        }
    }

    // The party grid's HP bars and mon icons, and only while that grid is the
    // thing on screen -- the tab is passed in rather than assumed, so the four
    // other tabs pay nothing for either animation.
    //
    // An animation that only moved its own small rects asks for the cheap path;
    // anything else -- a sliding HP bar, the panel appearing -- still needs the
    // screen rebuilt. Getting that split wrong shows up as a stale screen, not
    // a crash, so when in doubt a tick should ask for the full repaint.
    if (sInGame && UiPartyTick(sTab == UI_TAB_PARTY))
    {
        if (!UiPartyAnimOnly())
        {
            sNeedsRepaint = 1;
        }
        // An overlay has the animated layer to itself, so while one is up the
        // party's moving parts (the icons and the sliding HP block) are
        // painted into the tab by DrawCell instead. On the second-core path
        // every step is therefore a full repaint, which costs the frame nothing
        // there, and the grid keeps moving under the panel. The cheap path
        // cannot do it: it does not redraw the party at all while an overlay is
        // up, so a bar that starts sliding under one stalls at whatever the
        // last full paint baked, which is what the single-core path below
        // still does.
        else if (UiOverlayActive() && Ctr3dsRasteriserOnOwnCore())
        {
            sNeedsRepaint = 1;
        }
        // The single-core path, where the party is frozen under an overlay to
        // save the repaints: no cheap redraw while the quick-throw strip is up.
        // DrawAnimatedLayer has nothing to draw for the party then -- the icons
        // are frozen into the tab's own paint, which is what UiOverlayActive()
        // asked it for -- so the cheap path would put back four rects that
        // already hold what they held and then make the host upload an
        // unchanged screen. The tick itself still runs, so the phase carries on
        // underneath and the icons pick up where they were when the strip goes
        // down. The achievement toast freezes the grid the same way, so it is
        // excluded for the same reason.
        else if (!UiQuickBallActive() && !UiAchToastActive())
        {
            animParty = 1;
        }
    }

    // Per-frame, and deliberately not gated on sInGame or on the strip being
    // up: its whole job is to clear state once the battle is over, which is a
    // moment at which the strip is by definition already down. It asks for no
    // repaint, because everything it clears is invisible by then.
    UiQuickBallTick();

    // The shiny panel's sparkles, on the same terms. Both ticks live here
    // rather than inside Redraw because a tick that only ran when the screen
    // happened to repaint would stall exactly when it is the thing that ought
    // to be causing the repaint.
    if (sInGame && NoticeTick())
    {
        // The panel appearing, or a frame of its glint: the panel itself, not
        // just its sparkles.
        if (sNoticeFull)
            sNeedsRepaint = 1;
        else
            animNotice = 1;
    }

    // The achievement toast's countdown, and the next one waiting. Only while
    // the game runs: before that nothing is drawn, and a toast that timed out
    // on a blank screen would have told nobody anything.
    if (sInGame)
        UiAchToastTick();

    // The achievements list's NEW tags, taken on the frame the tab comes on
    // screen and dropped on the frame it goes. Before the hash, so a tab switch
    // paints the list already knowing which rows are new.
    UiTrophyTick(sInGame && sTab == UI_TAB_TROPHY);

    // This state can change without any touch at all -- taking damage, an
    // evolution, a level-up, the player changing the border in Options, or
    // being handed the Pokedex -- so it is polled rather than pushed.
    hash = UiStateHash();
    if (hash != sLastStateHash)
    {
        sLastStateHash = hash;
        sNeedsRepaint = 1;
    }

    // Full repaint wins over the cheap one: it redraws everything the cheap
    // path would have, and refreshes the snapshot the cheap path restores from.
    if (sNeedsRepaint)
        Redraw();
    else if (animParty || animNotice)
    {
        // No snapshot yet means nothing to restore from -- the first paint of a
        // session, or straight after the title screen. Build one.
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
