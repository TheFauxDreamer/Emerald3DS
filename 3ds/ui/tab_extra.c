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

// Tab visibility. Label on the left, then the two states, then a reminder that
// this exists for testing rather than as a way to skip the game.
#define ROW4_Y        157
#define TAB_Y         ROW4_Y
#define TAB_W         84
#define TAB_LABEL_X   16
#define TAB_X(i)      (60 + (i) * (TAB_W + 8))
#define TAB_HINT_X    (TAB_X(1) + TAB_W + 12)

// The pager, right-aligned to the interior edge at x=311 and sharing the row-1
// label line. 17px tall at y=8 ends at y=24, exactly abutting the row-1 buttons
// at y=25 without overlapping them, which is what makes it free.
#define PGR_W         26
#define PGR_H         17
#define PGR_Y         ROW1_LABEL_Y
#define PAGE_COUNT    3

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

    // Tab visibility. GAME is the real behaviour; ALL is the testing override.
    UiText(TAB_LABEL_X, TAB_Y + (BTN_H - UI_GLYPH_H) / 2,
           UiAscii(label, "TABS", sizeof(label)), UiThemeText(), UiThemeShadow());

    DrawButton(TAB_X(0), TAB_Y, TAB_W, UiAscii(label, "GAME", sizeof(label)),
               !Ctr3dsGetShowAllTabs());
    DrawButton(TAB_X(1), TAB_Y, TAB_W, UiAscii(label, "ALL", sizeof(label)),
               Ctr3dsGetShowAllTabs());

    UiText(TAB_HINT_X, TAB_Y + (BTN_H - UI_GLYPH_H) / 2,
           UiAscii(label, "for testing", sizeof(label)),
           UI_COL_DIM, UiThemeShadow());
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

    UiText(TAB_LABEL_X, ROW4_Y + (BTN_H - UI_GLYPH_H) / 2,
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

// PAGE 3: the audio A/B switches.
//
// A diagnostic page, and a third page rather than a squeeze onto page 2,
// because these are not settings anybody is meant to have a preference about.
// All three default ON, which is the real mixer; turning one off is how you
// find out which half of it a fault lives in, since neither half can be judged
// by ear while the other is playing.
//
// Rows 1 to 3 reuse page 2's two-button geometry unchanged.
static const struct { const char *label; u8 which; const char *note; } sAudioRows[] = {
    { "PSG",    CTR_AUDIO_DBG_PSG,    "the 4 GB voices"    },
    { "REVERB", CTR_AUDIO_DBG_REVERB, "479 of 529 songs"   },
    { "STEREO", CTR_AUDIO_DBG_STEREO, "off = downmix"      },
};

#define AUD_LABEL_Y(i)  (ROW1_LABEL_Y + (int)(i) * 50)
#define AUD_BTN_Y(i)    (AUD_LABEL_Y(i) + LABEL_TO_BTN)

static void DrawPage3(void)
{
    u8 label[40];
    u32 i;

    for (i = 0; i < ARRAY_COUNT(sAudioRows); i++)
    {
        int on = Ctr3dsGetAudioDbg(sAudioRows[i].which);

        UiText(16, AUD_LABEL_Y((int)i),
               UiAscii(label, sAudioRows[i].label, sizeof(label)),
               UiThemeText(), UiThemeShadow());

        // What the switch actually silences, because "PSG" means nothing
        // without it and this page exists to be used by someone who does not
        // already know the mixer.
        UiText(P2_HINT_X, AUD_LABEL_Y((int)i),
               UiAscii(label, sAudioRows[i].note, sizeof(label)),
               UI_COL_DIM, UiThemeShadow());

        DrawButton(WIDE_X(0), AUD_BTN_Y((int)i), WIDE_W,
                   UiAscii(label, "OFF", sizeof(label)), !on);
        DrawButton(WIDE_X(1), AUD_BTN_Y((int)i), WIDE_W,
                   UiAscii(label, "ON", sizeof(label)), on);
    }

    UiText(TAB_LABEL_X, ROW4_Y + (BTN_H - UI_GLYPH_H) / 2,
           UiAscii(label, "for finding sound bugs. all ON is normal.",
                   sizeof(label)),
           UI_COL_DIM, UiThemeShadow());
}

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
    else
        DrawPage3();

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

    for (u32 i = 0; i < 2; i++)
    {
        if (UiHit(t, TAB_X((int)i), TAB_Y, TAB_W, BTN_H))
        {
            Ctr3dsSetShowAllTabs((int)i);
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

static void TouchPage3(const CtrTouchState *t)
{
    u32 i;

    for (i = 0; i < ARRAY_COUNT(sAudioRows); i++)
    {
        if (UiHit(t, WIDE_X(0), AUD_BTN_Y((int)i), WIDE_W, BTN_H))
        {
            Ctr3dsSetAudioDbg(sAudioRows[i].which, 0);
            UiMarkDirty();
            return;
        }
        if (UiHit(t, WIDE_X(1), AUD_BTN_Y((int)i), WIDE_W, BTN_H))
        {
            Ctr3dsSetAudioDbg(sAudioRows[i].which, 1);
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
    else
        TouchPage3(t);
}
