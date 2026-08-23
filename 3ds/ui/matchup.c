// Type matchup readout. See matchup.h.
//
// The chart walk mirrors the authoritative one in src/battle_script_commands.c
// (around line 1386), NOT the simplified copy in battle_ai_switch_items.c.
// The difference matters: gTypeEffectiveness carries a TYPE_FORESIGHT marker
// partway through, and the rows after it are the Ghost immunities. They apply
// normally and are skipped only when the target is actually under Foresight.
// Treating the marker as a plain terminator would report Normal moves as
// hitting Ghosts.

#include "global.h"
#include "battle.h"
#include "battle_anim.h"
#include "battle_main.h"
#include "pokemon.h"
#include "constants/battle.h"
#include "constants/moves.h"
#include "constants/species.h"

#include "matchup.h"

static u8 OpposingBattler(void)
{
    return GetBattlerAtPosition(B_POSITION_OPPONENT_LEFT);
}

bool8 UiMatchupActive(void)
{
    if (!gMain.inBattle)
        return FALSE;

    return gBattleMons[OpposingBattler()].species != SPECIES_NONE;
}

// One attacking type against one defending pair, on the x10 scale.
static u16 TypeMultiplier(u8 atkType, u8 defType1, u8 defType2, bool8 foresighted)
{
    u16 mul = TYPE_MUL_NORMAL;
    s32 i = 0;

    while (TYPE_EFFECT_ATK_TYPE(i) != TYPE_ENDTABLE)
    {
        if (TYPE_EFFECT_ATK_TYPE(i) == TYPE_FORESIGHT)
        {
            // Under Foresight the rows beyond the marker (the Ghost
            // immunities) stop applying, which is what lets Normal hit Ghost.
            if (foresighted)
                break;

            i += 3;
            continue;
        }
        else if (TYPE_EFFECT_ATK_TYPE(i) == atkType)
        {
            if (TYPE_EFFECT_DEF_TYPE(i) == defType1)
                mul = (mul * TYPE_EFFECT_MULTIPLIER(i)) / TYPE_MUL_NORMAL;

            if (TYPE_EFFECT_DEF_TYPE(i) == defType2 && defType1 != defType2)
                mul = (mul * TYPE_EFFECT_MULTIPLIER(i)) / TYPE_MUL_NORMAL;
        }

        i += 3;
    }

    return mul;
}

u16 UiMatchupOffence(struct Pokemon *mon)
{
    u8 foe = OpposingBattler();
    bool8 foresighted = (gBattleMons[foe].status2 & STATUS2_FORESIGHT) != 0;
    u16 best = UI_MATCHUP_NA;

    for (u32 i = 0; i < MAX_MON_MOVES; i++)
    {
        u16 move = (u16)GetMonData(mon, MON_DATA_MOVE1 + i);
        u16 mul;

        if (move == MOVE_NONE)
            continue;

        // A status move has no effectiveness to report, so judging the mon by
        // one would be misleading.
        if (gBattleMoves[move].power == 0)
            continue;

        mul = TypeMultiplier(gBattleMoves[move].type,
                             gBattleMons[foe].types[0],
                             gBattleMons[foe].types[1],
                             foresighted);

        if (best == UI_MATCHUP_NA || mul > best)
            best = mul;
    }

    return best;
}

u16 UiMatchupRisk(struct Pokemon *mon)
{
    u8 foe = OpposingBattler();
    u16 species = (u16)GetMonData(mon, MON_DATA_SPECIES);
    u8 ourType1, ourType2;
    u16 worst = 0;

    if (species == SPECIES_NONE)
        return UI_MATCHUP_NA;

    ourType1 = gSpeciesInfo[species].types[0];
    ourType2 = gSpeciesInfo[species].types[1];

    // Judged on the opponent's own types rather than its moves, which we cannot
    // see. It is the same estimate a player makes before switching in.
    for (u32 i = 0; i < 2; i++)
    {
        u8 atkType = gBattleMons[foe].types[i];
        u16 mul;

        if (i == 1 && gBattleMons[foe].types[0] == gBattleMons[foe].types[1])
            break;

        mul = TypeMultiplier(atkType, ourType1, ourType2, FALSE);

        if (mul > worst)
            worst = mul;
    }

    return worst;
}

u32 UiMatchupOpponentKey(void)
{
    u8 foe;

    if (!gMain.inBattle)
        return 0;

    foe = OpposingBattler();

    return (u32)gBattleMons[foe].species
         | ((u32)gBattleMons[foe].types[0] << 16)
         | ((u32)gBattleMons[foe].types[1] << 24);
}
