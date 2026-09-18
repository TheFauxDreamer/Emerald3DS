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

static u16 ComputeOffence(struct Pokemon *mon)
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

        // A status move has no effectiveness, so ignore it.
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
