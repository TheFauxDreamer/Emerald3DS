#ifndef CTR_ACHIEVEMENTS_H
#define CTR_ACHIEVEMENTS_H

// Achievements: the game-side half.
//
// What each achievement is and when it unlocks lives in 3ds/achievements.c,
// read through the game's own accessors (FlagGet, GetGameStat, the Pokedex
// counts). Which ones a playthrough has is persisted host-side
// (3ds/host/achievements.c) through the CtrAchStore* calls in bridge.h.
//
// The bottom screen never reaches into that file. It reads everything through
// the provider below, so a second provider -- RetroAchievements, say, which
// would be a game-side adapter over a host-side rc_client -- could replace the
// built-in one without the TROPHY tab or the toast changing at all. See
// 3ds/ACHIEVEMENTS_PLAN.md for why that is not the first step.
//
// The one caller in src/ includes this inside an #if PLATFORM_3DS fence, like
// 3ds/tweaks.h, so no other target ever sees it.

#include "global.h"

struct Pokemon;

// One achievement as the UI shows it.
//
// The strings are ASCII for UiAscii(), with one exception it understands: the
// UTF-8 e-acute, so a description can spell Pokemon the way the game does.
struct AchView
{
    const char *title;
    const char *desc;
    u32   progress;   // how far along, clamped to goal
    u32   goal;       // above 1: a counter the UI may draw while locked
    bool8 unlocked;
    bool8 unseen;     // unlocked and not yet shown on the TROPHY tab
};

struct AchProvider
{
    u16   (*count)(void);
    void  (*get)(u16 index, struct AchView *out);
    u16   (*unlockedCount)(void);
    bool8 (*anyUnseen)(void);
    // Everything unlocked so far counts as seen. The TROPHY tab calls this on
    // the frame it comes on screen, after copying what was unseen for its own
    // NEW tags.
    void  (*markAllSeen)(void);
    // The next notification, or FALSE when there is none. `batch` is 1 for a
    // single unlock, `index`; above 1 it is a count of unlocks announced
    // together ("N unlocked from your save"), and `index` is the first of them.
    bool8 (*popToast)(u16 *index, u16 *batch);
    // Cheap identity of what the shell needs to repaint for: the unlocked count
    // and whether anything is unseen (the tab bar's dot).
    u32   (*stateKey)(void);
};

const struct AchProvider *AchActive(void);

// Once per displayed frame, from CtrBottomUpdate, before anything draws. Adopts
// the playthrough on screen and evaluates one definition, round-robin.
void AchTick(void);

// The caught-mon hook, called from Cmd_givecaughtmon (src/battle_script_commands.c)
// before the mon is handed over. Only the one achievement that is an EVENT
// rather than a state -- catching a shiny -- needs it.
void Ctr3dsAchOnCaught(struct Pokemon *mon);

// Debug page (3ds/ui/tab_extra.c, CTR_DEBUG_MENU only).
//
// Queue a notification for the first definition without unlocking anything, so
// the toast can be looked at without earning something.
void AchDebugTestToast(void);
// Forget this playthrough's unlocks and derive them again from the save, which
// is the backfill path a first load takes. The shiny, being an event, is lost.
void AchDebugResync(void);

#endif // CTR_ACHIEVEMENTS_H
