#ifndef CTR_ACHIEVEMENTS_H
#define CTR_ACHIEVEMENTS_H

// Achievements: the game-side half.
//
// 3ds/achievements.c defines each achievement and its unlock test, with the
// game's own accessors (FlagGet, GetGameStat, the Pokedex counts). The host
// side stores which ones each playthrough has (3ds/host/achievements.c),
// through the CtrAchStore* calls in bridge.h.
//
// The bottom screen never reads that file directly. It reads everything through
// the provider below. Thus a second provider could replace the built-in one
// with no change to the TROPHY tab or the toast. An example is
// RetroAchievements, as a game-side adapter over a host-side rc_client. See
// 3ds/ACHIEVEMENTS_PLAN.md.
//
// The one caller in src/ includes this inside #if PLATFORM_3DS, like
// 3ds/tweaks.h, so no other target sees it.

#include "global.h"

struct Pokemon;

// The two pages of the TROPHY tab. A provider with no pages puts everything in
// MAIN.
enum
{
    ACH_SECTION_MAIN,
    ACH_SECTION_POSTGAME,
    ACH_SECTION_COUNT
};

// The kind of achievement, which the UI shows as a color. The colors are Story
// gold, Legendary green, Pokemon red, Battle purple, Extras blue and Contests
// pink (see UiAchCategoryRamp in 3ds/ui/ui_shell.h). A provider with no
// categories uses ACH_CAT_STORY, the gold that all achievements had before
// categories.
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

// One achievement, as the UI shows it.
//
// The strings are ASCII for UiAscii(), with one exception that it knows: the
// UTF-8 e-acute, so a description can spell Pokemon as the game does.
struct AchView
{
    const char *title;
    // Usually a string literal, but it can point to a buffer that the next
    // get() writes again (Seasoned Traveller's "Still to visit: ..."). Use it
    // before you ask for another row.
    const char *desc;
    u32   progress;   // the progress, clamped to goal
    u32   goal;       // above 1: a counter that the UI can draw while locked
    bool8 unlocked;
    bool8 unseen;     // unlocked, and not yet seen on the TROPHY tab
    // Not to be shown yet. The provider already put in placeholder text and
    // removed the counter. The flag tells the UI to draw the row differently.
    bool8 hidden;
    // ACH_CAT_*. Set for hidden rows too. The UI must not draw it, because a
    // color tells what kind of achievement is hidden.
    u8    category;
};

struct AchProvider
{
    u16   (*count)(void);

    // Cheap questions about one achievement, answered from the provider's own
    // bits, with no condition read. The TROPHY tab counts and filters with
    // these on every paint. get() below is for the rows that it draws.
    u8    (*section)(u16 index);   // ACH_SECTION_*
    bool8 (*unlocked)(u16 index);
    bool8 (*unseen)(u16 index);

    void  (*get)(u16 index, struct AchView *out);
    u16   (*unlockedCount)(void);
    bool8 (*anyUnseen)(void);
    // Mark everything that is unlocked as seen. The TROPHY tab calls this on
    // the frame when it comes on the screen, after it copies the unseen rows
    // for its own NEW tags.
    void  (*markAllSeen)(void);
    // The next notification, or FALSE when there is none. `batch` is 1 for a
    // single unlock, `index`. Above 1, it is the count of unlocks shown
    // together ("N achievements unlocked"), and `index` is the first of them.
    bool8 (*popToast)(u16 *index, u16 *batch);
    // A key for what the shell must repaint for. It holds the unlocked count,
    // whether anything is unseen (the tab bar dot), and whether the hidden
    // achievements are revealed.
    u32   (*stateKey)(void);
};

const struct AchProvider *AchActive(void);

// Call once for each displayed frame, from CtrBottomUpdate, before any draw. It
// adopts the playthrough on the screen and checks one definition, in turn.
void AchTick(void);

// The caught-mon hook. Cmd_givecaughtmon (src/battle_script_commands.c) calls
// it before it gives the mon to the player. Only one achievement is an event,
// not a state: the shiny catch. It needs this hook.
void Ctr3dsAchOnCaught(struct Pokemon *mon);

// Debug page (3ds/ui/tab_extra.c, CTR_DEBUG_MENU only).
//
// Queue a notification and unlock nothing, so a tester can see the toast. Each
// press takes the first achievement of the next group, so a few presses show
// the colors of every category.
void AchDebugTestToast(void);
// Forget this playthrough's unlocks and find them again from the save, as on a
// first load. The events (the shiny, the low tide and the flute) are lost.
void AchDebugResync(void);
// The number of definitions with an id that repeats or does not fit the store.
// C cannot check that at compile time, so the debug page reports it.
u16  AchDebugBadIds(void);
// An achievement's real title and description, also while it is hidden. The
// debug page's width check must measure the text after the reveal too.
void AchDebugRealText(u16 index, const char **title, const char **desc);

#endif // CTR_ACHIEVEMENTS_H
