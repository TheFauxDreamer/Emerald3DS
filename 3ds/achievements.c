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
#include "pokemon.h"                  // GetMonData, IsMonShiny
#include "constants/flags.h"
#include "constants/game_stat.h"
#include "constants/maps.h"
#include "constants/species.h"

#include "bridge.h"
#include "achievements.h"

// ---- the tables -----------------------------------------------------------

enum
{
    ACH_FLAG,          // FlagGet(arg)
    ACH_FLAG_COUNT,    // how many of `count` flags from `arg`, `step` apart, are set
    ACH_FLAG_LIST,     // how many of the `count` flags in `list` are set
    ACH_STAT,          // GetGameStat(arg)
    ACH_DEX_HOENN,     // kinds owned in the Hoenn Pokedex
    ACH_CAUGHT,        // owns species `arg` or, if set, species `arg2`
    ACH_SECRET_BASE,   // the player's own base exists
    ACH_PARTY_COUNT,   // Pokemon in the party, eggs not counted
    ACH_PARTY_LEVEL,   // the highest level in the party
    ACH_EVENT,         // unlocked only by a hook; `arg` says which
};

// ACH_EVENT ids, for the hooks to find their entry by.
#define ACH_EVENT_SHINY 1

struct AchDef
{
    // The achievement's bit in achievements.bin. PERMANENT: an id is never
    // changed and never reused, because a saved unlock is nothing but this bit.
    // Row order is free -- it is only the display order -- but the id is not.
    u8  id;
    u8  kind;
    u8  step;          // ACH_FLAG_COUNT only
    const char *title;
    const char *desc;
    const u16 *list;   // ACH_FLAG_LIST only
    u16 arg;
    u16 arg2;          // ACH_CAUGHT only: a second species that also counts
    u16 count;         // ACH_FLAG_COUNT and ACH_FLAG_LIST
    u32 goal;          // unlocked once the value reaches this
};

#define FLAG(i, t, d, f) \
    { .id = i, .title = t, .desc = d, .kind = ACH_FLAG, .arg = f, .goal = 1 }
#define FLAGS(i, t, d, f, n, s, g) \
    { .id = i, .title = t, .desc = d, .kind = ACH_FLAG_COUNT, .arg = f, .count = n, .step = s, .goal = g }
#define FLAG_LIST(i, t, d, l, g) \
    { .id = i, .title = t, .desc = d, .kind = ACH_FLAG_LIST, .list = l, .count = ARRAY_COUNT(l), .goal = g }
#define STAT(i, t, d, s, g) \
    { .id = i, .title = t, .desc = d, .kind = ACH_STAT, .arg = s, .goal = g }
#define DEX_HOENN(i, t, d, g) \
    { .id = i, .title = t, .desc = d, .kind = ACH_DEX_HOENN, .goal = g }
#define CAUGHT(i, t, d, a, b) \
    { .id = i, .title = t, .desc = d, .kind = ACH_CAUGHT, .arg = a, .arg2 = b, .goal = 1 }
#define SECRET_BASE(i, t, d) \
    { .id = i, .title = t, .desc = d, .kind = ACH_SECRET_BASE, .goal = 1 }
#define PARTY_COUNT(i, t, d, g) \
    { .id = i, .title = t, .desc = d, .kind = ACH_PARTY_COUNT, .goal = g }
#define PARTY_LEVEL(i, t, d, g) \
    { .id = i, .title = t, .desc = d, .kind = ACH_PARTY_LEVEL, .goal = g }
#define EVENT(i, t, d, e) \
    { .id = i, .title = t, .desc = d, .kind = ACH_EVENT, .arg = e, .goal = 1 }

// The game spells these with an e-acute. UiAscii() turns the UTF-8 pair into
// the game's own glyph; octal rather than \x so a following hex letter ("d" in
// dex, "b" in blocks) cannot be swallowed into the escape.
#define POKEMON     "Pok\303\251mon"
#define POKEDEX     "Pok\303\251dex"
#define POKEBLOCKS  "Pok\303\251blocks"
#define POKENAV     "Pok\303\251Nav"

// All eight HMs. Their flags were numbered as the games were written, so they
// are scattered through the flag space, not a run FLAGS() could step along.
static const u16 sHmFlags[] =
{
    FLAG_RECEIVED_HM_CUT,      FLAG_RECEIVED_HM_FLY,
    FLAG_RECEIVED_HM_SURF,     FLAG_RECEIVED_HM_STRENGTH,
    FLAG_RECEIVED_HM_FLASH,    FLAG_RECEIVED_HM_ROCK_SMASH,
    FLAG_RECEIVED_HM_WATERFALL, FLAG_RECEIVED_HM_DIVE,
};

// Two tables, one per page of the TROPHY tab. Which table a row is in IS its
// section, so no row can be filed on the wrong page.
//
// IDS ARE PERMANENT. The next new achievement takes the next unused id (67 at
// the time of writing); a retired one leaves its id unused forever. The debug
// page counts duplicates, since C cannot check that at compile time.
//
// Titles must fit the TROPHY tab's title line and descriptions its second line
// (TROPHY_TITLE_MAX_W, TROPHY_DESC_MAX_W in 3ds/ui/tab_trophy.c). There is no
// clipping on this screen, so the debug page counts any that do not fit.
//
// Everything here can be earned in this port as it stands. That rules out the
// complete Hoenn Pokedex and anything past about 200 in the National one: both
// need trade evolutions, and trading waits on the Cable Club
// (local-wireless branch).

// MAIN: everything that can be done before the Hall of Fame, in story order.
static const struct AchDef sMainDefs[] =
{
    // The journey, in the order Emerald hands it out.
    FLAG(48, "A Journey Begins",   "Choose your first " POKEMON,      FLAG_SYS_POKEMON_GET),
    FLAG(49, "Friendly Rivalry",   "Beat your rival on Route 103",    FLAG_DEFEATED_RIVAL_ROUTE103),
    FLAG(50, "Field Research",     "Get a " POKEDEX " from Prof. Birch", FLAG_SYS_POKEDEX_GET),
    FLAG(51, "Hit the Ground Running", "Get the Running Shoes",       FLAG_SYS_B_DASH),
    FLAG(53, "Connected",          "Get a " POKENAV " in Rustboro",   FLAG_SYS_POKENAV_GET),
    FLAG(59, "A Cut Above",        "Get HM01 Cut in Rustboro",        FLAG_RECEIVED_HM_CUT),
    FLAG(52, "Special Delivery",   "Deliver Mr. Stone's letter to Steven", FLAG_DELIVERED_STEVEN_LETTER),
    FLAG(54, "Package Deal",       "Deliver the Devon Goods to Capt. Stern", FLAG_DELIVERED_DEVON_GOODS),
    FLAG(55, "Pedal Power",        "Get a Bike from Rydel in Mauville", FLAG_RECEIVED_BIKE),
    STAT(66, "In Good Hands",      "Leave a " POKEMON " at the Day Care", GAME_STAT_USED_DAYCARE, 1),
    PARTY_COUNT(56, "Full House",  "Have six " POKEMON " in your party", 6),
    STAT(57, "It's Evolving!",     "Evolve a " POKEMON " for the first time", GAME_STAT_EVOLVED_POKEMON, 1),
    FLAG(58, "Hot Pursuit",        "Stop Team Magma on Mt. Chimney",  FLAG_DEFEATED_EVIL_TEAM_MT_CHIMNEY),
    // A flag rather than the Pokedex, so it still counts when the randomiser
    // has turned the revived Lileep or Anorith into something else.
    FLAG(65, "Ancient History",    "Revive a fossil at Devon Corp.",  FLAG_RECEIVED_REVIVED_FOSSIL_MON),
    FLAG(60, "Surf's Up",          "Get HM03 Surf from Wally's father", FLAG_RECEIVED_HM_SURF),
    FLAG(61, "Take Flight",        "Get HM02 Fly on Route 119",       FLAG_RECEIVED_HM_FLY),
    // Emerald's one Master Ball is the item ball in Aqua Hideout B1F.
    FLAG(63, "The Best Ball",      "Find the Master Ball in the Aqua Hideout", FLAG_ITEM_AQUA_HIDEOUT_B1F_MASTER_BALL),
    FLAG_LIST(62, "HM Collector",  "Get all eight HMs",               sHmFlags, 8),

    // Gym badges.
    FLAG(0, "Stone Badge",         "Beat Roxanne in Rustboro City",   FLAG_BADGE01_GET),
    FLAG(1, "Knuckle Badge",       "Beat Brawly in Dewford Town",     FLAG_BADGE02_GET),
    FLAG(2, "Dynamo Badge",        "Beat Wattson in Mauville City",   FLAG_BADGE03_GET),
    FLAG(3, "Heat Badge",          "Beat Flannery in Lavaridge Town", FLAG_BADGE04_GET),
    FLAG(4, "Balance Badge",       "Beat your father in Petalburg",   FLAG_BADGE05_GET),
    FLAG(5, "Feather Badge",       "Beat Winona in Fortree City",     FLAG_BADGE06_GET),
    FLAG(6, "Mind Badge",          "Beat Tate and Liza in Mossdeep",  FLAG_BADGE07_GET),
    FLAG(7, "Rain Badge",          "Beat Juan in Sootopolis City",    FLAG_BADGE08_GET),

    // The end of the story.
    FLAG(8, "Rival No More",       "Beat Wally at Victory Road",      FLAG_DEFEATED_WALLY_VICTORY_ROAD),
    FLAG(9, "Champion",            "Enter the Hall of Fame",          FLAG_SYS_GAME_CLEAR),

    // Legends that can be faced before the Hall of Fame. The FLAG_DEFEATED_*
    // flags are set whether the encounter ends in a catch or a knockout (the
    // scripts set them on both branches), so these are about facing the legend,
    // and they still work with the randomiser on. Rayquaza is catchable once
    // the Sootopolis crisis is over (VAR_SKY_PILLAR_STATE reaches 2), and the
    // Regis once the Sealed Chamber is open, which needs only Dive.
    FLAG(11, "Sky High",           "Face Rayquaza atop Sky Pillar",   FLAG_DEFEATED_RAYQUAZA),
    FLAG(14, "Rock Solid",         "Face Regirock in the Desert Ruins", FLAG_DEFEATED_REGIROCK),
    FLAG(15, "Cold Snap",          "Face Regice in the Island Cave",  FLAG_DEFEATED_REGICE),
    FLAG(16, "Iron Will",          "Face Registeel in the Ancient Tomb", FLAG_DEFEATED_REGISTEEL),

    // The Pokedex.
    DEX_HOENN(19, "Researcher",     "Own 25 kinds in the Hoenn " POKEDEX,  25),
    DEX_HOENN(20, "Field Worker",   "Own 50 kinds in the Hoenn " POKEDEX,  50),
    DEX_HOENN(21, "Dex Enthusiast", "Own 100 kinds in the Hoenn " POKEDEX, 100),
    DEX_HOENN(22, "Dex Expert",     "Own 150 kinds in the Hoenn " POKEDEX, 150),

    // Catching and raising.
    STAT(23, "Gotcha!",            "Catch your first wild " POKEMON,  GAME_STAT_POKEMON_CAPTURES, 1),
    STAT(24, "Collector",          "Catch 100 wild " POKEMON,         GAME_STAT_POKEMON_CAPTURES, 100),
    STAT(25, "First Bite",         "Hook something while fishing",    GAME_STAT_FISHING_ENCOUNTERS, 1),
    STAT(26, "Hatchling",          "Hatch an Egg",                    GAME_STAT_HATCHED_EGGS, 1),
    STAT(27, "Breeder",            "Hatch 30 Eggs",                   GAME_STAT_HATCHED_EGGS, 30),
    STAT(28, "Growing Up",         "See 50 evolutions",               GAME_STAT_EVOLVED_POKEMON, 50),
    PARTY_LEVEL(64, "Maxed Out",   "Raise a " POKEMON " to level 100", 100),
    EVENT(29, "Shining Star",      "Catch a shiny " POKEMON,          ACH_EVENT_SHINY),

    // Battling.
    STAT(30, "Seasoned",           "Fight 100 trainer battles",       GAME_STAT_TRAINER_BATTLES, 100),
    STAT(31, "Nothing Happened",   "Use Splash in battle",            GAME_STAT_USED_SPLASH, 1),

    // Around Hoenn.
    STAT(32, "Marathon",           "Walk 100,000 steps",              GAME_STAT_STEPS, 100000),
    STAT(33, "Cable Car",          "Ride the cable car up Mt. Chimney", GAME_STAT_RODE_CABLE_CAR, 1),
    STAT(34, "Hot Springs",        "Soak in the Lavaridge hot springs", GAME_STAT_ENTERED_HOT_SPRINGS, 1),
    STAT(35, "On Safari",          "Enter the Safari Zone",           GAME_STAT_ENTERED_SAFARI_ZONE, 1),
    STAT(36, "Green Thumb",        "Plant 25 berries",                GAME_STAT_PLANTED_BERRIES, 25),
    STAT(37, "Blender",            "Make 25 " POKEBLOCKS,             GAME_STAT_POKEBLOCKS, 25),
    SECRET_BASE(38, "Home Base",   "Set up a Secret Base"),
    STAT(39, "Lucky Number",       "Win the Lilycove lottery",        GAME_STAT_WON_POKEMON_LOTTERY, 1),
    STAT(40, "Jackpot!",           "Hit a jackpot at the Game Corner", GAME_STAT_SLOT_JACKPOTS, 1),

    // Contests.
    STAT(41, "Star Performer",     "Win a " POKEMON " Contest",       GAME_STAT_WON_CONTEST, 1),
    STAT(42, "Ribbon Collector",   "Earn 10 ribbons",                 GAME_STAT_RECEIVED_RIBBONS, 10),
};

// POST-GAME: everything that only opens up after the Hall of Fame. Hidden until
// then (see sRevealed). The National Pokedex is Birch's reward for it; the
// abnormal weather that opens Terra Cave and Marine Cave only starts once
// FLAG_SYS_GAME_CLEAR is set (Route119_WeatherInstitute_2F/scripts.inc); the
// roaming Lati is released by it; and the S.S. Tidal to the Battle Frontier
// only sails after it.
static const struct AchDef sPostDefs[] =
{
    FLAG(10, "A Bigger Journey Begins", "Get the National " POKEDEX, FLAG_SYS_NATIONAL_DEX),
    FLAG(12, "Terra Firma",        "Face Groudon in the Terra Cave",  FLAG_DEFEATED_GROUDON),
    FLAG(13, "Deep Blue",          "Face Kyogre in the Marine Cave",  FLAG_DEFEATED_KYOGRE),
    // The roaming one sets no flag when caught (only the Southern Island event
    // does, and that needs the Eon Ticket), so this asks the Pokedex instead.
    CAUGHT(17, "Eon Chaser",       "Catch the roaming Latias or Latios", SPECIES_LATIAS, SPECIES_LATIOS),
    FLAG(18, "Odd Tree",           "Deal with the tree by the Frontier", FLAG_DEFEATED_SUDOWOODO),

    // The Battle Frontier. The seven Silver flags run from FLAG_SYS_TOWER_SILVER
    // two apart, each facility's Gold straight after its Silver.
    STAT(43, "Tower Climber",      "Win 7 in a row at the Battle Tower", GAME_STAT_BATTLE_TOWER_SINGLES_STREAK, 7),
    FLAGS(44, "Silver Symbol",     "Earn a Frontier Silver Symbol",   FLAG_SYS_TOWER_SILVER, 7, 2, 1),
    FLAGS(45, "Silver Set",        "Earn all 7 Silver Symbols",       FLAG_SYS_TOWER_SILVER, 7, 2, 7),
    FLAGS(46, "Gold Symbol",       "Earn a Frontier Gold Symbol",     FLAG_SYS_TOWER_GOLD, 7, 2, 1),
    FLAGS(47, "Frontier Legend",   "Earn all 7 Gold Symbols",         FLAG_SYS_TOWER_GOLD, 7, 2, 7),
};

#define MAIN_COUNT ((u16)ARRAY_COUNT(sMainDefs))
#define POST_COUNT ((u16)ARRAY_COUNT(sPostDefs))
#define ACH_COUNT  ((u16)(MAIN_COUNT + POST_COUNT))

// The largest id the store has a bit for.
#define ACH_ID_LIMIT (CTR_ACH_BYTES * 8)

STATIC_ASSERT(ARRAY_COUNT(sMainDefs) + ARRAY_COUNT(sPostDefs) <= ACH_ID_LIMIT,
              AchTablesFitTheStore);

// What a post-game row says until it is revealed.
#define HIDDEN_TITLE "Hidden Achievement"
#define HIDDEN_DESC  "Progress to discover"

// Provider index i: the main table first, then the post-game one.
static const struct AchDef *DefAt(u16 i)
{
    return i < MAIN_COUNT ? &sMainDefs[i] : &sPostDefs[i - MAIN_COUNT];
}

static u8 SectionAt(u16 i)
{
    return i < MAIN_COUNT ? ACH_SECTION_MAIN : ACH_SECTION_POSTGAME;
}

// ---- state ----------------------------------------------------------------

static u8    sUnlocked[CTR_ACH_BYTES];
static u8    sUnseen[CTR_ACH_BYTES];
static u32   sPlayerId;
static bool8 sLive;        // a playthrough has been adopted
static u16   sCursor;      // the next definition AchTick evaluates

// Whether the post-game page shows its real names: FLAG_SYS_GAME_CLEAR, set on
// entering the Hall of Fame, the same moment "Champion" unlocks. Cached while
// the save is Current() so the provider never reads a flag on the UI's behalf.
static bool8 sRevealed;

// Notifications waiting for the toast. Small, because it only has to hold what
// can unlock between two toasts; when it is full the newest entry grows into a
// batch instead of anything being dropped.
#define TOAST_QUEUE 8

static struct { u16 index, batch; } sToasts[TOAST_QUEUE];
static u8 sToastHead, sToastLen;

// Keyed on an achievement's id. An id past the store's end is refused rather
// than trusted: a typo in a table must not write past these arrays.
static bool8 BitGet(const u8 *bits, u8 id)
{
    return id < ACH_ID_LIMIT && ((bits[id / 8] >> (id % 8)) & 1);
}

static void BitSet(u8 *bits, u8 id)
{
    if (id < ACH_ID_LIMIT)
        bits[id / 8] |= (u8)(1 << (id % 8));
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

// The Battle Factory lends its challengers rental Pokemon, and puts them in
// gPlayerParty under the player's own OT ID -- at level 100 in Open Level. So
// the party is not the player's own anywhere in the Factory, and the two party
// conditions wait until they are out of it. All three rooms, not just the
// battle one, because a challenge can be saved and resumed from the others.
static bool8 InBattleFactory(void)
{
    s8 group = gSaveBlock1Ptr->location.mapGroup;
    s8 num = gSaveBlock1Ptr->location.mapNum;

    if (group != MAP_GROUP(MAP_BATTLE_FRONTIER_BATTLE_FACTORY_LOBBY))
        return FALSE;

    return num == MAP_NUM(MAP_BATTLE_FRONTIER_BATTLE_FACTORY_LOBBY)
        || num == MAP_NUM(MAP_BATTLE_FRONTIER_BATTLE_FACTORY_PRE_BATTLE_ROOM)
        || num == MAP_NUM(MAP_BATTLE_FRONTIER_BATTLE_FACTORY_BATTLE_ROOM);
}

// A party slot holding a Pokemon rather than nothing or an egg. Both fields sit
// before MON_DATA_ENCRYPT_SEPARATOR (include/pokemon.h), so nothing decrypts.
static bool8 PartySlotIsMon(struct Pokemon *mon)
{
    return GetMonData(mon, MON_DATA_SANITY_HAS_SPECIES)
        && !GetMonData(mon, MON_DATA_SANITY_IS_EGG);
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

    case ACH_FLAG_LIST:
        for (u16 i = 0; i < d->count; i++)
            if (FlagGet(d->list[i]))
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

    case ACH_PARTY_COUNT:
        if (InBattleFactory())
            return 0;
        for (u32 i = 0; i < PARTY_SIZE; i++)
            if (PartySlotIsMon(&gPlayerParty[i]))
                n++;
        return n;

    // MON_DATA_LEVEL is the party struct's own plain field, so this decrypts
    // nothing either.
    case ACH_PARTY_LEVEL:
        if (InBattleFactory())
            return 0;
        for (u32 i = 0; i < PARTY_SIZE; i++)
        {
            u32 level;

            if (!PartySlotIsMon(&gPlayerParty[i]))
                continue;

            level = GetMonData(&gPlayerParty[i], MON_DATA_LEVEL);
            if (level > n)
                n = level;
        }
        return n;

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
    BitSet(sUnlocked, DefAt(i)->id);
    BitSet(sUnseen, DefAt(i)->id);
    QueueToast(i, 1);
    Store();
}

// Every definition at once, unlocking whatever is already true without a
// notification each, then one for the lot. Returns how many.
//
// This is what makes a first load fair: a save that already has five badges
// gets them straight away, as one "5 achievements unlocked", rather than five
// toasts in a row or nothing at all. It is also how a build that adds
// achievements catches an older playthrough up with them.
static u16 CatchUp(void)
{
    u16 n = 0, first = 0;

    for (u16 i = 0; i < ACH_COUNT; i++)
    {
        const struct AchDef *d = DefAt(i);

        if (BitGet(sUnlocked, d->id) || !Met(d))
            continue;

        if (n == 0)
            first = i;

        BitSet(sUnlocked, d->id);
        BitSet(sUnseen, d->id);
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
    sRevealed = FlagGet(FLAG_SYS_GAME_CLEAR) != 0;

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
    const struct AchDef *d;

    if (!SaveLive())
        return;

    if (gMain.callback2 == CB2_Overworld && !Current())
        Adopt(PlayerId());

    if (!Current())
        return;

    // One flag read a frame. The shell folds this into its hash through
    // stateKey, so the post-game page repaints the frame it is revealed.
    sRevealed = FlagGet(FLAG_SYS_GAME_CLEAR) != 0;

    // One definition a frame: about seventy frames for both tables, and a
    // constant cost per frame however many there are.
    if (sCursor >= ACH_COUNT)
        sCursor = 0;

    d = DefAt(sCursor);
    if (!BitGet(sUnlocked, d->id) && Met(d))
        Unlock(sCursor);

    sCursor++;
}

void Ctr3dsAchOnCaught(struct Pokemon *mon)
{
    if (!Current() || mon == NULL || !IsMonShiny(mon))
        return;

    for (u16 i = 0; i < ACH_COUNT; i++)
    {
        const struct AchDef *d = DefAt(i);

        if (d->kind == ACH_EVENT && d->arg == ACH_EVENT_SHINY
            && !BitGet(sUnlocked, d->id))
            Unlock(i);
    }
}

// ---- the provider ---------------------------------------------------------

static u16 LocalCount(void)
{
    return ACH_COUNT;
}

static u8 LocalSection(u16 i)
{
    return SectionAt(i);
}

static bool8 LocalUnlocked(u16 i)
{
    return sLive && BitGet(sUnlocked, DefAt(i)->id);
}

static bool8 LocalUnseen(u16 i)
{
    return sLive && BitGet(sUnseen, DefAt(i)->id);
}

static void LocalGet(u16 i, struct AchView *out)
{
    const struct AchDef *d = DefAt(i);
    u32 value = 0;

    out->title = d->title;
    out->desc = d->desc;
    out->goal = d->goal;
    out->unlocked = LocalUnlocked(i);
    out->unseen = LocalUnseen(i);

    // A post-game row keeps its secret until the Hall of Fame, unless it has
    // already been earned: an unlocked row is always shown for what it is.
    out->hidden = SectionAt(i) == ACH_SECTION_POSTGAME && !out->unlocked && !sRevealed;

    if (out->hidden)
    {
        out->title = HIDDEN_TITLE;
        out->desc = HIDDEN_DESC;
        out->goal = 0;
        out->progress = 0;
        return;
    }

    if (out->unlocked)
        value = d->goal;
    else if (Current())
        value = Value(d);

    out->progress = value > d->goal ? d->goal : value;
}

// Both of these walk the tables rather than the raw bytes, so a bit with no
// row behind it -- an id since retired -- can never be counted or light the
// tab bar's dot.
static u16 LocalUnlockedCount(void)
{
    u16 n = 0;

    for (u16 i = 0; i < ACH_COUNT; i++)
        n += LocalUnlocked(i);

    return n;
}

static bool8 LocalAnyUnseen(void)
{
    for (u16 i = 0; i < ACH_COUNT; i++)
        if (LocalUnseen(i))
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
         | ((u32)sLive << 17)
         | ((u32)sRevealed << 18);
}

static const struct AchProvider sLocal =
{
    .count         = LocalCount,
    .section       = LocalSection,
    .unlocked      = LocalUnlocked,
    .unseen        = LocalUnseen,
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

void AchDebugRealText(u16 index, const char **title, const char **desc)
{
    const struct AchDef *d = DefAt(index < ACH_COUNT ? index : 0);

    *title = d->title;
    *desc = d->desc;
}

u16 AchDebugBadIds(void)
{
    u8  seen[CTR_ACH_BYTES] = {0};
    u16 bad = 0;

    for (u16 i = 0; i < ACH_COUNT; i++)
    {
        u8 id = DefAt(i)->id;

        if (id >= ACH_ID_LIMIT || BitGet(seen, id))
            bad++;
        else
            BitSet(seen, id);
    }

    return bad;
}
