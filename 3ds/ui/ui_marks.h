// The places on the MAP tab that have news, as the PokeNav Plus of the DS
// remakes marks them (game side):
//
// - REMATCH: a trainer in the Match Call list wants a rematch there.
// - ROAMER: the roaming Latias or Latios is there now.
// - OUTBREAK: the mass outbreak that the TV news told of is there.
//
// This file only reads. tab_map.c draws the marks, and view_encounters.c adds
// the roamer and the outbreak to the wild list of their place.

#ifndef CTR_UI_MARKS_H
#define CTR_UI_MARKS_H

#include "global.h"
#include "constants/region_map_sections.h"

enum
{
    UI_MARK_REMATCH  = 1 << 0,
    UI_MARK_ROAMER   = 1 << 1,
    UI_MARK_OUTBREAK = 1 << 2,
};

// One set of marks for each map section, and the number of trainers that want
// a rematch in each. Fill with UiMarksBuild.
struct UiMarks
{
    u8 kinds[MAPSEC_NONE];
    u8 rematches[MAPSEC_NONE];
    u8 all;                    // every kind that is somewhere
};

void UiMarksBuild(struct UiMarks *marks);

// TRUE when the map has the roamer, on the terms of the Pokedex area screen:
// it is active, and the player has seen its species.
bool8 UiRoamerAt(u8 mapGroup, u8 mapNum);

// TRUE when the TV's mass outbreak is on the map.
bool8 UiOutbreakAt(u8 mapGroup, u8 mapNum);

// A key for everything above, for the repaint hash. The marks change with no
// touch: a trainer calls, the roamer moves, the news airs.
u32 UiMarksKey(void);

#endif // CTR_UI_MARKS_H
