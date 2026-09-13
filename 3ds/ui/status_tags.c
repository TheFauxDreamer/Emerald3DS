// Status tags. See status_tags.h for what a tag is and why a mon can have two.
//
// This file only reads: the party through GetMonData, and the battle through
// the battle engine's globals.

#include "global.h"
#include "main.h"                   // gMain.inBattle
#include "pokemon.h"
#include "battle.h"                 // struct DisableStruct
#include "battle_anim.h"            // GetBattlerSide
#include "battle_controllers.h"     // Ctr3dsPlayerIsChoosingAction
#include "party_menu.h"             // GetAilmentFromStatus
#include "constants/battle.h"
#include "constants/party_menu.h"

#include "ui_draw.h"                // UI_STATUS_CNF
#include "ui_shell.h"               // UiAnimStepped
#include "ui_team.h"                // UiPartyMon
#include "status_tags.h"

// The time for which each tag holds, in displayed frames: one second.
//
// A flip occurs only on a UiAnimStepped() frame. Both step periods (6 and 12)
// divide 60, so each flip occurs one second after the last one. The icons step
// on the same frame. On the single-core path, this shares the repaint
// (SECOND_SCREEN_CHEATSHEET.md, section 7).
#define TAG_HOLD_FRAMES 60

// TRUE after the battle reaches the player's first action selection.
//
// gBattleMons is not cleared when a battle starts (see UiCatchableOpponent in
// matchup.c). BattleIntroDrawTrainersOrMonsSprites clears status2. Before that,
// status2 holds the values from the previous battle, and CNF could show during
// the intro. Nothing can cause confusion before the first action, so the wait
// costs nothing.
//
// Battles with no player choice (recorded battles, the Safari Zone) never set
// this. Confusion does not occur there.
static bool8 sBattleLive;

static u8    sConfused;     // one bit for each party slot
static bool8 sCycling;      // a slot has two tags
static u8    sHold;         // frames since the last flip
static u8    sPhase;        // the tag that each cycling slot shows
static bool8 sFlipped;      // sPhase changed on this frame

// The party slots that are confused now.
static u8 ReadConfused(void)
{
    u8 mask = 0;

    // Ended battles too: status2 is not cleared at the end either. After a win,
    // a catch or a run, confusion means nothing, but gMain.inBattle stays TRUE
    // through the fade.
    if (!sBattleLive || gBattleOutcome != 0)
        return 0;

    for (u32 b = 0; b < gBattlersCount && b < MAX_BATTLERS_COUNT; b++)
    {
        u32 slot;

        if (GetBattlerSide(b) != B_SIDE_PLAYER)
            continue;
        if (gAbsentBattlerFlags & (1u << b))
            continue;
        if (!(gBattleMons[b].status2 & STATUS2_CONFUSION))
            continue;

        slot = gBattlerPartyIndexes[b];
        if (slot >= PARTY_SIZE)
            continue;

        // Make sure that the battler is this party mon. A switch sets the party
        // index to the new mon (Cmd_getswitchedmondata) one or more frames
        // before the battle struct gets its data (Cmd_switchindataupdate).
        // Between the two, the struct still holds the old mon and its
        // confusion. Without this check, CNF shows on the new mon for that
        // time. The personality is in the plain header, so the check does not
        // decrypt. Use UiPartyMon, because gBattlerPartyIndexes holds field
        // slots and the game's party menu can reorder the array.
        if (gBattleMons[b].personality
            != GetMonData(UiPartyMon((u8)slot), MON_DATA_PERSONALITY))
            continue;

        mask |= 1 << slot;
    }

    return mask;
}

// The slot's tags in display order, the main status first. Two at most.
static u8 GetTags(u8 slot, u8 tags[2])
{
    struct Pokemon *mon;
    u8 n = 0;
    u8 ailment;

    if (slot >= PARTY_SIZE)
        return 0;

    mon = UiPartyMon(slot);

    // GetMonAilment() without its last step, the Pokerus check. That step
    // decrypts the mon and draws nothing (the party menu shows no Pokerus
    // badge). This runs for every slot on every animation step. HP and status
    // are outside the encrypted substructs, so the rest does not decrypt.
    ailment = (GetMonData(mon, MON_DATA_HP) == 0)
            ? AILMENT_FNT
            : GetAilmentFromStatus(GetMonData(mon, MON_DATA_STATUS));

    if (ailment != AILMENT_NONE)
        tags[n++] = ailment;

    // A fainted mon shows only FNT. FaintClearSetData clears its status2, but
    // this does not depend on that timing.
    if ((sConfused & (1 << slot)) && ailment != AILMENT_FNT)
        tags[n++] = UI_STATUS_CNF;

    return n;
}

void UiStatusTagsTick(void)
{
    sFlipped = FALSE;

    if (!gMain.inBattle)
    {
        sBattleLive = FALSE;
        sConfused = 0;
        sCycling = FALSE;
        sHold = 0;
        sPhase = 0;
        return;
    }

    if (!sBattleLive && Ctr3dsPlayerIsChoosingAction())
        sBattleLive = TRUE;

    sConfused = ReadConfused();

    sCycling = FALSE;
    for (u8 slot = 0; slot < PARTY_SIZE && !sCycling; slot++)
        sCycling = UiStatusTagCycles(slot);

    // Stay idle while nothing alternates. A mon that gets a second tag then
    // shows its main status first for a full second.
    if (!sCycling)
    {
        sHold = 0;
        sPhase = 0;
        return;
    }

    if (sHold < TAG_HOLD_FRAMES)
        sHold++;

    if (sHold >= TAG_HOLD_FRAMES && UiAnimStepped())
    {
        sHold = 0;
        sPhase++;
        sFlipped = TRUE;
    }
}

u8 UiStatusTag(u8 slot)
{
    u8 tags[2];
    u8 n = GetTags(slot, tags);

    if (n == 0)
        return AILMENT_NONE;

    return tags[sPhase % n];
}

// Check the confused bit first. It is almost always clear, and the tick asks
// for all six slots on every frame of a battle.
bool8 UiStatusTagCycles(u8 slot)
{
    u8 tags[2];

    if (slot >= PARTY_SIZE || !(sConfused & (1 << slot)))
        return FALSE;

    return GetTags(slot, tags) == 2;
}

bool8 UiStatusTagsFlipped(void)
{
    return sFlipped;
}

u32 UiStatusTagsKey(bool8 withPhase)
{
    u32 key = sConfused;

    // Add one, so that the first phase gives a key different from "nothing
    // cycles".
    if (withPhase && sCycling)
        key |= ((u32)sPhase + 1) << 8;

    return key;
}
