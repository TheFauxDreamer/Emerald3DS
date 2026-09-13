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

// The TROPHY tab's two pages. A provider with no such distinction puts
// everything in MAIN.
enum
{
    ACH_SECTION_MAIN,
    ACH_SECTION_POSTGAME,
    ACH_SECTION_COUNT
};

// What kind of achievement it is, which the UI shows as a colour (Story gold,
// Legendary green, Pokemon red, Battle purple, Extras blue, Contests pink; see
// UiAchCategoryRamp in 3ds/ui/ui_shell.h). A provider with no categories uses
// ACH_CAT_STORY, which is the gold every achievement had before there were any.
enum
{
    ACH_CAT_STORY,
    ACH_CAT_LEGEND,
    ACH_CAT_POKEMON,
    ACH_CAT_BATTLE,
    ACH_CAT_EXTRA,
    ACH_CAT_CONTEST,
    ACH_CAT_COUNT
};

// One achievement as the UI shows it.
//
// The strings are ASCII for UiAscii(), with one exception it understands: the
// UTF-8 e-acute, so a description can spell Pokemon the way the game does.
struct AchView
{
    const char *title;
    // Usually a string literal, but it may point at a buffer the next get()
    // rewrites (Seasoned Traveller's "Still to visit: ..."), so use it before
    // asking for another row.
    const char *desc;
    u32   progress;   // how far along, clamped to goal
    u32   goal;       // above 1: a counter the UI may draw while locked
    bool8 unlocked;
    bool8 unseen;     // unlocked and not yet shown on the TROPHY tab
    // Not to be shown yet. The provider has already swapped in placeholder text
    // and dropped the counter; the flag is for drawing the row differently.
    bool8 hidden;
    // ACH_CAT_*. Set for hidden rows too; it is the UI's job not to draw it,
    // since a colour would give away what kind of thing is hidden.
    u8    category;
};

struct AchProvider
{
    u16   (*count)(void);

    // Cheap questions about one achievement, answered from the provider's own
    // bits without reading a single condition. The TROPHY tab counts and
    // filters with these on every paint; get() below is for the rows it draws.
    u8    (*section)(u16 index);   // ACH_SECTION_*
    bool8 (*unlocked)(u16 index);
    bool8 (*unseen)(u16 index);

    void  (*get)(u16 index, struct AchView *out);
    u16   (*unlockedCount)(void);
    bool8 (*anyUnseen)(void);
    // Everything unlocked so far counts as seen. The TROPHY tab calls this on
    // the frame it comes on screen, after copying what was unseen for its own
    // NEW tags.
    void  (*markAllSeen)(void);
    // The next notification, or FALSE when there is none. `batch` is 1 for a
    // single unlock, `index`; above 1 it is a count of unlocks announced
    // together ("N achievements unlocked"), and `index` is the first of them.
    bool8 (*popToast)(u16 *index, u16 *batch);
    // Cheap identity of what the shell needs to repaint for: the unlocked count,
    // whether anything is unseen (the tab bar's dot), and whether hidden
    // achievements have been revealed.
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
// Queue a notification without unlocking anything, so the toast can be looked
// at without earning something. Each press takes the first achievement of the
// next group, so a few presses show every category's colours.
void AchDebugTestToast(void);
// Forget this playthrough's unlocks and derive them again from the save, which
// is the backfill path a first load takes. The shiny, being an event, is lost.
void AchDebugResync(void);
// How many definitions have an id that is repeated or will not fit the store.
// C cannot check that at compile time, so the debug page reports it.
u16  AchDebugBadIds(void);
// An achievement's real title and description even while it is hidden, for
// the debug page's width check, which has to measure what will be shown after
// the reveal as well as what is shown now.
void AchDebugRealText(u16 index, const char **title, const char **desc);

#endif // CTR_ACHIEVEMENTS_H
