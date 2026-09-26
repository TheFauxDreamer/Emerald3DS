// The places with news on the MAP tab. See ui_marks.h.
//
// Every value here is one the game keeps and reads itself:
// - gSaveBlock1Ptr->trainerRematches[i], with the map of entry i of
//   gRematchTable. DoesSomeoneWantRematchIn (src/battle_setup.c) makes the
//   same test for one map.
// - The roamer's save data and GetRoamerLocation (src/roamer.c).
// - The outbreak fields of gSaveBlock1Ptr. The TV news sets them, and the
//   game clears the species when the outbreak ends.
//
// Never call RoamerMove* or the functions that start, end or update these.
// They write the save.

#include "global.h"
#include "battle_setup.h"             // gRematchTable
#include "overworld.h"                // Overworld_GetMapHeaderByGroupAndId
#include "pokedex.h"
#include "pokemon.h"
#include "roamer.h"
#include "wild_encounter.h"           // gWildMonHeaders
#include "constants/maps.h"
#include "constants/region_map_sections.h"
#include "constants/species.h"

#include "ui_marks.h"

static mapsec_u16_t MapSecOf(u8 mapGroup, u8 mapNum)
{
    return Overworld_GetMapHeaderByGroupAndId(mapGroup, mapNum)->regionMapSectionId;
}

// TRUE when the map has a land or a water table, so a wild Pokemon can come
// there. The roamer's location is 0, 0 (Petalburg City) after a load until its
// first move, and the game never meets it there. This test drops that place.
static bool8 HasWildLandOrWater(u8 mapGroup, u8 mapNum)
{
    for (u32 i = 0; gWildMonHeaders[i].mapGroup != MAP_GROUP(MAP_UNDEFINED); i++)
        if (gWildMonHeaders[i].mapGroup == mapGroup && gWildMonHeaders[i].mapNum == mapNum)
            return gWildMonHeaders[i].landMonsInfo != NULL
                || gWildMonHeaders[i].waterMonsInfo != NULL;

    return FALSE;
}

static bool8 SpeciesSeen(u16 species)
{
    u16 national;

    if (species == SPECIES_NONE || species >= NUM_SPECIES)
        return FALSE;

    national = SpeciesToNationalPokedexNum(species);
    return national != 0 && GetSetPokedexFlag(national, FLAG_GET_SEEN);
}

static bool8 RoamerLocation(u8 *mapGroup, u8 *mapNum)
{
    const struct Roamer *roamer = &gSaveBlock1Ptr->roamer;

    if (!roamer->active || !SpeciesSeen(roamer->species))
        return FALSE;

    GetRoamerLocation(mapGroup, mapNum);
    return HasWildLandOrWater(*mapGroup, *mapNum);
}

bool8 UiRoamerAt(u8 mapGroup, u8 mapNum)
{
    u8 group, num;

    return RoamerLocation(&group, &num) && group == mapGroup && num == mapNum;
}

bool8 UiOutbreakAt(u8 mapGroup, u8 mapNum)
{
    return gSaveBlock1Ptr->outbreakPokemonSpecies != SPECIES_NONE
        && gSaveBlock1Ptr->outbreakLocationMapGroup == mapGroup
        && gSaveBlock1Ptr->outbreakLocationMapNum == mapNum;
}

static void Mark(struct UiMarks *marks, mapsec_u16_t mapSec, u8 kind)
{
    if (mapSec >= MAPSEC_NONE)
        return;

    marks->kinds[mapSec] |= kind;
    marks->all |= kind;
}

void UiMarksBuild(struct UiMarks *marks)
{
    u8 group, num;

    memset(marks, 0, sizeof(*marks));

    for (u32 i = 0; i < REMATCH_TABLE_ENTRIES; i++)
    {
        mapsec_u16_t mapSec;

        if (gSaveBlock1Ptr->trainerRematches[i] == 0)
            continue;

        mapSec = MapSecOf(gRematchTable[i].mapGroup, gRematchTable[i].mapNum);
        Mark(marks, mapSec, UI_MARK_REMATCH);
        if (mapSec < MAPSEC_NONE && marks->rematches[mapSec] < 0xFF)
            marks->rematches[mapSec]++;
    }

    if (RoamerLocation(&group, &num))
        Mark(marks, MapSecOf(group, num), UI_MARK_ROAMER);

    if (gSaveBlock1Ptr->outbreakPokemonSpecies != SPECIES_NONE)
        Mark(marks, MapSecOf(gSaveBlock1Ptr->outbreakLocationMapGroup,
                             gSaveBlock1Ptr->outbreakLocationMapNum),
             UI_MARK_OUTBREAK);
}

u32 UiMarksKey(void)
{
    u32 key = 0;
    u8 group = 0, num = 0;

    for (u32 i = 0; i < REMATCH_TABLE_ENTRIES; i++)
        if (gSaveBlock1Ptr->trainerRematches[i] != 0)
            key ^= (i + 1) * 2654435761u;

    if (RoamerLocation(&group, &num))
        key ^= (0x10000u | ((u32)group << 8) | num) * 0x85EBCA6Bu;

    key ^= ((u32)gSaveBlock1Ptr->outbreakPokemonSpecies
            | ((u32)gSaveBlock1Ptr->outbreakLocationMapGroup << 16)
            | ((u32)gSaveBlock1Ptr->outbreakLocationMapNum << 24)) * 0xC2B2AE35u;

    return key;
}
