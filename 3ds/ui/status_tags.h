// Status tags: the badges that a party mon has, and the one that shows.
//
// The party menu shows one stored status (PSN, PRZ, SLP, FRZ, BRN, FNT).
// Confusion is not stored on the mon. It is in gBattleMons[].status2 while the
// mon is on the field, and the game never shows it.
//
// Thus a mon can have two tags: its main status and CNF. With two, the badge
// alternates between them once a second. Every view that draws a badge uses
// this module, so all views show the same tag at the same time.

#ifndef CTR_UI_STATUS_TAGS_H
#define CTR_UI_STATUS_TAGS_H

#include "global.h"

// Call once for each displayed frame, after the shell's step clock and before
// any draw. Call it on every tab, so the confusion data and the clock are
// current for the next tab.
void UiStatusTagsTick(void);

// The tag that party slot `slot` shows now: an AILMENT_* value or UI_STATUS_CNF
// (ui_draw.h). Give it directly to UiStatusIcon(), which draws nothing for
// AILMENT_NONE.
u8 UiStatusTag(u8 slot);

// TRUE when the slot has two tags, so its badge changes with the phase.
bool8 UiStatusTagCycles(u8 slot);

// TRUE on the frame when the phase changed. This is always a UiAnimStepped()
// frame, so a repaint for it occurs with the icon step.
bool8 UiStatusTagsFlipped(void);

// A key for the shell's repaint hash: the confused slots, so a change of CNF
// repaints. With `withPhase`, also the tag that shows, while a slot cycles.
// BAG's picker needs this because it has no animated layer. The party grid
// passes FALSE and flips on its animated layer.
u32 UiStatusTagsKey(bool8 withPhase);

#endif // CTR_UI_STATUS_TAGS_H
