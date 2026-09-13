// Status tags. See status_tags.h for what a tag is and why a mon can carry two.
//
// Pure reads, like matchup.c: the party through GetMonData, and the battle
// through the same globals the battle engine keeps. Nothing here writes.

#include "global.h"
#include "main.h"                   // gMain.inBattle
#include "pokemon.h"
#include "battle.h"                 // struct DisableStruct, for the headers below
#include "battle_anim.h"            // GetBattlerSide
#include "battle_controllers.h"     // Ctr3dsPlayerIsChoosingAction
#include "party_menu.h"             // GetAilmentFromStatus
#include "constants/battle.h"
#include "constants/party_menu.h"

#include "ui_draw.h"                // UI_STATUS_CNF
#include "ui_shell.h"               // UiAnimStepped
#include "status_tags.h"

// How long each tag holds, in displayed frames: one second.
//
// A flip only ever happens on a UiAnimStepped() frame, and 60 divides both step
// periods (6 frames with the rasteriser on its own core, 12 without), so after
// the first flip every one lands exactly a second after the last AND on a frame
// the mon icons were stepping anyway. On the single-core path that is the rule
// the whole screen's frame rate rests on: a private period is a private repaint
// budget (SECOND_SCREEN_CHEATSHEET.md, section 7).
#define TAG_HOLD_FRAMES 60

// Whether this battle has reached the player's first action selection.
//
// gBattleMons is not cleared when a battle starts (see the note in
// UiCatchableOpponent, matchup.c), and status2 in particular is only zeroed in
// BattleIntroDrawTrainersOrMonsSprites (src/battle_main.c). Until then it holds
// whatever the LAST battle ended with, so a lead that won that battle while
// confused would flash CNF through the whole of the next one's intro. Nothing
// can inflict confusion before the player has chosen a first action, so
// waiting for that loses nothing real.
//
// The cost is battles the player never chooses in: recorded battles, and the
// Safari Zone, where confusion cannot happen anyway.
static bool8 sBattleLive;

static u8    sConfused;     // one bit per party slot
static bool8 sCycling;      // some slot carries two tags
static u8    sHold;         // frames since the last flip
static u8    sPhase;        // which tag each cycling slot shows
static bool8 sFlipped;      // sPhase moved this frame

// Which party slots are confused right now.
static u8 ReadConfused(void)
{
    u8 mask = 0;

    // Decided battles as well as unstarted ones: status2 is not cleared at the
    // end either, and a win, a catch or a run makes confusion meaningless while
    // gMain.inBattle is still TRUE through the fade out.
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

        // The battler really is this party mon. A switch points the party
        // index at the incoming mon (Cmd_getswitchedmondata) at least a frame
        // before the battle struct is refilled from it
        // (Cmd_switchindataupdate, src/battle_script_commands.c), and in
        // between the struct still holds the outgoing mon, confusion and all.
        // Without this, CNF would flash on the cell of the mon coming in. The
        // personality sits in the plaintext header, so this costs no decrypt.
        if (gBattleMons[b].personality
            != GetMonData(&gPlayerParty[slot], MON_DATA_PERSONALITY))
            continue;

        mask |= 1 << slot;
    }

    return mask;
}

// The slot's tags in the order they show, main status first. At most two.
static u8 GetTags(u8 slot, u8 tags[2])
{
    struct Pokemon *mon;
    u8 n = 0;
    u8 ailment;

    if (slot >= PARTY_SIZE)
        return 0;

    mon = &gPlayerParty[slot];

    // GetMonAilment() without its last step, the Pokerus check. That one
    // decrypts the mon, and its answer draws nothing (the party menu shows no
    // badge for Pokerus either), while this runs for every slot on every
    // animation step. HP and status are party fields, outside the encrypted
    // substructs, so what is left costs no decrypt at all.
    ailment = (GetMonData(mon, MON_DATA_HP) == 0)
            ? AILMENT_FNT
            : GetAilmentFromStatus(GetMonData(mon, MON_DATA_STATUS));

    if (ailment != AILMENT_NONE)
        tags[n++] = ailment;

    // A fainted mon is FNT and nothing else. Its status2 is cleared when it
    // faints anyway (FaintClearSetData), but this does not rely on the timing.
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

    // Idle while nothing alternates, so a mon that picks up a second tag shows
    // its main status first and holds it for a full second, rather than
    // starting partway through a cycle nobody could see.
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

// The confused bit first: it is almost always clear, and the tick asks this
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

    // Plus one so the first phase is not the same key as "nothing cycles".
    if (withPhase && sCycling)
        key |= ((u32)sPhase + 1) << 8;

    return key;
}
