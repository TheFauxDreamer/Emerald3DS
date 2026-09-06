// EXTRA tab: port features that are not part of the original game.
//
// Two pages, and the split between them is the point rather than a way to fit
// more in. PAGE 1 is host-side only: fast-forward, top-screen scale, button
// binds, and the show-all-tabs testing override. None of it changes how Emerald
// plays, which is what the rest of 3ds/ui/ is built on.
//
// PAGE 2 is the exception, and is deliberately kept behind a page turn rather
// than mixed in with page 1. Every option on it is a cheat: EXP All, a level
// cap, a species randomiser, a bag sort order. The behaviour lives in
// 3ds/tweaks.c; this file only draws the toggles and reads them back.
//
// Paging costs no vertical space at all. Page 1 already fills its 176px
// interior exactly (four rows, y=8 to y=183), so the pager sits on the row-1
// label line instead, whose right-hand side is empty on both pages.
//
// On both pages the first three button rows deliberately span the same 22..298,
// so the block reads as one control panel rather than three unrelated widgets.
// The fourth row is set apart: label beside its buttons, not above.

#include "global.h"

#include "../bridge.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"

// Ctr3dsCurrentLevelCap(), for the live "cap NN" readout on page 2.
#include "../tweaks.h"

// Doubling steps rather than 1/2/3/4: past 2x the interesting question is
// "much faster", and 3x sits too close to 2x to be worth a button.
static const u8 sSpeeds[] = { 1, 2, 4, 8 };
#define SPEED_COUNT   ARRAY_COUNT(sSpeeds)

// Indexed by button, never derived from the index: the modes are an enum, not a
// sequence, so arithmetic on the index would be a lie waiting to break.
static const u8 sScales[] = {
    CTR_TOP_SCALE_1X, CTR_TOP_SCALE_1_5X, CTR_TOP_SCALE_FILL
};
static const char *const sScaleNames[] = { "1x", "1.5x", "FILL" };
#define SCALE_COUNT   ARRAY_COUNT(sScales)

// What a button can be bound to, in tap order. MOD sits in the same cycle as
// the speeds rather than in a separate control, which is what makes the two
// mutually exclusive: a button holds one value, so binding a speed necessarily
// stops it being the modifier and there is no way to ask for both.
//
// Tapping wraps back to unbound, so every state is reachable with one finger.
static const u8 sTurboSteps[] = { CTR_BIND_OFF, 2, 4, 8, CTR_BIND_MOD };
static const char *const sTurboNames[CTR_TURBO_COUNT] = { "X", "Y", "ZL", "ZR" };

// The first three rows total 276px wide (4*60 + 3*12, and 3*84 + 2*12), centred
// in 320, which leaves 22 either side and clears the 8px window frame
// comfortably.
#define BTN_H         26
#define BTN_GAP       12

// Four rows do not fit at the old 52px pitch: a labelled row is 15 + 2 + 26 =
// 43px, and four of those plus gaps overruns the 176px interior. So the last
// row carries its label beside its buttons instead of above them, which is what
// buys the ~35px, and the MOD note moves up onto the BUTTON HOLD label's line
// where it belongs anyway.
#define ROW1_LABEL_Y  8
#define ROW2_LABEL_Y  58
#define ROW3_LABEL_Y  108
#define LABEL_TO_BTN  17

// Named for the row rather than for whatever page 1 happens to put there, so
// page 2 can share the grid without its code reading as if it were drawing
// game speeds and screen scales.
#define ROW1_BTN_Y    (ROW1_LABEL_Y + LABEL_TO_BTN)
#define ROW2_BTN_Y    (ROW2_LABEL_Y + LABEL_TO_BTN)
#define ROW3_BTN_Y    (ROW3_LABEL_Y + LABEL_TO_BTN)

#define SPD_W         60
#define SPD_Y         ROW1_BTN_Y
#define SPD_X(i)      (22 + (i) * (SPD_W + BTN_GAP))

#define SCL_W         84
#define SCL_Y         ROW2_BTN_Y
#define SCL_X(i)      (22 + (i) * (SCL_W + BTN_GAP))

#define TRB_W         60
#define TRB_Y         ROW3_BTN_Y
#define TRB_X(i)      (22 + (i) * (TRB_W + BTN_GAP))

// The MOD note shares the BUTTON HOLD label's line. That label is 63px wide, so
// 96 clears it, and the note is 163px, ending well inside the 311px interior.
#define TRB_NOTE_X    96

// What fast-forward does to the music, sharing the GAME SPEED label line the
// same way the MOD note shares BUTTON HOLD's -- page 1's four button rows
// already fill the 176px interior exactly, so a fifth row does not fit and the
// spare half of a label line is the only room there is.
//
// "GAME SPEED" ends near x=76. A third page moved the pager one button further
// left, taking its PAGE caption from x=226 to x=198, so this shrank from 100 to
// 88 to keep clear of it: 104..192 against a caption starting at 198. The
// widest label it draws, "MUSIC FAST", is 54px, so 88 is still generous.
//
// 17px tall at y=8 ends at y=24, abutting the row-1 buttons at y=25 without
// overlapping, exactly as the pager does.
//
// It belongs on this row because GAME SPEED is the only control it modifies.
#define FFA_X         104
#define FFA_W         88
#define FFA_Y         ROW1_LABEL_Y
#define FFA_H         PGR_H

// The fourth row, the one that carries its label beside its buttons rather than
// above them. Page 1 no longer uses it -- the tab-unlock override that lived
// here moved to the debug page -- so page 2's BAG SORT is its only tenant.
#define ROW4_Y        157
#define ROW4_LABEL_X  16

// The pager, right-aligned to the interior edge at x=311 and sharing the row-1
// label line. 17px tall at y=8 ends at y=24, exactly abutting the row-1 buttons
// at y=25 without overlapping them, which is what makes it free.
#define PGR_W         26
#define PGR_H         17
#define PGR_Y         ROW1_LABEL_Y
// Two pages of settings, plus the debug page when it is compiled in. Every
// other constant below is derived, so this is the only thing the flag moves.
#if CTR_DEBUG_MENU
#define PAGE_COUNT    3
#else
#define PAGE_COUNT    2
#endif

// Right-aligned to the interior edge, growing leftwards as pages are added, so
// the last button always lands in the same place however many there are. At
// PAGE_COUNT 2 this is the same arithmetic the two-page version spelled out
// longhand: 256 and 286.
#define PGR_X(i)      (CTR_BOTTOM_WIDTH - 8 - PGR_W \
                       - (PAGE_COUNT - 1 - (i)) * (PGR_W + 4))

// Page 2 rows 1 to 3 reuse the SCREEN SIZE geometry above unchanged. Row 4
// carries its label beside its buttons, as page 1's does, and three 75px
// buttons is what fits between the label and the edge.
#define P2_HINT_X     96
#define WIDE_W        SCL_W
#define WIDE_X(i)     SCL_X(i)
#define SORT_W        75
#define SORT_X(i)     (70 + (i) * (SORT_W + 8))

// Which page is showing. UI state only, deliberately not persisted: EXTRA
// always opens on page 1, so the cheats are never what greets you.
static u8 sPage;

// The selected button gets a doubled inset outline as well as accent text.
// Colour alone is easy to miss against the lighter window frames.
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

static void DrawPage1(void)
{
    u8 label[40];

    UiText(16, ROW1_LABEL_Y, UiAscii(label, "GAME SPEED", sizeof(label)),
           UiThemeText(), UiThemeShadow());

    for (u32 i = 0; i < SPEED_COUNT; i++)
    {
        char text[4];

        // Every offered speed is a single digit, so no wider label is needed.
        text[0] = (char)('0' + sSpeeds[i]);
        text[1] = 'x';
        text[2] = '\0';

        DrawButton(SPD_X((int)i), SPD_Y, SPD_W,
                   UiAscii(label, text, sizeof(label)),
                   sSpeeds[i] == Ctr3dsGetSpeed());
    }

    // Highlighted on FAST rather than on 1x: 1x is the default, and the accent
    // outline reads as "something has been changed here", which is how the
    // turbo binds below use it too.
    {
        int fast = (Ctr3dsGetFfAudio() == CTR_FFAUDIO_FAST);

        DrawButtonH(FFA_X, FFA_Y, FFA_W, FFA_H,
                    UiAscii(label, fast ? "MUSIC FAST" : "MUSIC 1x",
                            sizeof(label)),
                    fast);
    }

    UiText(16, ROW2_LABEL_Y, UiAscii(label, "SCREEN SIZE", sizeof(label)),
           UiThemeText(), UiThemeShadow());

    for (u32 i = 0; i < SCALE_COUNT; i++)
        DrawButton(SCL_X((int)i), SCL_Y, SCL_W,
                   UiAscii(label, sScaleNames[i], sizeof(label)),
                   sScales[i] == Ctr3dsGetTopScale());

    UiText(16, ROW3_LABEL_Y, UiAscii(label, "BUTTON HOLD", sizeof(label)),
           UiThemeText(), UiThemeShadow());

    // MOD needs saying: it is not obvious that one of these buttons is what
    // makes the Pokedex arrows jump. ZL and ZR do not exist on an Old 3DS, so
    // a binding there would otherwise look broken rather than unsupported.
    UiText(TRB_NOTE_X, ROW3_LABEL_Y,
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

    UiText(16, ROW1_LABEL_Y, UiAscii(label, "EXP ALL", sizeof(label)),
           UiThemeText(), UiThemeShadow());

    DrawButton(WIDE_X(0), ROW1_BTN_Y, WIDE_W, UiAscii(label, "OFF", sizeof(label)),
               !Ctr3dsGetExpAll());
    DrawButton(WIDE_X(1), ROW1_BTN_Y, WIDE_W, UiAscii(label, "ON", sizeof(label)),
               Ctr3dsGetExpAll());

    UiText(16, ROW2_LABEL_Y, UiAscii(label, "LEVEL CAP", sizeof(label)),
           UiThemeText(), UiThemeShadow());

    // The cap the player is currently under, so the row says what it is doing
    // rather than only that it is on. Tracks badge progress, which is why EXTRA
    // needs a state key in bottom_screen.c.
    mode = Ctr3dsGetLevelCap();
    if (mode != CTR_CAP_OFF)
    {
        int x = P2_HINT_X;

        x += UiText(x, ROW2_LABEL_Y, UiAscii(label, "cap ", sizeof(label)),
                    UI_COL_DIM, UiThemeShadow());
        UiNum(x, ROW2_LABEL_Y, cap, UI_COL_DIM, UiThemeShadow());
    }

    DrawButton(WIDE_X(0), ROW2_BTN_Y, WIDE_W, UiAscii(label, "OFF", sizeof(label)),
               mode == CTR_CAP_OFF);
    DrawButton(WIDE_X(1), ROW2_BTN_Y, WIDE_W, UiAscii(label, "SOFT", sizeof(label)),
               mode == CTR_CAP_SOFT);
    DrawButton(WIDE_X(2), ROW2_BTN_Y, WIDE_W, UiAscii(label, "HARD", sizeof(label)),
               mode == CTR_CAP_HARD);

    UiText(16, ROW3_LABEL_Y, UiAscii(label, "RANDOMISER", sizeof(label)),
           UiThemeText(), UiThemeShadow());

    // Worth saying: nothing already caught or already on screen changes, and
    // the mapping is keyed on this save's trainer ID, so it is the same every
    // launch and toggling it off and back on does not reshuffle anything.
    UiText(P2_HINT_X, ROW3_LABEL_Y,
           UiAscii(label, "new encounters only", sizeof(label)),
           UI_COL_DIM, UiThemeShadow());

    DrawButton(WIDE_X(0), ROW3_BTN_Y, WIDE_W, UiAscii(label, "OFF", sizeof(label)),
               !Ctr3dsGetRandomizer());
    DrawButton(WIDE_X(1), ROW3_BTN_Y, WIDE_W, UiAscii(label, "ON", sizeof(label)),
               Ctr3dsGetRandomizer());

    UiText(ROW4_LABEL_X, ROW4_Y + (BTN_H - UI_GLYPH_H) / 2,
           UiAscii(label, "BAG SORT", sizeof(label)),
           UiThemeText(), UiThemeShadow());

    mode = Ctr3dsGetBagSort();

    DrawButton(SORT_X(0), ROW4_Y, SORT_W, UiAscii(label, "OFF", sizeof(label)),
               mode == CTR_BAGSORT_OFF);
    DrawButton(SORT_X(1), ROW4_Y, SORT_W, UiAscii(label, "TYPE", sizeof(label)),
               mode == CTR_BAGSORT_TYPE);
    DrawButton(SORT_X(2), ROW4_Y, SORT_W, UiAscii(label, "NAME", sizeof(label)),
               mode == CTR_BAGSORT_NAME);
}

// PAGE 3: the debug menu.
//
// Everything that exists to test the PORT rather than to play the game, in one
// place behind one compile-time value. Before this they were scattered: the tab
// override sat on page 1 among the display settings, the shiny switch on page 2
// among the cheats, and the audio A/B had a page of its own. Three different
// answers to "is this for players?", none of them easy to turn off together.
//
// CTR_DEBUG_MENU in 3ds/bridge.h is the switch. At 0 this page and its pager
// button vanish, and the three settings behind it are held at their neutral
// values host-side as well, so nothing here can leak into a shipping build
// through settings.bin.
//
// One uniform row shape, unlike the pages either side: label, two buttons, and
// a note saying what the switch actually does. The notes are not decoration --
// "PSG" and "DIRECT" mean nothing to someone who has not read the mixer, and
// this page exists to be usable by exactly that person.
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

static const struct { const char *label, *off, *on, *note; } sDebugRows[DBG_ROW_COUNT] =
{
    [DBG_SHINY]  = { "SHINY",  "OFF",  "NEXT", "next wild encounter" },
    [DBG_TABS]   = { "TABS",   "GAME", "ALL",  "ignore save unlocks" },
    [DBG_PSG]    = { "PSG",    "OFF",  "ON",   "the 4 GB voices"     },
    [DBG_DIRECT] = { "DIRECT", "OFF",  "ON",   "the sampled half"    },
    [DBG_REVERB] = { "REVERB", "OFF",  "ON",   "479 of 529 songs"    },
    [DBG_STEREO] = { "STEREO", "OFF",  "ON",   "off = downmix"       },
};

// The four audio rows in the order they appear above. Indexed, never derived
// from the row number by arithmetic: CTR_AUDIO_DBG_* is an enum, and DIRECT is
// CTR_AUDIO_DBG_DS rather than the third value in sequence.
static const u8 sDebugAudio[] = {
    CTR_AUDIO_DBG_PSG, CTR_AUDIO_DBG_DS,
    CTR_AUDIO_DBG_REVERB, CTR_AUDIO_DBG_STEREO,
};

// Rows start below the pager rather than beside it: the pager occupies y=8..25
// across the full width of the row-1 label line on every page, and this page
// wants that width for six rows rather than three. 22px buttons at a 26px pitch
// put row 0 at y=30 and row 5 ending at 182, inside the 184px floor.
//
// Columns: label at 16 (widest is 36px, "DIRECT"), buttons at 62 and 128 ending
// at 188, notes from 198 to the 311px interior edge. The widest note is 103px,
// so the note column has 10px to spare and the buttons never reach it.
#define DBG_BTN_H     22
#define DBG_PITCH     26
#define DBG_Y(i)      (30 + (int)(i) * DBG_PITCH)
#define DBG_LABEL_X   16
#define DBG_BTN_W     60
#define DBG_BTN_X(c)  (62 + (c) * (DBG_BTN_W + 6))
#define DBG_NOTE_X    198

// Reading a row and writing a row, in one place each, so the draw and the touch
// handler cannot disagree about which switch a row means.
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

static void DrawPage3(void)
{
    u8 label[40];
    u32 i;

    // The page says what it is, in the space left of the pager. Worth the line:
    // this is the one page whose contents are not meant to reach a player, and
    // a build that still has it needs to be obvious at a glance.
    UiText(DBG_LABEL_X, ROW1_LABEL_Y, UiAscii(label, "DEBUG", sizeof(label)),
           UiThemeText(), UiThemeShadow());
    UiText(DBG_LABEL_X + 56, ROW1_LABEL_Y,
           UiAscii(label, "not in shipping builds", sizeof(label)),
           UI_COL_DIM, UiThemeShadow());

    for (i = 0; i < DBG_ROW_COUNT; i++)
    {
        int y = DBG_Y(i);
        int on = DebugRowOn(i);

        UiText(DBG_LABEL_X, y + (DBG_BTN_H - UI_GLYPH_H) / 2,
               UiAscii(label, sDebugRows[i].label, sizeof(label)),
               UiThemeText(), UiThemeShadow());

        DrawButtonH(DBG_BTN_X(0), y, DBG_BTN_W, DBG_BTN_H,
                    UiAscii(label, sDebugRows[i].off, sizeof(label)), !on);
        DrawButtonH(DBG_BTN_X(1), y, DBG_BTN_W, DBG_BTN_H,
                    UiAscii(label, sDebugRows[i].on, sizeof(label)), on);

        UiText(DBG_NOTE_X, y + (DBG_BTN_H - UI_GLYPH_H) / 2,
               UiAscii(label, sDebugRows[i].note, sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
    }
}

static void TouchPage3(const CtrTouchState *t)
{
    for (u32 i = 0; i < DBG_ROW_COUNT; i++)
    {
        for (u32 c = 0; c < 2; c++)
        {
            if (UiHit(t, DBG_BTN_X((int)c), DBG_Y(i), DBG_BTN_W, DBG_BTN_H))
            {
                DebugRowSet(i, (int)c);
                UiMarkDirty();
                return;
            }
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
#if CTR_DEBUG_MENU
    else
        DrawPage3();
#endif

    DrawPager();
}

// Page 2's "cap NN" readout moves when the player earns a badge, which happens
// nowhere near this tab. Everything else here changes only through the touch
// handler below, which marks dirty itself, but the cap alone is enough to need
// a key: without one the readout would sit stale until the tab was re-entered.
//
// Split in two because the PARTY tab's cheat tags need the same state and none
// of EXTRA's own page number. The cap VALUE is the part that actually moves on
// its own; the rest only changes under this tab's own touch handler.
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
    // sPage takes bits 0-1 and the tweaks start at bit 4, so bit 2 is free.
    // Folded in even though the only way to change it is the button above,
    // which marks the screen dirty itself: a setting that can go stale on
    // screen is exactly what this hash exists to prevent.
    // The three audio switches go at bit 24, clear of UiTweakStateKey's own
    // range (it reaches bit 13, so bit 17 after the shift below).
    u32 audio = 0;

    for (u32 i = 0; i < CTR_AUDIO_DBG_COUNT; i++)
        audio |= (u32)(Ctr3dsGetAudioDbg((int)i) != 0) << i;

    return (u32)sPage
         | ((u32)(Ctr3dsGetFfAudio() == CTR_FFAUDIO_FAST) << 2)
         // Bit 3, the last one free below the tweaks. Unlike every other
         // switch on these pages this one can change with no touch at all:
         // 3ds/tweaks.c clears it the moment the armed encounter is created,
         // which can happen with this tab on screen.
         | ((u32)(Ctr3dsGetShinyTest() != 0) << 3)
         | (UiTweakStateKey() << 4)
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
            // Cycle to the next step, wrapping through 0. Searching for the
            // current value rather than storing an index keeps the button and
            // the host in agreement even if one is changed elsewhere.
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

    if (UiHit(t, WIDE_X(0), ROW1_BTN_Y, WIDE_W, BTN_H))
    {
        Ctr3dsSetExpAll(0);
        UiMarkDirty();
        return;
    }
    if (UiHit(t, WIDE_X(1), ROW1_BTN_Y, WIDE_W, BTN_H))
    {
        Ctr3dsSetExpAll(1);
        UiMarkDirty();
        return;
    }

    for (u32 i = 0; i < ARRAY_COUNT(sCapModes); i++)
    {
        if (UiHit(t, WIDE_X((int)i), ROW2_BTN_Y, WIDE_W, BTN_H))
        {
            Ctr3dsSetLevelCap(sCapModes[i]);
            UiMarkDirty();
            return;
        }
    }

    if (UiHit(t, WIDE_X(0), ROW3_BTN_Y, WIDE_W, BTN_H))
    {
        Ctr3dsSetRandomizer(0);
        UiMarkDirty();
        return;
    }
    if (UiHit(t, WIDE_X(1), ROW3_BTN_Y, WIDE_W, BTN_H))
    {
        Ctr3dsSetRandomizer(1);
        UiMarkDirty();
        return;
    }

    for (u32 i = 0; i < ARRAY_COUNT(sSortModes); i++)
    {
        if (UiHit(t, SORT_X((int)i), ROW4_Y, SORT_W, BTN_H))
        {
            Ctr3dsSetBagSort(sSortModes[i]);

            // Apply it straight away rather than waiting for the next bag
            // open, so the button that was just tapped has a visible effect.
            // Ctr3dsSortBagNow refuses unless the player is stood in the
            // overworld with no script running.
            Ctr3dsSortBagNow();

            UiMarkDirty();
            return;
        }
    }
}

void UiExtraTouch(const CtrTouchState *t)
{
    if (!t->justReleased)
        return;

    // The pager is live on both pages and is tested before either page's own
    // controls, so nothing can ever sit underneath it.
    for (u32 i = 0; i < PAGE_COUNT; i++)
    {
        if (UiHit(t, PGR_X((int)i), PGR_Y, PGR_W, PGR_H))
        {
            sPage = (u8)i;
            UiMarkDirty();
            return;
        }
    }

    if (sPage == 0)
        TouchPage1(t);
    else if (sPage == 1)
        TouchPage2(t);
#if CTR_DEBUG_MENU
    else
        TouchPage3(t);
#endif
}
