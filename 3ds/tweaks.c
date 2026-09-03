// Gameplay tweaks: EXP All, a badge-based level cap, a species randomiser, and
// a persistent bag sort order.
//
// These are the port's first genuine cheats, and the line is worth drawing
// clearly. Everything else the EXTRA tab offers (fast-forward, top-screen
// scale, button binds, the show-all-tabs override) leaves the game playing
// exactly as it shipped. Every option here deliberately does not. They are
// opt-in, default off, and each one is a single toggle the player has to reach
// for.
//
// All the logic lives in this file so that the hooks inside src/ stay to one or
// two lines each, fenced with #if PLATFORM_3DS. That matters for a decomp: the
// less original source a port rewrites, the easier it stays to rebase.
//
// This is a GAME-SIDE translation unit under the two-worlds rule in bridge.h:
// game headers plus bridge.h, never <3ds.h>. src/siirtc.c is the precedent for
// reading a host setting from game code.

#include "global.h"
#include "battle_pike.h"
#include "battle_pyramid.h"
#include "event_data.h"
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

// ---- Level cap -------------------------------------------------------------

// Emerald's real gym leader ace levels, then the Elite Four. The cap is the
// level of the fight you are walking into, so it is keyed on the badge you have
// NOT yet earned: before any badge the cap is Roxanne's Nosepass at 15.
//
// Taken from rh-hideout/pokeemerald-expansion's src/caps.c, which is the
// widely-used implementation of this idea. This tree is vanilla pokeemerald and
// has no include/config/caps.h to inherit.
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

    // FlagGet resolves to &gSaveBlock1Ptr->flags[...], and that pointer is NULL
    // until a file is loaded (src/load_save.c). The bottom screen polls this
    // through its repaint hash from the very first frame, long before then, so
    // without this the badge sweep below is a null dereference. On a real 3DS
    // that is an instant data abort; Azahar let it pass.
    //
    // No save means no badges, and the honest answer to "what is the cap" is
    // that there is not one yet.
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
    // Each level past the cap costs another halving-and-then-some. Five steps is
    // enough that overlevelling stops being worth doing without ever reaching a
    // flat zero, which is what separates SOFT from HARD.
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

    // Never zero: "gained 0 EXP" reads as a bug rather than a rule. Mirrors the
    // game's own `if (*exp == 0) *exp = 1;` in Cmd_getexp.
    return exp != 0 ? exp : 1;
}

u32 Ctr3dsClampCappedExp(u16 species, u32 exp)
{
    u32 ceiling;
    u8 cap = Ctr3dsCurrentLevelCap();

    if (cap >= MAX_LEVEL || species == SPECIES_NONE || species > SPECIES_CHIMECHO)
        return exp;

    // gExperienceTables is [growthRate][MAX_LEVEL + 1], so indexing by the cap
    // is always in bounds. Clamping the TOTAL rather than the gain is what
    // TryIncrementMonLevel already does when it pins exp to the MAX_LEVEL entry.
    ceiling = gExperienceTables[gSpeciesInfo[species].growthRate][cap];

    return exp > ceiling ? ceiling : exp;
}

// ---- Randomiser ------------------------------------------------------------

// Valid species are 1..SPECIES_CELEBI and SPECIES_TREECKO..SPECIES_CHIMECHO.
// The 25 slots between them are SPECIES_OLD_UNOWN_B..Z, placeholders that have
// gSpeciesInfo entries but are named "?" and are not real Pokemon. Everything
// at or above SPECIES_EGG is not a species at all: the SPECIES_UNOWN_B+ ids are
// graphics pseudo-ids, and indexing gSpeciesInfo with any of them overreads.
#define VALID_SPECIES_COUNT (SPECIES_CELEBI + (SPECIES_CHIMECHO - SPECIES_TREECKO + 1))

static u16 SpeciesFromIndex(u32 index)
{
    if (index < SPECIES_CELEBI)
        return (u16)(index + 1);

    return (u16)(index - SPECIES_CELEBI + SPECIES_TREECKO);
}

// The field HMs that gate main-line progression. Fly is deliberately absent:
// it is a convenience, never a requirement, and leaving it out keeps the
// mapping freer. The index CanSpeciesLearnTMHM wants is the item's offset from
// ITEM_TM01, the same arithmetic party_menu.c uses.
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

// The save's own trainer ID. Using it rather than a stored seed means the
// mapping is stable for one playthrough, differs between playthroughs, and is
// unaffected by toggling the randomiser off and back on. Nothing has to be
// persisted for any of that to hold.
static u32 RandomizerSeed(void)
{
    const u8 *id;

    // Same hazard as the level cap above: this pointer is NULL until a file
    // exists. Nothing should be creating Pokemon that early, but the cost of
    // being sure is one compare against a crash that only shows on hardware.
    if (gSaveBlock2Ptr == NULL)
        return 0;

    id = gSaveBlock2Ptr->playerTrainerId;

    return (u32)id[0] | ((u32)id[1] << 8) | ((u32)id[2] << 16) | ((u32)id[3] << 24);
}

u16 Ctr3dsMapSpecies(u16 species)
{
    // Enough tries that the HM constraint below effectively always finds a
    // candidate, few enough that the loop is bounded whatever the data says.
    enum { MAX_TRIES = 16 };

    u32 need;
    u32 h;
    u32 i;

    if (!Ctr3dsGetRandomizer())
        return species;

    if (species == SPECIES_NONE || species > SPECIES_CHIMECHO)
        return species;

    // Without a save there is no trainer ID, so there is no stable mapping to
    // give. Returning the original keeps the identity rather than inventing a
    // seed of 0 that a later call would not reproduce.
    if (gSaveBlock2Ptr == NULL)
        return species;

    // The softlock guard. Surf, Waterfall and Dive gate progression outright,
    // and Cut, Strength, Rock Smash and Flash gate large parts of the map. A
    // mapping that stripped one of those from every obtainable species would be
    // unwinnable, so the replacement must be able to learn everything the
    // original could. The property this buys is easy to state: wherever vanilla
    // gave you a mon that could learn a field HM, so does the randomiser.
    //
    // Items are never randomised at all, so the HMs themselves, the badges and
    // every key item are safe by construction rather than by guard.
    need = FieldHmMask(species);

    // ISO_RANDOMIZE on a local, the idiom this codebase already uses for a
    // private stream (see sFeebasRngValue in src/wild_encounter.c). It touches
    // neither gRngValue nor gRng2Value, so battle and link RNG are unperturbed.
    h = RandomizerSeed() ^ (species * 2654435761u);

    for (i = 0; i < MAX_TRIES; i++)
    {
        u16 candidate;

        h = ISO_RANDOMIZE1(h);
        h = ISO_RANDOMIZE2(h);

        // The low bits of a linear congruential generator are the weakest, so
        // take the index from higher up the word.
        candidate = SpeciesFromIndex((h >> 8) % VALID_SPECIES_COUNT);

        if ((FieldHmMask(candidate) & need) == need)
            return candidate;
    }

    // Nothing qualified. Leaving the species alone is the only safe answer:
    // it is exactly what vanilla would have given, so it cannot break anything.
    return species;
}

u16 Ctr3dsMapWildSpecies(u16 species)
{
    // The Battle Pike and Battle Pyramid do not store a species in the species
    // field of their wild tables: they store a 1-BASED INDEX into a second
    // table, create the mon with it, then read it back with
    // GetMonData(...) - 1 and overwrite it with the real species
    // (src/battle_pike.c:1113, src/battle_pyramid.c:1360). Feeding those an
    // arbitrary species id is an out-of-bounds read, so they are excluded
    // rather than randomised. Both predicates are plain gMapHeader tests and
    // are safe to call during encounter generation.
    if (InBattlePike() || InBattlePyramid_())
        return species;

    return Ctr3dsMapSpecies(species);
}

// ---- Bag sort --------------------------------------------------------------

static void SwapSlots(struct ItemSlot *a, struct ItemSlot *b)
{
    struct ItemSlot temp;

    SWAP(*a, *b, temp);
}

// Item names are game-encoded and not necessarily EOS-terminated: the field is
// a fixed u8[ITEM_NAME_LENGTH] and a maximum-length name fills it. So the end
// of the array counts as end of string.
//
// EOS is 0xFF, which sorts ABOVE every letter, so it cannot be compared as an
// ordinary byte or "POTION" would sort after "POTIONS". It is tested for
// explicitly instead.
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

    // Push empty slots to the end first, so the used items are a prefix and
    // neither sort below has to reason about emptiness. This is also what makes
    // it safe to detect the end of the list by itemId: GetBagItemQuantity is
    // static to src/item.c, so the encrypted quantity is not readable here.
    CompactItemsInBagPocket(pocket);

    if (mode == CTR_BAGSORT_TYPE)
    {
        // Ascending item id, which is category order in Emerald: medicine,
        // then balls, then battle items, and so on. The game already applies
        // this to the TM and berry pockets on every bag open.
        SortBerriesOrTMHMs(pocket);
        return;
    }

    for (count = 0; count < pocket->capacity && pocket->itemSlots[count].itemId != ITEM_NONE; count++)
        ;

    // Selection sort, the same shape as SortBerriesOrTMHMs. A pocket holds at
    // most a few dozen slots and this runs on a bag open, not on a repaint.
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

    // Ctr3dsSortBagPocket above is called from inside the game's own bag code,
    // where the game knows what it is doing. This one is called from
    // CtrBottomUpdate, which runs between frames with no such guarantee, so it
    // carries the same gate the BAG tab's item use does. Re-ordering a pocket
    // while the in-game bag is open would slide an item out from under its
    // cursor, and mid-script it could contradict whatever the script is about
    // to do.
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
