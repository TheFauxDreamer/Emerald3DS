// DEX tab: Emerald's own Pokedex, on the bottom screen.
//
// Reachable only once FLAG_SYS_POKEDEX_GET is set, mirroring the start menu
// (src/start_menu.c BuildNormalStartMenu). Everything shown comes from the
// game's own accessors and its own art, so it agrees with Emerald's Pokedex by
// construction. Nothing here writes game state: GetSetPokedexFlag is only ever
// called with the FLAG_GET_* cases.
//
// The layout follows the real dex: the selected mon on the left, the scrolling
// list on the right, and an entry screen with the sprite, footprint, category,
// height, weight and description. Height and weight are the game's imperial
// format, not the raw decimetres and hectograms in the table.

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

// Defined in the game's data; only ever declared extern (see
// src/international_string_util.c, which does the same).
extern const struct PokedexEntry gPokedexEntries[];

// ---------------------------------------------------------------- layout ---
//
// 14 + 26 tiles fills the 40-tile width. The left pane has to hold a 64px
// sprite inside its 8px frame, which 14 tiles (96px of interior) does with
// room to spare.
#define LEFT_TW        14
#define RIGHT_TX       LEFT_TW
#define RIGHT_TW       ((CTR_BOTTOM_WIDTH / 8) - LEFT_TW)
#define PANEL_TH       (UI_CONTENT_H / 8)

#define PIC_X          24
#define PIC_Y          16
#define SEEN_Y         112
#define OWN_Y          136
#define COUNT_RIGHT    104

#define LIST_X         (RIGHT_TX * 8)          // 112
#define LIST_Y         14
#define ROW_H          24
#define VISIBLE_ROWS   6
#define BALL_X         (LIST_X + 10)
#define NUM_X          (LIST_X + 22)
#define NAME_X         (LIST_X + 60)

#define PAGE_Y         158
#define PAGE_W         52
#define PAGE_H         22
#define PAGE_UP_X      (LIST_X + 30)
#define PAGE_DN_X      (LIST_X + 120)

// Entry screen.
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
static u16   sCursor;          // row index into the current dex order
static bool8 sEntryOpen;

static bool8 NationalMode(void)
{
    return gSaveBlock2Ptr->pokedex.mode == DEX_MODE_NATIONAL
        && FlagGet(FLAG_SYS_NATIONAL_DEX);
}

static u16 DexLength(void)
{
    return NationalMode() ? NATIONAL_DEX_COUNT : HOENN_DEX_COUNT;
}

// Row n of the list is dex entry n+1, in whichever order the player's Pokedex
// is currently set to. Returns the NATIONAL number, which is what every
// accessor below is keyed on.
static u16 RowToNationalNum(u16 row)
{
    u16 n = row + 1;

    return NationalMode() ? n : HoennToNationalOrder(n);
}

// ------------------------------------------------------ height and weight --
//
// PrintMonHeight and PrintMonWeight (src/pokedex.c:4154) are static, so this is
// their arithmetic reproduced rather than called, rounding included. Getting
// the rounding wrong is the failure that shows: Bulbasaur must read 2'04" and
// 15.2 lbs.
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

    // The game pads with CHAR_SPACER rather than suppressing, so the decimal
    // point stays in the same column down a list of entries.
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

// "No" followed by three zero-padded digits, the way CreateMonDexNum builds it
// (src/pokedex.c:2429). gText_NumberClear01 is the game's own prefix and
// carries an extra symbol plus a control code, which UiText now handles.
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

static void DrawSelectedPane(void)
{
    u16 national = RowToNationalNum(sCursor);
    u8 label[12];
    u16 seen, caught;

    UiWindowFrame(0, 0, LEFT_TW, PANEL_TH);

    // Only a mon the player has actually met gets a picture, which is what the
    // real dex does.
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

    UiText(16, SEEN_Y, UiAscii(label, "SEEN", sizeof(label)),
           UI_COL_DIM, UiThemeShadow());
    UiNumRight(COUNT_RIGHT, SEEN_Y, (s32)seen, UiThemeText(), UiThemeShadow());

    UiText(16, OWN_Y, UiAscii(label, "OWN", sizeof(label)),
           UI_COL_DIM, UiThemeShadow());
    UiNumRight(COUNT_RIGHT, OWN_Y, (s32)caught, UiThemeText(), UiThemeShadow());
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
            UiRect(LIST_X + 8, y - 3, RIGHT_TW * 8 - 24, ROW_H - 2, UI_COL_ACCENT);

        // A ball only for caught, nothing for merely seen: the same three-state
        // readout the real list gives.
        if (GetSetPokedexFlag(national, FLAG_GET_CAUGHT))
            UiPokeball(BALL_X, y + 4);

        // Whichever mode we are in, the number shown is the row's own
        // position: RowToNationalNum already applied the ordering.
        FormatDexNum(label, (u16)(index + 1));
        UiText(NUM_X, y, label, UI_COL_DIM, UiThemeShadow());

        if (GetSetPokedexFlag(national, FLAG_GET_SEEN))
            UiText(NAME_X, y, gSpeciesNames[NationalPokedexNumToSpecies(national)],
                   UiThemeText(), UiThemeShadow());
        else
            UiText(NAME_X, y, UiAscii(label, "----------", sizeof(label)),
                   UI_COL_DIM, UiThemeShadow());
    }

    if (sScroll > 0)
    {
        UiRect(PAGE_UP_X, PAGE_Y, PAGE_W, PAGE_H, UI_COL_DIM);
        UiArrow(PAGE_UP_X + (PAGE_W - UI_ARROW_W) / 2,
                PAGE_Y + (PAGE_H - UI_ARROW_H) / 2, TRUE, UI_COL_ACCENT);
    }

    if (sScroll + VISIBLE_ROWS < len)
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

    // Category, height and weight are only revealed once the mon is caught,
    // matching PrintMonInfo (src/pokedex.c:4102).
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

    // Centred as a BLOCK, not per line, which is what the game does:
    // GetStringCenterAlignXOffset measures with GetStringWidth, and that
    // returns the widest line. UiTextWidth has the same contract.
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

// Seen/caught can change without the party changing (seeing a wild mon), so the
// shell needs this in its repaint hash. Counting walks the whole dex, so it is
// only worth doing while this tab is the one on screen.
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
    // The mode can change while this tab is open (the National Dex arriving),
    // which shortens or lengthens the list under the cursor.
    u16 len = DexLength();

    if (sCursor >= len)
        sCursor = len ? (u16)(len - 1) : 0;
    if (sScroll + VISIBLE_ROWS > len)
        sScroll = (len > VISIBLE_ROWS) ? (u16)(len - VISIBLE_ROWS) : 0;

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

    if (!t->justReleased)
        return;

    if (sEntryOpen)
    {
        if (UiHit(t, BACK_X, BACK_Y, BACK_W, BACK_H))
        {
            sEntryOpen = FALSE;
            UiMarkDirty();
        }
        return;
    }

    len = DexLength();

    if (UiHit(t, PAGE_UP_X, PAGE_Y, PAGE_W, PAGE_H) && sScroll > 0)
    {
        sScroll--;
        UiMarkDirty();
        return;
    }

    if (UiHit(t, PAGE_DN_X, PAGE_Y, PAGE_W, PAGE_H)
     && sScroll + VISIBLE_ROWS < len)
    {
        sScroll++;
        UiMarkDirty();
        return;
    }

    // First tap moves the cursor and previews the mon, a second tap on the same
    // row opens the entry. That is the real dex's cursor-then-A, and the same
    // idiom the BAG tab uses.
    if (t->x >= LIST_X && t->y >= LIST_Y && t->y < LIST_Y + VISIBLE_ROWS * ROW_H)
    {
        u16 index = sScroll + (u16)((t->y - LIST_Y) / ROW_H);

        if (index >= len)
            return;

        if (index == sCursor)
        {
            // Nothing to show for an entry the player has never seen.
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
