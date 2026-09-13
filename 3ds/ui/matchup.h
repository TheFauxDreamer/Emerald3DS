// Information about the mon on the other side of a battle (game side).
//
// It gives two answers: the type matchup for the party grid's arrows, and
// whether the opponent is a shiny that the player can catch. Both apply only in
// battle. Both only read. Both are about the opposing side, so they share a
// file.
//
// Multipliers use the game's x10 scale (TYPE_MUL_NORMAL is 10): 20 is super
// effective, 5 is resisted and 0 is immune.

#ifndef CTR_UI_MATCHUP_H
#define CTR_UI_MATCHUP_H

#include "global.h"
#include "pokemon.h"

// No damaging move, so nothing to show.
#define UI_MATCHUP_NA 0xFFFF

// TRUE when a battle has a live opponent.
bool8 UiMatchupActive(void);

// The best multiplier of the mon's damaging moves against the opponent, or
// UI_MATCHUP_NA if it has none.
u16 UiMatchupOffence(struct Pokemon *mon);

// The worst multiplier of the opponent's types against this mon: the risk of a
// switch to it.
u16 UiMatchupRisk(struct Pokemon *mon);

// A key for the current opponent, for the shell's repaint hash. The badges must
// update when the opponent switches.
u32 UiMatchupOpponentKey(void);

// TRUE when the player can throw a ball at the opponent: a live wild encounter
// that the player can catch, with no result yet.
//
// It becomes FALSE when the encounter ends (caught, knocked out, fled or ran),
// so anything that uses it clears itself.
//
// It does not tell if a ball can be thrown now. Ctr3dsPlayerIsChoosingAction()
// tells that.
bool8 UiCatchableOpponent(void);

// TRUE when the opponent is shiny and the player can throw a ball at it. Only
// this case interrupts the player.
//
// It becomes FALSE when the encounter ends (caught, knocked out, fled or ran),
// so anything that uses it clears itself.
//
// `species` gets the species. `identity` gets a value that changes with the
// encounter, so a dismissal does not apply to the next encounter. Either can be
// NULL. They are written only when the result is TRUE.
bool8 UiShinyOpponent(u16 *species, u32 *identity);

#endif // CTR_UI_MATCHUP_H
