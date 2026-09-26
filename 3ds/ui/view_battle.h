// The battle panel: tap a move to use it, or a Pokemon to switch it in (game
// side).
//
// It takes the PARTY tab's place while the player is choosing an action or a
// move, as the DS games' bottom screen does, and gives the grid back when the
// turn starts. It only asks: every choice goes through the player's battle
// controller (Ctr3dsQueueBattleMove, Ctr3dsQueueBattleSwitch in
// src/battle_controller_player.c), which makes it the way the d-pad does, so
// the engine checks it the way it checks the d-pad.
//
// Everything is above y 152, so the quick-throw strip (y 152..192) and the
// achievement toast (y 0..40) still fit over it.

#ifndef CTR_VIEW_BATTLE_H
#define CTR_VIEW_BATTLE_H

#include "global.h"
#include "../bridge.h"

// TRUE while a player battler is choosing. The PARTY tab then shows this panel
// in place of its grid.
bool8 UiBattlePanelActive(void);

void  UiBattlePanelDraw(void);
void  UiBattlePanelTouch(const CtrTouchState *t);

// For UiPartyStateKey: the battler that is choosing, its moves' PP, and the
// panel's own selection and message.
u32   UiBattlePanelKey(void);

// Clears the selected Pokemon and the message. The shell calls it when the
// PARTY tab is left.
void  UiBattlePanelLeave(void);

#endif // CTR_VIEW_BATTLE_H
