// Quick throw: the ball you last used, one tap away during a catchable battle.
// See ui_quickball.h for what this is and why the strip sits where it does.
//
// It writes game state, which makes this the second file in the bottom-screen
// UI to do so after tab_bag.c, and it does it the same way: NOT by applying
// anything itself, but by handing the item to Ctr3dsQueueBattleItem()
// (src/battle_controller_player.c), which registers B_ACTION_USE_ITEM so the
// throw costs a turn and the opponent responds exactly as the d-pad route
// does. Nothing about the throw is reimplemented here; that function's own
// comments explain why gActiveBattler and gBattlerInMenuId have to be saved
// and restored around it, and none of that is this file's business.

#include "global.h"
#include "main.h"
#include "item.h"
#include "battle.h"                 // struct DisableStruct, for the header below
#include "battle_controllers.h"
#include "constants/items.h"
#include "constants/item.h"

#include "../bridge.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "matchup.h"
#include "ui_quickball.h"

// The interior, inside the frame's 8px border: x 8..312, y 160..184.
#define QB_IN_X   (UI_QB_X + 8)
#define QB_IN_W   (UI_QB_W - 16)
#define QB_IN_Y   (UI_QB_Y + 8)
#define QB_IN_H   (UI_QB_H - 16)

// One row, everything centred in the 24px interior against its own height.
#define QB_TEXT_Y  (QB_IN_Y + (QB_IN_H - UI_GLYPH_H) / 2)
#define QB_BALL_Y  (QB_IN_Y + (QB_IN_H - UI_BALL_ICON_H) / 2)

#define QB_BALL_X  (QB_IN_X + 6)
#define QB_NAME_X  (QB_BALL_X + UI_BALL_ICON_W + 7)

// The button is the right-hand end of the strip. 80px is wide enough that a
// fingertip cannot reach it by accident from the name, which matters because a
// single tap on it throws -- there is no confirm step.
#define QB_BTN_W   80
#define QB_BTN_H   22
#define QB_BTN_X   (QB_IN_X + QB_IN_W - QB_BTN_W)
#define QB_BTN_Y   (QB_IN_Y + (QB_IN_H - QB_BTN_H) / 2)

// Everything left of the button, which is the region that cycles. Deliberately
// the whole of it rather than the name's own measured width: the thing being
// tapped is "the ball", and the widest ball name is only 72px in a 224px band.
#define QB_PICK_X  QB_IN_X
#define QB_PICK_W  (QB_BTN_X - QB_IN_X - 8)

// Quantity and messages share this right edge, clear of the button.
#define QB_RIGHT   (QB_BTN_X - 10)

// Which ball the player has cycled to this battle, or ITEM_NONE for "whichever
// one is remembered".
//
// Session only, and never written to settings.bin. Browsing must not clobber
// the memory: the persisted value changes when a ball is actually THROWN and at
// no other time, which is what makes the recommendation a record of what the
// player does rather than of what they looked at.
static u16 sOverride;
static u8  sMessage;

enum { QB_MSG_NONE, QB_MSG_NO_ROOM, QB_MSG_NOT_NOW };

// -------------------------------------------------------- which ball --------

static bool8 IsBall(u16 item)
{
    return item >= FIRST_BALL && item <= LAST_BALL;
}

// How many kinds of ball are in the pocket, and the nth of them.
//
// The pocket is kept compacted by the game (CompactItemsInBagPocket), so the
// first empty slot is the end of the list -- the same walk PocketCount() in
// tab_bag.c does, and for the same reason.
static u16 BallCount(void)
{
    u16 capacity = gBagPockets[BALLS_POCKET].capacity;
    u16 n = 0;

    while (n < capacity
           && BagGetItemIdByPocketPosition(POCKET_POKE_BALLS, n) != ITEM_NONE)
        n++;

    return n;
}

// The first ball worth offering when there is no memory to go on.
//
// The Master Ball is skipped, and this is the one place the single-tap decision
// needs protecting. Pocket order is acquisition order until the EXTRA tab's BAG
// SORT is switched on, at which point TYPE sort is ascending item id and the
// Master Ball is item 1 -- so without this, turning on a sorting preference
// would quietly put a one-tap Master Ball throw under the player's thumb on
// every encounter. A Master Ball they actually threw is still remembered and
// still offered back; it is only ever the FALLBACK that refuses to suggest one.
static u16 FirstOfferableBall(void)
{
    u16 count = BallCount();

    for (u16 i = 0; i < count; i++)
    {
        u16 item = BagGetItemIdByPocketPosition(POCKET_POKE_BALLS, i);

        if (IsBall(item) && item != ITEM_MASTER_BALL)
            return item;
    }

    return ITEM_NONE;
}

// The ball the strip is offering, or ITEM_NONE if there is nothing to offer.
//
// Every candidate is checked against the bag before it is returned, which is
// also where the settings byte is range-checked: it crosses the seam as a raw
// number precisely so that host-side code does not have to know what a ball id
// is (see bridge.h), so this is where it stops being trusted.
static u16 QuickBallItem(void)
{
    u16 remembered;

    if (IsBall(sOverride) && CheckBagHasItem(sOverride, 1))
        return sOverride;

    remembered = (u16)Ctr3dsGetLastBall();

    if (IsBall(remembered) && CheckBagHasItem(remembered, 1))
        return remembered;

    return FirstOfferableBall();
}

// -------------------------------------------------------------- state -------

bool8 UiQuickBallActive(void)
{
    // First, and cheapest: a player who switched this off pays for none of the
    // three questions below, on any frame, on any tab.
    if (Ctr3dsGetQuickBallOff())
        return FALSE;

    if (!UiCatchableOpponent())
        return FALSE;

    // "A throw is legal right now", which is a question about the player's
    // controller rather than about the battle. It is also why the strip is
    // absent in the Safari Zone: that runs its own controller
    // (src/battle_controller_safari.c) whose identically named action handler is
    // a different function, so this is FALSE there. The touch BAG cannot throw a
    // Safari Ball either, so the strip is consistent with the rest of the UI
    // rather than uniquely limited.
    if (!Ctr3dsPlayerIsChoosingAction())
        return FALSE;

    return QuickBallItem() != ITEM_NONE;
}

u32 UiQuickBallStateKey(void)
{
    u16 item;

    if (!UiQuickBallActive())
        return 0;

    item = QuickBallItem();

    // The quantity is in here because it is drawn: throwing the second-to-last
    // ball has to move the count on screen even though nothing else about the
    // strip changed. The message is what a refusal shows, and it can appear
    // with no other state moving at all.
    return (u32)item
         | ((u32)CountTotalItemQuantityInBag(item) << 16)
         | ((u32)sMessage << 28);
}

// ------------------------------------------------------------ drawing -------

void UiQuickBallDraw(void)
{
    u16 item = QuickBallItem();
    const u8 *name;
    u8 label[24];
    bool8 canCycle;
    int nameW;

    if (item == ITEM_NONE)
        return;

    canCycle = (BallCount() > 1);

    UiWindowFrame(UI_QB_TX, UI_QB_TY, UI_QB_TW, UI_QB_TH);

    // The ball's own art, not a generic one: this is the thing the player reads
    // while cycling, and twelve identical Poke Balls told them nothing.
    UiBallIcon(QB_BALL_X, QB_BALL_Y, item);

    name = GetItemName(item);
    nameW = UiText(QB_NAME_X, QB_TEXT_Y, name, UiThemeText(), UiThemeShadow());

    // "There are more of these", drawn only when there actually are. It is the
    // same arrow both list pagers use and it already means that, so it needs no
    // learning -- and it is DIM rather than accent on purpose: the button at the
    // other end of the strip is the accent-coloured thing here, and two of those
    // on one row would read as two actions of equal weight.
    //
    // Recolouring the name instead was the first attempt and it fought the
    // button for exactly that reason.
    if (canCycle)
        UiArrow(QB_NAME_X + nameW + 4,
                QB_TEXT_Y + (UI_GLYPH_H - UI_ARROW_H) / 2, FALSE, UI_COL_DIM);

    if (sMessage != QB_MSG_NONE)
    {
        // In the quantity's place rather than on a line of its own: there is no
        // second line, and the count is the thing the player least needs while
        // being told the throw did not happen.
        static const char *const text[] = {
            [QB_MSG_NO_ROOM] = "No room for it.",
            [QB_MSG_NOT_NOW] = "Not right now.",
        };

        UiTextRight(QB_RIGHT, QB_TEXT_Y,
                    UiAscii(label, text[sMessage], sizeof(label)),
                    UI_COL_ACCENT, UiThemeShadow());
    }
    else
    {
        // CountTotalItemQuantityInBag rather than one pocket slot's quantity,
        // so the number agrees with the bag even when a ball is split across
        // stacks, however the pocket happens to be sorted.
        s32 qty = (s32)CountTotalItemQuantityInBag(item);

        // Same "x12" idiom the party cell uses for HP: the label is placed off
        // the number's measured width so the pair stays glued together as the
        // count shrinks, rather than the x sitting at a fixed stop.
        UiAscii(label, "x", sizeof(label));
        UiText(QB_RIGHT - UiNumWidth(qty) - UiTextWidth(label), QB_TEXT_Y,
               label, UI_COL_DIM, UiThemeShadow());
        UiNumRight(QB_RIGHT, QB_TEXT_Y, qty, UiThemeText(), UiThemeShadow());
    }

    UiRect(QB_BTN_X, QB_BTN_Y, QB_BTN_W, QB_BTN_H, UI_COL_ACCENT);
    UiAscii(label, "THROW", sizeof(label));
    UiText(QB_BTN_X + (QB_BTN_W - UiTextWidth(label)) / 2,
           QB_BTN_Y + (QB_BTN_H - UI_GLYPH_H) / 2,
           label, UiThemeText(), UiThemeShadow());
}

// -------------------------------------------------------------- input -------

// Move to the next kind of ball in the pocket, wrapping. Sets the session
// override only -- see sOverride.
static void CycleBall(void)
{
    u16 count = BallCount();
    u16 current = QuickBallItem();
    u16 at = 0;

    if (count <= 1)
        return;

    for (u16 i = 0; i < count; i++)
    {
        if (BagGetItemIdByPocketPosition(POCKET_POKE_BALLS, i) == current)
        {
            at = i;
            break;
        }
    }

    for (u16 step = 1; step <= count; step++)
    {
        u16 item = BagGetItemIdByPocketPosition(POCKET_POKE_BALLS,
                                                (u16)((at + step) % count));

        if (IsBall(item))
        {
            sOverride = item;
            sMessage = QB_MSG_NONE;
            return;
        }
    }
}

static void ThrowTapped(void)
{
    u16 item = QuickBallItem();

    if (item == ITEM_NONE)
        return;

    // Everything this needs to refuse for is inside Ctr3dsQueueBattleItem,
    // including the full-party-and-box check, so there is no gate of our own to
    // duplicate here and get subtly wrong. QUEUED is the only outcome that
    // spent the turn.
    switch (Ctr3dsQueueBattleItem(item, 0))
    {
    case CTR3DS_ITEM_QUEUED:
        // The strip is about to go down anyway -- the action is chosen, so
        // UiQuickBallActive() is FALSE on the next frame -- but the override
        // has to be cleared for the encounter after this one, and this is the
        // point at which the player's choice became a real preference.
        sOverride = ITEM_NONE;
        sMessage = QB_MSG_NONE;
        break;

    // The only way a ball reaches this from Ctr3dsApplyBattleItem: party full
    // and every box full too.
    case CTR3DS_ITEM_NOT_NOW:
        sMessage = QB_MSG_NO_ROOM;
        break;

    default:
        sMessage = QB_MSG_NOT_NOW;
        break;
    }
}

void UiQuickBallTouch(const CtrTouchState *t)
{
    // Acting on release, like every other control on this screen: a touch that
    // slides off THROW must not throw.
    if (!t->justReleased)
        return;

    if (UiHit(t, QB_BTN_X, QB_BTN_Y, QB_BTN_W, QB_BTN_H))
    {
        ThrowTapped();
        UiMarkDirty();
        return;
    }

    if (UiHit(t, QB_PICK_X, QB_IN_Y, QB_PICK_W, QB_IN_H))
    {
        CycleBall();
        UiMarkDirty();
        return;
    }
}

// Called once a frame by the shell, whether or not the strip is up.
//
// The override and the message belong to ONE encounter, and neither has any
// other way to be cleared: the strip is down for most of a battle, so it cannot
// clear its own state on the way out, and a message left over from a full box
// would reappear on the next mon. gMain.inBattle is the outer bracket rather
// than gBattleOutcome because it stays true through the catch sequence and the
// nickname prompt, which is exactly the window in which this must NOT be reset.
void UiQuickBallTick(void)
{
    if (!gMain.inBattle && (sOverride != ITEM_NONE || sMessage != QB_MSG_NONE))
    {
        sOverride = ITEM_NONE;
        sMessage = QB_MSG_NONE;
    }
}
