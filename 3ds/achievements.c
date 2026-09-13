// Achievements: what they are, when they unlock, and the built-in provider the
// bottom screen reads them through (achievements.h).
//
// Every condition is read through the game's own accessors -- FlagGet,
// GetGameStat, the Pokedex counts -- for the reason SECOND_SCREEN_CHEATSHEET.md
// gives for the whole bottom screen: stats are XOR-encrypted and flags live
// behind gSaveBlock1Ptr, so a raw read could disagree with the game's own
// screens. Nothing here writes game state.
//
// This is a GAME-SIDE translation unit under the two-worlds rule in bridge.h:
// game headers plus bridge.h, never <3ds.h>. The unlocked bits are handed to
// the host through CtrAchStoreLoad/CtrAchStoreSave, which is where they are
// persisted per playthrough.
//
// Almost every achievement is a STATE -- a badge flag, a count that has passed
// a goal -- so it is polled rather than hooked, and src/ gains exactly one
// fenced line: the shiny catch, the one EVENT that leaves no state behind.

#include "global.h"
#include "event_data.h"               // FlagGet
#include "main.h"                     // gMain
#include "overworld.h"                // CB2_Overworld, GetGameStat
#include "pokedex.h"                  // GetHoennPokedexCount, GetSetPokedexFlag
#include "pokemon.h"                  // IsMonShiny, SpeciesToNationalPokedexNum
#include "constants/flags.h"
#include "constants/game_stat.h"
#include "constants/species.h"

#include "bridge.h"
#include "achievements.h"

// ---- the table ------------------------------------------------------------

enum
{
    ACH_FLAG,          // FlagGet(arg)
    ACH_FLAG_COUNT,    // how many of `count` flags from `arg`, `step` apart, are set
    ACH_STAT,          // GetGameStat(arg)
    ACH_DEX_HOENN,     // kinds owned in the Hoenn Pokedex
    ACH_CAUGHT,        // owns species `arg` or, if set, species `arg2`
    ACH_SECRET_BASE,   // the player's own base exists
    ACH_EVENT,         // unlocked only by a hook; `arg` says which
};

// ACH_EVENT ids, for the hooks to find their entry by.
#define ACH_EVENT_SHINY 1

struct AchDef
{
    const char *title;
    const char *desc;
    u8  kind;
    u8  step;          // ACH_FLAG_COUNT only
    u16 arg;
    u16 arg2;          // ACH_CAUGHT only: a second species that also counts
    u16 count;         // ACH_FLAG_COUNT only
    u32 goal;          // unlocked once the value reaches this
};

#define FLAG(t, d, f)            { t, d, ACH_FLAG, 0, f, 0, 0, 1 }
#define FLAGS(t, d, f, n, s, g)  { t, d, ACH_FLAG_COUNT, s, f, 0, n, g }
#define STAT(t, d, s, g)         { t, d, ACH_STAT, 0, s, 0, 0, g }
#define DEX_HOENN(t, d, g)       { t, d, ACH_DEX_HOENN, 0, 0, 0, 0, g }
#define CAUGHT(t, d, a, b)       { t, d, ACH_CAUGHT, 0, a, b, 0, 1 }
#define SECRET_BASE(t, d)        { t, d, ACH_SECRET_BASE, 0, 0, 0, 0, 1 }
#define EVENT(t, d, e)           { t, d, ACH_EVENT, 0, e, 0, 0, 1 }

// The game spells these with an e-acute. UiAscii() turns the UTF-8 pair into
// the game's own glyph; octal rather than \x so a following hex letter ("d" in
// dex, "b" in blocks) cannot be swallowed into the escape.
#define POKEMON     "Pok\303\251mon"
#define POKEDEX     "Pok\303\251dex"
#define POKEBLOCKS  "Pok\303\251blocks"

// APPEND ONLY. An achievement's position here is its bit in achievements.bin,
// so reordering, inserting or deleting moves every saved unlock after it. Retire
// an entry by leaving it in place.
//
// Titles must fit the TROPHY tab's title line and descriptions its second line
// (TROPHY_TITLE_MAX_W, TROPHY_DESC_MAX_W in 3ds/ui/tab_trophy.c). There is no
// clipping on this screen, so the debug page counts any that do not fit.
//
// Everything here can be earned in this port as it stands. That rules out the
// complete Hoenn Pokedex and anything past about 200 in the National one: both
// need trade evolutions, and trading waits on the Cable Club
// (local-wireless branch).
static const struct AchDef sAchDefs[] =
{
    // Gym badges.
    FLAG("Stone Badge",    "Beat Roxanne in Rustboro City",    FLAG_BADGE01_GET),
    FLAG("Knuckle Badge",  "Beat Brawly in Dewford Town",      FLAG_BADGE02_GET),
    FLAG("Dynamo Badge",   "Beat Wattson in Mauville City",    FLAG_BADGE03_GET),
    FLAG("Heat Badge",     "Beat Flannery in Lavaridge Town",  FLAG_BADGE04_GET),
    FLAG("Balance Badge",  "Beat your father in Petalburg",    FLAG_BADGE05_GET),
    FLAG("Feather Badge",  "Beat Winona in Fortree City",      FLAG_BADGE06_GET),
    FLAG("Mind Badge",     "Beat Tate and Liza in Mossdeep",   FLAG_BADGE07_GET),
    FLAG("Rain Badge",     "Beat Juan in Sootopolis City",     FLAG_BADGE08_GET),

    // The story.
    FLAG("Rival No More",  "Beat Wally at Victory Road",       FLAG_DEFEATED_WALLY_VICTORY_ROAD),
    FLAG("Champion",       "Enter the Hall of Fame",           FLAG_SYS_GAME_CLEAR),
    FLAG("Wider World",    "Get the National " POKEDEX,        FLAG_SYS_NATIONAL_DEX),

    // Legends. The FLAG_DEFEATED_* flags are set whether the encounter ends in
    // a catch or a knockout (the scripts set them on both branches), so these
    // are about facing the legend, and they still work with the randomiser on.
    FLAG("Sky High",       "Face Rayquaza atop Sky Pillar",    FLAG_DEFEATED_RAYQUAZA),
    FLAG("Terra Firma",    "Face Groudon in the Terra Cave",   FLAG_DEFEATED_GROUDON),
    FLAG("Deep Blue",      "Face Kyogre in the Marine Cave",   FLAG_DEFEATED_KYOGRE),
    FLAG("Rock Solid",     "Face Regirock in the Desert Ruins", FLAG_DEFEATED_REGIROCK),
    FLAG("Cold Snap",      "Face Regice in the Island Cave",   FLAG_DEFEATED_REGICE),
    FLAG("Iron Will",      "Face Registeel in the Ancient Tomb", FLAG_DEFEATED_REGISTEEL),
    // The roaming one sets no flag when caught (only the Southern Island event
    // does, and that needs the Eon Ticket), so this asks the Pokedex instead.
    CAUGHT("Eon Chaser",   "Catch the roaming Latias or Latios", SPECIES_LATIAS, SPECIES_LATIOS),
    FLAG("Odd Tree",       "Deal with the tree by the Frontier", FLAG_DEFEATED_SUDOWOODO),

    // The Pokedex.
    DEX_HOENN("Researcher",     "Own 25 kinds in the Hoenn " POKEDEX,  25),
    DEX_HOENN("Field Worker",   "Own 50 kinds in the Hoenn " POKEDEX,  50),
    DEX_HOENN("Dex Enthusiast", "Own 100 kinds in the Hoenn " POKEDEX, 100),
    DEX_HOENN("Dex Expert",     "Own 150 kinds in the Hoenn " POKEDEX, 150),

    // Catching and raising.
    STAT("Gotcha!",        "Catch your first wild " POKEMON,   GAME_STAT_POKEMON_CAPTURES, 1),
    STAT("Collector",      "Catch 100 wild " POKEMON,          GAME_STAT_POKEMON_CAPTURES, 100),
    STAT("First Bite",     "Hook something while fishing",     GAME_STAT_FISHING_ENCOUNTERS, 1),
    STAT("Hatchling",      "Hatch an Egg",                     GAME_STAT_HATCHED_EGGS, 1),
    STAT("Breeder",        "Hatch 30 Eggs",                    GAME_STAT_HATCHED_EGGS, 30),
    STAT("Growing Up",     "See 50 evolutions",                GAME_STAT_EVOLVED_POKEMON, 50),
    EVENT("Shining Star",  "Catch a shiny " POKEMON,           ACH_EVENT_SHINY),

    // Battling.
    STAT("Seasoned",       "Fight 100 trainer battles",        GAME_STAT_TRAINER_BATTLES, 100),
    STAT("Nothing Happened", "Use Splash in battle",           GAME_STAT_USED_SPLASH, 1),

    // Around Hoenn.
    STAT("Marathon",       "Walk 100,000 steps",               GAME_STAT_STEPS, 100000),
    STAT("Cable Car",      "Ride the cable car up Mt. Chimney", GAME_STAT_RODE_CABLE_CAR, 1),
    STAT("Hot Springs",    "Soak in the Lavaridge hot springs", GAME_STAT_ENTERED_HOT_SPRINGS, 1),
    STAT("On Safari",      "Enter the Safari Zone",            GAME_STAT_ENTERED_SAFARI_ZONE, 1),
    STAT("Green Thumb",    "Plant 25 berries",                 GAME_STAT_PLANTED_BERRIES, 25),
    STAT("Blender",        "Make 25 " POKEBLOCKS,              GAME_STAT_POKEBLOCKS, 25),
    SECRET_BASE("Home Base", "Set up a Secret Base"),
    STAT("Lucky Number",   "Win the Lilycove lottery",         GAME_STAT_WON_POKEMON_LOTTERY, 1),
    STAT("Jackpot!",       "Hit a jackpot at the Game Corner", GAME_STAT_SLOT_JACKPOTS, 1),

    // Contests.
    STAT("Star Performer", "Win a " POKEMON " Contest",        GAME_STAT_WON_CONTEST, 1),
    STAT("Ribbon Collector", "Earn 10 ribbons",                GAME_STAT_RECEIVED_RIBBONS, 10),

    // The Battle Frontier. The seven Silver flags run from FLAG_SYS_TOWER_SILVER
    // two apart, each facility's Gold straight after its Silver.
    STAT("Tower Climber",  "Win 7 in a row at the Battle Tower", GAME_STAT_BATTLE_TOWER_SINGLES_STREAK, 7),
    FLAGS("Silver Symbol", "Earn a Frontier Silver Symbol",    FLAG_SYS_TOWER_SILVER, 7, 2, 1),
    FLAGS("Silver Set",    "Earn all 7 Silver Symbols",        FLAG_SYS_TOWER_SILVER, 7, 2, 7),
    FLAGS("Gold Symbol",   "Earn a Frontier Gold Symbol",      FLAG_SYS_TOWER_GOLD, 7, 2, 1),
    FLAGS("Frontier Legend", "Earn all 7 Gold Symbols",        FLAG_SYS_TOWER_GOLD, 7, 2, 7),
};

#define ACH_COUNT ((u16)ARRAY_COUNT(sAchDefs))

STATIC_ASSERT(ARRAY_COUNT(sAchDefs) <= CTR_ACH_BYTES * 8, AchTableFitsTheStore);

// ---- state ----------------------------------------------------------------

static u8    sUnlocked[CTR_ACH_BYTES];
static u8    sUnseen[CTR_ACH_BYTES];
static u32   sPlayerId;
static bool8 sLive;        // a playthrough has been adopted
static u16   sCursor;      // the next definition AchTick evaluates

// Notifications waiting for the toast. Small, because it only has to hold what
// can unlock between two toasts; when it is full the newest entry grows into a
// batch instead of anything being dropped.
#define TOAST_QUEUE 8

static struct { u16 index, batch; } sToasts[TOAST_QUEUE];
static u8 sToastHead, sToastLen;

static bool8 BitGet(const u8 *bits, u16 i)
{
    return (bits[i / 8] >> (i % 8)) & 1;
}

static void BitSet(u8 *bits, u16 i)
{
    bits[i / 8] |= (u8)(1 << (i % 8));
}

// The bottom screen's SaveDataLive(), for the same reason: both pointers start
// NULL and every FlagGet goes through gSaveBlock1Ptr.
static bool8 SaveLive(void)
{
    return gSaveBlock1Ptr != NULL && gSaveBlock2Ptr != NULL;
}

static u32 PlayerId(void)
{
    return T1_READ_32(gSaveBlock2Ptr->playerTrainerId);
}

// TRUE while the save in memory is the playthrough being scored. Everything
// that reads a condition asks this first, and it is not the same question as
// sLive: the save block changes owner without passing through the overworld.
//
//   New Game: the trainer ID changes in Birch's speech, over the old save's
//   flags, until NewGameInitData() clears them.
//
//   Soft reset: the intro reloads the save on the card, which need not be the
//   playthrough just being played. Start a new game, never save it, reset, and
//   the old save is back in memory under the new game's record.
//
// In both, the ID in the save block stops matching the adopted one, so nothing
// is evaluated until the next overworld frame adopts whoever it now is.
static bool8 Current(void)
{
    return sLive && SaveLive() && PlayerId() == sPlayerId;
}

static void QueueToast(u16 index, u16 batch)
{
    if (sToastLen == TOAST_QUEUE)
    {
        sToasts[(sToastHead + sToastLen - 1) % TOAST_QUEUE].batch += batch;
        return;
    }

    sToasts[(sToastHead + sToastLen) % TOAST_QUEUE].index = index;
    sToasts[(sToastHead + sToastLen) % TOAST_QUEUE].batch = batch;
    sToastLen++;
}

static void Store(void)
{
    CtrAchStoreSave(sPlayerId, sUnlocked, sUnseen);
}

// ---- evaluation -----------------------------------------------------------

static bool8 Owns(u16 species)
{
    return species != SPECIES_NONE
        && GetSetPokedexFlag(SpeciesToNationalPokedexNum(species), FLAG_GET_CAUGHT);
}

// How far along a definition is right now. Only ever called while Current():
// there is save data to read, and it belongs to the adopted playthrough.
static u32 Value(const struct AchDef *d)
{
    u32 n = 0;

    switch (d->kind)
    {
    case ACH_FLAG:
        return FlagGet(d->arg) ? 1 : 0;

    case ACH_FLAG_COUNT:
        for (u16 i = 0; i < d->count; i++)
            if (FlagGet(d->arg + i * d->step))
                n++;
        return n;

    case ACH_STAT:
        return GetGameStat(d->arg);

    case ACH_DEX_HOENN:
        return GetHoennPokedexCount(FLAG_GET_CAUGHT);

    case ACH_CAUGHT:
        return (Owns(d->arg) || Owns(d->arg2)) ? 1 : 0;

    case ACH_SECRET_BASE:
        return gSaveBlock1Ptr->secretBases[0].secretBaseId != 0 ? 1 : 0;

    default:                          // ACH_EVENT: nothing to read
        return 0;
    }
}

static bool8 Met(const struct AchDef *d)
{
    return Value(d) >= d->goal;
}

static void Unlock(u16 i)
{
    BitSet(sUnlocked, i);
    BitSet(sUnseen, i);
    QueueToast(i, 1);
    Store();
}

// Every definition at once, unlocking whatever is already true without a
// notification each, then one for the lot. Returns how many.
//
// This is what makes a first load fair: a save that already has five badges
// gets them straight away, as one "5 achievements unlocked", rather than five
// toasts in a row or nothing at all.
static u16 CatchUp(void)
{
    u16 n = 0, first = 0;

    for (u16 i = 0; i < ACH_COUNT; i++)
    {
        if (BitGet(sUnlocked, i) || !Met(&sAchDefs[i]))
            continue;

        if (n == 0)
            first = i;

        BitSet(sUnlocked, i);
        BitSet(sUnseen, i);
        n++;
    }

    if (n > 0)
        QueueToast(first, n);

    return n;
}

// Start keeping score for the playthrough whose save is loaded.
//
// Only ever called on a CB2_Overworld frame, and that is load bearing. A New
// Game gets its trainer ID in the middle of Birch's speech, while the previous
// save's flags are still in the save block until NewGameInitData() clears them
// at the end of it. Adopting the new ID then would credit the new playthrough
// with everything the old one had done. By the first overworld frame the save
// block is the new game's, whichever way it was reached.
static void Adopt(u32 id)
{
    bool8 known;

    sPlayerId = id;
    sLive = TRUE;
    sCursor = 0;

    // Anything still waiting belonged to the playthrough before.
    sToastHead = 0;
    sToastLen = 0;

    known = CtrAchStoreLoad(id, sUnlocked, sUnseen) != 0;

    // A record that is missing, or older than the save (a write lost to a
    // closed lid, say), is caught up the same way.
    if (CatchUp() > 0 || !known)
        Store();
}

void AchTick(void)
{
    if (!SaveLive())
        return;

    if (gMain.callback2 == CB2_Overworld && !Current())
        Adopt(PlayerId());

    if (!Current())
        return;

    // One definition a frame: about fifty frames for the whole table, and a
    // constant cost per frame however many there are.
    if (sCursor >= ACH_COUNT)
        sCursor = 0;

    if (!BitGet(sUnlocked, sCursor) && Met(&sAchDefs[sCursor]))
        Unlock(sCursor);

    sCursor++;
}

void Ctr3dsAchOnCaught(struct Pokemon *mon)
{
    if (!Current() || mon == NULL || !IsMonShiny(mon))
        return;

    for (u16 i = 0; i < ACH_COUNT; i++)
        if (sAchDefs[i].kind == ACH_EVENT && sAchDefs[i].arg == ACH_EVENT_SHINY
            && !BitGet(sUnlocked, i))
            Unlock(i);
}

// ---- the provider ---------------------------------------------------------

static u16 LocalCount(void)
{
    return ACH_COUNT;
}

static void LocalGet(u16 i, struct AchView *out)
{
    const struct AchDef *d = &sAchDefs[i];
    u32 value = 0;

    out->title = d->title;
    out->desc = d->desc;
    out->goal = d->goal;
    out->unlocked = sLive && BitGet(sUnlocked, i);
    out->unseen = sLive && BitGet(sUnseen, i);

    if (out->unlocked)
        value = d->goal;
    else if (Current())
        value = Value(d);

    out->progress = value > d->goal ? d->goal : value;
}

static u16 LocalUnlockedCount(void)
{
    u16 n = 0;

    if (!sLive)
        return 0;

    for (u16 i = 0; i < ACH_COUNT; i++)
        n += BitGet(sUnlocked, i);

    return n;
}

static bool8 LocalAnyUnseen(void)
{
    if (!sLive)
        return FALSE;

    for (u32 b = 0; b < CTR_ACH_BYTES; b++)
        if (sUnseen[b])
            return TRUE;

    return FALSE;
}

static void LocalMarkAllSeen(void)
{
    if (!LocalAnyUnseen())
        return;

    for (u32 b = 0; b < CTR_ACH_BYTES; b++)
        sUnseen[b] = 0;

    Store();
}

static bool8 LocalPopToast(u16 *index, u16 *batch)
{
    if (sToastLen == 0)
        return FALSE;

    *index = sToasts[sToastHead].index;
    *batch = sToasts[sToastHead].batch;
    sToastHead = (sToastHead + 1) % TOAST_QUEUE;
    sToastLen--;
    return TRUE;
}

static u32 LocalStateKey(void)
{
    return (u32)LocalUnlockedCount()
         | ((u32)LocalAnyUnseen() << 16)
         | ((u32)sLive << 17);
}

static const struct AchProvider sLocal =
{
    .count         = LocalCount,
    .get           = LocalGet,
    .unlockedCount = LocalUnlockedCount,
    .anyUnseen     = LocalAnyUnseen,
    .markAllSeen   = LocalMarkAllSeen,
    .popToast      = LocalPopToast,
    .stateKey      = LocalStateKey,
};

const struct AchProvider *AchActive(void)
{
    return &sLocal;
}

// ---- debug ----------------------------------------------------------------

void AchDebugTestToast(void)
{
    QueueToast(0, 1);
}

void AchDebugResync(void)
{
    if (!Current())
        return;

    for (u32 b = 0; b < CTR_ACH_BYTES; b++)
    {
        sUnlocked[b] = 0;
        sUnseen[b] = 0;
    }

    sCursor = 0;
    CatchUp();
    Store();
}
