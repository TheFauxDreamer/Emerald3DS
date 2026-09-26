// The battle panel: tap a move to use it, or a Pokemon to switch it in (game
// side).
//
// It takes the PARTY tab's place for the whole battle, from the first choice
// until the battle has an outcome, as the DS games' bottom screen does. The
// moves can be tapped only while the player is choosing; between choices they
// stay on screen, so the panel does not flicker back to the grid each turn.
//
// It only asks: every choice goes through the player's battle controller
// (Ctr3dsQueueBattleMove, Ctr3dsQueueBattleSwitch in
// src/battle_controller_player.c), which makes it the way the d-pad does, so
// the engine checks it the way it checks the d-pad.
//
// Everything is above y 152, so the quick-throw strip (y 152..192) and the
// achievement toast (y 0..40) still fit over it.

#ifndef CTR_VIEW_BATTLE_H
#define CTR_VIEW_BATTLE_H

#include "global.h"
#include "../bridge.h"

// TRUE from the battle's first choice until it has an outcome. The PARTY tab
// then shows this panel in place of its grid.
bool8 UiBattlePanelActive(void);

void  UiBattlePanelDraw(void);
void  UiBattlePanelTouch(const CtrTouchState *t);

// For UiPartyStateKey: the battler shown, whether it is choosing, its moves'
// PP, and the panel's own selection and message.
u32   UiBattlePanelKey(void);

// Clears the selected Pokemon and the message. The shell calls it when the
// PARTY tab is left.
void  UiBattlePanelLeave(void);

#endif // CTR_VIEW_BATTLE_H
