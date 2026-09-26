// FRIENDSHIP: each Pokemon's friendship, as the Poketch's Friendship Checker
// shows it. See view_home.h.
//
// The hearts are the tiers of the game's friendship raters in Verdanturf and
// Pacifidlog, from Ctr3dsFriendshipScore (src/field_specials.c). The exact
// value, the evolution that needs friendship (Ctr3dsFriendshipEvolution,
// src/pokemon.c) and the power of Return or Frustration show under them.

#include "global.h"
#include "field_specials.h"           // Ctr3dsFriendshipScore
#include "pokemon.h"                  // Ctr3dsFriendshipEvolution
#include "constants/moves.h"
#include "constants/pokemon.h"
#include "constants/species.h"

#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "ui_team.h"
#include "view_home.h"

// Two columns of three cells, in the order of the party grid.
#define CELL_W       152
#define CELL_H       51
#define CELL_X(i)    (8 + ((i) % 2) * CELL_W)
#define CELL_Y(i)    (UI_PAGE_TOP + ((i) / 2) * CELL_H)
#define ICON_DX      4
#define ICON_DY      4
#define TEXT_DX      40
#define NAME_DY      1
#define HEART_DY     21
#define VALUE_DY     16
#define NOTE_DY      34
#define CELL_RIGHT   146

// Five hearts: the tiers 1-49 to 200-254 fill one each. MAX fills all five and
// gives them a gold edge.
#define HEARTS       5
#define HEART_W      7
#define HEART_H      6
#define HEART_GAP    2

static const u8 sHeart[HEART_H][HEART_W] =
{
    {0,1,1,0,1,1,0},
    {1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1},
    {0,1,1,1,1,1,0},
    {0,0,1,1,1,0,0},
    {0,0,0,1,0,0,0},
};

static bool8 HeartInk(int r, int c)
{
    return r >= 0 && r < HEART_H && c >= 0 && c < HEART_W && sHeart[r][c];
}

// An edge pixel has a side that is not ink.
static bool8 HeartEdge(int r, int c)
{
    return !HeartInk(r - 1, c) || !HeartInk(r + 1, c)
        || !HeartInk(r, c - 1) || !HeartInk(r, c + 1);
}

static void DrawHeart(int x, int y, bool8 full, bool8 max)
{
    u16 edge = max ? UI_COL_SHINY : full ? UI_COL_ACH_PINK_EDGE : UI_COL_DIM;

    for (int r = 0; r < HEART_H; r++)
        for (int c = 0; c < HEART_W; c++)
        {
            if (!HeartInk(r, c))
                continue;

            if (HeartEdge(r, c))
                UiFillRect(x + c, y + r, 1, 1, edge);
            else if (full)
                UiFillRect(x + c, y + r, 1, 1, UI_COL_ACH_PINK);
        }
}

// ASCII helpers for the small line.
static char *PutText(char *p, const char *s)
{
    while (*s != '\0')
        *p++ = *s++;
    *p = '\0';
    return p;
}

static char *PutNum(char *p, u32 v)
{
    char digits[4];
    int n = 0;

    do
    {
        digits[n++] = '0' + v % 10;
        v /= 10;
    } while (v != 0 && n < (int)sizeof(digits));

    while (n > 0)
        *p++ = digits[--n];
    *p = '\0';
    return p;
}

// MOVE_RETURN or MOVE_FRUSTRATION when the Pokemon knows one, else MOVE_NONE.
static u16 FriendshipMove(struct Pokemon *mon)
{
    for (u32 i = 0; i < MAX_MON_MOVES; i++)
    {
        u16 move = GetMonData(mon, MON_DATA_MOVE1 + i);

        if (move == MOVE_RETURN || move == MOVE_FRUSTRATION)
            return move;
    }

    return MOVE_NONE;
}

// The small line: the evolution that needs friendship, then the power of
// Return or Frustration.
static void Note(struct Pokemon *mon, u32 friendship, char *out)
{
    u8 threshold;
    u8 method = Ctr3dsFriendshipEvolution(GetMonData(mon, MON_DATA_SPECIES), &threshold);
    u16 move = FriendshipMove(mon);
    char *p = out;

    *p = '\0';

    if (method != 0)
    {
        p = PutText(p, move != MOVE_NONE ? "evo " : "evolves at ");
        p = PutNum(p, threshold);
        if (method == EVO_FRIENDSHIP_DAY)
            p = PutText(p, " by day");
        else if (method == EVO_FRIENDSHIP_NIGHT)
            p = PutText(p, " at night");
        if (friendship >= threshold)
            p = PutText(p, " OK");
    }

    // The power as Cmd_friendshiptodamagecalculation
    // (src/battle_script_commands.c) makes it. That function writes
    // gDynamicBasePower, so its one line is copied here.
    if (move != MOVE_NONE)
    {
        if (method != 0)
            p = PutText(p, "  ");
        p = PutText(p, move == MOVE_RETURN ? "RETURN " : "FRUST. ");
        PutNum(p, move == MOVE_RETURN ? 10 * friendship / 25
                                      : 10 * (MAX_FRIENDSHIP - friendship) / 25);
    }
}

static void DrawCell(u8 slot)
{
    struct Pokemon *mon = UiPartyMon(slot);
    u16 species = GetMonData(mon, MON_DATA_SPECIES);
    int x = CELL_X(slot), y = CELL_Y(slot);
    u8 name[POKEMON_NAME_LENGTH + 1];
    u8 text[40];
    char ascii[40];
    u32 friendship;
    u8 tier;

    if (species == SPECIES_NONE)
        return;

    UiMonIcon(x + ICON_DX, y + ICON_DY, species, GetMonData(mon, MON_DATA_PERSONALITY));

    // An egg's friendship field holds its egg cycles, not friendship.
    if (GetMonData(mon, MON_DATA_IS_EGG))
    {
        UiText(x + TEXT_DX, y + NAME_DY, UiAscii(text, "EGG", sizeof(text)),
               UI_COL_DIM, UiThemeShadow());
        return;
    }

    GetMonData(mon, MON_DATA_NICKNAME, name);
    UiTextClipped(x + TEXT_DX, y + NAME_DY, CELL_RIGHT - TEXT_DX, name,
                  UiThemeText(), UiThemeShadow());

    friendship = GetMonData(mon, MON_DATA_FRIENDSHIP);
    tier = Ctr3dsFriendshipScore(mon);

    for (u32 i = 0; i < HEARTS; i++)
        DrawHeart(x + TEXT_DX + i * (HEART_W + HEART_GAP), y + HEART_DY,
                  tier > FRIENDSHIP_NONE && i < (u32)tier - FRIENDSHIP_1_TO_49 + 1,
                  tier == FRIENDSHIP_MAX);

    UiNumRight(x + CELL_RIGHT, y + VALUE_DY, friendship,
               tier == FRIENDSHIP_MAX ? UI_COL_ACCENT : UiThemeText(), UiThemeShadow());

    Note(mon, friendship, ascii);
    if (ascii[0] != '\0')
        UiTextSmall(x + TEXT_DX, y + NOTE_DY, UiAscii(text, ascii, sizeof(text)),
                    UI_COL_DIM, UiThemeShadow());
}

void UiFriendshipPageDraw(void)
{
    for (u8 i = 0; i < PARTY_SIZE; i++)
        DrawCell(i);
}

u32 UiFriendshipPageKey(void)
{
    u32 key = 0;

    // Friendship grows as the player walks and falls when a Pokemon faints,
    // with no touch here.
    for (u8 i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *mon = UiPartyMon(i);

        key ^= (GetMonData(mon, MON_DATA_SPECIES)
                | (GetMonData(mon, MON_DATA_FRIENDSHIP) << 16)
                | ((u32)FriendshipMove(mon) << 24)
                | ((u32)GetMonData(mon, MON_DATA_IS_EGG) << 31))
             * (2654435761u + 2 * i);
    }

    return key;
}
