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

    // Nothing qualified in MAX_TRIES draws, and that is not the rare event the
    // loop above assumes. The mask is preserved exactly, so a species that
    // learns most of the field HMs draws from a very small pool: Tentacool's
    // is 12 of 386, which 16 independent draws miss 60% of the time. Returning
    // the original here therefore left the species with the TIGHTEST pools as
    // the ones least likely to be randomised at all -- every water route kept
    // its Tentacool on most save files -- which is most of why the feature
    // looked like it was barely doing anything.
    //
    // So pick from the pool directly instead of giving up: count what qualifies,
    // then take the nth. A species always covers its own mask, so the count is
    // never zero and this always answers. It costs two passes over the species
    // list, but it only runs when the draws above have already failed.
    //
    // Chosen over the obvious "scan forward from where the hash landed and take
    // the first match", which is one pass but badly clumped: that hands the
    // whole gap after a long run of non-qualifying species to whichever mon
    // ends the run, and measured over Tentacool's 12-species pool it gave one
    // of them 60% of the seeds. Counting first makes every member of the pool
    // equally likely, which is the property the draws above have and the reason
    // they are still tried first.
    //
    // The guarantee is untouched: every candidate returned here passes the same
    // mask test the loop above applies, so wherever vanilla gave a mon that
    // could learn a field HM, this still does.
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

    // Unreachable: the pool above always contains at least the original itself,
    // so the pick always lands. Kept because a "cannot happen" is not a thing
    // to leave to a fallthrough.
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

// ---- Shiny test switch -----------------------------------------------------
//
// Makes the next wild encounter shiny, so the bottom screen's shiny notice can
// be exercised without waiting out the real odds of one encounter in 8192.
//
// It CREATES the mon rather than editing one the game already made, and that is
// not a preference. In Gen 3 the personality is both the substructure order
// (GetSubstruct, src/pokemon.c:71) and half the encryption key
// (EncryptBoxMon, :3541, keyed on otId ^ personality). SetMonData does not
// re-encrypt for MON_DATA_PERSONALITY -- the field is below
// MON_DATA_ENCRYPT_SEPARATOR, so SetBoxMonData's decrypt/encrypt bracket does
// not run for it at all (:4163). Writing a new personality into a finished mon
// therefore leaves four substructs encrypted under the old key and read back
// through the new one: the checksum fails and the game marks it a Bad Egg.
// Which is to say the obvious implementation quietly destroys the Pokemon it
// was asked to make shiny.
bool8 Ctr3dsTryCreateShinyTestMon(u16 species, u8 level)
{
    // The target is not a parameter, and that is the point. The only slot this
    // is ever allowed to fill is the wild encounter's, so it names that slot
    // itself rather than accepting one: there is no argument a caller could
    // pass that would aim this at gPlayerParty. CreateWildMon hardcodes the
    // same slot in both of its own creation calls, for the same reason.
    struct Pokemon *mon = &gEnemyParty[0];
    u32 otId, personality;
    u16 lo, hi;

    if (!Ctr3dsGetShinyTest())
        return FALSE;

    // Same hazard RandomizerSeed() guards above: NULL until a file exists.
    // Left armed rather than consumed, so arming it from the title screen is
    // not silently thrown away.
    if (gSaveBlock2Ptr == NULL)
        return FALSE;

    // The Battle Pike and Battle Pyramid reach CreateWildMon too, and they are
    // excluded for the same reason Ctr3dsMapWildSpecies excludes them plus one
    // more. Their wild tables hold a 1-BASED INDEX in the species field, not a
    // species: the mon is created with the index and the real species is
    // written over it afterwards (src/battle_pike.c:1113,
    // src/battle_pyramid.c:1360), so what this function would make is not the
    // Pokemon the player ends up facing. And nothing in the Frontier can be
    // caught, so the notice this switch exists to test would not appear anyway.
    // Staying armed rather than firing means walking through the Pyramid does
    // not silently spend the arm.
    if (InBattlePike() || InBattlePyramid_())
        return FALSE;

    // A WILD encounter, never a trainer battle. Structurally true and audited:
    // CreateWildMon is static to src/wild_encounter.c and has no callers
    // outside it, so a trainer battle cannot reach this function at all --
    // trainer parties are built by CreateNPCTrainerParty (src/battle_main.c),
    // which never comes near the wild path. Emerald backs that up from the
    // other side: those parties are created with OT_ID_RANDOM_NO_SHINY, which
    // rerolls the trainer's ID until the mon is NOT shiny, so an opposing
    // trainer's Pokemon cannot be shiny in this game whatever this switch does.
    //
    // The one thing not settled by where the code sits is WHEN it runs, so that
    // is checked. Every wild encounter is generated in the overworld before the
    // battle starts (gMain.inBattle is not set until src/battle_main.c:708), so
    // being in a battle here means something other than a wild encounter is
    // asking, and the arm is kept for a real one.
    if (gMain.inBattle)
        return FALSE;

    // NEVER an existing Pokemon, and never one of the player's. gPlayerParty and
    // gEnemyParty are separate arrays (src/pokemon.c:83-84) and the only write
    // below is CreateMon into the enemy slot named above, so the player's team
    // is not merely left alone, it is unreachable from here. Nor is anything
    // else that already exists: a box mon, a gift, an egg, or the roamer, which
    // lives in the save and is rebuilt by BattleSetup_StartRoamerBattle without
    // coming through this path at all.
    //
    // The slot itself is still checked for emptiness. CreateWildMon calls
    // ZeroEnemyPartyMons() immediately above this, so it is empty by
    // construction today; the check is what keeps a future caller from
    // silently replacing a live Pokemon with a different one.
    //
    // A pure read: GetBoxMonData only runs its decrypt/encrypt bracket for a
    // field ABOVE MON_DATA_ENCRYPT_SEPARATOR, and this one is below it, so
    // asking the question does not disturb the mon being asked about.
    if (GetMonData(mon, MON_DATA_SANITY_HAS_SPECIES))
        return FALSE;

    // The same trainer ID CreateBoxMon would give the mon a moment from now
    // under OT_ID_PLAYER_ID (src/pokemon.c), because shininess is a property of
    // the pair and we have to know one to choose the other.
    otId = T1_READ_32(gSaveBlock2Ptr->playerTrainerId);

    // Solve GET_SHINY_VALUE(otId, p) == 0 for the high half. The low half stays
    // random, so gender, the stored ability slot and the nature still vary
    // between test shinies instead of every one being the same Pokemon.
    //
    // Constructed, not searched. CreateBoxMon's OT_ID_RANDOM_NO_SHINY loop
    // spins Random32 until it does NOT get a shiny, which is cheap because it
    // almost always exits first time; inverting it would average 8192 spins and
    // drag the RNG far enough to change what the game does next. A test switch
    // must not alter the run it is being used to observe.
    lo = Random();
    hi = (u16)(HIHALF(otId) ^ LOHALF(otId) ^ lo);
    personality = ((u32)hi << 16) | lo;

    CreateMon(mon, species, level, USE_RANDOM_IVS, TRUE, personality,
              OT_ID_PLAYER_ID, 0);

    // One shot, which is what the button says. Cleared here rather than by the
    // UI because this is the only place that knows the encounter happened, and
    // an armed switch nobody disarms means every Zigzagoon is gold.
    Ctr3dsSetShinyTest(0);

    return TRUE;
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
