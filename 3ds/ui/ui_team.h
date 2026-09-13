// Whose Pokemon each party slot holds (game side).
//
// In a battle, gPlayerParty is not a reliable picture of the player's team,
// for two reasons, and every view that lists the party reads it through here:
//
// - Beside a partner trainer (Steven at the Space Center, the Battle
//   Frontier's multi partners), slots 3-5 hold the PARTNER's Pokemon, put
//   there by FillPartnerParty (src/battle_tower.c). The game's own party menu
//   draws them in a colour of their own, and so does this screen.
// - While the in-battle party menu, or the summary screen opened from it, is
//   up, gPlayerParty is physically in battle order (UpdatePartyToBattleOrder,
//   src/party_menu.c). In a multi battle that puts the partner's lead in slot
//   1. UiPartyMon undoes it, so a slot here is always the same Pokemon and a
//   partner's mark never lands on one of the player's.

#ifndef CTR_UI_TEAM_H
#define CTR_UI_TEAM_H

#include "global.h"

// The Pokemon in party slot `slot`, in field order whatever the game's party
// menu is doing. Use this rather than &gPlayerParty[slot] wherever a slot
// number is tied to a cell on screen or kept from one frame to the next.
//
// Field order is also what everything else in the port uses:
// gBattlerPartyIndexes holds field ids, and Ctr3dsQueueBattleItem indexes
// gPlayerParty directly, which is right because it only acts during action
// selection, when no menu has the party shuffled.
struct Pokemon *UiPartyMon(u8 slot);

// TRUE while the player is fighting beside a partner trainer.
//
// Only while gMain.inBattle, which is the test the game's own IsMultiBattle()
// makes. gBattleTypeFlags is not cleared when a battle ends, and once the
// script's LoadPlayerParty has put the player's full team back, slots 3-5 are
// the player's own again. This port has no link, so the partner is always an
// in-game one (BATTLE_TYPE_INGAME_PARTNER) and the player is always slots 0-2.
bool8 UiAllyPresent(void);

// TRUE for a slot that belongs to the partner rather than the player. Empty
// slots included (the Frontier's multi partners bring two, so slot 5 is
// empty); a view draws the partner's colour only on a slot with a Pokemon.
bool8 UiAllySlot(u8 slot);

// Cheap identity for the shell's repaint hash: the partner's slots as a bit
// mask, zero with no partner. Nothing else in the hash moves when a partner
// battle starts or ends, so without it the marks would not appear until
// something else repainted.
u32 UiTeamKey(void);

// Paint the interior of the window frame at (x, y, w, h), in pixels, in the
// partner's ground colour: the frame's centre tiles, inside its 8px border.
// Call it straight after UiWindowFrame and before anything else in the cell.
void UiAllyFrameGround(int x, int y, int w, int h);

// The partner's name in a tag of height `h`: the partner's ground, a two-step
// gold rule and the name in the theme's text colours, so it reads as the key
// to the coloured cells. The name is the partner's OT name on their Pokemon,
// which FillPartnerParty sets; GetTrainerPartnerName() would overwrite
// gStringVar1 for a Frontier partner, and nothing here may touch game state.
//
// UiAllyTagWidth is the width UiAllyTag will draw, for right-aligning it.
// Both return 0, and UiAllyTag draws nothing, when there is no partner.
int UiAllyTagWidth(void);
int UiAllyTag(int x, int y, int h);

#endif // CTR_UI_TEAM_H
