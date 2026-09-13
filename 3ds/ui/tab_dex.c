// DEX tab: the game's own Pokedex, on the bottom screen.
//
// It shows only after FLAG_SYS_POKEDEX_GET is set, as the start menu does
// (BuildNormalStartMenu). All data and art come from the game, so it agrees
// with the game's Pokedex. Nothing here writes game state: GetSetPokedexFlag
// gets only the FLAG_GET_* cases.
//
// The layout follows the real dex. The selected mon is on the left and the list
// is on the right. The entry screen shows the sprite, footprint, category,
// height, weight and description. Height and weight use the imperial format.

#include "global.h"
#include "pokedex.h"
#include "pokemon.h"
#include "data.h"
#include "event_data.h"
#include "strings.h"
#include "international_string_util.h"   // CopyMonCategoryText
#include "constants/flags.h"
#include "constants/pokedex.h"
#include "constants/species.h"
#include "constants/characters.h"

#include "../bridge.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"

// Defined in the game's data, and only declared extern here (as in
// src/international_string_util.c).
extern const struct PokedexEntry gPokedexEntries[];

// ---------------------------------------------------------------- layout ---
//
// 14 + 26 tiles fill the 40-tile width. The left pane holds a 64px sprite in
// its 8px frame. 14 tiles give 96px of interior, which is enough.
#define LEFT_TW        14
#define RIGHT_TX       LEFT_TW
#define RIGHT_TW       ((CTR_BOTTOM_WIDTH / 8) - LEFT_TW)
#define PANEL_TH       (UI_CONTENT_H / 8)

#define PIC_X          24
#define PIC_Y          16
#define SEEN_Y         112
#define OWN_Y          136

// The interior of the left pane, where the sprite is centered:
// PIC_X is 8 + (96 - 64) / 2. The counts are centered in the same span, so the
// column is one centered stack.
#define LEFT_IN_X      8
#define LEFT_IN_W      (LEFT_TW * 8 - 16)   // 96
#define COUNT_GAP      10

#define LIST_X         (RIGHT_TX * 8)          // 112
#define LIST_Y         14
#define ROW_H          24
#define VISIBLE_ROWS   6
// A cursor column at the interior edge of the pane, then the other columns.
// Every row keeps the gap, so the list does not move when the cursor moves.
#define CURSOR_X       (LIST_X + 8)
#define BALL_X         (LIST_X + 20)
#define NUM_X          (LIST_X + 32)
#define NAME_X         (LIST_X + 70)

#define PAGE_Y         158
#define PAGE_W         52
#define PAGE_H         22
#define PAGE_UP_X      (LIST_X + 30)
#define PAGE_DN_X      (LIST_X + 120)

// Hold X or Y to make an arrow jump. Do not use a GBA button. The game keeps
// running on the top screen, so a held GBA button also goes to the game. With
// the L=A option, L is an A press (src/main.c). X and Y are not mapped, so the
// game never sees them.
#define JUMP_ROWS      5

// The entry screen.
#define E_PIC_X        24
#define E_PIC_Y        28
#define E_FOOT_X       48
#define E_FOOT_Y       98
#define E_TEXT_X       104
#define E_VALUE_X      144
#define E_NUM_Y        20
#define E_NAME_X       152
#define E_CAT_Y        42
#define E_HT_Y         62
#define E_WT_Y         82
#define E_DESC_Y       120
#define BACK_X         (CTR_BOTTOM_WIDTH - 50)
#define BACK_Y         8
#define BACK_W         42
#define BACK_H         22

static u16   sScroll;
static u16   sCursor;          // the row index in the current dex order
static bool8 sEntryOpen;
// One counter for each arrow, so a held arrow scrolls the list. The dex has 386
// rows, which is why UiHoldRepeat exists.
static UiHold sHoldUp, sHoldDn;

static bool8 NationalMode(void)
{
    return gSaveBlock2Ptr->pokedex.mode == DEX_MODE_NATIONAL
        && FlagGet(FLAG_SYS_NATIONAL_DEX);
}

static u16 DexLength(void)
{
    return NationalMode() ? NATIONAL_DEX_COUNT : HOENN_DEX_COUNT;
}

// Row n of the list is dex entry n+1, in the order that the player's Pokedex
// uses. Returns the national number, which all accessors below use.
static u16 RowToNationalNum(u16 row)
{
    u16 n = row + 1;

    return NationalMode() ? n : HoennToNationalOrder(n);
}

// ------------------------------------------------------ height and weight --
//
// PrintMonHeight and PrintMonWeight (src/pokedex.c) are static, so this copies
// their arithmetic and rounding. Bulbasaur must show 2'04" and 15.2 lbs.
static void FormatHeight(u8 *dst, u16 height)
{
    u32 inches = (height * 10000) / 254;
    u32 feet;
    int i = 0;

    if (inches % 10 >= 5)
        inches += 10;

    feet = inches / 120;
    inches = (inches - feet * 120) / 10;

    if (feet >= 10)
        dst[i++] = CHAR_0 + feet / 10;
    dst[i++] = CHAR_0 + feet % 10;
    dst[i++] = CHAR_SGL_QUOTE_RIGHT;
    dst[i++] = CHAR_0 + inches / 10;
    dst[i++] = CHAR_0 + inches % 10;
    dst[i++] = CHAR_DBL_QUOTE_RIGHT;
    dst[i] = EOS;
}

static void FormatWeight(u8 *dst, u16 weight)
{
    u32 lbs = (weight * 100000) / 4536;
    bool8 leading = FALSE;
    int i = 0;

    if (lbs % 10 >= 5)
        lbs += 10;

    // The game pads with CHAR_SPACER, so the decimal point stays in the same
    // column in all entries.
    for (u32 div = 100000; div >= 1000; div /= 10)
    {
        u32 digit = (lbs / div) % 10;

        if (digit == 0 && !leading)
        {
            dst[i++] = CHAR_SPACER;
        }
        else
        {
            leading = TRUE;
            dst[i++] = CHAR_0 + digit;
        }
    }

    dst[i++] = CHAR_0 + (lbs / 100) % 10;
    dst[i++] = CHAR_PERIOD;
    dst[i++] = CHAR_0 + (lbs / 10) % 10;
    dst[i++] = CHAR_SPACE;
    dst[i++] = CHAR_l;
    dst[i++] = CHAR_b;
    dst[i++] = CHAR_s;
    dst[i++] = CHAR_PERIOD;
    dst[i] = EOS;
}

// "No" and three zero-padded digits, as in CreateMonDexNum (src/pokedex.c).
// gText_NumberClear01 is the game's prefix. It has an extra symbol and a
// control code, which UiText can handle.
static void FormatDexNum(u8 *dst, u16 num)
{
    int i = 0;

    while (gText_NumberClear01[i] != EOS)
    {
        dst[i] = gText_NumberClear01[i];
        i++;
    }

    dst[i++] = CHAR_0 + num / 100;
    dst[i++] = CHAR_0 + (num / 10) % 10;
    dst[i++] = CHAR_0 + num % 10;
    dst[i] = EOS;
}

// ------------------------------------------------------------ list screen --

// Move the cursor, and move the visible window only as much as necessary. The
// selected mon and the sprite in the left pane must stay in the list.
static void MoveCursor(int delta)
{
    u16 len = DexLength();
    int next;

    if (len == 0)
        return;

    next = (int)sCursor + delta;
    if (next < 0)
        next = 0;
    if (next >= (int)len)
        next = (int)len - 1;

    if ((u16)next == sCursor)
        return;

    sCursor = (u16)next;

    if (sCursor < sScroll)
        sScroll = sCursor;
    else if (sCursor >= sScroll + VISIBLE_ROWS)
        sScroll = (u16)(sCursor - VISIBLE_ROWS + 1);

    UiMarkDirty();
}

// One row, or JUMP_ROWS while the modifier is held.
static int CursorStep(void)
{
    return Ctr3dsUiModifierHeld() ? JUMP_ROWS : 1;
}

static void DrawSelectedPane(void)
{
    u16 national = RowToNationalNum(sCursor);
    u16 seen, caught;

    UiWindowFrame(0, 0, LEFT_TW, PANEL_TH);

    // Only a mon that the player has met gets a picture, as in the real dex.
    if (GetSetPokedexFlag(national, FLAG_GET_SEEN))
        UiMonPic(PIC_X, PIC_Y, NationalPokedexNumToSpecies(national));

    if (NationalMode())
    {
        seen   = GetNationalPokedexCount(FLAG_GET_SEEN);
        caught = GetNationalPokedexCount(FLAG_GET_CAUGHT);
    }
    else
    {
        seen   = GetHoennPokedexCount(FLAG_GET_SEEN);
        caught = GetHoennPokedexCount(FLAG_GET_CAUGHT);
    }

    // One block for both rows. The labels and the counts have different widths,
    // so two separately centered rows look ragged. Use the widest of each
    // column: the rows then align, and the block is centered under the sprite.
    {
        u8 seenLabel[8], ownLabel[8];
        int labelW, numW, total, x;

        UiAscii(seenLabel, "SEEN", sizeof(seenLabel));
        UiAscii(ownLabel,  "OWN",  sizeof(ownLabel));

        labelW = UiTextWidth(seenLabel);
        if (UiTextWidth(ownLabel) > labelW)
            labelW = UiTextWidth(ownLabel);

        numW = UiNumWidth((s32)seen);
        if (UiNumWidth((s32)caught) > numW)
            numW = UiNumWidth((s32)caught);

        total = labelW + COUNT_GAP + numW;
        x = LEFT_IN_X + (LEFT_IN_W - total) / 2;

        UiText(x, SEEN_Y, seenLabel, UI_COL_DIM, UiThemeShadow());
        UiNumRight(x + total, SEEN_Y, (s32)seen, UiThemeText(), UiThemeShadow());

        UiText(x, OWN_Y, ownLabel, UI_COL_DIM, UiThemeShadow());
        UiNumRight(x + total, OWN_Y, (s32)caught, UiThemeText(), UiThemeShadow());
    }
}

static void DrawList(void)
{
    u16 len = DexLength();
    u8 label[16];

    UiWindowFrame(RIGHT_TX, 0, RIGHT_TW, PANEL_TH);

    for (u32 row = 0; row < VISIBLE_ROWS; row++)
    {
        u16 index = sScroll + (u16)row;
        int y = LIST_Y + (int)row * ROW_H;
        u16 national;

        if (index >= len)
            break;

        national = RowToNationalNum(index);

        if (index == sCursor)
            UiChevron(CURSOR_X, y + (UI_GLYPH_H - UI_CHEVRON_H) / 2);

        // A ball for caught, nothing for seen, as in the real list.
        if (GetSetPokedexFlag(national, FLAG_GET_CAUGHT))
            UiPokeball(BALL_X, y + 4);

        // The number is the row's position in every mode, because
        // RowToNationalNum already applied the order.
        FormatDexNum(label, (u16)(index + 1));
        UiText(NUM_X, y, label, UI_COL_DIM, UiThemeShadow());

        if (GetSetPokedexFlag(national, FLAG_GET_SEEN))
            UiText(NAME_X, y, gSpeciesNames[NationalPokedexNumToSpecies(national)],
                   UiThemeText(), UiThemeShadow());
        else
            UiText(NAME_X, y, UiAscii(label, "----------", sizeof(label)),
                   UI_COL_DIM, UiThemeShadow());
    }

    // The arrows follow the cursor, not the scroll position. They stay live
    // until the selection is at an end.
    if (sCursor > 0)
    {
        UiRect(PAGE_UP_X, PAGE_Y, PAGE_W, PAGE_H, UI_COL_DIM);
        UiArrow(PAGE_UP_X + (PAGE_W - UI_ARROW_W) / 2,
                PAGE_Y + (PAGE_H - UI_ARROW_H) / 2, TRUE, UI_COL_ACCENT);
    }

    if (len > 0 && sCursor < len - 1)
    {
        UiRect(PAGE_DN_X, PAGE_Y, PAGE_W, PAGE_H, UI_COL_DIM);
        UiArrow(PAGE_DN_X + (PAGE_W - UI_ARROW_W) / 2,
                PAGE_Y + (PAGE_H - UI_ARROW_H) / 2, FALSE, UI_COL_ACCENT);
    }
}

// ----------------------------------------------------------- entry screen --

static void DrawEntry(void)
{
    u16 national = RowToNationalNum(sCursor);
    u16 species = NationalPokedexNumToSpecies(national);
    const struct PokedexEntry *entry = &gPokedexEntries[national];
    bool8 owned = GetSetPokedexFlag(national, FLAG_GET_CAUGHT) != 0;
    const u8 *description;
    u8 buf[40];

    UiWindowFrame(0, 0, CTR_BOTTOM_WIDTH / 8, PANEL_TH);

    UiMonPic(E_PIC_X, E_PIC_Y, species);
    UiFootprint(E_FOOT_X, E_FOOT_Y, species, UiThemeText());

    FormatDexNum(buf, (u16)(sCursor + 1));
    UiText(E_TEXT_X, E_NUM_Y, buf, UI_COL_DIM, UiThemeShadow());
    UiText(E_NAME_X, E_NUM_Y, gSpeciesNames[species], UiThemeText(), UiThemeShadow());

    // Category, height and weight show only after the mon is caught, as in
    // PrintMonInfo (src/pokedex.c).
    if (owned)
    {
        CopyMonCategoryText(national, buf);
        UiText(E_TEXT_X, E_CAT_Y, buf, UI_COL_DIM, UiThemeShadow());
    }
    else
    {
        UiText(E_TEXT_X, E_CAT_Y, gText_5MarksPokemon, UI_COL_DIM, UiThemeShadow());
    }

    UiText(E_TEXT_X, E_HT_Y, gText_HTHeight, UI_COL_DIM, UiThemeShadow());
    UiText(E_TEXT_X, E_WT_Y, gText_WTWeight, UI_COL_DIM, UiThemeShadow());

    if (owned)
    {
        FormatHeight(buf, entry->height);
        UiText(E_VALUE_X, E_HT_Y, buf, UiThemeText(), UiThemeShadow());
        FormatWeight(buf, entry->weight);
        UiText(E_VALUE_X, E_WT_Y, buf, UiThemeText(), UiThemeShadow());
    }
    else
    {
        UiText(E_VALUE_X, E_HT_Y, gText_UnkHeight, UiThemeText(), UiThemeShadow());
        UiText(E_VALUE_X, E_WT_Y, gText_UnkWeight, UiThemeText(), UiThemeShadow());
    }

    // Center the text as a block, not by line, as the game does.
    // GetStringCenterAlignXOffset uses the widest line, and so does
    // UiTextWidth.
    description = entry->description;
    if (description != NULL)
        UiText(8 + (CTR_BOTTOM_WIDTH - 16 - UiTextWidth(description)) / 2,
               E_DESC_Y, description, UiThemeText(), UiThemeShadow());

    UiRect(BACK_X, BACK_Y, BACK_W, BACK_H, UI_COL_DIM);
    UiAscii(buf, "BACK", sizeof(buf));
    UiText(BACK_X + (BACK_W - UiTextWidth(buf)) / 2, BACK_Y + 3, buf,
           UI_COL_ACCENT, UiThemeShadow());
}

// ------------------------------------------------------------------ shell --

// The seen and caught counts can change without a party change (a wild mon is
// seen), so the shell needs this key. The count walks the full dex, so use it
// only while this tab is on the screen.
u32 UiDexStateKey(void)
{
    if (NationalMode())
        return ((u32)GetNationalPokedexCount(FLAG_GET_SEEN) << 16)
             |  (u32)GetNationalPokedexCount(FLAG_GET_CAUGHT);

    return ((u32)GetHoennPokedexCount(FLAG_GET_SEEN) << 16)
         |  (u32)GetHoennPokedexCount(FLAG_GET_CAUGHT);
}

void UiDexDraw(void)
{
    // The mode can change while this tab is open (the National Dex arrives).
    // The list under the cursor then becomes shorter or longer.
    u16 len = DexLength();

    if (sCursor >= len)
        sCursor = len ? (u16)(len - 1) : 0;
    if (sScroll + VISIBLE_ROWS > len)
        sScroll = (len > VISIBLE_ROWS) ? (u16)(len - VISIBLE_ROWS) : 0;

    // Two separate clamps can put the cursor outside the window. The left pane
    // then shows a mon that is not in the list. Apply the same rule as
    // MoveCursor.
    if (sCursor < sScroll)
        sScroll = sCursor;
    else if (sCursor >= sScroll + VISIBLE_ROWS)
        sScroll = (u16)(sCursor - VISIBLE_ROWS + 1);

    if (sEntryOpen)
    {
        DrawEntry();
        return;
    }

    DrawSelectedPane();
    DrawList();
}

void UiDexTouch(const CtrTouchState *t)
{
    u16 len;

    if (sEntryOpen)
    {
        if (t->justReleased && UiHit(t, BACK_X, BACK_Y, BACK_W, BACK_H))
        {
            sEntryOpen = FALSE;
            UiMarkDirty();
        }
        return;
    }

    // Test both arrows before the justReleased guard below, because a held
    // arrow acts on frames with no release. A plain tap still acts once, on
    // release. With the jump modifier held, each repeat moves JUMP_ROWS, which
    // makes the end of the national dex reachable.
    if (UiHoldRepeat(&sHoldUp, t, PAGE_UP_X, PAGE_Y, PAGE_W, PAGE_H))
    {
        MoveCursor(-CursorStep());
        return;
    }

    if (UiHoldRepeat(&sHoldDn, t, PAGE_DN_X, PAGE_Y, PAGE_W, PAGE_H))
    {
        MoveCursor(CursorStep());
        return;
    }

    if (!t->justReleased)
        return;

    len = DexLength();

    // The first tap moves the cursor and shows the mon. A second tap on the
    // same row opens the entry, like the cursor and A in the real dex, and like
    // the BAG tab.
    if (t->x >= LIST_X && t->y >= LIST_Y && t->y < LIST_Y + VISIBLE_ROWS * ROW_H)
    {
        u16 index = sScroll + (u16)((t->y - LIST_Y) / ROW_H);

        if (index >= len)
            return;

        if (index == sCursor)
        {
            // Nothing to show for an entry that the player has not seen.
            if (!GetSetPokedexFlag(RowToNationalNum(index), FLAG_GET_SEEN))
                return;

            sEntryOpen = TRUE;
        }
        else
        {
            sCursor = index;
        }

        UiMarkDirty();
    }
}
