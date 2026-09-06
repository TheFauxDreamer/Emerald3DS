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

// The shiny test switch, from the EXTRA tab's debug page.
//
// Returns TRUE when it has created a guaranteed-shiny `mon` and disarmed
// itself, in which case the caller must NOT create one of its own. FALSE, and
// NOTHING touched, in every other case.
//
// The contract, because it is the whole point of the feature:
//
//   It only ever affects the next Pokemon ENCOUNTERED IN THE WILD, and it does
//   that by CREATING one into gEnemyParty[0], which it names itself rather
//   than taking as an argument: THE PLAYER'S TEAM IS NOT REACHABLE FROM HERE,
//   there being no parameter that could aim it there. It never edits, replaces
//   or reads back a Pokemon that already exists -- not one in the party, not
//   one in a box, not the roamer, not a gift, not an egg -- and it refuses
//   outright if the slot it is about to fill is not empty.
//
//   It can never touch a TRAINER battle. The only caller is CreateWildMon,
//   which is static to src/wild_encounter.c with no callers outside it, so no
//   trainer path reaches this code; and Emerald builds trainer parties with
//   OT_ID_RANDOM_NO_SHINY, which rerolls until the mon is not shiny anyway.
//   Being in a battle at all is refused as well, since every wild encounter is
//   generated from the overworld before the battle begins.
//
// Creating rather than editing is also forced by the save format: personality
// is the substructure order and half the encryption key, so writing one into a
// finished Pokemon makes a Bad Egg. See the comment on the definition.
//
// Wild encounters only, and not the Battle Pike or Battle Pyramid, whose wild
// tables do not hold species ids at all. The switch stays armed when it
// declines, so it fires on the next encounter that does qualify.
bool8 Ctr3dsTryCreateShinyTestMon(u16 species, u8 level);

// Reorder one bag pocket in place. A no-op when the sort is set to OFF.
void Ctr3dsSortBagPocket(u8 pocketId);

// Reorder every pocket now, for callers on the bottom screen. Carries an
// overworld safety gate that Ctr3dsSortBagPocket deliberately does not.
void Ctr3dsSortBagNow(void);

#endif // CTR_TWEAKS_H
