// TROPHY tab: the achievements list.
//
// Always available, like BAG and EXTRA. A locked list is a list of goals, so
// there is nothing to unlock first.
//
// It reads everything through AchActive() (3ds/achievements.h), so it works
// with any provider. Nothing here writes game state. It changes only the
// provider's "seen" bits.
//
// One window over the full content area. It contains:
// - two page buttons (MAIN and POST-GAME) with their counts
// - a gold progress bar for the page
// - four rows of two lines
// - the DEX tab's paging arrows
// The provider tells the page of each achievement and which ones are hidden.
// Before the Hall of Fame, the full post-game page is hidden.

#include "global.h"

#include "../bridge.h"
#include "../achievements.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"

// ---------------------------------------------------------------- layout ---
//
// 40x24 tiles, with the interior at x 8..312, y 8..184. The text has an 8px
// margin inside that, from x 16 to x 304.
#define TROPHY_TW      (CTR_BOTTOM_WIDTH / 8)
#define TROPHY_TH      (UI_CONTENT_H / 8)

#define IN_L           16
#define IN_R           (CTR_BOTTOM_WIDTH - 16)          // 304

// The two page buttons on the top line: 140px each with 8px between, from IN_L
// to IN_R. They are 17px tall at y 8, like the EXTRA pager, and end at 24,
// clear of the bar.
#define SEC_Y          8
#define SEC_H          17
#define SEC_W          140
#define SEC_GAP        8
#define SEC_X(s)       (IN_L + (s) * (SEC_W + SEC_GAP))  // 16, 164

// A 4px bar under the buttons, at 26..30, which leaves 3px above the first row.
#define BAR_X          IN_L
#define BAR_Y          26
#define BAR_W          (IN_R - IN_L)                    // 288
#define BAR_H          4

// Four rows of two lines, 31px apart: 33, 64, 95 and 126. The last ends at 157,
// clear of the paging arrows at 160. A fifth row would need 186.
#define LIST_Y         33
#define ROW_H          31
#define VISIBLE_ROWS   4
#define DESC_DY        15

// The marker column, then the text. All markers are centered on MARK_CX:
// - unlocked: the big sparkle
// - locked: a small hollow square
// - hidden: a question mark
#define MARK_CX        (IN_L + 8)                       // 24
#define TEXT_X         (IN_L + 20)                      // 36

// The right end of the title line shows a counter ("37/50") while locked, or a
// NEW tag after the unlock. Never both, so they share one column.
#define TAG_COL_W      56

// Limits for the achievement text. The debug page checks them, because nothing
// on this screen clips (SECOND_SCREEN_CHEATSHEET.md section 9).
#define TROPHY_TITLE_MAX_W  (IN_R - TAG_COL_W - TEXT_X)  // 212
#define TROPHY_DESC_MAX_W   (IN_R - TEXT_X)              // 268

// Show a counter only for small goals. A 100,000-step goal would change on each
// step, and the state key would repaint the tab each time.
#define COUNTER_MAX    999

// The DEX tab's arrows, at the same size, centered under the list. They are
// 22px tall at y 160 and end at 182, inside the 184 floor.
#define PAGE_Y         160
#define PAGE_W         52
#define PAGE_H         22
#define PAGE_UP_X      (CTR_BOTTOM_WIDTH / 2 - 12 - PAGE_W)   // 96
#define PAGE_DN_X      (CTR_BOTTOM_WIDTH / 2 + 12)            // 172

// The NEW mask is a bitset over the first MAX_TRACKED entries. That is the
// capacity of the host store (bridge.h).
#define MAX_TRACKED    (CTR_ACH_BYTES * 8)

#define NO_ROW         0xFFFF

static const char *const sSectionNames[ACH_SECTION_COUNT] =
{
    [ACH_SECTION_MAIN]     = "MAIN",
    [ACH_SECTION_POSTGAME] = "POST-GAME",
};

// The colors of each category (ui_shell.h tells why each ramp has this shape).
// Story keeps the shiny gold, so a provider with no categories looks the same
// as before.
static const struct UiRamp sCategoryRamps[ACH_CAT_COUNT] =
{
    [ACH_CAT_STORY]   = { UI_COL_SHINY_PALE,      UI_COL_SHINY,      UI_COL_SHINY_EDGE      },
    [ACH_CAT_LEGEND]  = { UI_COL_ACH_GREEN_PALE,  UI_COL_ACH_GREEN,  UI_COL_ACH_GREEN_EDGE  },
    [ACH_CAT_POKEMON] = { UI_COL_ACH_RED_PALE,    UI_COL_ACH_RED,    UI_COL_ACH_RED_EDGE    },
    [ACH_CAT_BATTLE]  = { UI_COL_ACH_PURPLE_PALE, UI_COL_ACH_PURPLE, UI_COL_ACH_PURPLE_EDGE },
    [ACH_CAT_EXTRA]   = { UI_COL_ACH_BLUE_PALE,   UI_COL_ACH_BLUE,   UI_COL_ACH_BLUE_EDGE   },
    [ACH_CAT_CONTEST] = { UI_COL_ACH_PINK_PALE,   UI_COL_ACH_PINK,   UI_COL_ACH_PINK_EDGE   },
};

const struct UiRamp *UiAchCategoryRamp(u8 category)
{
    return &sCategoryRamps[category < ACH_CAT_COUNT ? category : ACH_CAT_STORY];
}

// ----------------------------------------------------------------- state ---

// The page on the screen, and the scroll position of each page. This is UI
// state, like EXTRA's page. The tab moves to the page that has something new.
static u8     sSection;
static u16    sScroll[ACH_SECTION_COUNT];
static UiHold sHoldUp, sHoldDn;

// What was unseen when the tab came on the screen, and what unlocked while it
// stayed there, by provider index. The provider clears its own unseen bits when
// the tab shows (markAllSeen), so the tags live here. They last until the
// player leaves the tab.
static u8    sNewMask[CTR_ACH_BYTES];
static bool8 sVisible;

static bool8 MaskGet(u16 i)
{
    return i < MAX_TRACKED && ((sNewMask[i / 8] >> (i % 8)) & 1);
}

static void MaskSet(u16 i)
{
    if (i < MAX_TRACKED)
        sNewMask[i / 8] |= (u8)(1 << (i % 8));
}

// ---- pages ------------------------------------------------------------------
//
// Linear scans over the provider's section() and unlocked(), which read only
// the provider's bits. The cost is small, and the page structure stays the
// provider's business.

static u16 SectionCount(u8 s)
{
    const struct AchProvider *p = AchActive();
    u16 count = p->count(), n = 0;

    for (u16 i = 0; i < count; i++)
        if (p->section(i) == s)
            n++;

    return n;
}

static u16 SectionUnlocked(u8 s)
{
    const struct AchProvider *p = AchActive();
    u16 count = p->count(), n = 0;

    for (u16 i = 0; i < count; i++)
        if (p->section(i) == s && p->unlocked(i))
            n++;

    return n;
}

// The provider index of row k on page s, or NO_ROW after the end.
static u16 SectionIndex(u8 s, u16 k)
{
    const struct AchProvider *p = AchActive();
    u16 count = p->count();

    for (u16 i = 0; i < count; i++)
    {
        if (p->section(i) != s)
            continue;
        if (k == 0)
            return i;
        k--;
    }

    return NO_ROW;
}

// The row on its page of provider index i.
static u16 RowInSection(u16 i)
{
    const struct AchProvider *p = AchActive();
    u8 s = p->section(i);
    u16 k = 0;

    for (u16 j = 0; j < i; j++)
        if (p->section(j) == s)
            k++;

    return k;
}

static u16 MaxScroll(u16 rows)
{
    return rows > VISIBLE_ROWS ? (u16)(rows - VISIBLE_ROWS) : 0;
}

static void ClampScroll(void)
{
    u16 max = MaxScroll(SectionCount(sSection));

    if (sScroll[sSection] > max)
        sScroll[sSection] = max;
}

static void Scroll(int delta)
{
    int max = (int)MaxScroll(SectionCount(sSection));
    int next = (int)sScroll[sSection] + delta;

    if (next < 0)
        next = 0;
    if (next > max)
        next = max;

    if ((u16)next == sScroll[sSection])
        return;

    sScroll[sSection] = (u16)next;
    UiMarkDirty();
}

static bool8 CounterShown(const struct AchView *v)
{
    return !v->unlocked && !v->hidden && v->goal > 1 && v->goal <= COUNTER_MAX;
}

// ---------------------------------------------------------------- drawing --

static int FractionWidth(s32 a, s32 b)
{
    u8 slash[4];

    return UiNumWidth(a) + UiTextWidth(UiAscii(slash, "/", sizeof(slash)))
         + UiNumWidth(b);
}

// "a/b", right-aligned at xRight, as one block, so the pair stays together when
// the digit count changes.
static void DrawFraction(int xRight, int y, s32 a, s32 b, u16 fg)
{
    u8 slash[4];
    int x;

    UiAscii(slash, "/", sizeof(slash));
    x = xRight - UiNumWidth(b);
    UiNum(x, y, b, fg, UiThemeShadow());
    x -= UiTextWidth(slash);
    UiText(x, y, slash, fg, UiThemeShadow());
    x -= UiNumWidth(a);
    UiNum(x, y, a, fg, UiThemeShadow());
}

// A page button: its name and its count, centered as one block. The active
// button gets EXTRA's double inset outline and accent text (DrawButtonH in
// tab_extra.c), because color alone is not clear on the light frames.
static void DrawSectionButton(u8 s)
{
    u8 name[16];
    int x = SEC_X(s), y = SEC_Y;
    bool8 active = (s == sSection);
    u16 fg = active ? UI_COL_ACCENT : UiThemeText();
    s32 got = SectionUnlocked(s), of = SectionCount(s);
    int nameW, total, tx;

    UiRect(x, y, SEC_W, SEC_H, UI_COL_DIM);
    if (active)
    {
        UiRect(x + 2, y + 2, SEC_W - 4, SEC_H - 4, UI_COL_ACCENT);
        UiRect(x + 3, y + 3, SEC_W - 6, SEC_H - 6, UI_COL_ACCENT);
    }

    UiAscii(name, sSectionNames[s], sizeof(name));
    nameW = UiTextWidth(name);
    total = nameW + 6 + FractionWidth(got, of);
    tx = x + (SEC_W - total) / 2;

    UiText(tx, y + (SEC_H - UI_GLYPH_H) / 2, name, fg, UiThemeShadow());
    DrawFraction(tx + total, y + (SEC_H - UI_GLYPH_H) / 2, got, of, fg);
}

static void DrawHeader(void)
{
    u16 got = SectionUnlocked(sSection), of = SectionCount(sSection);
    int fill;

    for (u8 s = 0; s < ACH_SECTION_COUNT; s++)
        DrawSectionButton(s);

    // The shiny notice's gold, so achievements look the same everywhere on this
    // screen. Two tones, like the HP bar, so the fill looks like a bar. It
    // measures the page on the screen, like the list.
    UiFillRect(BAR_X, BAR_Y, BAR_W, BAR_H, UI_COL_HP_BACK);

    fill = of ? (BAR_W * got) / of : 0;
    if (fill > 0)
    {
        UiFillRect(BAR_X, BAR_Y, fill, BAR_H, UI_COL_SHINY);
        UiFillRect(BAR_X, BAR_Y, fill, 1, UI_COL_SHINY_PALE);
    }
}

static void DrawRow(u16 index, int y)
{
    const struct AchProvider *p = AchActive();
    const struct UiRamp *ramp;
    struct AchView v;
    u8 text[64];
    u16 titleFg, titleShadow, descFg;

    p->get(index, &v);
    ramp = UiAchCategoryRamp(v.category);

    if (v.unlocked)
    {
        // The category's own sparkle, and the title in the body color over the
        // dark edge, as in the notice headline. This is legible on every frame.
        // The description stays in the frame's text color, so it stays easy to
        // read.
        UiSparkleRamp(MARK_CX, y + 7, UI_SPARKLE_SIZES - 1,
                      ramp->pale, ramp->body, ramp->edge);
        titleFg = ramp->body;
        titleShadow = ramp->edge;
        descFg = UiThemeText();
    }
    else if (v.hidden)
    {
        // No color. A color would tell what kind of achievement is hidden.
        UiAscii(text, "?", sizeof(text));
        UiText(MARK_CX - UiTextWidth(text) / 2, y, text, UI_COL_DIM, UiThemeShadow());
        titleFg = descFg = UI_COL_DIM;
        titleShadow = UiThemeShadow();
    }
    else
    {
        // Locked: the category color shows on the empty marker, and the text
        // stays dim. The row reads first as "not earned" and then as its
        // category.
        UiRect(MARK_CX - 4, y + 3, 9, 9, ramp->body);
        titleFg = descFg = UI_COL_DIM;
        titleShadow = UiThemeShadow();
    }

    UiText(TEXT_X, y, UiAscii(text, v.title, sizeof(text)), titleFg, titleShadow);
    UiText(TEXT_X, y + DESC_DY, UiAscii(text, v.desc, sizeof(text)),
           descFg, UiThemeShadow());

    if (MaskGet(index))
        UiTextRight(IN_R, y, UiAscii(text, "NEW", sizeof(text)),
                    UI_COL_SHINY, UI_COL_SHINY_EDGE);
    else if (CounterShown(&v))
        DrawFraction(IN_R, y, (s32)v.progress, (s32)v.goal, UI_COL_DIM);
}

static void DrawPager(void)
{
    if (sScroll[sSection] > 0)
    {
        UiRect(PAGE_UP_X, PAGE_Y, PAGE_W, PAGE_H, UI_COL_DIM);
        UiArrow(PAGE_UP_X + (PAGE_W - UI_ARROW_W) / 2,
                PAGE_Y + (PAGE_H - UI_ARROW_H) / 2, TRUE, UI_COL_ACCENT);
    }

    if (sScroll[sSection] < MaxScroll(SectionCount(sSection)))
    {
        UiRect(PAGE_DN_X, PAGE_Y, PAGE_W, PAGE_H, UI_COL_DIM);
        UiArrow(PAGE_DN_X + (PAGE_W - UI_ARROW_W) / 2,
                PAGE_Y + (PAGE_H - UI_ARROW_H) / 2, FALSE, UI_COL_ACCENT);
    }
}

void UiTrophyDraw(void)
{
    ClampScroll();

    UiWindowFrame(0, 0, TROPHY_TW, TROPHY_TH);
    DrawHeader();

    for (u16 r = 0; r < VISIBLE_ROWS; r++)
    {
        u16 index = SectionIndex(sSection, sScroll[sSection] + r);

        if (index == NO_ROW)
            break;

        DrawRow(index, LIST_Y + (int)r * ROW_H);
    }

    DrawPager();
}

// ------------------------------------------------------------------ touch --

void UiTrophyTouch(const CtrTouchState *t)
{
    // Before the justReleased guard, so a held arrow scrolls the list, one page
    // at a time. The MAIN list has fifteen pages, which a hold crosses in about
    // a second. An arrow that does not show is at the end of the list, where
    // Scroll() does nothing.
    if (UiHoldRepeat(&sHoldUp, t, PAGE_UP_X, PAGE_Y, PAGE_W, PAGE_H))
    {
        Scroll(-VISIBLE_ROWS);
        return;
    }

    if (UiHoldRepeat(&sHoldDn, t, PAGE_DN_X, PAGE_Y, PAGE_W, PAGE_H))
    {
        Scroll(VISIBLE_ROWS);
        return;
    }

    if (!t->justReleased)
        return;

    for (u8 s = 0; s < ACH_SECTION_COUNT; s++)
    {
        if (UiHit(t, SEC_X(s), SEC_Y, SEC_W, SEC_H))
        {
            if (s != sSection)
            {
                sSection = s;
                UiMarkDirty();
            }
            return;
        }
    }
}

// ------------------------------------------------------------------ shell --

// Copy the provider's unseen bits into the NEW mask, then mark them seen.
// Returns the first unseen index, or NO_ROW. Providers list MAIN before
// POST-GAME, so the first index prefers the main page.
static u16 AdoptUnseen(void)
{
    const struct AchProvider *p = AchActive();
    u16 count = p->count(), first = NO_ROW;

    for (u16 i = 0; i < count; i++)
    {
        if (!p->unseen(i))
            continue;

        MaskSet(i);
        if (first == NO_ROW)
            first = i;
    }

    p->markAllSeen();
    return first;
}

void UiTrophyTick(bool8 visible)
{
    if (!visible)
    {
        // The visit ends, and its NEW tags go.
        if (sVisible)
        {
            for (u32 b = 0; b < CTR_ACH_BYTES; b++)
                sNewMask[b] = 0;
            sVisible = FALSE;
        }
        return;
    }

    if (!sVisible)
    {
        u16 first = AdoptUnseen();

        sVisible = TRUE;

        // Open on the latest unlock, on its page, not where the list was. Thus
        // VIEW on the toast shows the achievement that it announced.
        if (first != NO_ROW)
        {
            sSection = AchActive()->section(first);
            sScroll[sSection] = RowInSection(first);
            ClampScroll();
        }

        UiMarkDirty();
    }
    // An unlock while the tab shows: tag it, and mark it seen, so the tab bar
    // dot does not appear for something on the screen.
    else if (AchActive()->anyUnseen())
    {
        AdoptUnseen();
        UiMarkDirty();
    }
}

// The page, its scroll position, and the counters of the visible rows. The
// counters change without a touch on this tab (a catch, a hatch, a trainer
// battle). Unlocks and the post-game reveal are the shell's top[8]. The NEW
// tags, the page and the scroll change only through code that marks the screen
// dirty.
u32 UiTrophyStateKey(void)
{
    const struct AchProvider *p = AchActive();
    u32 key = sSection | ((u32)sScroll[sSection] << 1);

    for (u16 r = 0; r < VISIBLE_ROWS; r++)
    {
        u16 index = SectionIndex(sSection, sScroll[sSection] + r);
        struct AchView v;

        if (index == NO_ROW)
            break;

        p->get(index, &v);
        if (CounterShown(&v))
            key = key * 31u + v.progress;
    }

    return key;
}

// Check the real text of every achievement, hidden or not, because the
// post-game page must fit after the reveal. Also check each row as it is now,
// which covers the placeholder text of hidden rows.
u16 UiTrophyTooWide(void)
{
    const struct AchProvider *p = AchActive();
    u16 count = p->count(), n = 0;
    u8 text[64];

    for (u16 i = 0; i < count; i++)
    {
        const char *title, *desc;
        struct AchView v;

        AchDebugRealText(i, &title, &desc);
        p->get(i, &v);

        if (UiTextWidth(UiAscii(text, title, sizeof(text))) > TROPHY_TITLE_MAX_W
            || UiTextWidth(UiAscii(text, desc, sizeof(text))) > TROPHY_DESC_MAX_W
            || UiTextWidth(UiAscii(text, v.title, sizeof(text))) > TROPHY_TITLE_MAX_W
            || UiTextWidth(UiAscii(text, v.desc, sizeof(text))) > TROPHY_DESC_MAX_W)
            n++;
    }

    return n;
}
