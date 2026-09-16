// Gameplay tweaks: EXP All, a level cap from the badges, a species randomizer,
// and a persistent bag sort order.
//
// These are cheats. The other EXTRA options (fast-forward, top-screen scale,
// button binds, the tab override) leave the game as it shipped. Every option
// here changes it. Each one is off by default, and the player must turn it on.
//
// All the logic is in this file, so the hooks in src/ stay one or two lines,
// inside #if PLATFORM_3DS. A port that changes less original source is easier
// to rebase.
//
// This is a game-side file under the two-worlds rule in bridge.h. It includes
// game headers and bridge.h, never <3ds.h>. The file src/siirtc.c also reads a
// host setting from game code.

#include "global.h"
#include "battle_pike.h"
#include "battle_pyramid.h"
#include "event_data.h"
#include "event_object_movement.h"
#include "item.h"
#include "main.h"
#include "overworld.h"
#include "script.h"
#include "pokemon.h"
#include "random.h"
#include "constants/characters.h"
#include "constants/item.h"
#include "constants/items.h"
#include "constants/pokemon.h"
#include "constants/species.h"

#include "bridge.h"
#include "tweaks.h"

// ---- EXP All ---------------------------------------------------------------

bool8 Ctr3dsExpAllOn(void)
{
    return Ctr3dsGetExpAll() ? TRUE : FALSE;
}

// ---- Follower --------------------------------------------------------------

bool8 Ctr3dsFollowerOn(void)
{
    return Ctr3dsGetFollowerOn() ? TRUE : FALSE;
}

void Ctr3dsRefreshFollowerNow(void)
{
    // The same gate as Ctr3dsSortBagNow. A follower that appears during a
    // script can block the script's movement. The next map load, return to
    // the field or heal updates the follower if this refuses.
    if (gMain.inBattle)
        return;
    if (gMain.callback2 != CB2_Overworld)
        return;
    if (ArePlayerFieldControlsLocked())
        return;
    if (ScriptContext_IsEnabled())
        return;

    UpdateFollowingPokemon();
}

// ---- Phone calls -----------------------------------------------------------

bool8 Ctr3dsMatchCallSuppressed(void)
{
    return Ctr3dsGetPhoneCallsOff() ? TRUE : FALSE;
}

// ---- Level cap -------------------------------------------------------------

// The ace levels of the game's gym leaders, then the Elite Four. The cap is the
// level of the next fight, so it uses the first badge that the player does not
// have. Before any badge, the cap is 15 (Roxanne's Nosepass).
//
// The values come from src/caps.c in rh-hideout/pokeemerald-expansion. This
// tree is vanilla pokeemerald and has no include/config/caps.h.
static const struct { u16 flag; u8 cap; } sLevelCaps[] =
{
    { FLAG_BADGE01_GET, 15 },
    { FLAG_BADGE02_GET, 19 },
    { FLAG_BADGE03_GET, 23 },
    { FLAG_BADGE04_GET, 29 },
    { FLAG_BADGE05_GET, 31 },
    { FLAG_BADGE06_GET, 33 },
    { FLAG_BADGE07_GET, 42 },
    { FLAG_BADGE08_GET, 46 },
    { FLAG_IS_CHAMPION, 58 },
};

u8 Ctr3dsCurrentLevelCap(void)
{
    u32 i;

    if (Ctr3dsGetLevelCap() == CTR_CAP_OFF)
        return MAX_LEVEL;

    // FlagGet reads &gSaveBlock1Ptr->flags[...], and that pointer is NULL until
    // a file loads (src/load_save.c). The bottom screen polls this through its
    // repaint hash from the first frame. Without this check, the badge loop
    // below reads through NULL, which is a data abort on a real 3DS.
    //
    // No save means no badges, so there is no cap yet.
    if (gSaveBlock1Ptr == NULL)
        return MAX_LEVEL;

    for (i = 0; i < ARRAY_COUNT(sLevelCaps); i++)
    {
        if (!FlagGet(sLevelCaps[i].flag))
            return sLevelCaps[i].cap;
    }

    return MAX_LEVEL;
}

bool8 Ctr3dsHardCapBlocks(u8 level)
{
    if (Ctr3dsGetLevelCap() != CTR_CAP_HARD)
        return FALSE;

    return level >= Ctr3dsCurrentLevelCap();
}

u32 Ctr3dsSoftCapExp(u8 level, u32 exp)
{
    // Each level above the cap halves the exp again, and a little more. Five
    // steps make overleveling useless but never reach zero. That is the
    // difference between SOFT and HARD.
    static const u8 sDivisors[] = { 4, 8, 16, 32, 64 };
    u32 over;

    if (Ctr3dsGetLevelCap() != CTR_CAP_SOFT)
        return exp;

    if (level < Ctr3dsCurrentLevelCap())
        return exp;

    over = level - Ctr3dsCurrentLevelCap();
    if (over >= ARRAY_COUNT(sDivisors))
        over = ARRAY_COUNT(sDivisors) - 1;

    exp /= sDivisors[over];

    // Never zero: "gained 0 EXP" looks like a bug. The game does the same in
    // Cmd_getexp: `if (*exp == 0) *exp = 1;`.
    return exp != 0 ? exp : 1;
}

u32 Ctr3dsClampCappedExp(u16 species, u32 exp)
{
    u32 ceiling;
    u8 cap = Ctr3dsCurrentLevelCap();

    if (cap >= MAX_LEVEL || species == SPECIES_NONE || species > SPECIES_CHIMECHO)
        return exp;

    // The gExperienceTables array is [growthRate][MAX_LEVEL + 1], so an index
    // by the cap is always in bounds. Clamp the total, not the gain, as
    // TryIncrementMonLevel does at MAX_LEVEL.
    ceiling = gExperienceTables[gSpeciesInfo[species].growthRate][cap];

    return exp > ceiling ? ceiling : exp;
}

// ---- Randomizer ------------------------------------------------------------

// The valid species are 1..SPECIES_CELEBI and
// SPECIES_TREECKO..SPECIES_CHIMECHO. The 25 slots between them are
// SPECIES_OLD_UNOWN_B..Z: placeholders named "?", not real Pokemon. Nothing at
// or above SPECIES_EGG is a species. The SPECIES_UNOWN_B+ ids are graphics ids,
// and gSpeciesInfo overreads with them.
#define VALID_SPECIES_COUNT (SPECIES_CELEBI + (SPECIES_CHIMECHO - SPECIES_TREECKO + 1))

static u16 SpeciesFromIndex(u32 index)
{
    if (index < SPECIES_CELEBI)
        return (u16)(index + 1);

    return (u16)(index - SPECIES_CELEBI + SPECIES_TREECKO);
}

// The field HMs that the main story needs. Fly is not here: it is never
// necessary, and without it the mapping has more choice. CanSpeciesLearnTMHM
// takes the item's offset from ITEM_TM01, as in party_menu.c.
static u32 FieldHmMask(u16 species)
{
    static const u8 sFieldHms[] =
    {
        ITEM_HM01 - ITEM_TM01,   // Cut
        ITEM_HM03 - ITEM_TM01,   // Surf
        ITEM_HM04 - ITEM_TM01,   // Strength
        ITEM_HM05 - ITEM_TM01,   // Flash
        ITEM_HM06 - ITEM_TM01,   // Rock Smash
        ITEM_HM07 - ITEM_TM01,   // Waterfall
        ITEM_HM08 - ITEM_TM01,   // Dive
    };
    u32 mask = 0;
    u32 i;

    for (i = 0; i < ARRAY_COUNT(sFieldHms); i++)
    {
        if (CanSpeciesLearnTMHM(species, sFieldHms[i]))
            mask |= 1 << i;
    }

    return mask;
}

// The save's own trainer ID, not a stored seed. Thus the mapping is stable for
// one playthrough and different between playthroughs. A toggle off and on does
// not change it, and nothing must persist.
static u32 RandomizerSeed(void)
{
    const u8 *id;

    // The same risk as the level cap above: this pointer is NULL until a file
    // exists. Nothing creates Pokemon that early, but one compare prevents a
    // crash that shows only on hardware.
    if (gSaveBlock2Ptr == NULL)
        return 0;

    id = gSaveBlock2Ptr->playerTrainerId;

    return (u32)id[0] | ((u32)id[1] << 8) | ((u32)id[2] << 16) | ((u32)id[3] << 24);
}

u16 Ctr3dsMapSpecies(u16 species)
{
    // Enough tries that the HM rule below almost always finds a candidate, and
    // few enough that the loop has a limit.
    enum { MAX_TRIES = 16 };

    u32 need;
    u32 h;
    u32 i;

    if (!Ctr3dsGetRandomizer())
        return species;

    if (species == SPECIES_NONE || species > SPECIES_CHIMECHO)
        return species;

    // Without a save there is no trainer ID, so there is no stable mapping.
    // Return the original species. A seed of 0 would not agree with later
    // calls.
    if (gSaveBlock2Ptr == NULL)
        return species;

    // The softlock guard. Surf, Waterfall and Dive block the story, and Cut,
    // Strength, Rock Smash and Flash block large parts of the map. The
    // replacement must learn every field HM that the original can learn. Thus,
    // where the original game gave a mon that can learn a field HM, the
    // randomizer does too.
    //
    // Items are never randomized, so the HMs, the badges and all key items are
    // always safe.
    need = FieldHmMask(species);

    // ISO_RANDOMIZE on a local variable, as for sFeebasRngValue in
    // src/wild_encounter.c. It does not touch gRngValue or gRng2Value, so
    // battle and link RNG do not change.
    h = RandomizerSeed() ^ (species * 2654435761u);

    for (i = 0; i < MAX_TRIES; i++)
    {
        u16 candidate;

        h = ISO_RANDOMIZE1(h);
        h = ISO_RANDOMIZE2(h);

        // The low bits of a linear congruential generator are the weakest. Take
        // the index from higher bits.
        candidate = SpeciesFromIndex((h >> 8) % VALID_SPECIES_COUNT);

        if ((FieldHmMask(candidate) & need) == need)
            return candidate;
    }

    // No candidate qualified in MAX_TRIES draws. That is not rare. The HM mask
    // must match, so a species that learns many field HMs has a very small
    // pool. For example, Tentacool's pool is 12 of 386, and 16 random draws
    // miss it 60% of the time.
    //
    // Thus pick from the pool directly: count the candidates, then take the
    // nth. A species always covers its own mask, so the count is never zero.
    //
    // Do not scan forward from the hash and take the first match. That favors
    // the species after a long gap in the pool: in Tentacool's pool, one
    // species got 60% of the seeds. A count makes each member equally likely,
    // like the draws above.
    //
    // Every candidate here passes the same mask test as the loop above, so the
    // HM guarantee stays.
    {
        u32 count = 0;
        u32 pick;

        for (i = 0; i < VALID_SPECIES_COUNT; i++)
        {
            if ((FieldHmMask(SpeciesFromIndex(i)) & need) == need)
                count++;
        }

        pick = (h >> 8) % count;

        for (i = 0; i < VALID_SPECIES_COUNT; i++)
        {
            u16 candidate = SpeciesFromIndex(i);

            if ((FieldHmMask(candidate) & need) == need && pick-- == 0)
                return candidate;
        }
    }

    // This cannot occur: the pool always contains the original, so the pick
    // always lands. Keep a return for safety.
    return species;
}

u16 Ctr3dsMapWildSpecies(u16 species)
{
    // The Battle Pike and the Battle Pyramid do not store a species in their
    // wild tables. They store a 1-based index into a second table and create
    // the mon with it. Then they read the index back with GetMonData(...) - 1
    // and write the real species (src/battle_pike.c, src/battle_pyramid.c). A
    // random species id there reads out of bounds, so these two places are not
    // randomized. Both tests only read gMapHeader and are safe during encounter
    // generation.
    if (InBattlePike() || InBattlePyramid_())
        return species;

    return Ctr3dsMapSpecies(species);
}

// ---- Shiny test switch -----------------------------------------------------
//
// Make the next wild encounter shiny, to test the bottom screen's shiny notice.
// The real odds are one encounter in 8192.
//
// This function creates the mon. Do not edit a mon that already exists. In Gen
// 3, the personality sets the substructure order (GetSubstruct) and is half the
// encryption key (EncryptBoxMon, keyed on otId ^ personality). SetMonData does
// not encrypt again for MON_DATA_PERSONALITY, because that field is below
// MON_DATA_ENCRYPT_SEPARATOR. A new personality in a finished mon thus reads
// the substructs with the wrong key. The checksum fails, and the game marks the
// mon a Bad Egg.
bool8 Ctr3dsTryCreateShinyTestMon(u16 species, u8 level)
{
    // There is no target parameter. The only slot that this can fill is the
    // wild encounter's, so the function names that slot itself. No caller can
    // aim it at gPlayerParty. CreateWildMon uses the same fixed slot in both of
    // its calls.
    struct Pokemon *mon = &gEnemyParty[0];
    u32 otId, personality;
    u16 lo, hi;

    if (!Ctr3dsGetShinyTest())
        return FALSE;

    // The same risk as RandomizerSeed() above: NULL until a file exists. Stay
    // armed, so a switch set on the title screen is not lost.
    if (gSaveBlock2Ptr == NULL)
        return FALSE;

    // The Battle Pike and the Battle Pyramid also reach CreateWildMon. Exclude
    // them for the same reason as Ctr3dsMapWildSpecies: their tables hold a
    // 1-based index, and the real species is written after (src/battle_pike.c,
    // src/battle_pyramid.c). Thus the mon made here is not the mon that the
    // player meets. Also, nothing in the Frontier can be caught, so the notice
    // does not appear. Stay armed, so the Pyramid does not use up the switch.
    if (InBattlePike() || InBattlePyramid_())
        return FALSE;

    // A wild encounter only, never a trainer battle. CreateWildMon is static to
    // src/wild_encounter.c and has no callers outside it, so a trainer battle
    // cannot reach this function. CreateNPCTrainerParty (src/battle_main.c)
    // builds trainer parties with OT_ID_RANDOM_NO_SHINY, so a trainer's Pokemon
    // is never shiny.
    //
    // Check the timing. Every wild encounter is made in the overworld before
    // the battle starts (gMain.inBattle is set later, in src/battle_main.c). If
    // a battle is already running, the caller is not a wild encounter, so stay
    // armed.
    if (gMain.inBattle)
        return FALSE;

    // Never an existing Pokemon, and never one of the player's. The arrays
    // gPlayerParty and gEnemyParty are separate, and the only write below is
    // CreateMon into the enemy slot above. Thus the player's team cannot be
    // reached from here. Nor can a box mon, a gift, an egg or the roamer, which
    // BattleSetup_StartRoamerBattle builds on another path.
    //
    // Still check that the slot is empty. CreateWildMon calls
    // ZeroEnemyPartyMons() just before this, so it is empty today. The check
    // stops a future caller from replacing a live Pokemon.
    //
    // This read does not change the mon. GetBoxMonData decrypts only for fields
    // above MON_DATA_ENCRYPT_SEPARATOR, and this field is below it.
    if (GetMonData(mon, MON_DATA_SANITY_HAS_SPECIES))
        return FALSE;

    // The same trainer ID that CreateBoxMon gives the mon under OT_ID_PLAYER_ID
    // (src/pokemon.c). Shininess depends on the pair, so one value must be
    // known to choose the other.
    otId = T1_READ_32(gSaveBlock2Ptr->playerTrainerId);

    // Solve GET_SHINY_VALUE(otId, p) == 0 for the high half. The low half stays
    // random, so gender, the ability slot and the nature still vary between
    // test shinies.
    //
    // Calculate the value; do not search for it. A search would need about 8192
    // Random32 calls on average, which moves the RNG and changes what the game
    // does next. A test switch must not change the run that it tests.
    lo = Random();
    hi = (u16)(HIHALF(otId) ^ LOHALF(otId) ^ lo);
    personality = ((u32)hi << 16) | lo;

    CreateMon(mon, species, level, USE_RANDOM_IVS, TRUE, personality,
              OT_ID_PLAYER_ID, 0);

    // One shot, as the button says. Clear it here, because only this function
    // knows that the encounter occurred. A switch that stays armed makes every
    // wild mon shiny.
    Ctr3dsSetShinyTest(0);

    return TRUE;
}

// ---- Bag sort --------------------------------------------------------------

static void SwapSlots(struct ItemSlot *a, struct ItemSlot *b)
{
    struct ItemSlot temp;

    SWAP(*a, *b, temp);
}

// Item names are game-encoded and do not always end with EOS. The field is a
// fixed u8[ITEM_NAME_LENGTH], and a name of maximum length fills it. Thus the
// end of the array is also the end of the string.
//
// EOS is 0xFF, which sorts after every letter. As a normal byte, "POTION" would
// sort after "POTIONS". Thus test for EOS explicitly.
static bool8 NameSortsFirst(u16 a, u16 b)
{
    const u8 *pa = GetItemName(a);
    const u8 *pb = GetItemName(b);
    u32 i;

    for (i = 0; i < ITEM_NAME_LENGTH; i++)
    {
        if (pa[i] == EOS && pb[i] == EOS)
            return FALSE;
        if (pa[i] == EOS)
            return TRUE;
        if (pb[i] == EOS)
            return FALSE;
        if (pa[i] != pb[i])
            return pa[i] < pb[i];
    }

    return FALSE;
}

void Ctr3dsSortBagPocket(u8 pocketId)
{
    struct BagPocket *pocket;
    u16 count;
    u16 i, j;
    int mode = Ctr3dsGetBagSort();

    if (mode == CTR_BAGSORT_OFF || pocketId >= POCKETS_COUNT)
        return;

    pocket = &gBagPockets[pocketId];

    // Move the empty slots to the end first, so the used items are a prefix and
    // neither sort below must check for empty slots. The end of the list is
    // then found by itemId. GetBagItemQuantity is static to src/item.c, so the
    // encrypted quantity cannot be read here.
    CompactItemsInBagPocket(pocket);

    if (mode == CTR_BAGSORT_TYPE)
    {
        // Item id order, which is category order in the game: medicine, then
        // balls, then battle items, and so on. The game already does this to
        // the TM and berry pockets at each bag open.
        SortBerriesOrTMHMs(pocket);
        return;
    }

    for (count = 0; count < pocket->capacity && pocket->itemSlots[count].itemId != ITEM_NONE; count++)
        ;

    // A selection sort, like SortBerriesOrTMHMs. A pocket holds a few dozen
    // slots at most, and this runs on a bag open, not on a repaint.
    for (i = 0; i + 1 < count; i++)
    {
        for (j = i + 1; j < count; j++)
        {
            if (NameSortsFirst(pocket->itemSlots[j].itemId, pocket->itemSlots[i].itemId))
                SwapSlots(&pocket->itemSlots[i], &pocket->itemSlots[j]);
        }
    }
}

void Ctr3dsSortBagNow(void)
{
    u8 i;

    // The game's own bag code calls Ctr3dsSortBagPocket above, at a safe time.
    // CtrBottomUpdate calls this one between frames, with no such guarantee.
    // Thus it has the same gate as the BAG tab's item use. A sort while the
    // game's bag is open moves an item under the cursor. A sort during a script
    // can conflict with the script.
    if (gMain.inBattle)
        return;
    if (gMain.callback2 != CB2_Overworld)
        return;
    if (ArePlayerFieldControlsLocked())
        return;
    if (ScriptContext_IsEnabled())
        return;

    for (i = 0; i < POCKETS_COUNT; i++)
        Ctr3dsSortBagPocket(i);
}
