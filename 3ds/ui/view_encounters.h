// The wild encounter list for a place, on the bottom screen (game side).
//
// Opened from the MAP tab's caption band and drawn over the whole content area,
// the way the DEX tab's entry screen is. It is a pure read: everything shown
// comes from gWildMonHeaders and the Pokedex seen flags, and the only reason it
// is a file of its own rather than more of tab_map.c is that the region map's
// geometry and the fly path are already the whole of that file.
//
// What the player has SEEN gets its icon, name and type badges. What they have
// not gets a silhouette and nothing else, so the list is a record of what is
// left to find rather than a spoiler for it.

#ifndef CTR_VIEW_ENCOUNTERS_H
#define CTR_VIEW_ENCOUNTERS_H

#include "global.h"
#include "../bridge.h"

// The two things the MAP tab's caption can be describing, and the two ways of
// finding a wild table for it.
//
// PLAYER is not a convenience: the region map has no cave interiors, so
// Ctr3dsGetRegionMapPlayerPos resolves a cave to the outdoor mapsec of its
// escape warp. Asking by mapsec while standing in Granite Cave would answer for
// Route 106. Only the map the player is literally on can answer indoors, and
// that is gSaveBlock1Ptr->location, read live on every draw so the list follows
// them from room to room.
enum
{
    UI_ENC_SRC_PLAYER,   // the map the player is standing on
    UI_ENC_SRC_MAPSEC,   // every map that resolves to a tapped mapsec
};

// Whether this place has any wild Pokemon at all. The MAP tab's WILD button is
// drawn, and answers taps, only when this is TRUE.
bool8 UiEncountersAvailable(u8 source, mapsec_u16_t mapSecId);

void  UiEncountersOpen(u8 source, mapsec_u16_t mapSecId);
void  UiEncountersClose(void);
bool8 UiEncountersIsOpen(void);

// Paints the whole content area. Only call it while UiEncountersIsOpen().
void  UiEncountersDraw(void);

// Consumes every touch in the content area while the panel is up.
void  UiEncountersTouch(const CtrTouchState *t);

// Cheap identity of what the panel is showing, for the MAP tab's repaint hash.
// Zero while it is closed, so the caller can fold it in unconditionally.
u32   UiEncountersStateKey(void);

#endif // CTR_VIEW_ENCOUNTERS_H
