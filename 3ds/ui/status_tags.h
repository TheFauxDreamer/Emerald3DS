// Status tags: which badges a party mon carries, and which one is showing.
//
// The badge under a mon's icon used to be exactly GetMonAilment(): the one
// persistent status the party menu shows (PSN, PRZ, SLP, FRZ, BRN, FNT). That
// misses confusion, which is not stored on the mon at all. It lives in
// gBattleMons[].status2 for as long as the mon is on the field, and the game
// never draws it anywhere, not even on the healthbox.
//
// So a mon can now carry two tags: its main status and CNF. With one, the
// badge is that tag. With two, the badge alternates between them once a
// second, and every view that draws a badge asks this module rather than
// calling GetMonAilment() itself, so the party grid, the detail view and BAG's
// target picker all show the same tag at the same moment.

#ifndef CTR_UI_STATUS_TAGS_H
#define CTR_UI_STATUS_TAGS_H

#include "global.h"

// Once per displayed frame, after the shell's step clock has advanced and
// before anything draws. Not gated on the tab: the confusion readout and the
// clock have to be current whichever tab comes up next.
void UiStatusTagsTick(void);

// The tag party slot `slot` shows right now: an AILMENT_* value, or
// UI_STATUS_CNF (ui_draw.h). Hand it straight to UiStatusIcon(), which draws
// nothing for AILMENT_NONE.
u8 UiStatusTag(u8 slot);

// TRUE when the slot carries two tags, so its badge changes when the phase does.
bool8 UiStatusTagCycles(u8 slot);

// TRUE on the frame the phase advanced. Always a UiAnimStepped() frame, so a
// view that repaints for it coalesces with the icon step.
bool8 UiStatusTagsFlipped(void);

// Cheap identity for the shell's repaint hash: which slots are confused, so
// gaining or losing CNF repaints. With `withPhase`, also which tag is showing,
// but only while some slot actually cycles. That is for a view with no
// animated layer of its own (BAG's picker), whose only way to show a flip is a
// full repaint; the party grid passes FALSE and flips on its animated layer.
u32 UiStatusTagsKey(bool8 withPhase);

#endif // CTR_UI_STATUS_TAGS_H
