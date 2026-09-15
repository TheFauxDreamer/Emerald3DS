// Whose Pokemon each party slot holds (game side).
//
// In a battle, gPlayerParty does not always show the player's team, for two
// reasons. Thus every view that lists the party reads it through here:
// - With a partner trainer (Steven at the Space Center, the Battle Frontier's
//   multi partners), slots 3-5 hold the partner's Pokemon, from
//   FillPartnerParty (src/battle_tower.c). The game's party menu shows them in
//   their own color, and so does this screen.
// - While the in-battle party menu, or the summary screen from it, is up,
//   gPlayerParty is in battle order (UpdatePartyToBattleOrder). In a multi
//   battle, the partner's lead is then in slot 1. UiPartyMon undoes this, so a
//   slot is always the same Pokemon.

#ifndef CTR_UI_TEAM_H
#define CTR_UI_TEAM_H

#include "global.h"

// The Pokemon in party slot `slot`, in field order, while the game's party menu
// is open or not. Use this, not &gPlayerParty[slot], when a slot number is tied
// to a cell on the screen or kept between frames.
//
// The rest of the port uses field order too. The gBattlerPartyIndexes array
// holds field ids. Ctr3dsQueueBattleItem indexes gPlayerParty directly, which
// is correct because it acts only during action selection, when no menu
// reorders the party.
struct Pokemon *UiPartyMon(u8 slot);

// TRUE while the player fights with a partner trainer.
//
// Only while gMain.inBattle, the same test as the game's IsMultiBattle(). The
// game does not clear gBattleTypeFlags after a battle. After the script's
// LoadPlayerParty, slots 3-5 are the player's again. This port has no link, so
// the partner is always in-game (BATTLE_TYPE_INGAME_PARTNER) and the player has
// slots 0-2.
bool8 UiAllyPresent(void);

// TRUE for a slot that belongs to the partner. Empty slots count too: a
// Frontier partner brings two Pokemon, so slot 5 is empty. A view draws the
// partner's color only on a slot with a Pokemon.
bool8 UiAllySlot(u8 slot);

// A key for the shell's repaint hash: the partner's slots as a bit mask, zero
// with no partner. Nothing else in the hash changes when a partner battle
// starts or ends, so without this the marks do not appear.
u32 UiTeamKey(void);

// Paint the interior of the window frame at (x, y, w, h), in pixels, in the
// partner's ground color. The interior is the center tiles, inside the 8px
// border. Call it just after UiWindowFrame, before anything else in the cell.
void UiAllyFrameGround(int x, int y, int w, int h);

// The partner's name in a tag of height `h`: the partner's ground, a two-step
// gold rule, and the name in the theme text colors. It is the key to the
// colored cells. The name is the OT name on the partner's Pokemon, which
// FillPartnerParty sets. Do not use GetTrainerPartnerName(): it writes
// gStringVar1 for a Frontier partner, and nothing here may change game state.
//
// UiAllyTagWidth gives the width that UiAllyTag draws, to right-align it. Both
// return 0, and UiAllyTag draws nothing, when there is no partner.
int UiAllyTagWidth(void);
int UiAllyTag(int x, int y, int h);

#endif // CTR_UI_TEAM_H
