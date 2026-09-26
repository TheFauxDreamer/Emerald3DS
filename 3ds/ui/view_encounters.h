// The wild encounter list for a place, on the bottom screen (game side).
//
// It opens from the MAP tab's caption band and covers the full content area,
// like the DEX entry screen. It only reads: the data comes from gWildMonHeaders
// and the Pokedex seen flags. It is a separate file because tab_map.c already
// holds the region map and the fly path.
//
// Chips across the top choose the method: LAND, SURF, SMASH (Rock Smash), and
// the OLD, GOOD and SUPER rods. Only the methods that the place has show. Each
// mon has its level range and its chance, most common first.
//
// A mon that the player has seen shows its icon, name and type badges. A mon
// that the player has not seen shows only a silhouette, and still its level and
// chance. Thus the list shows what is left to find, and does not spoil it.

#ifndef CTR_VIEW_ENCOUNTERS_H
#define CTR_VIEW_ENCOUNTERS_H

#include "global.h"
#include "../bridge.h"

// The two things that the MAP tab's caption can describe, and the two ways to
// find a wild table for them.
//
// PLAYER is necessary. The region map has no cave interiors, so
// Ctr3dsGetRegionMapPlayerPos gives a cave the outdoor mapsec of its exit. A
// lookup by mapsec in Granite Cave would give Route 106. Indoors, only the map
// that the player is on gives the correct answer. That is
// gSaveBlock1Ptr->location, read on each draw, so the list follows the player
// from room to room.
enum
{
    UI_ENC_SRC_PLAYER,   // the map that the player stands on
    UI_ENC_SRC_MAPSEC,   // every map that belongs to a tapped mapsec
};

// TRUE when this place has wild Pokemon. The MAP tab's WILD button shows, and
// works, only when this is TRUE.
bool8 UiEncountersAvailable(u8 source, mapsec_u16_t mapSecId);

void  UiEncountersOpen(u8 source, mapsec_u16_t mapSecId);
void  UiEncountersClose(void);
bool8 UiEncountersIsOpen(void);

// Paints the full content area. Call it only while UiEncountersIsOpen().
void  UiEncountersDraw(void);

// Takes every touch in the content area while the panel is up.
void  UiEncountersTouch(const CtrTouchState *t);

// A key for what the panel shows, for the MAP tab's repaint hash. Zero while it
// is closed, so the caller can always fold it in.
u32   UiEncountersStateKey(void);

#endif // CTR_VIEW_ENCOUNTERS_H
