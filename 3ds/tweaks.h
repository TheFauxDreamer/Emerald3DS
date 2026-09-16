#ifndef CTR_TWEAKS_H
#define CTR_TWEAKS_H

// Gameplay tweaks: the behavior of page 2 of the EXTRA tab.
//
// The toggles are host side (3ds/host/main.c, stored by settings.c), and the
// game reads them through 3ds/bridge.h. This header is the game-side half: all
// code that needs game headers. It is not in src/, so each hook in the original
// sources stays one line.
//
// Every caller in src/ includes this inside #if PLATFORM_3DS, so no other
// target sees it, and `make compare` does not change.

#include "global.h"

// EXP All. TRUE means: treat every living party member as if it holds an Exp.
// Share. This uses the game's own split between participants and holders.
bool8 Ctr3dsExpAllOn(void);

// The current level cap from the badges, or MAX_LEVEL when the cap is off.
u8 Ctr3dsCurrentLevelCap(void);

// HARD only: this mon is at or above the cap and gets no exp.
bool8 Ctr3dsHardCapBlocks(u8 level);

// SOFT only: less exp, by how far the mon is above the cap. In the other modes
// it returns exp as it is, so callers need no mode test.
u32 Ctr3dsSoftCapExp(u8 level, u32 exp);

// Clamp a total exp value to the cap, for the paths that add exp outside
// battle. Returns exp as it is when the cap is off.
u32 Ctr3dsClampCappedExp(u16 species, u32 exp);

// The randomizer's species mapping. It is the same for a given save. It returns
// the species as it is when the randomizer is off or the species is not real.
u16 Ctr3dsMapSpecies(u16 species);

// The same, for wild encounters, where the Battle Pike and the Battle Pyramid
// are excluded. See the comment on the definition.
u16 Ctr3dsMapWildSpecies(u16 species);

// The shiny test switch, from the EXTRA tab's debug page.
//
// Returns TRUE when it created a shiny `mon` and disarmed itself. The caller
// must then not create a mon. In all other cases it returns FALSE and changes
// nothing.
//
// The rules:
// - It affects only the next Pokemon met in the wild. It creates that mon in
//   gEnemyParty[0], a slot that it names itself. There is no parameter, so it
//   cannot reach the player's team. It never edits, replaces or reads back an
//   existing Pokemon (party, box, roamer, gift or egg). It refuses if its slot
//   is not empty.
// - It never touches a trainer battle. The only caller is CreateWildMon, which
//   is static to src/wild_encounter.c. The game builds trainer parties with
//   OT_ID_RANDOM_NO_SHINY, which rerolls until the mon is not shiny. It also
//   refuses during a battle, because the game makes every wild encounter in the
//   overworld before the battle starts.
//
// It must create the mon, because the personality sets the substructure order
// and is half the encryption key. A new personality in a finished Pokemon makes
// a Bad Egg. See the comment on the definition.
//
// Wild encounters only, and not the Battle Pike or the Battle Pyramid, whose
// wild tables do not hold species ids. The switch stays armed when it refuses,
// so it fires on the next valid encounter.
bool8 Ctr3dsTryCreateShinyTestMon(u16 species, u8 level);

// Sort one bag pocket in place. Does nothing when the sort is OFF.
void Ctr3dsSortBagPocket(u8 pocketId);

// Sort every pocket now, for callers on the bottom screen. It has an overworld
// safety gate that Ctr3dsSortBagPocket does not need.
void Ctr3dsSortBagNow(void);

// TRUE when the game's unrequested Match Call must not start: the trainer who
// calls during a route, stops the player and talks.
//
// Only that call. The seven scripted story calls use StartMatchCallFromScript,
// and the PokeNav's Match Call screen never makes the task. Neither goes
// through this function, so nothing that the story needs can be turned off
// here.
bool8 Ctr3dsMatchCallSuppressed(void);

// TRUE when the first Pokemon of the party walks behind the player.
bool8 Ctr3dsFollowerOn(void);

// Show, change or remove the follower now, for callers on the bottom screen.
// It has the overworld gate of Ctr3dsSortBagNow.
void Ctr3dsRefreshFollowerNow(void);

// TRUE when the Day Care Pokemon walk in the Route 117 yard. The templates are
// made at map load (AddDayCareYardTemplates).
bool8 Ctr3dsDayCareYardOn(void);

#endif // CTR_TWEAKS_H
