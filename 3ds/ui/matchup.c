// Readouts about the opposing mon. See matchup.h.
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

// ------------------------------------------------------------- memo --------
//
// A cache in front of the two walks above, not a replacement for them.
//
// The PARTY grid asks for both readouts for all six cells on every repaint, and
// each walk crosses gTypeEffectiveness -- roughly 12,000 iterations a repaint --
// to produce an answer that only changes when the opponent switches, or this
// mon's species or moves do. On a screen whose frame cost is measured in whole
// VBlanks that is worth removing.
//
// Keyed on exactly the inputs the walks read: the opponent (species and both
// types, via UiMatchupOpponentKey) plus this mon's species and four move ids.
// Reading the moves still costs its GetMonData decrypts -- the saving is the
// table walk, which is the expensive half.
//
// Indexed by the mon's slot in gPlayerParty, derived from the pointer the caller
// already passes. Anything outside that array bypasses the memo rather than
// aliasing someone else's entry.
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

// Both readouts share one key, so a miss fills both and the paired call hits.
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
// Battles the player cannot throw a ball in. BATTLE_TYPE_TRAINER covers far
// more than it looks: the whole Battle Frontier, the Battle Tower, secret bases
// and Trainer Hill all set it, so it is one test rather than seven. The rest
// are the battles that are wild but still not yours to catch in -- Wally's
// tutorial catch, Birch's bag on Route 101, and the replay paths, where the
// player is not the one choosing actions at all.
//
// The Safari Zone is deliberately NOT here. Safari Balls are balls.
#define UNCATCHABLE_BATTLE (BATTLE_TYPE_TRAINER          \
                          | BATTLE_TYPE_LINK             \
                          | BATTLE_TYPE_RECORDED         \
                          | BATTLE_TYPE_RECORDED_LINK    \
                          | BATTLE_TYPE_EREADER_TRAINER  \
                          | BATTLE_TYPE_WALLY_TUTORIAL   \
                          | BATTLE_TYPE_FIRST_BATTLE)

// Split out of UiShinyOpponent rather than duplicated, because the quick-throw
// strip asks the same question and two copies of this would drift. Every check
// below records a bug; see the comments.
bool8 UiCatchableOpponent(void)
{
    struct Pokemon *foe = &gEnemyParty[0];

    if (!gMain.inBattle)
        return FALSE;

    if (gBattleTypeFlags & UNCATCHABLE_BATTLE)
        return FALSE;

    // The encounter is over the moment the game says so, however it ended:
    // B_OUTCOME_CAUGHT, _WON (you knocked it out), _RAN, _MON_FLED, _LOST and
    // the rest all land here. gBattleOutcome is 0 while the fight is live and
    // is cleared by BattleStartClearSetData (src/battle_main.c:3149), so it is
    // the game's own answer to "is there still something to catch", and a
    // better one than gMain.inBattle, which stays true through the catch
    // sequence, the nickname prompt and the fade out.
    if (gBattleOutcome != 0)
        return FALSE;

    // gEnemyParty, not gBattleMons, and the difference is not cosmetic.
    // BattleStartClearSetData() does not zero gBattleMons, so between
    // gMain.inBattle going true (src/battle_main.c:708) and the intro's
    // BattleIntroGetMonsData completing, that array still holds the PREVIOUS
    // battle's mons. A stale type matchup during the transition is a shrug; a
    // shiny alert for a mon that is no longer there is not, and neither is a
    // ball offered against one. gEnemyParty is written before inBattle is set
    // in both cases -- CreateWildMon during the encounter, CreateNPCTrainerParty
    // on the line above it -- so it is correct from the first frame.
    //
    // Slot 0 is the whole answer here because Emerald has no wild double
    // battles: every battle with a second opponent is a trainer battle, and
    // those returned above.
    return GetMonData(foe, MON_DATA_SANITY_HAS_SPECIES) != 0;
}

bool8 UiShinyOpponent(u16 *species, u32 *identity)
{
    struct Pokemon *foe = &gEnemyParty[0];
    u32 otId, personality;

    if (!UiCatchableOpponent())
        return FALSE;

    // These two sit BEFORE MON_DATA_ENCRYPT_SEPARATOR (include/pokemon.h:8-19),
    // so they answer from the plaintext header and cost no decryption. The
    // shell polls this every frame, so that matters; the species read below
    // does decrypt, which is why it is last and only reached for a real shiny.
    otId        = GetMonData(foe, MON_DATA_OT_ID);
    personality = GetMonData(foe, MON_DATA_PERSONALITY);

    if (!IsShinyOtIdPersonality(otId, personality))
        return FALSE;

    if (species != NULL)
        *species = (u16)GetMonData(foe, MON_DATA_SPECIES);

    // The personality alone, which is rerolled for every wild mon and is what
    // the shininess above was computed from. Two encounters sharing one is as
    // likely as two sharing a shiny, which is to say it does not happen.
    if (identity != NULL)
        *identity = personality;

    return TRUE;
}
