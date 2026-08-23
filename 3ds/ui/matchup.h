// Type matchup readout for the party grid (game side).
//
// Only meaningful while a battle is running. Multipliers are returned on the
// game's own x10 scale (TYPE_MUL_NORMAL is 10), so 20 is super effective, 5 is
// resisted and 0 is immune.

#ifndef CTR_UI_MATCHUP_H
#define CTR_UI_MATCHUP_H

#include "global.h"
#include "pokemon.h"

// No damaging move to judge by, so nothing to show.
#define UI_MATCHUP_NA 0xFFFF

// TRUE when there is a battle with a live opponent to compare against.
bool8 UiMatchupActive(void);

// Best multiplier among the mon's DAMAGING moves against the current opponent.
// UI_MATCHUP_NA if it knows none.
u16 UiMatchupOffence(struct Pokemon *mon);

// Worst multiplier the opponent's own types achieve against this mon, i.e. how
// dangerous it is to send this one in.
u16 UiMatchupRisk(struct Pokemon *mon);

// Cheap identity of the current opponent, for the shell's repaint hash: the
// badges must refresh when the other side switches.
u32 UiMatchupOpponentKey(void);

#endif // CTR_UI_MATCHUP_H
