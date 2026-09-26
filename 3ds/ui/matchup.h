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

// The type that `move` has when `mon` uses it. Hidden Power takes its type from
// the IVs, and in battle Weather Ball takes the weather's. Other moves: the
// type in gBattleMoves.
u8 UiMatchupMoveType(struct Pokemon *mon, u16 move);

// `move`'s multiplier against the opposing battler at `position`
// (B_POSITION_OPPONENT_LEFT or _RIGHT), as the battle engine finds it: with
// Foresight, Levitate, and the moves whose damage is fixed, which can only be
// 0 or 10. UI_MATCHUP_NA for a move with no effectiveness (status moves, Bide,
// Struggle). Call it only when UiMatchupFoePresent(position).
u16 UiMatchupMove(struct Pokemon *mon, u16 move, u8 position);

// TRUE when the opposing battler at `position` is on the field and not
// fainted. The right position exists only in a double battle.
bool8 UiMatchupFoePresent(u8 position);

// A key for everything the matchups read that is not the player's mon: both
// opponents, their Foresight state, and the weather. The badges must update
// when any of them changes.
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
