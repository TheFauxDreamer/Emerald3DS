// Whose Pokemon each party slot holds. See ui_team.h.
//
// Pure reads, like matchup.c and status_tags.c. Nothing here writes.

#include "global.h"
#include "main.h"                   // gMain.inBattle
#include "pokemon.h"
#include "battle.h"                 // gBattleTypeFlags
#include "party_menu.h"             // GetPartyIdFromBattlePartyId
#include "constants/battle.h"
#include "constants/characters.h"   // EOS
#include "constants/species.h"

#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "ui_team.h"

// Text inset from the tag's edge: the two-pixel rule plus three of air, the
// same five the PARTY tab's cheat tags leave.
#define TAG_PAD 5

struct Pokemon *UiPartyMon(u8 slot)
{
    if (slot >= PARTY_SIZE)
        slot = 0;

    // While the party menu has the array in battle order, field slot `slot`
    // is at the menu position GetPartyIdFromBattlePartyId names: that is
    // exactly where UpdatePartyToBattleOrder copied it. It stays true while
    // the menu is up, because a switch there (TrySwitchInPokemon) swaps the
    // Pokemon and their order entries together.
    if (gMain.inBattle && Ctr3dsPartyInBattleOrder())
        return &gPlayerParty[GetPartyIdFromBattlePartyId(slot)];

    return &gPlayerParty[slot];
}

bool8 UiAllyPresent(void)
{
    return gMain.inBattle && (gBattleTypeFlags & BATTLE_TYPE_INGAME_PARTNER) != 0;
}

bool8 UiAllySlot(u8 slot)
{
    return UiAllyPresent() && slot >= MULTI_PARTY_SIZE && slot < PARTY_SIZE;
}

u32 UiTeamKey(void)
{
    u32 mask = 0;

    for (u8 i = 0; i < PARTY_SIZE; i++)
        if (UiAllySlot(i))
            mask |= 1u << i;

    return mask;
}

void UiAllyFrameGround(int x, int y, int w, int h)
{
    UiFillRect(x + 8, y + 8, w - 16, h - 16, UI_COL_ALLY_GROUND);
}

// The partner's name, from the first of their slots that holds a Pokemon.
// FALSE with no partner, which also covers the frames either side of a
// battle, when their Pokemon are in the party but no longer marked.
static bool8 AllyName(u8 *name)
{
    for (u8 i = MULTI_PARTY_SIZE; i < PARTY_SIZE; i++)
    {
        struct Pokemon *mon;

        if (!UiAllySlot(i))
            continue;

        mon = UiPartyMon(i);
        if (GetMonData(mon, MON_DATA_SPECIES) == SPECIES_NONE)
            continue;

        GetMonData(mon, MON_DATA_OT_NAME, name);
        return name[0] != EOS;
    }

    return FALSE;
}

int UiAllyTagWidth(void)
{
    u8 name[PLAYER_NAME_LENGTH + 1];

    if (!AllyName(name))
        return 0;

    return UiTextWidth(name) + TAG_PAD * 2;
}

int UiAllyTag(int x, int y, int h)
{
    u8 name[PLAYER_NAME_LENGTH + 1];
    int w;

    if (!AllyName(name))
        return 0;

    w = UiTextWidth(name) + TAG_PAD * 2;

    // The achievement toast's two-step rule, in the partner's gold, around the
    // same ground the partner's cells are painted in.
    UiFillRect(x, y, w, h, UI_COL_ALLY_GROUND);
    UiRect(x, y, w, h, UI_COL_ALLY);
    UiRect(x + 1, y + 1, w - 2, h - 2, UI_COL_ALLY_EDGE);
    UiText(x + TAG_PAD, y + (h - UI_GLYPH_H) / 2, name,
           UiThemeText(), UiThemeShadow());

    return w;
}
