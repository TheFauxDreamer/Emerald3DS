// Achievements: what they are, when they unlock, and the built-in provider the
// bottom screen reads them through (achievements.h).
//
// Every condition is read through the game's own accessors -- FlagGet,
// GetGameStat, CheckBagHasItem, the Pokedex counts -- for the reason
// SECOND_SCREEN_CHEATSHEET.md gives for the whole bottom screen: stats and bag
// quantities are XOR-encrypted and flags live behind gSaveBlock1Ptr, so a raw
// read could disagree with the game's own screens. Nothing here writes game
// state.
//
// This is a GAME-SIDE translation unit under the two-worlds rule in bridge.h:
// game headers plus bridge.h, never <3ds.h>. The unlocked bits are handed to
// the host through CtrAchStoreLoad/CtrAchStoreSave, which is where they are
// persisted per playthrough.
//
// Almost every achievement is a STATE -- a badge flag, a count that has passed
// a goal -- so it is polled rather than hooked. Two things leave no state
// behind, and are watched as they happen instead:
//
//   The shiny catch, the one hook: src/ gains exactly one fenced line for it.
//
//   Where the player has been, read on every overworld frame (NotePlace) and
//   needing no hook at all. The save keeps the towns, and of the routes only
//   what was found or fought on them, so the store keeps every map section
//   the player has stood in.

#include "global.h"
#include "battle_setup.h"             // GetTrainerFlagFromScriptPointer
#include "event_data.h"               // FlagGet
#include "item.h"                     // CheckBagHasItem
#include "main.h"                     // gMain
#include "overworld.h"                // CB2_Overworld, GetGameStat, Overworld_GetMapHeaderByGroupAndId
#include "pokedex.h"                  // GetHoennPokedexCount, GetSetPokedexFlag
#include "pokemon.h"                  // GetMonData, IsMonShiny
#include "region_map.h"               // Ctr3dsGetMapSecType
#include "constants/event_bg.h"
#include "constants/event_objects.h"
#include "constants/flags.h"
#include "constants/game_stat.h"
#include "constants/items.h"
#include "constants/layouts.h"
#include "constants/maps.h"
#include "constants/region_map_sections.h"
#include "constants/species.h"
#include "constants/trainer_types.h"

#include "bridge.h"
#include "achievements.h"

// ---- the tables -----------------------------------------------------------

enum
{
    ACH_FLAG,          // FlagGet(arg)
    ACH_FLAG_COUNT,    // how many of `count` flags from `arg`, `step` apart, are set
    ACH_FLAG_LIST,     // how many of the `count` flags in `list` are set
    ACH_ITEM_LIST,     // how many of the `count` items in `list` are in the bag
    ACH_STAT,          // GetGameStat(arg)
    ACH_DEX_HOENN,     // kinds owned in the Hoenn Pokedex
    ACH_CAUGHT,        // owns species `arg` or, if set, species `arg2`
    ACH_SECRET_BASE,   // the player's own base exists
    ACH_PARTY_COUNT,   // Pokemon in the party, eggs not counted
    ACH_PARTY_LEVEL,   // the highest level in the party
    ACH_PLACES,        // how many of the `count` map sections from `arg` the player has stood in
    ACH_EVENT,         // unlocked only as it happens; `arg` says which
};

// ACH_EVENT ids, for whatever sees the moment to find their entry by.
#define ACH_EVENT_SHINY    1          // Ctr3dsAchOnCaught
#define ACH_EVENT_LOW_TIDE 2          // NotePlace

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
    // Shown in place of desc until the row is earned (a hidden row shows
    // neither), to say how to finish it. NULL for none.
    const char *hint;
    const u16 *list;   // ACH_FLAG_LIST and ACH_ITEM_LIST
    u16 arg;
    u16 arg2;          // ACH_CAUGHT only: a second species that also counts
    u16 count;         // ACH_FLAG_COUNT, ACH_FLAG_LIST and ACH_ITEM_LIST
    u32 goal;          // unlocked once the value reaches this
};

#define FLAG(i, t, d, f) \
    { .id = i, .title = t, .desc = d, .kind = ACH_FLAG, .arg = f, .goal = 1 }
#define FLAGS(i, t, d, f, n, s, g) \
    { .id = i, .title = t, .desc = d, .kind = ACH_FLAG_COUNT, .arg = f, .count = n, .step = s, .goal = g }
#define FLAG_LIST(i, t, d, l, g) \
    { .id = i, .title = t, .desc = d, .kind = ACH_FLAG_LIST, .list = l, .count = ARRAY_COUNT(l), .goal = g }
#define ITEM_LIST(i, t, d, h, l, g) \
    { .id = i, .title = t, .desc = d, .hint = h, .kind = ACH_ITEM_LIST, .list = l, .count = ARRAY_COUNT(l), .goal = g }
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
#define PLACES(i, t, d, first, n) \
    { .id = i, .title = t, .desc = d, .kind = ACH_PLACES, .arg = first, .count = n, .goal = n }
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

// The four event items, in the order they are handed over: Dad gives them after
// the Hall of Fame, or any S.S. Tidal ferry attendant does for a save already
// past that scene (data/scripts/ctr3ds_event_tickets.inc). They are key items,
// which cannot be deposited, tossed or sold, so the count only goes up. Inside
// the Battle Pyramid, CheckBagHasItem answers for the Pyramid bag instead
// (src/item.c), so a locked row reads low there; that cannot unlock anything
// falsely, and an unlock is never taken back.
static const u16 sEventItems[] =
{
    ITEM_EON_TICKET, ITEM_AURORA_TICKET, ITEM_OLD_SEA_MAP, ITEM_MYSTIC_TICKET,
};

// The groups. Each is one page of the TROPHY tab and one colour, and a row's
// page and colour are nothing but which group it is in, so neither can be
// filed wrong. sGroups below lists them MAIN first, and within a page they
// read as colour blocks in that order.
//
// IDS ARE PERMANENT. The next new achievement takes the next unused id (75 at
// the time of writing); a retired one leaves its id unused forever. Rows move
// between groups freely, because only the id is stored. The debug page counts
// duplicate or out-of-range ids, since C cannot check that at compile time.
//
// Titles must fit the TROPHY tab's title line and descriptions its second line
// (TROPHY_TITLE_MAX_W, TROPHY_DESC_MAX_W in 3ds/ui/tab_trophy.c). There is no
// clipping on this screen, so the debug page counts any that do not fit.
//
// Everything here can be earned in this port as it stands. That rules out the
// complete Hoenn Pokedex and anything past about 200 in the National one: both
// need trade evolutions, and trading waits on the Cable Club
// (local-wireless branch).

// MAIN, Story (gold): the journey in the order Emerald hands it out, with each
// badge where it falls in it.
static const struct AchDef sMainStory[] =
{
    FLAG(48, "A Journey Begins",   "Choose your first " POKEMON,      FLAG_SYS_POKEMON_GET),
    FLAG(49, "Friendly Rivalry",   "Beat your rival on Route 103",    FLAG_DEFEATED_RIVAL_ROUTE103),
    FLAG(50, "Field Research",     "Get a " POKEDEX " from Prof. Birch", FLAG_SYS_POKEDEX_GET),
    FLAG(51, "Hit the Ground Running", "Get the Running Shoes",       FLAG_SYS_B_DASH),
    FLAG(0,  "Stone Badge",        "Beat Roxanne in Rustboro City",   FLAG_BADGE01_GET),
    FLAG(53, "Connected",          "Get a " POKENAV " in Rustboro",   FLAG_SYS_POKENAV_GET),
    FLAG(59, "A Cut Above",        "Get HM01 Cut in Rustboro",        FLAG_RECEIVED_HM_CUT),
    FLAG(52, "Special Delivery",   "Deliver Mr. Stone's letter to Steven", FLAG_DELIVERED_STEVEN_LETTER),
    FLAG(1,  "Knuckle Badge",      "Beat Brawly in Dewford Town",     FLAG_BADGE02_GET),
    FLAG(54, "Package Deal",       "Deliver the Devon Goods to Capt. Stern", FLAG_DELIVERED_DEVON_GOODS),
    FLAG(55, "Pedal Power",        "Get a Bike from Rydel in Mauville", FLAG_RECEIVED_BIKE),
    FLAG(2,  "Dynamo Badge",       "Beat Wattson in Mauville City",   FLAG_BADGE03_GET),
    FLAG(58, "Hot Pursuit",        "Stop Team Magma on Mt. Chimney",  FLAG_DEFEATED_EVIL_TEAM_MT_CHIMNEY),
    FLAG(3,  "Heat Badge",         "Beat Flannery in Lavaridge Town", FLAG_BADGE04_GET),
    FLAG(4,  "Balance Badge",      "Beat your father in Petalburg",   FLAG_BADGE05_GET),
    FLAG(60, "Surf's Up",          "Get HM03 Surf from Wally's father", FLAG_RECEIVED_HM_SURF),
    FLAG(61, "Take Flight",        "Get HM02 Fly on Route 119",       FLAG_RECEIVED_HM_FLY),
    FLAG(5,  "Feather Badge",      "Beat Winona in Fortree City",     FLAG_BADGE06_GET),
    // Emerald's one Master Ball is the item ball in Aqua Hideout B1F.
    FLAG(63, "The Best Ball",      "Find the Master Ball in the Aqua Hideout", FLAG_ITEM_AQUA_HIDEOUT_B1F_MASTER_BALL),
    FLAG(6,  "Mind Badge",         "Beat Tate and Liza in Mossdeep",  FLAG_BADGE07_GET),
    FLAG(7,  "Rain Badge",         "Beat Juan in Sootopolis City",    FLAG_BADGE08_GET),
    FLAG_LIST(62, "HM Collector",  "Get all eight HMs",               sHmFlags, 8),
    FLAG(8,  "Rival No More",      "Beat Wally at Victory Road",      FLAG_DEFEATED_WALLY_VICTORY_ROAD),
    FLAG(9,  "Champion",           "Enter the Hall of Fame",          FLAG_SYS_GAME_CLEAR),
};

// MAIN, Legendary (green): the legends that can be faced before the Hall of
// Fame. The FLAG_DEFEATED_* flags are set whether the encounter ends in a
// catch or a knockout (the scripts set them on both branches), so these are
// about facing the legend, and they still work with the randomiser on.
// Rayquaza is catchable once the Sootopolis crisis is over
// (VAR_SKY_PILLAR_STATE reaches 2), and the Regis once the Sealed Chamber is
// open, which needs only Dive.
static const struct AchDef sMainLegends[] =
{
    FLAG(11, "Sky High",           "Face Rayquaza atop Sky Pillar",   FLAG_DEFEATED_RAYQUAZA),
    FLAG(14, "Rock Solid",         "Face Regirock in the Desert Ruins", FLAG_DEFEATED_REGIROCK),
    FLAG(15, "Cold Snap",          "Face Regice in the Island Cave",  FLAG_DEFEATED_REGICE),
    FLAG(16, "Iron Will",          "Face Registeel in the Ancient Tomb", FLAG_DEFEATED_REGISTEEL),
};

// MAIN, Pokemon (red): catching, raising and the Pokedex, roughly easiest first.
static const struct AchDef sMainPokemon[] =
{
    STAT(23, "Gotcha!",            "Catch your first wild " POKEMON,  GAME_STAT_POKEMON_CAPTURES, 1),
    STAT(57, "It's Evolving!",     "Evolve a " POKEMON " for the first time", GAME_STAT_EVOLVED_POKEMON, 1),
    STAT(66, "In Good Hands",      "Leave a " POKEMON " at the Day Care", GAME_STAT_USED_DAYCARE, 1),
    PARTY_COUNT(56, "Full House",  "Have six " POKEMON " in your party", 6),
    STAT(26, "Hatchling",          "Hatch an Egg",                    GAME_STAT_HATCHED_EGGS, 1),
    // A flag rather than the Pokedex, so it still counts when the randomiser
    // has turned the revived Lileep or Anorith into something else.
    FLAG(65, "Ancient History",    "Revive a fossil at Devon Corp.",  FLAG_RECEIVED_REVIVED_FOSSIL_MON),
    DEX_HOENN(19, "Researcher",     "Own 25 kinds in the Hoenn " POKEDEX,  25),
    DEX_HOENN(20, "Field Worker",   "Own 50 kinds in the Hoenn " POKEDEX,  50),
    STAT(25, "First Bite",         "Hook something while fishing",    GAME_STAT_FISHING_ENCOUNTERS, 1),
    STAT(28, "Growing Up",         "See 50 evolutions",               GAME_STAT_EVOLVED_POKEMON, 50),
    STAT(27, "Breeder",            "Hatch 30 Eggs",                   GAME_STAT_HATCHED_EGGS, 30),
    STAT(24, "Collector",          "Catch 100 wild " POKEMON,         GAME_STAT_POKEMON_CAPTURES, 100),
    DEX_HOENN(21, "Dex Enthusiast", "Own 100 kinds in the Hoenn " POKEDEX, 100),
    DEX_HOENN(22, "Dex Expert",     "Own 150 kinds in the Hoenn " POKEDEX, 150),
    PARTY_LEVEL(64, "Maxed Out",   "Raise a " POKEMON " to level 100", 100),
    EVENT(29, "Shining Star",      "Catch a shiny " POKEMON,          ACH_EVENT_SHINY),
};

// MAIN, Battle (purple).
static const struct AchDef sMainBattle[] =
{
    STAT(31, "Nothing Happened",   "Use Splash in battle",            GAME_STAT_USED_SPLASH, 1),
    STAT(30, "Battle Hardened",    "Fight 100 trainer battles",       GAME_STAT_TRAINER_BATTLES, 100),
};

// MAIN, Extras (blue): the things to do around Hoenn besides the story,
// roughly in the order the game opens them up.
static const struct AchDef sMainExtras[] =
{
    STAT(33, "Cable Car",          "Ride the cable car up Mt. Chimney", GAME_STAT_RODE_CABLE_CAR, 1),
    STAT(34, "Hot Springs",        "Soak in the Lavaridge hot springs", GAME_STAT_ENTERED_HOT_SPRINGS, 1),
    SECRET_BASE(38, "Home Base",   "Set up a Secret Base"),
    STAT(36, "Green Thumb",        "Plant 25 berries",                GAME_STAT_PLANTED_BERRIES, 25),
    STAT(40, "Jackpot!",           "Hit a jackpot at the Game Corner", GAME_STAT_SLOT_JACKPOTS, 1),
    STAT(35, "On Safari",          "Enter the Safari Zone",           GAME_STAT_ENTERED_SAFARI_ZONE, 1),
    STAT(37, "Blender",            "Make 25 " POKEBLOCKS,             GAME_STAT_POKEBLOCKS, 25),
    STAT(39, "Lucky Number",       "Win the Lilycove lottery",        GAME_STAT_WON_POKEMON_LOTTERY, 1),
    // Low tide is 03:00-08:59 and 15:00-20:59 on the game's clock
    // (UpdateShoalTideFlag, src/time_events.c).
    EVENT(68, "Tide's Out",        "Enter Shoal Cave at low tide",    ACH_EVENT_LOW_TIDE),
    STAT(32, "Marathon",           "Walk 100,000 steps",              GAME_STAT_STEPS, 100000),
    // The sixteen towns and cities, then Routes 101 to 134: one contiguous run
    // of map sections, all reachable before the Hall of Fame.
    PLACES(67, "Seasoned Traveller", "Visit every route and town in Hoenn",
           MAPSEC_LITTLEROOT_TOWN, MAPSEC_ROUTE_134 - MAPSEC_LITTLEROOT_TOWN + 1),
};

// MAIN, Contests (pink).
static const struct AchDef sMainContests[] =
{
    STAT(41, "Star Performer",     "Win a " POKEMON " Contest",       GAME_STAT_WON_CONTEST, 1),
    STAT(42, "Ribbon Collector",   "Earn 10 ribbons",                 GAME_STAT_RECEIVED_RIBBONS, 10),
};

// POST-GAME: everything that only opens up after the Hall of Fame, hidden
// until then (see sRevealed). The National Pokedex is Birch's reward for it;
// the abnormal weather that opens Terra Cave and Marine Cave only starts once
// FLAG_SYS_GAME_CLEAR is set (Route119_WeatherInstitute_2F/scripts.inc); the
// roaming Lati is released by it; the S.S. Tidal to the Battle Frontier only
// sails after it; and the four event items only come after it.
static const struct AchDef sPostStory[] =
{
    FLAG(10, "A Bigger Journey Begins", "Get the National " POKEDEX, FLAG_SYS_NATIONAL_DEX),
    // Mostly for saves already past Dad's scene, which have none of the four
    // until they talk to a ferry attendant: the hint says so.
    ITEM_LIST(74, "New Adventures Await", "Get all three tickets and the Old Sea Map",
              "Ask any ferry attendant for the rest", sEventItems, 4),
};

// Sudowoodo is a special encounter rather than a legendary, but it is faced
// the same way and belongs with them more than anywhere else.
static const struct AchDef sPostLegends[] =
{
    FLAG(12, "Terra Firma",        "Face Groudon in the Terra Cave",  FLAG_DEFEATED_GROUDON),
    FLAG(13, "Deep Blue",          "Face Kyogre in the Marine Cave",  FLAG_DEFEATED_KYOGRE),
    // The roaming one sets no flag when caught (only the Southern Island one
    // does, below), so this asks the Pokedex instead.
    CAUGHT(17, "Eon Chaser",       "Catch the roaming Latias or Latios", SPECIES_LATIAS, SPECIES_LATIOS),
    // The four event islands, opened by the items New Adventures Await counts.
    // Each flag is set only on a catch, so these are about catching rather than
    // facing, and they still work with the randomiser on.
    FLAG(69, "Southern Secret",    "Catch the Lati on Southern Island", FLAG_CAUGHT_LATIAS_OR_LATIOS),
    FLAG(70, "Faraway Friend",     "Catch Mew on Faraway Island",     FLAG_CAUGHT_MEW),
    // Deoxys has no FLAG_CAUGHT_*. FLAG_BATTLED_DEOXYS, despite its name, is
    // set only on the catch branch (BirthIsland_Exterior/scripts.inc).
    FLAG(71, "Out of This World",  "Catch Deoxys on Birth Island",    FLAG_BATTLED_DEOXYS),
    FLAG(72, "Rainbow Wing",       "Catch Ho-Oh atop Navel Rock",     FLAG_CAUGHT_HO_OH),
    FLAG(73, "Silver Wing",        "Catch Lugia deep in Navel Rock",  FLAG_CAUGHT_LUGIA),
    FLAG(18, "Odd Tree",           "Deal with the tree by the Frontier", FLAG_DEFEATED_SUDOWOODO),
};

// The Battle Frontier. The seven Silver flags run from FLAG_SYS_TOWER_SILVER
// two apart, each facility's Gold straight after its Silver.
static const struct AchDef sPostBattle[] =
{
    STAT(43, "Tower Climber",      "Win 7 in a row at the Battle Tower", GAME_STAT_BATTLE_TOWER_SINGLES_STREAK, 7),
    FLAGS(44, "Silver Symbol",     "Earn a Frontier Silver Symbol",   FLAG_SYS_TOWER_SILVER, 7, 2, 1),
    FLAGS(45, "Silver Set",        "Earn all 7 Silver Symbols",       FLAG_SYS_TOWER_SILVER, 7, 2, 7),
    FLAGS(46, "Gold Symbol",       "Earn a Frontier Gold Symbol",     FLAG_SYS_TOWER_GOLD, 7, 2, 1),
    FLAGS(47, "Frontier Legend",   "Earn all 7 Gold Symbols",         FLAG_SYS_TOWER_GOLD, 7, 2, 7),
};

struct AchGroup
{
    u8  section;       // ACH_SECTION_*
    u8  category;      // ACH_CAT_*
    u16 count;
    const struct AchDef *rows;
};

#define GROUP(s, c, r) { .section = s, .category = c, .count = ARRAY_COUNT(r), .rows = r }

// MAIN before POST-GAME, which is what lets "the first unseen achievement"
// prefer the main page.
static const struct AchGroup sGroups[] =
{
    GROUP(ACH_SECTION_MAIN,     ACH_CAT_STORY,   sMainStory),
    GROUP(ACH_SECTION_MAIN,     ACH_CAT_LEGEND,  sMainLegends),
    GROUP(ACH_SECTION_MAIN,     ACH_CAT_POKEMON, sMainPokemon),
    GROUP(ACH_SECTION_MAIN,     ACH_CAT_BATTLE,  sMainBattle),
    GROUP(ACH_SECTION_MAIN,     ACH_CAT_EXTRA,   sMainExtras),
    GROUP(ACH_SECTION_MAIN,     ACH_CAT_CONTEST, sMainContests),
    GROUP(ACH_SECTION_POSTGAME, ACH_CAT_STORY,   sPostStory),
    GROUP(ACH_SECTION_POSTGAME, ACH_CAT_LEGEND,  sPostLegends),
    GROUP(ACH_SECTION_POSTGAME, ACH_CAT_BATTLE,  sPostBattle),
};

// The largest id the store has a bit for. Unique ids below it also bound the
// number of rows, so the id check is the capacity check.
#define ACH_ID_LIMIT (CTR_ACH_BYTES * 8)

// One bit per map section. BitGet and BitSet guard with ACH_ID_LIMIT, which is
// only right for the place bits too while the two arrays are the same size.
#define PLACE_LIMIT (CTR_ACH_PLACE_BYTES * 8)
STATIC_ASSERT(PLACE_LIMIT == ACH_ID_LIMIT, PlaceBitsMatchIdBits);

// What a post-game row says until it is revealed.
#define HIDDEN_TITLE "Hidden Achievement"
#define HIDDEN_DESC  "Progress to discover"

// With this many places left or fewer, an ACH_PLACES row names them in place
// of its description: the last few are the hard ones to find, and the counter already
// says what the goal is. Three is what fits. The widest three, "Still to
// visit: Ever Grande, Verdanturf, Sootopolis", measure 261px of the 268 the
// description line has (TROPHY_DESC_MAX_W), and the debug page measures the
// line whenever it is showing.
#define PLACES_NAMED 3

// The towns' names for that line, in map-section order. Without "Town" and
// "City", which is what lets three fit. The game's own names are no use here:
// they are upper case and in its own character set.
static const char *const sTownNames[] =
{
    "Littleroot", "Oldale",     "Dewford",   "Lavaridge",
    "Fallarbor",  "Verdanturf", "Pacifidlog", "Petalburg",
    "Slateport",  "Mauville",   "Rustboro",  "Fortree",
    "Lilycove",   "Mossdeep",   "Sootopolis", "Ever Grande",
};

// AppendPlaceName relies on the towns coming first and the routes straight
// after, in number order.
STATIC_ASSERT(ARRAY_COUNT(sTownNames) == MAPSEC_EVER_GRANDE_CITY + 1, TownNamesCoverTowns);
STATIC_ASSERT(MAPSEC_ROUTE_101 == MAPSEC_EVER_GRANDE_CITY + 1, RoutesFollowTowns);
STATIC_ASSERT(MAPSEC_ROUTE_134 == MAPSEC_ROUTE_101 + 33, RoutesInOrder);

// The groups flattened into provider order once, on first use, so that every
// per-frame question about achievement i is an array index rather than a walk.
// The provider index runs through the groups in sGroups order.
static const struct AchDef *sFlat[ACH_ID_LIMIT];
static u8  sFlatSection[ACH_ID_LIMIT];
static u8  sFlatCategory[ACH_ID_LIMIT];
static u16 sFlatCount;
static u16 sFlatDropped;   // rows past ACH_ID_LIMIT, reported as bad ids
static bool8 sFlatBuilt;

static void BuildFlat(void)
{
    if (sFlatBuilt)
        return;

    for (u32 g = 0; g < ARRAY_COUNT(sGroups); g++)
    {
        for (u16 r = 0; r < sGroups[g].count; r++)
        {
            if (sFlatCount == ACH_ID_LIMIT)
            {
                sFlatDropped++;
                continue;
            }

            sFlat[sFlatCount] = &sGroups[g].rows[r];
            sFlatSection[sFlatCount] = sGroups[g].section;
            sFlatCategory[sFlatCount] = sGroups[g].category;
            sFlatCount++;
        }
    }

    sFlatBuilt = TRUE;
}

static u16 Count(void)
{
    BuildFlat();
    return sFlatCount;
}

#define ACH_COUNT Count()

// Callers only ever pass i < ACH_COUNT; anything else gets the first row
// rather than a wild read.
static const struct AchDef *DefAt(u16 i)
{
    BuildFlat();
    return sFlat[i < sFlatCount ? i : 0];
}

static u8 SectionAt(u16 i)
{
    BuildFlat();
    return i < sFlatCount ? sFlatSection[i] : ACH_SECTION_MAIN;
}

static u8 CategoryAt(u16 i)
{
    BuildFlat();
    return i < sFlatCount ? sFlatCategory[i] : ACH_CAT_STORY;
}

// ---- state ----------------------------------------------------------------

static u8    sUnlocked[CTR_ACH_BYTES];
static u8    sUnseen[CTR_ACH_BYTES];
static u8    sPlaces[CTR_ACH_PLACE_BYTES];   // map sections stood in (NotePlace)
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
    CtrAchStoreSave(sPlayerId, sUnlocked, sUnseen, sPlaces);
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

    case ACH_ITEM_LIST:
        for (u16 i = 0; i < d->count; i++)
            if (CheckBagHasItem(d->list[i], 1))
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

    case ACH_PLACES:
        for (u16 i = 0; i < d->count; i++)
            if (BitGet(sPlaces, d->arg + i))
                n++;
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

// Every EVENT row for `event` that is still locked. Only ever called while
// Current().
static void UnlockEvent(u16 event)
{
    for (u16 i = 0; i < ACH_COUNT; i++)
    {
        const struct AchDef *d = DefAt(i);

        if (d->kind == ACH_EVENT && d->arg == event && !BitGet(sUnlocked, d->id))
            Unlock(i);
    }
}

// Where the player is standing, on every overworld frame rather than on the
// round-robin: a map can be crossed in less than one lap of the rows. The tide
// cannot be read back out of the save afterwards, and nor can a route crossed
// without finding or fighting anything on it.
//
// Overworld frames only, so never partway through a map load. By the first one
// on a new map its ON_TRANSITION script has run, and in Shoal Cave that is the
// script that picks the tide.
static void NotePlace(void)
{
    u8 mapsec = gMapHeader.regionMapSectionId;

    if (mapsec < PLACE_LIMIT && !BitGet(sPlaces, mapsec))
    {
        BitSet(sPlaces, mapsec);
        Store();
    }

    // One map, two layouts: the entrance room's ON_TRANSITION swaps in the
    // high-tide one with setmaplayoutindex. That changes the save's
    // mapLayoutId (SetCurrentMapLayout, src/overworld.c) and not gMapHeader's,
    // so the save's is the one that says which tide the player walked in on.
    if (gSaveBlock1Ptr->location.mapGroup == MAP_GROUP(MAP_SHOAL_CAVE_LOW_TIDE_ENTRANCE_ROOM)
        && gSaveBlock1Ptr->location.mapNum == MAP_NUM(MAP_SHOAL_CAVE_LOW_TIDE_ENTRANCE_ROOM)
        && gSaveBlock1Ptr->mapLayoutId == LAYOUT_SHOAL_CAVE_LOW_TIDE_ENTRANCE_ROOM)
        UnlockEvent(ACH_EVENT_LOW_TIDE);
}

// The towns the save already knows about, for a playthrough older than the
// place bits. The game sets FLAG_VISITED_* on arriving in each, and asking
// through the fly map's own question keeps this from carrying a copy of that
// list. Routes have no such flag; SeedRoutes finds what it can of them.
// Returns whether anything was new.
static bool8 SeedTowns(void)
{
    bool8 changed = FALSE;

    for (u8 m = MAPSEC_LITTLEROOT_TOWN; m <= MAPSEC_EVER_GRANDE_CITY; m++)
    {
        if (BitGet(sPlaces, m) || Ctr3dsGetMapSecType(m) != MAPSECTYPE_CITY_CANFLY)
            continue;

        BitSet(sPlaces, m);
        changed = TRUE;
    }

    return changed;
}

// The two routes with nothing on them to find or fight. Each has a story flag
// that is only ever set there, or somewhere reached only through there.
static const struct { u16 flag; u8 mapsec; } sRouteStoryFlags[] =
{
    // Birch's bag is on Route 101 (Route101/scripts.inc).
    { FLAG_SYS_POKEMON_GET,          MAPSEC_ROUTE_101 },
    // Set on Mt. Pyre's summit (MtPyre_Summit/scripts.inc), a story step
    // before the eighth badge. Mt. Pyre's only way in is from Route 122.
    { FLAG_RECEIVED_RED_OR_BLUE_ORB, MAPSEC_ROUTE_122 },
};

// Whether the save shows the player was on this map: an item ball picked up, a
// trainer beaten or a hidden item found. These are the game's own tests for
// which to show and who still wants a battle (src/trainer_see.c,
// src/item_use.c). No script sets an item flag without the pickup, and none
// sets a route trainer's flag without the battle.
//
// Item balls only among the objects with flags: every other object's flag is a
// FLAG_HIDE_* that scripts set from anywhere. A trainer object's script starts
// with its trainerbattle, which is what GetTrainerFlagFromScriptPointer reads.
static bool8 MapShowsVisit(const struct MapEvents *events)
{
    for (u32 i = 0; i < events->objectEventCount; i++)
    {
        const struct ObjectEventTemplate *obj = &events->objectEvents[i];

        if (obj->graphicsId == OBJ_EVENT_GFX_ITEM_BALL && FlagGet(obj->flagId))
            return TRUE;

        if (obj->trainerType != TRAINER_TYPE_NONE && obj->script != NULL
            && GetTrainerFlagFromScriptPointer(obj->script))
            return TRUE;
    }

    for (u32 i = 0; i < events->bgEventCount; i++)
    {
        const struct BgEvent *bg = &events->bgEvents[i];

        if (bg->kind == BG_EVENT_HIDDEN_ITEM
            && FlagGet(bg->bgUnion.hiddenItem.hiddenItemId + FLAG_HIDDEN_ITEMS_START))
            return TRUE;
    }

    return FALSE;
}

// The route maps are one run in one group (data/maps/map_groups.json). Each
// counts towards its own header's section, the one NotePlace would have noted,
// so nothing here assumes which route a map is.
STATIC_ASSERT(MAP_GROUP(MAP_ROUTE134) == MAP_GROUP(MAP_ROUTE101), RouteMapsShareGroup);
STATIC_ASSERT(MAP_NUM(MAP_ROUTE134) == MAP_NUM(MAP_ROUTE101) + 33, RouteMapsInOrder);

// The routes the save already knows about, for a playthrough older than the
// place bits: the same backfill as SeedTowns, from what the player found or
// fought on each route map, and from sRouteStoryFlags. A route crossed without
// either leaves nothing to find, and counts from the first frame this code
// sees the player on it. A few hundred flag reads, once per adoption. Returns
// whether anything was new.
static bool8 SeedRoutes(void)
{
    bool8 changed = FALSE;

    for (u16 num = MAP_NUM(MAP_ROUTE101); num <= MAP_NUM(MAP_ROUTE134); num++)
    {
        const struct MapHeader *header = Overworld_GetMapHeaderByGroupAndId(MAP_GROUP(MAP_ROUTE101), num);
        u8 mapsec = header->regionMapSectionId;

        if (mapsec >= PLACE_LIMIT || BitGet(sPlaces, mapsec) || !MapShowsVisit(header->events))
            continue;

        BitSet(sPlaces, mapsec);
        changed = TRUE;
    }

    for (u32 i = 0; i < ARRAY_COUNT(sRouteStoryFlags); i++)
    {
        u8 mapsec = sRouteStoryFlags[i].mapsec;

        if (BitGet(sPlaces, mapsec) || !FlagGet(sRouteStoryFlags[i].flag))
            continue;

        BitSet(sPlaces, mapsec);
        changed = TRUE;
    }

    return changed;
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
    bool8 known, seeded;

    sPlayerId = id;
    sLive = TRUE;
    sCursor = 0;
    sRevealed = FlagGet(FLAG_SYS_GAME_CLEAR) != 0;

    // Anything still waiting belonged to the playthrough before.
    sToastHead = 0;
    sToastLen = 0;

    known = CtrAchStoreLoad(id, sUnlocked, sUnseen, sPlaces) != 0;

    // Before the catch-up, so it counts what they find.
    seeded = SeedTowns();
    seeded |= SeedRoutes();

    // A record that is missing, or older than the save (a write lost to a
    // closed lid, say), is caught up the same way. CatchUp() first: it has to
    // run whatever the rest says.
    if (CatchUp() > 0 || !known || seeded)
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

    if (gMain.callback2 == CB2_Overworld)
        NotePlace();

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

    UnlockEvent(ACH_EVENT_SHINY);
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

// Appends to buf, never past its last byte, and keeps it terminated.
static void Append(char *buf, u32 size, u32 *len, const char *s)
{
    while (*s != '\0' && *len + 1 < size)
        buf[(*len)++] = *s++;

    buf[*len] = '\0';
}

// Towns and routes only. LocalGet never asks for anything else.
static void AppendPlaceName(char *buf, u32 size, u32 *len, u8 mapsec)
{
    u32 route;
    char num[4];

    if (mapsec <= MAPSEC_EVER_GRANDE_CITY)
    {
        Append(buf, size, len, sTownNames[mapsec]);
        return;
    }

    route = 101 + (mapsec - MAPSEC_ROUTE_101);
    num[0] = (char)('0' + route / 100);
    num[1] = (char)('0' + route / 10 % 10);
    num[2] = (char)('0' + route % 10);
    num[3] = '\0';

    Append(buf, size, len, "Route ");
    Append(buf, size, len, num);
}

// "Still to visit: Route 105, Route 134". The buffer is rewritten on every
// call, which AchView.desc allows for: the caller uses it before asking again.
static const char *PlacesLeftText(const struct AchDef *d)
{
    static char sText[64];
    u32 len = 0;
    bool8 first = TRUE;

    Append(sText, sizeof(sText), &len, "Still to visit: ");

    for (u16 i = 0; i < d->count; i++)
    {
        u8 mapsec = d->arg + i;

        if (BitGet(sPlaces, mapsec))
            continue;

        if (!first)
            Append(sText, sizeof(sText), &len, ", ");

        AppendPlaceName(sText, sizeof(sText), &len, mapsec);
        first = FALSE;
    }

    return sText;
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
    out->category = CategoryAt(i);

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

    // Until it is earned, a row with a hint says how to finish it.
    if (!out->unlocked && d->hint != NULL)
        out->desc = d->hint;

    // The last few places are the hard ones to find. Only rows inside the
    // towns and routes, the places AppendPlaceName can name.
    if (d->kind == ACH_PLACES && !out->unlocked && Current()
        && value < d->goal && d->goal - value <= PLACES_NAMED
        && d->arg + d->count - 1 <= MAPSEC_ROUTE_134)
        out->desc = PlacesLeftText(d);
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

// The first row of each MAIN group in turn. MAIN only, because it has all six
// categories and nothing hidden: a post-game row would announce itself as
// "Hidden Achievement" before the Hall of Fame.
void AchDebugTestToast(void)
{
    static u8 sNext;
    u32 mainGroups = 0;
    u16 index = 0;

    // sGroups lists every MAIN group first, so they are its leading run.
    while (mainGroups < ARRAY_COUNT(sGroups)
           && sGroups[mainGroups].section == ACH_SECTION_MAIN)
        mainGroups++;

    if (mainGroups == 0)
        return;
    if (sNext >= mainGroups)
        sNext = 0;

    for (u32 g = 0; g < sNext; g++)
        index += sGroups[g].count;

    QueueToast(index, 1);
    sNext = (u8)((sNext + 1) % mainGroups);
}

// sPlaces is kept. SeedTowns and SeedRoutes can only work some of the places
// out again (a route crossed without finding or fighting anything leaves no
// trace in the save), and CatchUp() re-derives Seasoned Traveller from them.
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
    // Rows that did not fit the flattened table at all are bad by definition.
    u16 bad = (BuildFlat(), sFlatDropped);

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
