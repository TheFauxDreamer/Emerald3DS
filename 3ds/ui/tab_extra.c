// EXTRA tab: port features that are not part of the original game.
//
// Page 1 is host side only: fast-forward, top-screen scale and button binds. It
// does not change how the game plays.
//
// Page 2 contains cheats: EXP All, a level cap, a species randomizer and a bag
// sort order. They are on their own page, so the player does not see them
// first. The file 3ds/tweaks.c holds the behavior. This file only draws the
// toggles.
//
// Page 3 is quality of life. Page 4 is the follower and its options. Page 5 is
// the debug menu, if the build has it.
//
// The pager uses the right end of the top line (y 8..25) on each page. Pages 1
// and 2 use the same horizontal span, 22..298, so they look like one panel.
// Each page has its own row grid.

#include "global.h"

#include "../bridge.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"

// Ctr3dsCurrentLevelCap(), for the live "cap NN" value on page 2.
#include "../tweaks.h"

#if CTR_DEBUG_MENU
// The debug page's achievement row: a test toast and a resync.
#include "../achievements.h"
#endif

// The speeds double at each step. A 3x scale is too close to 2x to need a
// button.
static const u8 sSpeeds[] = { 1, 2, 4, 8 };
#define SPEED_COUNT   ARRAY_COUNT(sSpeeds)

// An index for each button. The modes are an enum, not a sequence, so do not
// calculate a mode from the index.
static const u8 sScales[] = {
    CTR_TOP_SCALE_1X, CTR_TOP_SCALE_1_5X, CTR_TOP_SCALE_FILL
};
static const char *const sScaleNames[] = { "1x", "1.5x", "FILL" };
#define SCALE_COUNT   ARRAY_COUNT(sScales)

// The values that a button can bind to, in tap order. MOD is in the same cycle
// as the speeds. A button holds one value, so it cannot be a speed and the
// modifier at the same time.
//
// The cycle wraps back to "not bound", so one finger can reach every state.
static const u8 sTurboSteps[] = { CTR_BIND_OFF, 2, 4, 8, CTR_BIND_MOD };
static const char *const sTurboNames[CTR_TURBO_COUNT] = { "X", "Y", "ZL", "ZR" };

// The first three rows are 276px wide, centered in 320. That leaves 22px on
// each side, clear of the 8px window frame.
#define BTN_H         26
#define BTN_GAP       12

// A label is 17px above its buttons, so a row with a label is 43px tall. The
// interior is y 8..183, which is 176px.
#define LABEL_TO_BTN    17
#define LABELLED_ROW_H  (LABEL_TO_BTN + BTN_H)

// The top line, y 8..25. The pager uses its right end on each page. On its
// left there is only the DEBUG caption of the debug page. All other rows start
// below it, at y 30.
#define TOP_LINE_Y    8

// A check row: an on/off setting as a checkbox, its label and a dim hint on one
// line. The full row width is the touch target, so a tap on the label also
// toggles. The labels are at most 63px ("PHONE CALLS"), so they end before the
// hint column at x 110. The widest hint is 135px and ends at x 245.
#define CHK_ROW_H     26
#define CHK_BOX_X     16
#define CHK_LABEL_X   38
#define CHK_HINT_X    110
#define CHK_HIT_X     8
#define CHK_HIT_W     (CTR_BOTTOM_WIDTH - 2 * CHK_HIT_X)

// Page 2's grid from y 30: a check row, a labelled row of three buttons, a
// check row and a row with its label next to its buttons. It ends at y 175.
#define P2_EXP_Y        30
#define P2_CAP_LABEL_Y  64
#define P2_CAP_BTN_Y    (P2_CAP_LABEL_Y + LABEL_TO_BTN)
#define P2_RAND_Y       115
#define P2_SORT_Y       149
#define P2_SORT_LABEL_X 16

// Page 3's grid: check rows at a 30px pitch from y 30. Four rows end at y 146,
// and the interior ends at y 183. A fifth row fits.
#define P3_ROW_PITCH  30
#define P3_ROW_Y(i)   (30 + (i) * P3_ROW_PITCH)

// Page 4's grid: rows at page 2's 34px pitch from y 30. It has check rows and
// rows with a label next to two buttons. Four rows end at y 158.
#define P4_ROW_PITCH  34
#define P4_ROW_Y(i)   (30 + (i) * P4_ROW_PITCH)

// Page 1's grid: three rows. They start below the pager line, as on the other
// pages. The rows have a 55px pitch from y 30. Thus the last button is at
// 157..183.
#define P1_ROW_GAP    12
#define P1_ROW_Y(i)   (30 + (i) * (LABELLED_ROW_H + P1_ROW_GAP))
#define P1_BTN_Y(i)   (P1_ROW_Y(i) + LABEL_TO_BTN)

#define SPD_W         60
#define SPD_Y         P1_BTN_Y(0)
#define SPD_X(i)      (22 + (i) * (SPD_W + BTN_GAP))

#define SCL_W         84
#define SCL_Y         P1_BTN_Y(1)
#define SCL_X(i)      (22 + (i) * (SCL_W + BTN_GAP))

#define TRB_W         60
#define TRB_Y         P1_BTN_Y(2)
#define TRB_X(i)      (22 + (i) * (TRB_W + BTN_GAP))

// The MOD note shares the line of the BUTTON HOLD label. That label is 63px
// wide, so x 96 is clear of it. The note is 163px and ends inside the 311px
// interior.
#define TRB_NOTE_X    96

// What fast-forward does to the music. It shares the line of the GAME SPEED
// label, because it changes only that control. A row of its own would cost
// 43px.
//
// "GAME SPEED" ends near x 76, so 104 is clear. The width is 88 because the
// widest label, "MUSIC FAST", is 54px. The control is 17px tall at y 30. Thus
// it ends at y 47, next to the row-1 buttons.
#define FFA_X         104
#define FFA_W         88
#define FFA_Y         P1_ROW_Y(0)
#define FFA_H         PGR_H

// The pager, right-aligned to the interior edge at x 311, on the top line of
// each page. It is 17px tall at y 8 and ends at y 24. Every page starts its
// rows below it.
//
// It uses TOP_LINE_Y, not a page's first row. Thus the pager does not move on a
// page turn.
#define PGR_W         26
#define PGR_H         17
#define PGR_Y         TOP_LINE_Y
// Four pages of settings, and the debug page if the build has it. All other
// constants below come from this value.
//
// Page 3 exists because page 2 has no vertical space left.
#if CTR_DEBUG_MENU
#define PAGE_COUNT    5
#else
#define PAGE_COUNT    4
#endif

// Right-aligned to the interior edge. The pager grows to the left when there
// are more pages, so the last button stays in the same place. The first button
// is at x 196 with four pages and at x 166 with five.
#define PGR_X(i)      (CTR_BOTTOM_WIDTH - 8 - PGR_W \
                       - (PAGE_COUNT - 1 - (i)) * (PGR_W + 4))

// The LEVEL CAP buttons use the columns of the SCREEN SIZE row (SCL_X and
// SCL_W, not SCL_Y). Thus the two pages align horizontally. BAG SORT has its
// label next to its buttons, and three 75px buttons fit after the label.
#define P2_HINT_X     96
#define WIDE_W        SCL_W
#define WIDE_X(i)     SCL_X(i)
#define SORT_W        75
#define SORT_X(i)     (70 + (i) * (SORT_W + 8))

// The page that shows. This is UI state and does not persist. EXTRA always
// opens on page 1, so the player does not see the cheats first.
static u8 sPage;

// The selected button gets a double inset outline and accent text. Color alone
// is not clear on the light window frames.
static void DrawButtonH(int x, int y, int w, int h, const u8 *label, int active)
{
    UiRect(x, y, w, h, UI_COL_DIM);

    if (active)
    {
        UiRect(x + 2, y + 2, w - 4, h - 4, UI_COL_ACCENT);
        UiRect(x + 3, y + 3, w - 6, h - 6, UI_COL_ACCENT);
    }

    UiText(x + (w - UiTextWidth(label)) / 2, y + (h - UI_GLYPH_H) / 2,
           label, active ? UI_COL_ACCENT : UiThemeText(), UiThemeShadow());
}

static void DrawButton(int x, int y, int w, const u8 *label, int active)
{
    DrawButtonH(x, y, w, BTN_H, label, active);
}

// One on/off setting: the checkbox, the label and a dim hint, centered in a row
// of height h. The hint can be NULL. Checked means "on" in the words of the
// label.
static void DrawCheckRow(int y, int h, const char *text, const char *hint,
                         int checked)
{
    u8 label[40];

    UiCheckBox(CHK_BOX_X, y + (h - UI_CHECKBOX_SIZE) / 2, checked ? TRUE : FALSE);
    UiText(CHK_LABEL_X, y + (h - UI_GLYPH_H) / 2,
           UiAscii(label, text, sizeof(label)), UiThemeText(), UiThemeShadow());

    if (hint != NULL)
        UiText(CHK_HINT_X, y + (h - UI_GLYPH_H) / 2,
               UiAscii(label, hint, sizeof(label)), UI_COL_DIM, UiThemeShadow());
}

// TRUE if the touch is on a check row. The draw uses the same y and h.
static bool8 HitCheckRow(const CtrTouchState *t, int y, int h)
{
    return UiHit(t, CHK_HIT_X, y, CHK_HIT_W, h) ? TRUE : FALSE;
}

static void DrawPage1(void)
{
    u8 label[40];

    UiText(16, P1_ROW_Y(0), UiAscii(label, "GAME SPEED", sizeof(label)),
           UiThemeText(), UiThemeShadow());

    for (u32 i = 0; i < SPEED_COUNT; i++)
    {
        char text[4];

        // Each speed is one digit, so the label needs no more width.
        text[0] = (char)('0' + sSpeeds[i]);
        text[1] = 'x';
        text[2] = '\0';

        DrawButton(SPD_X((int)i), SPD_Y, SPD_W,
                   UiAscii(label, text, sizeof(label)),
                   sSpeeds[i] == Ctr3dsGetSpeed());
    }

    // Highlight FAST, not 1x. The 1x speed is the default, and the accent
    // outline means "changed". The turbo binds below use it the same way.
    {
        int fast = (Ctr3dsGetFfAudio() == CTR_FFAUDIO_FAST);

        DrawButtonH(FFA_X, FFA_Y, FFA_W, FFA_H,
                    UiAscii(label, fast ? "MUSIC FAST" : "MUSIC 1x",
                            sizeof(label)),
                    fast);
    }

    UiText(16, P1_ROW_Y(1), UiAscii(label, "SCREEN SIZE", sizeof(label)),
           UiThemeText(), UiThemeShadow());

    for (u32 i = 0; i < SCALE_COUNT; i++)
        DrawButton(SCL_X((int)i), SCL_Y, SCL_W,
                   UiAscii(label, sScaleNames[i], sizeof(label)),
                   sScales[i] == Ctr3dsGetTopScale());

    UiText(16, P1_ROW_Y(2), UiAscii(label, "BUTTON HOLD", sizeof(label)),
           UiThemeText(), UiThemeShadow());

    // Explain MOD: one of these buttons makes the Pokedex arrows jump. ZL and
    // ZR do not exist on an Old 3DS. Without the note, a bind there looks
    // broken.
    UiText(TRB_NOTE_X, P1_ROW_Y(2),
           UiAscii(label, "MOD jumps lists. ZL/ZR: New 3DS.", sizeof(label)),
           UI_COL_DIM, UiThemeShadow());

    for (u32 i = 0; i < CTR_TURBO_COUNT; i++)
    {
        int bind = Ctr3dsGetTurboBind((int)i);
        char text[10];
        int n = 0;

        // "X 4x", "X MOD", or "X -" when nothing is bound.
        text[n++] = sTurboNames[i][0];
        if (sTurboNames[i][1] != '\0')
            text[n++] = sTurboNames[i][1];
        text[n++] = ' ';

        if (bind == CTR_BIND_OFF)
        {
            text[n++] = '-';
        }
        else if (bind == CTR_BIND_MOD)
        {
            text[n++] = 'M';
            text[n++] = 'O';
            text[n++] = 'D';
        }
        else
        {
            text[n++] = (char)('0' + bind);
            text[n++] = 'x';
        }
        text[n] = '\0';

        DrawButton(TRB_X((int)i), TRB_Y, TRB_W,
                   UiAscii(label, text, sizeof(label)), bind != CTR_BIND_OFF);
    }

}

static void DrawPage2(void)
{
    u8 label[40];
    int cap = Ctr3dsCurrentLevelCap();
    int mode;

    DrawCheckRow(P2_EXP_Y, CHK_ROW_H, "EXP ALL", NULL, Ctr3dsGetExpAll());

    UiText(16, P2_CAP_LABEL_Y, UiAscii(label, "LEVEL CAP", sizeof(label)),
           UiThemeText(), UiThemeShadow());

    // The cap that applies now, so the row shows what it does. It follows the
    // badges, so EXTRA needs a state key in bottom_screen.c.
    mode = Ctr3dsGetLevelCap();
    if (mode != CTR_CAP_OFF)
    {
        int x = P2_HINT_X;

        x += UiText(x, P2_CAP_LABEL_Y, UiAscii(label, "cap ", sizeof(label)),
                    UI_COL_DIM, UiThemeShadow());
        UiNum(x, P2_CAP_LABEL_Y, cap, UI_COL_DIM, UiThemeShadow());
    }

    DrawButton(WIDE_X(0), P2_CAP_BTN_Y, WIDE_W, UiAscii(label, "OFF", sizeof(label)),
               mode == CTR_CAP_OFF);
    DrawButton(WIDE_X(1), P2_CAP_BTN_Y, WIDE_W, UiAscii(label, "SOFT", sizeof(label)),
               mode == CTR_CAP_SOFT);
    DrawButton(WIDE_X(2), P2_CAP_BTN_Y, WIDE_W, UiAscii(label, "HARD", sizeof(label)),
               mode == CTR_CAP_HARD);

    // Nothing that is already caught or on the screen changes. The mapping uses
    // this save's trainer ID, so it is the same at each launch. A toggle off
    // and on does not change it.
    DrawCheckRow(P2_RAND_Y, CHK_ROW_H, "RANDOMISER", "new encounters only",
                 Ctr3dsGetRandomizer());

    UiText(P2_SORT_LABEL_X, P2_SORT_Y + (BTN_H - UI_GLYPH_H) / 2,
           UiAscii(label, "BAG SORT", sizeof(label)),
           UiThemeText(), UiThemeShadow());

    mode = Ctr3dsGetBagSort();

    DrawButton(SORT_X(0), P2_SORT_Y, SORT_W, UiAscii(label, "OFF", sizeof(label)),
               mode == CTR_BAGSORT_OFF);
    DrawButton(SORT_X(1), P2_SORT_Y, SORT_W, UiAscii(label, "TYPE", sizeof(label)),
               mode == CTR_BAGSORT_TYPE);
    DrawButton(SORT_X(2), P2_SORT_Y, SORT_W, UiAscii(label, "NAME", sizeof(label)),
               mode == CTR_BAGSORT_NAME);
}

// ---- PAGE 3: quality of life -----------------------------------------------
//
// One check row for each setting, on page 3's grid. The stored values for
// PHONE CALLS, QUICK BALL and BATTLE ANIM are "off" flags, so the checkbox
// shows the inverse: checked means the calls, the strip or the animation occur.
enum {
    P3_PHONE_CALLS,
    P3_QUICK_BALL,
    P3_BATTLE_ANIM,
    P3_DAY_CARE,
};

static void DrawPage3(void)
{
    // This stops only the trainers who call without a reason during a route.
    // All scripted calls still occur (StartMatchCallFromScript), for example
    // Norman, Wally, Scott and the Rayquaza call. The PokeNav's Match Call
    // screen still works.
    //
    // A stopped call also does not offer a rematch. Rematches still occur on
    // map load, and the PokeNav still shows who is ready.
    DrawCheckRow(P3_ROW_Y(P3_PHONE_CALLS), CHK_ROW_H, "PHONE CALLS",
                 "story calls still ring", !Ctr3dsGetPhoneCallsOff());

    // The quick-throw strip (3ds/ui/ui_quickball.c). It covers a tab while the
    // player uses it: the bottom 40 rows of the content area, during action
    // selection in each wild battle. Thus it gets a switch. It is stored for
    // each console, not for each save.
    DrawCheckRow(P3_ROW_Y(P3_QUICK_BALL), CHK_ROW_H, "QUICK BALL",
                 "last ball, one tap", !Ctr3dsGetQuickBallOff());

    // The bottom-screen animation in battle (sliding HP bars and cycling
    // icons). The values stay correct either way. The hint says what off gives,
    // which depends on the path. On one core, off gives frames to the top
    // screen, because each repaint costs a VBlank. With a second core, off only
    // gives a still screen.
    DrawCheckRow(P3_ROW_Y(P3_BATTLE_ANIM), CHK_ROW_H, "BATTLE ANIM",
                 Ctr3dsRasteriserOnOwnCore() ? "off = still in battle"
                                             : "off = smoother battles",
                 !Ctr3dsGetBattleAnimOff());

    // The Day Care Pokemon walk in the Route 117 yard. It is separate from
    // FOLLOWER, and it is for the console. A change shows at the next load of
    // Route 117, because the yard objects are made at map load.
    DrawCheckRow(P3_ROW_Y(P3_DAY_CARE), CHK_ROW_H, "DAY CARE",
                 "Pokemon in the yard", Ctr3dsGetDayCareYard());
}

// ---- PAGE 4: the follower --------------------------------------------------
//
// The FOLLOWER switch and its options, on page 4's grid. The aarant fork sets
// the options at build time (OW_MON_*). Each save keeps its own values.
enum {
    P4_FOLLOWER,
    P4_WHO,
    P4_BOBBING,
    P4_BALL,
};

// A label and two buttons. The buttons use the right two columns of LEVEL CAP
// on page 2, so the pages align.
static void DrawChoiceRow(int y, const char *text, const char *left,
                          const char *right, int rightOn)
{
    u8 label[40];

    UiText(16, y + (BTN_H - UI_GLYPH_H) / 2,
           UiAscii(label, text, sizeof(label)), UiThemeText(), UiThemeShadow());
    DrawButton(WIDE_X(1), y, WIDE_W, UiAscii(label, left, sizeof(label)),
               !rightOn);
    DrawButton(WIDE_X(2), y, WIDE_W, UiAscii(label, right, sizeof(label)),
               rightOn);
}

static void DrawPage4(void)
{
    int starter = (Ctr3dsGetFollowerWho() == CTR_FOLLOWER_STARTER);

    // The hint follows WHO, so the row says which Pokemon walks behind.
    DrawCheckRow(P4_ROW_Y(P4_FOLLOWER), CHK_ROW_H, "FOLLOWER",
                 starter ? "your starter walks behind"
                         : "lead Pokemon walks behind",
                 Ctr3dsGetFollowerOn());

    // STARTER: only the Pokemon from Birch's bag follows, as Pikachu in Yellow.
    DrawChoiceRow(P4_ROW_Y(P4_WHO), "WHO", "LEAD", "STARTER", starter);

    // The stored value is an "off" flag, so the checkbox shows the inverse.
    DrawCheckRow(P4_ROW_Y(P4_BOBBING), CHK_ROW_H, "BOBBING",
                 "up and down as it walks", !Ctr3dsGetFollowerBobOff());

    // OWN is the ball that caught the Pokemon.
    DrawChoiceRow(P4_ROW_Y(P4_BALL), "BALL", "OWN", "POKE BALL",
                  Ctr3dsGetFollowerPokeBall());
}

// PAGE 5: the debug menu.
//
// This page holds all controls that test the port, not the game, behind one
// compile-time value: CTR_DEBUG_MENU in 3ds/bridge.h. At 0, this page and its
// pager button are not in the build. The host also holds the three settings at
// their neutral values, so nothing can come from settings.bin into a release.
//
// Each switch is a check row: a checkbox, a label, and a note that says what
// the switch does. The notes are necessary: "PSG" and "DIRECT" mean nothing to
// a tester who does not know the mixer.
#if CTR_DEBUG_MENU

enum {
    DBG_SHINY,
    DBG_TABS,
    DBG_PSG,
    DBG_DIRECT,
    DBG_REVERB,
    DBG_STEREO,
    DBG_ROW_COUNT
};

static const struct { const char *label, *note; } sDebugRows[DBG_ROW_COUNT] =
{
    [DBG_SHINY]  = { "SHINY",    "next wild encounter" },
    [DBG_TABS]   = { "ALL TABS", "ignore save unlocks" },
    [DBG_PSG]    = { "PSG",      "the 4 GB voices"     },
    [DBG_DIRECT] = { "DIRECT",   "the sampled half"    },
    [DBG_REVERB] = { "REVERB",   "479 of 529 songs"    },
    [DBG_STEREO] = { "STEREO",   "off = downmix"       },
};

// The four audio rows, in the order above. Do not calculate the index from the
// row number. CTR_AUDIO_DBG_* is an enum, and DIRECT is CTR_AUDIO_DBG_DS, not
// the third value.
static const u8 sDebugAudio[] = {
    CTR_AUDIO_DBG_PSG, CTR_AUDIO_DBG_DS,
    CTR_AUDIO_DBG_REVERB, CTR_AUDIO_DBG_STEREO,
};

// The rows start below the pager, because the pager uses the top line. This
// page has seven rows: six switches and the achievements row. The rows are
// 20px tall at a 22px pitch. Row 0 starts at y 30 and row 6 ends at y 182.
//
// The switches use the check row columns. The achievements row has its label
// at x 16, its buttons at 62 and 128, and its note from 198 to the interior
// edge at 311. The widest note is 103px.
#define DBG_BTN_H     20
#define DBG_PITCH     22
#define DBG_Y(i)      (30 + (int)(i) * DBG_PITCH)
#define DBG_LABEL_X   16
#define DBG_BTN_W     60
#define DBG_BTN_X(c)  (62 + (c) * (DBG_BTN_W + 6))
#define DBG_NOTE_X    198

// One function reads a row and one writes a row, so the draw and the touch
// handler always agree on which switch a row is.
static int DebugRowOn(u32 row)
{
    switch (row)
    {
    case DBG_SHINY: return Ctr3dsGetShinyTest() != 0;
    case DBG_TABS:  return Ctr3dsGetShowAllTabs() != 0;
    default:        return Ctr3dsGetAudioDbg(sDebugAudio[row - DBG_PSG]) != 0;
    }
}

static void DebugRowSet(u32 row, int on)
{
    switch (row)
    {
    case DBG_SHINY: Ctr3dsSetShinyTest(on);   break;
    case DBG_TABS:  Ctr3dsSetShowAllTabs(on); break;
    default:        Ctr3dsSetAudioDbg(sDebugAudio[row - DBG_PSG], on); break;
    }
}

// The achievements row, below the six switches. It has two actions, not a
// switch, so it has no state and is not in the table. TEST queues a toast and
// unlocks nothing. RESYNC forgets this playthrough's unlocks and calculates
// them again from the save.
//
// RESYNC needs a second tap, as each stateful action on this screen does. The
// first tap arms it and changes the label. The second acts. Any other tap on
// the page disarms it. The shiny catch is an event that no save can calculate
// again, so RESYNC can lose it.
//
// The note shows two checks, the worse one first. A duplicate or out-of-range
// id is serious: each id is a bit in achievements.bin, so two achievements
// would share one unlock. Then the text width: nothing clips on this screen, so
// a text that is too wide runs into its neighbor.
#define DBG_ACH_ROW   DBG_ROW_COUNT

static bool8 sAchResyncArmed;

static void DrawAchRow(void)
{
    int y = DBG_Y(DBG_ACH_ROW);
    u16 badIds = AchDebugBadIds();
    u16 tooWide = UiTrophyTooWide();
    u8 label[40];

    UiText(DBG_LABEL_X, y + (DBG_BTN_H - UI_GLYPH_H) / 2,
           UiAscii(label, "ACH", sizeof(label)), UiThemeText(), UiThemeShadow());

    DrawButtonH(DBG_BTN_X(0), y, DBG_BTN_W, DBG_BTN_H,
                UiAscii(label, "TEST", sizeof(label)), FALSE);
    DrawButtonH(DBG_BTN_X(1), y, DBG_BTN_W, DBG_BTN_H,
                UiAscii(label, sAchResyncArmed ? "SURE?" : "RESYNC", sizeof(label)),
                sAchResyncArmed);

    if (badIds == 0 && tooWide == 0)
    {
        UiText(DBG_NOTE_X, y + (DBG_BTN_H - UI_GLYPH_H) / 2,
               UiAscii(label, "all text fits", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
    }
    else
    {
        int x = DBG_NOTE_X;

        x += UiNum(x, y + (DBG_BTN_H - UI_GLYPH_H) / 2, badIds ? badIds : tooWide,
                   UI_COL_ACCENT, UiThemeShadow());
        UiText(x, y + (DBG_BTN_H - UI_GLYPH_H) / 2,
               UiAscii(label, badIds ? " bad ids" : " too wide", sizeof(label)),
               UI_COL_ACCENT, UiThemeShadow());
    }
}

// TRUE if the touch was on the achievements row. Call only on release.
static bool8 TouchAchRow(const CtrTouchState *t)
{
    int y = DBG_Y(DBG_ACH_ROW);

    if (UiHit(t, DBG_BTN_X(0), y, DBG_BTN_W, DBG_BTN_H))
    {
        sAchResyncArmed = FALSE;
        AchDebugTestToast();
        UiMarkDirty();
        return TRUE;
    }

    if (UiHit(t, DBG_BTN_X(1), y, DBG_BTN_W, DBG_BTN_H))
    {
        if (sAchResyncArmed)
            AchDebugResync();

        sAchResyncArmed = !sAchResyncArmed;
        UiMarkDirty();
        return TRUE;
    }

    return FALSE;
}

static void DrawPageDebug(void)
{
    u8 label[40];
    u32 i;

    // The page shows its name to the left of the pager. Players must not see
    // this page, so a build that has it must be clear at a glance.
    UiText(DBG_LABEL_X, TOP_LINE_Y, UiAscii(label, "DEBUG", sizeof(label)),
           UiThemeText(), UiThemeShadow());
    // The caption ends at x 125, before the PAGE label of the pager.
    UiText(DBG_LABEL_X + 56, TOP_LINE_Y,
           UiAscii(label, "test build", sizeof(label)),
           UI_COL_DIM, UiThemeShadow());

    for (i = 0; i < DBG_ROW_COUNT; i++)
    {
        DrawCheckRow(DBG_Y(i), DBG_BTN_H, sDebugRows[i].label,
                     sDebugRows[i].note, DebugRowOn(i));
    }

    DrawAchRow();
}

static void TouchPageDebug(const CtrTouchState *t)
{
    if (TouchAchRow(t))
        return;

    // Any other tap on the page disarms RESYNC.
    if (sAchResyncArmed)
    {
        sAchResyncArmed = FALSE;
        UiMarkDirty();
    }

    for (u32 i = 0; i < DBG_ROW_COUNT; i++)
    {
        if (HitCheckRow(t, DBG_Y(i), DBG_BTN_H))
        {
            DebugRowSet(i, !DebugRowOn(i));
            UiMarkDirty();
            return;
        }
    }
}

#endif // CTR_DEBUG_MENU

static void DrawPager(void)
{
    u8 label[8];
    u32 i;

    UiTextRight(PGR_X(0) - 4, PGR_Y + (PGR_H - UI_GLYPH_H) / 2,
                UiAscii(label, "PAGE", sizeof(label)),
                UI_COL_DIM, UiThemeShadow());

    for (i = 0; i < PAGE_COUNT; i++)
    {
        char text[2];

        text[0] = (char)('1' + i);
        text[1] = '\0';

        DrawButtonH(PGR_X((int)i), PGR_Y, PGR_W, PGR_H,
                    UiAscii(label, text, sizeof(label)), sPage == i);
    }
}

void UiExtraDraw(void)
{
    UiWindowFrame(0, 0, CTR_BOTTOM_WIDTH / 8, UI_CONTENT_H / 8);

    if (sPage == 0)
        DrawPage1();
    else if (sPage == 1)
        DrawPage2();
    else if (sPage == 2)
        DrawPage3();
    else if (sPage == 3)
        DrawPage4();
#if CTR_DEBUG_MENU
    else
        DrawPageDebug();
#endif

    DrawPager();
}

// Page 2's "cap NN" value changes when the player gets a badge, which occurs
// away from this tab. Everything else changes only through the touch handler
// below, which marks the screen dirty. The cap alone needs a key, or the value
// stays stale.
//
// There are two functions because the PARTY tab's cheat tags need the same
// state but not EXTRA's page number.
u32 UiTweakStateKey(void)
{
    return (u32)Ctr3dsCurrentLevelCap()
         | ((u32)Ctr3dsGetLevelCap() << 8)
         | ((u32)(Ctr3dsGetExpAll() != 0) << 10)
         | ((u32)(Ctr3dsGetRandomizer() != 0) << 11)
         | ((u32)Ctr3dsGetBagSort() << 12);
}

u32 UiExtraStateKey(void)
{
    // The sPage value uses bits 0-2 and the tweaks use bits 4-17. Bit 3 is for
    // MUSIC FAST. Only its button changes it, but fold it in anyway: a setting
    // on the screen must not go stale. The audio switches use bits 24-27.
    u32 audio = 0;

    for (u32 i = 0; i < CTR_AUDIO_DBG_COUNT; i++)
        audio |= (u32)(Ctr3dsGetAudioDbg((int)i) != 0) << i;

    return (u32)sPage
         | ((u32)(Ctr3dsGetFfAudio() == CTR_FFAUDIO_FAST) << 3)
         | (UiTweakStateKey() << 4)
         // This switch can change without a touch: 3ds/tweaks.c clears it when
         // the armed encounter starts, which can occur while this tab is on the
         // screen.
         | ((u32)(Ctr3dsGetShinyTest() != 0) << 18)
         | (audio << 24);
}

static void TouchPage1(const CtrTouchState *t)
{
    if (UiHit(t, FFA_X, FFA_Y, FFA_W, FFA_H))
    {
        Ctr3dsSetFfAudio(Ctr3dsGetFfAudio() == CTR_FFAUDIO_FAST
                             ? CTR_FFAUDIO_NORMAL : CTR_FFAUDIO_FAST);
        UiMarkDirty();
        return;
    }

    for (u32 i = 0; i < SPEED_COUNT; i++)
    {
        if (UiHit(t, SPD_X((int)i), SPD_Y, SPD_W, BTN_H))
        {
            Ctr3dsSetSpeed(sSpeeds[i]);
            UiMarkDirty();
            return;
        }
    }

    for (u32 i = 0; i < SCALE_COUNT; i++)
    {
        if (UiHit(t, SCL_X((int)i), SCL_Y, SCL_W, BTN_H))
        {
            Ctr3dsSetTopScale(sScales[i]);
            UiMarkDirty();
            return;
        }
    }

    for (u32 i = 0; i < CTR_TURBO_COUNT; i++)
    {
        if (UiHit(t, TRB_X((int)i), TRB_Y, TRB_W, BTN_H))
        {
            // Go to the next step and wrap through 0. Find the current value,
            // do not store an index, so the button and the host always agree.
            int cur = Ctr3dsGetTurboBind((int)i);
            u32 step = 0;

            for (u32 k = 0; k < ARRAY_COUNT(sTurboSteps); k++)
                if (sTurboSteps[k] == cur)
                    step = k;

            step = (step + 1) % ARRAY_COUNT(sTurboSteps);
            Ctr3dsSetTurboBind((int)i, sTurboSteps[step]);
            UiMarkDirty();
            return;
        }
    }
}

static void TouchPage2(const CtrTouchState *t)
{
    static const u8 sCapModes[]  = { CTR_CAP_OFF, CTR_CAP_SOFT, CTR_CAP_HARD };
    static const u8 sSortModes[] = { CTR_BAGSORT_OFF, CTR_BAGSORT_TYPE, CTR_BAGSORT_NAME };

    if (HitCheckRow(t, P2_EXP_Y, CHK_ROW_H))
    {
        Ctr3dsSetExpAll(!Ctr3dsGetExpAll());
        UiMarkDirty();
        return;
    }

    for (u32 i = 0; i < ARRAY_COUNT(sCapModes); i++)
    {
        if (UiHit(t, WIDE_X((int)i), P2_CAP_BTN_Y, WIDE_W, BTN_H))
        {
            Ctr3dsSetLevelCap(sCapModes[i]);
            UiMarkDirty();
            return;
        }
    }

    if (HitCheckRow(t, P2_RAND_Y, CHK_ROW_H))
    {
        Ctr3dsSetRandomizer(!Ctr3dsGetRandomizer());
        UiMarkDirty();
        return;
    }

    for (u32 i = 0; i < ARRAY_COUNT(sSortModes); i++)
    {
        if (UiHit(t, SORT_X((int)i), P2_SORT_Y, SORT_W, BTN_H))
        {
            Ctr3dsSetBagSort(sSortModes[i]);

            // Apply it now, not at the next bag open, so the tap has a visible
            // effect. Ctr3dsSortBagNow refuses unless the player is in the
            // overworld with no script.
            Ctr3dsSortBagNow();

            UiMarkDirty();
            return;
        }
    }
}

static void TouchPage3(const CtrTouchState *t)
{
    // Each tap sets the inverse of the value that the draw shows.
    if (HitCheckRow(t, P3_ROW_Y(P3_PHONE_CALLS), CHK_ROW_H))
        Ctr3dsSetPhoneCallsOff(!Ctr3dsGetPhoneCallsOff());
    else if (HitCheckRow(t, P3_ROW_Y(P3_QUICK_BALL), CHK_ROW_H))
        Ctr3dsSetQuickBallOff(!Ctr3dsGetQuickBallOff());
    else if (HitCheckRow(t, P3_ROW_Y(P3_BATTLE_ANIM), CHK_ROW_H))
        Ctr3dsSetBattleAnimOff(!Ctr3dsGetBattleAnimOff());
    else if (HitCheckRow(t, P3_ROW_Y(P3_DAY_CARE), CHK_ROW_H))
        Ctr3dsSetDayCareYard(!Ctr3dsGetDayCareYard());
    else
        return;

    UiMarkDirty();
}

static void TouchPage4(const CtrTouchState *t)
{
    int who = -1;

    if (HitCheckRow(t, P4_ROW_Y(P4_FOLLOWER), CHK_ROW_H))
    {
        // The follower appears or goes away now if the player is in the
        // overworld with no script. Otherwise the next map load does it.
        Ctr3dsSetFollowerOn(!Ctr3dsGetFollowerOn());
        Ctr3dsRefreshFollowerNow();
    }
    else if (UiHit(t, WIDE_X(1), P4_ROW_Y(P4_WHO), WIDE_W, BTN_H))
        who = CTR_FOLLOWER_LEAD;
    else if (UiHit(t, WIDE_X(2), P4_ROW_Y(P4_WHO), WIDE_W, BTN_H))
        who = CTR_FOLLOWER_STARTER;
    // BOBBING and BALL need no refresh. They show at the next step and at the
    // next ball animation.
    else if (HitCheckRow(t, P4_ROW_Y(P4_BOBBING), CHK_ROW_H))
        Ctr3dsSetFollowerBobOff(!Ctr3dsGetFollowerBobOff());
    else if (UiHit(t, WIDE_X(1), P4_ROW_Y(P4_BALL), WIDE_W, BTN_H))
        Ctr3dsSetFollowerPokeBall(0);
    else if (UiHit(t, WIDE_X(2), P4_ROW_Y(P4_BALL), WIDE_W, BTN_H))
        Ctr3dsSetFollowerPokeBall(1);
    else
        return;

    // A new WHO can change the follower. Refresh it, as FOLLOWER does.
    if (who >= 0 && who != Ctr3dsGetFollowerWho())
    {
        Ctr3dsSetFollowerWho(who);
        Ctr3dsRefreshFollowerNow();
    }

    UiMarkDirty();
}

void UiExtraTouch(const CtrTouchState *t)
{
    if (!t->justReleased)
        return;

    // The pager is live on every page. Test it before the page's own controls,
    // so nothing can be under it.
    for (u32 i = 0; i < PAGE_COUNT; i++)
    {
        if (UiHit(t, PGR_X((int)i), PGR_Y, PGR_W, PGR_H))
        {
            sPage = (u8)i;
#if CTR_DEBUG_MENU
            // A page turn disarms RESYNC too, so it is not armed when the debug
            // page comes back.
            sAchResyncArmed = FALSE;
#endif
            UiMarkDirty();
            return;
        }
    }

    if (sPage == 0)
        TouchPage1(t);
    else if (sPage == 1)
        TouchPage2(t);
    else if (sPage == 2)
        TouchPage3(t);
    else if (sPage == 3)
        TouchPage4(t);
#if CTR_DEBUG_MENU
    else
        TouchPageDebug(t);
#endif
}
