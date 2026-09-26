// Information about the opposing mon. See matchup.h.
//
// The type chart walk copies the real one in src/battle_script_commands.c, not
// the simple copy in battle_ai_switch_items.c. The gTypeEffectiveness table has
// a TYPE_FORESIGHT marker. The rows after it are the Ghost immunities. They
// apply, except when the target is under Foresight. Do not treat the marker as
// the end of the table, or Normal moves hit Ghosts.

#include "global.h"
#include "battle.h"
#include "battle_anim.h"
#include "battle_main.h"
#include "pokemon.h"
#include "constants/abilities.h"
#include "constants/battle.h"
#include "constants/battle_move_effects.h"
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
            // Under Foresight, the rows after the marker (the Ghost immunities)
            // do not apply. Thus Normal can hit Ghost.
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

// ------------------------------------------------------------ one move ----
//
// A move's multiplier as the battle engine finds it (Cmd_typecalc,
// src/battle_script_commands.c), not as the table alone gives it. Thus a mark
// agrees with the message that the battle prints.

// FALSE while a Cloud Nine or Air Lock mon is on the field: WEATHER_HAS_EFFECT
// (include/battle_util.h). That macro calls AbilityBattleEffects, which writes
// gLastUsedAbility, so this file does its own read-only walk.
static bool8 WeatherHasEffect(void)
{
    for (u32 i = 0; i < gBattlersCount; i++)
    {
        u8 ability = gBattleMons[i].ability;

        if ((ability == ABILITY_CLOUD_NINE || ability == ABILITY_AIR_LOCK)
         && gBattleMons[i].hp != 0)
            return FALSE;
    }

    return TRUE;
}

u8 UiMatchupMoveType(struct Pokemon *mon, u16 move)
{
    // Cmd_hiddenpowercalc, from the IVs. The battle reads them from
    // gBattleMons, which copies them from the mon, so they are the same.
    if (move == MOVE_HIDDEN_POWER)
    {
        u8 typeBits = ((GetMonData(mon, MON_DATA_HP_IV) & 1) << 0)
                    | ((GetMonData(mon, MON_DATA_ATK_IV) & 1) << 1)
                    | ((GetMonData(mon, MON_DATA_DEF_IV) & 1) << 2)
                    | ((GetMonData(mon, MON_DATA_SPEED_IV) & 1) << 3)
                    | ((GetMonData(mon, MON_DATA_SPATK_IV) & 1) << 4)
                    | ((GetMonData(mon, MON_DATA_SPDEF_IV) & 1) << 5);
        u8 type = ((NUMBER_OF_MON_TYPES - 3) * typeBits) / 63 + 1;

        if (type >= TYPE_MYSTERY)
            type++;
        return type;
    }

    // Cmd_setweatherballtype. Only in battle: gBattleWeather keeps the last
    // battle's value after it ends.
    if (gBattleMoves[move].effect == EFFECT_WEATHER_BALL
     && gMain.inBattle && WeatherHasEffect())
    {
        if (gBattleWeather & B_WEATHER_RAIN)
            return TYPE_WATER;
        if (gBattleWeather & B_WEATHER_SANDSTORM)
            return TYPE_ROCK;
        if (gBattleWeather & B_WEATHER_SUN)
            return TYPE_FIRE;
        if (gBattleWeather & B_WEATHER_HAIL)
            return TYPE_ICE;
    }

    return gBattleMoves[move].type;
}

// The moves whose damage does not depend on the chart. Their scripts run
// typecalc, then clear the super effective and not very effective flags
// (bicbyte), so only "no effect" is left. Counter and Mirror Coat use
// typecalc2, with the same result.
static bool8 IsFixedDamage(u16 move)
{
    switch (gBattleMoves[move].effect)
    {
    case EFFECT_LEVEL_DAMAGE:
    case EFFECT_DRAGON_RAGE:
    case EFFECT_SONICBOOM:
    case EFFECT_PSYWAVE:
    case EFFECT_SUPER_FANG:
    case EFFECT_ENDEAVOR:
    case EFFECT_COUNTER:
    case EFFECT_MIRROR_COAT:
    case EFFECT_OHKO:
        return TRUE;
    default:
        return FALSE;
    }
}

bool8 UiMatchupFoePresent(u8 position)
{
    u8 foe;

    if (!UiMatchupActive())
        return FALSE;

    if (position == B_POSITION_OPPONENT_RIGHT
     && !(gBattleTypeFlags & BATTLE_TYPE_DOUBLE))
        return FALSE;

    foe = GetBattlerAtPosition(position);

    if (foe >= gBattlersCount || (gAbsentBattlerFlags & (1u << foe)))
        return FALSE;

    return gBattleMons[foe].species != SPECIES_NONE && gBattleMons[foe].hp != 0;
}

u16 UiMatchupMove(struct Pokemon *mon, u16 move, u8 position)
{
    u8 foe = GetBattlerAtPosition(position);
    u8 type;
    u16 mul;

    if (move == MOVE_NONE)
        return UI_MATCHUP_NA;

    // A status move has no effectiveness. Bide's release never reads the
    // chart, and Struggle skips typecalc.
    if (gBattleMoves[move].power == 0
     || gBattleMoves[move].effect == EFFECT_BIDE
     || move == MOVE_STRUGGLE)
        return UI_MATCHUP_NA;

    type = UiMatchupMoveType(mon, move);

    // Cmd_typecalc tests Levitate before the chart.
    if (gBattleMons[foe].ability == ABILITY_LEVITATE && type == TYPE_GROUND)
        return 0;

    mul = TypeMultiplier(type, gBattleMons[foe].types[0],
                         gBattleMons[foe].types[1],
                         (gBattleMons[foe].status2 & STATUS2_FORESIGHT) != 0);

    if (IsFixedDamage(move) && mul != 0)
        return TYPE_MUL_NORMAL;

    return mul;
}

static u16 ComputeOffence(struct Pokemon *mon)
{
    u16 best = UI_MATCHUP_NA;

    for (u32 i = 0; i < MAX_MON_MOVES; i++)
    {
        u16 mul = UiMatchupMove(mon, (u16)GetMonData(mon, MON_DATA_MOVE1 + i),
                                B_POSITION_OPPONENT_LEFT);

        if (mul == UI_MATCHUP_NA)
            continue;

        if (best == UI_MATCHUP_NA || mul > best)
            best = mul;
    }

    return best;
}

static u16 ComputeRisk(struct Pokemon *mon)
{
    u8 foe = OpposingBattler();
    u16 species = (u16)GetMonData(mon, MON_DATA_SPECIES);
    u8 ourType1, ourType2;
    u16 worst = 0;

    if (species == SPECIES_NONE)
        return UI_MATCHUP_NA;

    ourType1 = gSpeciesInfo[species].types[0];
    ourType2 = gSpeciesInfo[species].types[1];

    // Use the opponent's types, not its moves, which the player cannot see. A
    // player makes the same estimate before a switch.
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

// ------------------------------------------------------------- memo --------
//
// A cache in front of the two walks above.
//
// The PARTY grid asks for both values for six cells on each repaint. Each walk
// crosses gTypeEffectiveness, but the answer changes only when the opponent
// switches, or when this mon's species or moves change.
//
// The key is the walks' inputs: the opponent (species and both types, from
// UiMatchupOpponentKey), and this mon's species and four move ids. The move
// reads still decrypt. The cache saves the table walk, which costs more.
//
// The index is the mon's slot in gPlayerParty, from the caller's pointer. A
// pointer outside that array does not use the cache.
#define MATCHUP_NO_SLOT (-1)

static struct {
    u32   key;
    u16   off, risk;
    bool8 valid;
} sMemo[PARTY_SIZE];

static s32 MemoSlot(struct Pokemon *mon)
{
    s32 slot = (s32)(mon - gPlayerParty);

    return (slot >= 0 && slot < PARTY_SIZE) ? slot : MATCHUP_NO_SLOT;
}

static u32 MemoKey(struct Pokemon *mon)
{
    u32 key = UiMatchupOpponentKey() * 33u + (u32)GetMonData(mon, MON_DATA_SPECIES);

    for (u32 i = 0; i < MAX_MON_MOVES; i++)
        key = key * 33u + (u32)GetMonData(mon, MON_DATA_MOVE1 + i);

    return key;
}

// Both values share one key, so a miss fills both and the second call hits.
static void MatchupBoth(struct Pokemon *mon, u16 *off, u16 *risk)
{
    s32 slot = MemoSlot(mon);
    u32 key;

    if (slot == MATCHUP_NO_SLOT)
    {
        *off  = ComputeOffence(mon);
        *risk = ComputeRisk(mon);
        return;
    }

    key = MemoKey(mon);

    if (!sMemo[slot].valid || sMemo[slot].key != key)
    {
        sMemo[slot].off   = ComputeOffence(mon);
        sMemo[slot].risk  = ComputeRisk(mon);
        sMemo[slot].key   = key;
        sMemo[slot].valid = TRUE;
    }

    *off  = sMemo[slot].off;
    *risk = sMemo[slot].risk;
}

u16 UiMatchupOffence(struct Pokemon *mon)
{
    u16 off, risk;

    MatchupBoth(mon, &off, &risk);
    return off;
}

u16 UiMatchupRisk(struct Pokemon *mon)
{
    u16 off, risk;

    MatchupBoth(mon, &off, &risk);
    return risk;
}

// Every input of UiMatchupMove that is not the mon: each opponent's species,
// types, Foresight and Levitate (the ability follows the species, so the
// species covers it), whether it is on the field, and the weather for Weather
// Ball.
static u32 FoeKey(u8 position)
{
    u8 foe = GetBattlerAtPosition(position);

    if (!UiMatchupFoePresent(position))
        return 0;

    // Species ids use bits 0..8, so bit 11 is free.
    return (u32)gBattleMons[foe].species
         | ((u32)((gBattleMons[foe].status2 & STATUS2_FORESIGHT) != 0) << 11)
         | ((u32)gBattleMons[foe].types[0] << 16)
         | ((u32)gBattleMons[foe].types[1] << 24);
}

u32 UiMatchupOpponentKey(void)
{
    u32 key;

    if (!gMain.inBattle)
        return 0;

    // The left opponent as before, so the grid keeps its answers. The right
    // one and the weather through their own multipliers, so they cannot cancel
    // it.
    key = FoeKey(B_POSITION_OPPONENT_LEFT);
    key ^= FoeKey(B_POSITION_OPPONENT_RIGHT) * 2654435761u;
    key ^= (u32)(WeatherHasEffect() ? gBattleWeather : 0) * 0x85EBCA6Bu;

    return key;
}

// ------------------------------------------------------- catchable check ---
//
// The battles where the player cannot throw a ball. BATTLE_TYPE_TRAINER covers
// the Battle Frontier, the Battle Tower, secret bases and Trainer Hill. The
// other flags are wild battles that the player cannot catch in: Wally's
// tutorial, Birch's bag on Route 101, and the replays.
//
// The Safari Zone is not in this list. Safari Balls are balls.
#define UNCATCHABLE_BATTLE (BATTLE_TYPE_TRAINER          \
                          | BATTLE_TYPE_LINK             \
                          | BATTLE_TYPE_RECORDED         \
                          | BATTLE_TYPE_RECORDED_LINK    \
                          | BATTLE_TYPE_EREADER_TRAINER  \
                          | BATTLE_TYPE_WALLY_TUTORIAL   \
                          | BATTLE_TYPE_FIRST_BATTLE)

// UiShinyOpponent and the quick-throw strip both use this, so there is one
// copy. Each check below prevents a bug; see the comments.
bool8 UiCatchableOpponent(void)
{
    struct Pokemon *foe = &gEnemyParty[0];

    if (!gMain.inBattle)
        return FALSE;

    if (gBattleTypeFlags & UNCATCHABLE_BATTLE)
        return FALSE;

    // The encounter ends when the game sets a result: caught, won, ran, fled,
    // lost or another. The value of gBattleOutcome is 0 while the fight is
    // live, and BattleStartClearSetData clears it. It is better than
    // gMain.inBattle, which stays TRUE through the catch, the nickname prompt
    // and the fade.
    if (gBattleOutcome != 0)
        return FALSE;

    // Use gEnemyParty, not gBattleMons. BattleStartClearSetData() does not
    // clear gBattleMons. Until BattleIntroGetMonsData runs, gBattleMons holds
    // the previous battle's mons. A shiny alert for a mon that is not there is
    // a bug. The game writes gEnemyParty before it sets inBattle, so it is
    // correct from the first frame.
    //
    // Slot 0 is enough because the game has no wild double battles. A battle
    // with a second opponent is a trainer battle, which returned above.
    return GetMonData(foe, MON_DATA_SANITY_HAS_SPECIES) != 0;
}

bool8 UiShinyOpponent(u16 *species, u32 *identity)
{
    struct Pokemon *foe = &gEnemyParty[0];
    u32 otId, personality;

    if (!UiCatchableOpponent())
        return FALSE;

    // These two are before MON_DATA_ENCRYPT_SEPARATOR (include/pokemon.h), so
    // they read the plain header and do not decrypt. The shell polls this every
    // frame. The species read below decrypts, so it comes last and runs only
    // for a real shiny.
    otId        = GetMonData(foe, MON_DATA_OT_ID);
    personality = GetMonData(foe, MON_DATA_PERSONALITY);

    if (!IsShinyOtIdPersonality(otId, personality))
        return FALSE;

    if (species != NULL)
        *species = (u16)GetMonData(foe, MON_DATA_SPECIES);

    // The personality alone. It is new for each wild mon, and it decides the
    // shiny test above. Two encounters with the same value do not occur in
    // practice.
    if (identity != NULL)
        *identity = personality;

    return TRUE;
}
