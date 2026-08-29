#ifndef CTR_TWEAKS_H
#define CTR_TWEAKS_H

// Gameplay tweaks: the behaviour behind page 2 of the EXTRA tab.
//
// The toggles themselves live host-side (3ds/host/main.c, persisted by
// settings.c) and are read through 3ds/bridge.h. This header is the game-side
// half: everything that needs game headers to mean anything, kept out of src/
// so the hooks scattered through the original sources stay one line each.
//
// Every caller in src/ includes this inside an #if PLATFORM_3DS fence, so no
// other target ever sees it and `make compare` is unaffected.

#include "global.h"

// EXP All. TRUE means treat every living party member as though it holds an
// Exp. Share, which reuses the game's own participant/share split.
bool8 Ctr3dsExpAllOn(void);

// The current badge-based level cap, or MAX_LEVEL when the cap is switched off.
u8 Ctr3dsCurrentLevelCap(void);

// HARD only: this mon is at or past the cap and must gain nothing at all.
bool8 Ctr3dsHardCapBlocks(u8 level);

// SOFT only: exp reduced by how far past the cap the mon is. Returns exp
// unchanged in every other mode, so callers need no mode test of their own.
u32 Ctr3dsSoftCapExp(u8 level, u32 exp);

// Clamp an absolute exp total to the cap, for the paths that add exp outside
// battle. Returns exp unchanged when the cap is off.
u32 Ctr3dsClampCappedExp(u16 species, u32 exp);

// The randomiser's species mapping. Deterministic for a given save, and the
// identity when the randomiser is off or the species is not a real one.
u16 Ctr3dsMapSpecies(u16 species);

// As above, but for wild encounters, where the Battle Pike and Battle Pyramid
// have to be excluded. See the comment on the definition.
u16 Ctr3dsMapWildSpecies(u16 species);

// Reorder one bag pocket in place. A no-op when the sort is set to OFF.
void Ctr3dsSortBagPocket(u8 pocketId);

// Reorder every pocket now, for callers on the bottom screen. Carries an
// overworld safety gate that Ctr3dsSortBagPocket deliberately does not.
void Ctr3dsSortBagNow(void);

#endif // CTR_TWEAKS_H
