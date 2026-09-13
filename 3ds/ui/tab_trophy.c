// TROPHY tab: the achievements list.
//
// Always available, like BAG and EXTRA: there is nothing to unlock before the
// list itself is worth reading, and a locked list is a list of goals.
//
// It reads everything through AchActive() (3ds/achievements.h) and nothing
// else, so it does not know or care which provider is behind it. Nothing here
// writes game state; the one thing it changes is the provider's "seen" bits.
//
// One window over the whole content area: a header with the count and a gold
// progress bar, four two-line rows, and the DEX tab's paging arrows underneath.

#include "global.h"

#include "../bridge.h"
#include "../achievements.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"

// ---------------------------------------------------------------- layout ---
//
// 40x24 tiles, interior x 8..312, y 8..184. Text keeps an 8px margin inside
// that, so it runs from x 16 to x 304.
#define TROPHY_TW      (CTR_BOTTOM_WIDTH / 8)
#define TROPHY_TH      (UI_CONTENT_H / 8)

#define IN_L           16
#define IN_R           (CTR_BOTTOM_WIDTH - 16)          // 304

// The header line, then a 4px bar under it: 8..23 for the text, 26..30 for the
// bar, which leaves the first row a clear 3px.
#define HEAD_Y         8
#define BAR_X          IN_L
#define BAR_Y          26
#define BAR_W          (IN_R - IN_L)                    // 288
#define BAR_H          4

// Four rows of two lines, 31px apart: 33, 64, 95 and 126, the last ending at
// 157, which clears the paging arrows at 160. A fifth would need 186.
#define LIST_Y         33
#define ROW_H          31
#define VISIBLE_ROWS   4
#define DESC_DY        15

// The marker column, then the text. The marker is the big sparkle frame for an
// unlocked row (16x14 around its axis) and a small hollow square for a locked
// one, both centred on MARK_CX.
#define MARK_CX        (IN_L + 8)                       // 24
#define TEXT_X         (IN_L + 20)                      // 36

// The right end of the title line carries either a counter ("37/50") while
// locked or a NEW tag once unlocked, never both, so they share one column.
#define TAG_COL_W      56

// Exported limits for the achievement text, checked on the debug page because
// there is no clipping on this screen (SECOND_SCREEN_CHEATSHEET.md section 9).
#define TROPHY_TITLE_MAX_W  (IN_R - TAG_COL_W - TEXT_X)  // 212
#define TROPHY_DESC_MAX_W   (IN_R - TEXT_X)              // 268

// A counter is drawn only for goals short enough to be worth counting towards.
// A 100,000-step goal would print a number that moves on every step, and the
// state key below would repaint the tab for each one.
#define COUNTER_MAX    999

// The DEX tab's arrows, at the same size, centred under the list. 160 + 22
// ends at 182, inside the 184 floor.
#define PAGE_Y         160
#define PAGE_W         52
#define PAGE_H         22
#define PAGE_UP_X      (CTR_BOTTOM_WIDTH / 2 - 12 - PAGE_W)   // 96
#define PAGE_DN_X      (CTR_BOTTOM_WIDTH / 2 + 12)            // 172

// The NEW mask is a bitset over the first MAX_TRACKED entries, which is what the
// host store can hold anyway (bridge.h).
#define MAX_TRACKED    (CTR_ACH_BYTES * 8)

// ----------------------------------------------------------------- state ---

static u16   sScroll;              // first visible row
static UiHold sHoldUp, sHoldDn;

// What was unseen when the tab came on screen, plus anything unlocked while it
// stayed there. The provider's own unseen bits are cleared the moment the tab
// shows (markAllSeen), so the tags have to live here: they last the visit,
// and a visit ends when the tab is left.
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

static u16 MaxScroll(u16 count)
{
    return count > VISIBLE_ROWS ? (u16)(count - VISIBLE_ROWS) : 0;
}

static void ClampScroll(u16 count)
{
    if (sScroll > MaxScroll(count))
        sScroll = MaxScroll(count);
}

static void Scroll(int delta)
{
    u16 count = AchActive()->count();
    int next = (int)sScroll + delta;

    if (next < 0)
        next = 0;
    if (next > (int)MaxScroll(count))
        next = (int)MaxScroll(count);

    if ((u16)next == sScroll)
        return;

    sScroll = (u16)next;
    UiMarkDirty();
}

static bool8 CounterShown(const struct AchView *v)
{
    return !v->unlocked && v->goal > 1 && v->goal <= COUNTER_MAX;
}

// ---------------------------------------------------------------- drawing --

// "a/b", right-aligned at xRight, as one block so the pair stays together as
// the numbers change width.
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

static void DrawHeader(u16 unlocked, u16 count)
{
    u8 label[24];
    int fill;

    UiText(IN_L, HEAD_Y, UiAscii(label, "ACHIEVEMENTS", sizeof(label)),
           UiThemeText(), UiThemeShadow());
    DrawFraction(IN_R, HEAD_Y, unlocked, count, UiThemeText());

    // The shiny notice's gold, so an achievement reads as the same kind of
    // thing on every part of this screen. Two tones for the same reason the HP
    // bar has two: a flat fill reads as a block, a highlight reads as a bar.
    UiFillRect(BAR_X, BAR_Y, BAR_W, BAR_H, UI_COL_HP_BACK);

    fill = count ? (BAR_W * unlocked) / count : 0;
    if (fill > 0)
    {
        UiFillRect(BAR_X, BAR_Y, fill, BAR_H, UI_COL_SHINY);
        UiFillRect(BAR_X, BAR_Y, fill, 1, UI_COL_SHINY_PALE);
    }
}

static void DrawRow(u16 index, int y)
{
    const struct AchProvider *p = AchActive();
    struct AchView v;
    u8 text[64];
    u16 fg;

    p->get(index, &v);

    fg = v.unlocked ? UiThemeText() : UI_COL_DIM;

    if (v.unlocked)
        UiSparkle(MARK_CX, y + 7, UI_SPARKLE_SIZES - 1);
    else
        UiRect(MARK_CX - 4, y + 3, 9, 9, UI_COL_DIM);

    UiText(TEXT_X, y, UiAscii(text, v.title, sizeof(text)), fg, UiThemeShadow());
    UiText(TEXT_X, y + DESC_DY, UiAscii(text, v.desc, sizeof(text)),
           fg, UiThemeShadow());

    if (MaskGet(index))
        UiTextRight(IN_R, y, UiAscii(text, "NEW", sizeof(text)),
                    UI_COL_SHINY, UI_COL_SHINY_EDGE);
    else if (CounterShown(&v))
        DrawFraction(IN_R, y, (s32)v.progress, (s32)v.goal, UI_COL_DIM);
}

static void DrawPager(u16 count)
{
    if (sScroll > 0)
    {
        UiRect(PAGE_UP_X, PAGE_Y, PAGE_W, PAGE_H, UI_COL_DIM);
        UiArrow(PAGE_UP_X + (PAGE_W - UI_ARROW_W) / 2,
                PAGE_Y + (PAGE_H - UI_ARROW_H) / 2, TRUE, UI_COL_ACCENT);
    }

    if (sScroll < MaxScroll(count))
    {
        UiRect(PAGE_DN_X, PAGE_Y, PAGE_W, PAGE_H, UI_COL_DIM);
        UiArrow(PAGE_DN_X + (PAGE_W - UI_ARROW_W) / 2,
                PAGE_Y + (PAGE_H - UI_ARROW_H) / 2, FALSE, UI_COL_ACCENT);
    }
}

void UiTrophyDraw(void)
{
    const struct AchProvider *p = AchActive();
    u16 count = p->count();

    ClampScroll(count);

    UiWindowFrame(0, 0, TROPHY_TW, TROPHY_TH);
    DrawHeader(p->unlockedCount(), count);

    for (u16 r = 0; r < VISIBLE_ROWS && sScroll + r < count; r++)
        DrawRow(sScroll + r, LIST_Y + (int)r * ROW_H);

    DrawPager(count);
}

// ------------------------------------------------------------------ touch --

void UiTrophyTouch(const CtrTouchState *t)
{
    // Ahead of any justReleased guard, so a held arrow runs the list. A page
    // at a time: 48 rows is twelve pages, which a hold crosses in a second.
    // An arrow that is not drawn is at the end of the list, where Scroll()
    // clamps to no change and asks for nothing.
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
}

// ------------------------------------------------------------------ shell --

// Copy the provider's unseen bits into the NEW mask, then mark them seen.
// Returns the first index that was unseen, or 0xFFFF if none was.
static u16 AdoptUnseen(void)
{
    const struct AchProvider *p = AchActive();
    u16 count = p->count(), first = 0xFFFF;

    for (u16 i = 0; i < count; i++)
    {
        struct AchView v;

        p->get(i, &v);
        if (!v.unseen)
            continue;

        MaskSet(i);
        if (first == 0xFFFF)
            first = i;
    }

    p->markAllSeen();
    return first;
}

void UiTrophyTick(bool8 visible)
{
    if (!visible)
    {
        // The visit is over, and so are its NEW tags.
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

        // Open on what was just unlocked, rather than on wherever the list was
        // left, so VIEW on the toast lands on the thing it announced.
        if (first != 0xFFFF)
        {
            sScroll = first;
            ClampScroll(AchActive()->count());
        }

        UiMarkDirty();
    }
    // Unlocked while the tab was already showing: tag it too, and mark it seen
    // so the tab bar's dot does not appear for something on screen.
    else if (AchActive()->anyUnseen())
    {
        AdoptUnseen();
        UiMarkDirty();
    }
}

// The visible rows' counters, which move with no touch on this tab (a catch, a
// hatch, a trainer battle). Unlocks are the shell's own top[9], and the NEW
// tags and the scroll position only change through code that marks dirty.
u32 UiTrophyStateKey(void)
{
    const struct AchProvider *p = AchActive();
    u16 count = p->count();
    u32 key = sScroll;

    for (u16 r = 0; r < VISIBLE_ROWS && sScroll + r < count; r++)
    {
        struct AchView v;

        p->get(sScroll + r, &v);
        if (CounterShown(&v))
            key = key * 31u + v.progress;
    }

    return key;
}

u16 UiTrophyTooWide(void)
{
    const struct AchProvider *p = AchActive();
    u16 count = p->count(), n = 0;
    u8 text[64];

    for (u16 i = 0; i < count; i++)
    {
        struct AchView v;

        p->get(i, &v);
        if (UiTextWidth(UiAscii(text, v.title, sizeof(text))) > TROPHY_TITLE_MAX_W
            || UiTextWidth(UiAscii(text, v.desc, sizeof(text))) > TROPHY_DESC_MAX_W)
            n++;
    }

    return n;
}
