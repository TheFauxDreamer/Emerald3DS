// Quick throw: the last ball that the player used, one tap away in a catchable
// battle. See ui_quickball.h for what it is and why the strip is there.
//
// It writes game state, like tab_bag.c, and in the same way. It does not apply
// anything itself. It gives the item to Ctr3dsQueueBattleItem()
// (src/battle_controller_player.c), which registers B_ACTION_USE_ITEM. Thus the
// throw costs a turn and the opponent responds, as on the d-pad route.

#include "global.h"
#include "main.h"
#include "item.h"
#include "battle.h"                 // struct DisableStruct
#include "battle_controllers.h"
#include "constants/items.h"
#include "constants/item.h"

#include "../bridge.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "matchup.h"
#include "ui_quickball.h"

// The interior, inside the 8px border: x 8..312, y 160..184.
#define QB_IN_X   (UI_QB_X + 8)
#define QB_IN_W   (UI_QB_W - 16)
#define QB_IN_Y   (UI_QB_Y + 8)
#define QB_IN_H   (UI_QB_H - 16)

// One row. Everything is centered in the 24px interior.
#define QB_TEXT_Y  (QB_IN_Y + (QB_IN_H - UI_GLYPH_H) / 2)
#define QB_BALL_Y  (QB_IN_Y + (QB_IN_H - UI_BALL_ICON_H) / 2)

// HIDE is the left end of the strip: a down arrow that makes it small again.
#define QB_HIDE_X  QB_IN_X
#define QB_HIDE_W  26
#define QB_HIDE_H  22
#define QB_HIDE_Y  (QB_IN_Y + (QB_IN_H - QB_HIDE_H) / 2)

#define QB_BALL_X  (QB_HIDE_X + QB_HIDE_W + 8)
#define QB_NAME_X  (QB_BALL_X + UI_BALL_ICON_W + 7)

// The button is the right end of the strip. At 80px wide, a finger on the name
// cannot reach it by accident. A single tap throws, with no confirm step.
#define QB_BTN_W   80
#define QB_BTN_H   22
#define QB_BTN_X   (QB_IN_X + QB_IN_W - QB_BTN_W)
#define QB_BTN_Y   (QB_IN_Y + (QB_IN_H - QB_BTN_H) / 2)

// Everything to the left of the button, which is the area that cycles. Use the
// full area, not the name's width: the target is "the ball". The widest ball
// name is 72px in a 224px band.
#define QB_PICK_X  (QB_HIDE_X + QB_HIDE_W + 4)
#define QB_PICK_W  (QB_BTN_X - QB_PICK_X - 8)

// The quantity and the messages share this right edge, clear of the button.
#define QB_RIGHT   (QB_BTN_X - 10)

// The small box: the ball and its count in the 40x24 interior.
#define QB_MINI_IN_X  (UI_QB_MINI_X + 8)
#define QB_MINI_BALL_X (QB_MINI_IN_X + 2)
#define QB_MINI_RIGHT (UI_QB_MINI_X + UI_QB_MINI_W - 9)

// The ball that the player cycled to in this battle, or ITEM_NONE for the
// remembered ball.
//
// Only for this session, and never written to settings.bin. The stored value
// changes only when a ball is thrown, so it records what the player does, not
// what the player looked at.
static u16 sOverride;
static u8  sMessage;

// TRUE after a tap on the small box, until HIDE or the end of the encounter.
static bool8 sOpen;

enum { QB_MSG_NONE, QB_MSG_NO_ROOM, QB_MSG_NOT_NOW };

// -------------------------------------------------------- which ball --------

static bool8 IsBall(u16 item)
{
    return item >= FIRST_BALL && item <= LAST_BALL;
}

// The number of ball kinds in the pocket, and the nth kind.
//
// The game keeps the pocket compact (CompactItemsInBagPocket), so the first
// empty slot is the end of the list, as in PocketCount() in tab_bag.c.
static u16 BallCount(void)
{
    u16 capacity = gBagPockets[BALLS_POCKET].capacity;
    u16 n = 0;

    while (n < capacity
           && BagGetItemIdByPocketPosition(POCKET_POKE_BALLS, n) != ITEM_NONE)
        n++;

    return n;
}

// The first ball to offer when there is no stored ball.
//
// Skip the Master Ball. With BAG SORT set to TYPE, the pocket is in item id
// order and the Master Ball is item 1. Without this skip, a sort preference
// would put a one-tap Master Ball throw under the player's thumb. A Master Ball
// that the player threw is still stored and offered. Only the fallback skips
// it.
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

// The ball that the strip offers, or ITEM_NONE if there is none.
//
// Check every candidate against the bag. This is also where the settings byte
// gets its range check. It crosses the seam as a raw number, so the host does
// not need to know what a ball id is (see bridge.h).
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
    // First, and cheapest. A player who turned this off pays for none of the
    // three tests below.
    if (Ctr3dsGetQuickBallOff())
        return FALSE;

    if (!UiCatchableOpponent())
        return FALSE;

    // "A throw is legal now" is a question about the player's controller, not
    // about the battle. The Safari Zone has its own controller
    // (src/battle_controller_safari.c), with a different action handler. Thus
    // this is FALSE there, and the strip does not show. The touch BAG cannot
    // throw a Safari Ball either.
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

    // The quantity is in the key because it shows. A throw must update the
    // count, even when nothing else changes. The message shows a refusal, which
    // can occur with no other change.
    return (u32)item
         | ((u32)CountTotalItemQuantityInBag(item) << 16)
         | ((u32)sMessage << 28)
         | ((u32)sOpen << 31);
}

// ------------------------------------------------------------ drawing -------

// The small box: the ball and how many there are, with no controls. A tap
// anywhere on it opens the strip.
static void DrawMini(u16 item)
{
    u8 label[4];
    s32 qty = (s32)CountTotalItemQuantityInBag(item);

    UiWindowFrame(UI_QB_MINI_TX, UI_QB_TY, UI_QB_MINI_TW, UI_QB_TH);
    UiBallIcon(QB_MINI_BALL_X, QB_BALL_Y, item);

    UiAscii(label, "x", sizeof(label));
    UiText(QB_MINI_RIGHT - UiNumWidth(qty) - UiTextWidth(label), QB_TEXT_Y,
           label, UI_COL_DIM, UiThemeShadow());
    UiNumRight(QB_MINI_RIGHT, QB_TEXT_Y, qty, UiThemeText(), UiThemeShadow());
}

void UiQuickBallDraw(void)
{
    u16 item = QuickBallItem();
    const u8 *name;
    u8 label[24];
    bool8 canCycle;
    int nameW;

    if (item == ITEM_NONE)
        return;

    if (!sOpen)
    {
        DrawMini(item);
        return;
    }

    canCycle = (BallCount() > 1);

    UiWindowFrame(UI_QB_TX, UI_QB_TY, UI_QB_TW, UI_QB_TH);

    UiRect(QB_HIDE_X, QB_HIDE_Y, QB_HIDE_W, QB_HIDE_H, UI_COL_DIM);
    UiArrow(QB_HIDE_X + (QB_HIDE_W - UI_ARROW_W) / 2,
            QB_HIDE_Y + (QB_HIDE_H - UI_ARROW_H) / 2, FALSE, UI_COL_DIM);

    // The ball's own art, not a generic ball. The player reads it while
    // cycling.
    UiBallIcon(QB_BALL_X, QB_BALL_Y, item);

    name = GetItemName(item);
    nameW = UiText(QB_NAME_X, QB_TEXT_Y, name, UiThemeText(), UiThemeShadow());

    // "There are more of these", only when there are. It is the same arrow as
    // the list pagers, so it needs no learning. It is dim, not accent: the
    // button at the other end is the accent item, and two accent items look
    // like two equal actions.
    if (canCycle)
        UiArrow(QB_NAME_X + nameW + 4,
                QB_TEXT_Y + (UI_GLYPH_H - UI_ARROW_H) / 2, FALSE, UI_COL_DIM);

    if (sMessage != QB_MSG_NONE)
    {
        // In the place of the quantity, because there is no second line. The
        // player needs the count least while the refusal shows.
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
        // CountTotalItemQuantityInBag, not the quantity of one pocket slot.
        // Thus the number agrees with the bag when a ball is in more than one
        // stack.
        s32 qty = (s32)CountTotalItemQuantityInBag(item);

        // The same "x12" style as the HP in the party cell. The position of the
        // label depends on the number's width, so the pair stays together when
        // the count shrinks.
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

// Go to the next kind of ball in the pocket, and wrap. This sets only the
// session override (see sOverride).
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

    // Ctr3dsQueueBattleItem contains every refusal check, the full party and
    // box check too. Do not copy those checks here. QUEUED is the only result
    // that used the turn.
    switch (Ctr3dsQueueBattleItem(item, 0))
    {
    case CTR3DS_ITEM_QUEUED:
        // The strip closes anyway, because the action is chosen. But clear the
        // override now, for the next encounter. The player's choice is now a
        // real preference.
        sOverride = ITEM_NONE;
        sMessage = QB_MSG_NONE;
        break;

    // The only way that Ctr3dsApplyBattleItem refuses a ball: the party is full
    // and all boxes are full.
    case CTR3DS_ITEM_NOT_NOW:
        sMessage = QB_MSG_NO_ROOM;
        break;

    default:
        sMessage = QB_MSG_NOT_NOW;
        break;
    }
}

bool8 UiQuickBallHit(const CtrTouchState *t)
{
    if (sOpen)
        return UiHit(t, UI_QB_X, UI_QB_Y, UI_QB_W, UI_QB_H);

    return UiHit(t, UI_QB_MINI_X, UI_QB_Y, UI_QB_MINI_W, UI_QB_H);
}

void UiQuickBallTouch(const CtrTouchState *t)
{
    // Act on release, like every other control on this screen. A touch that
    // slides off THROW must not throw.
    if (!t->justReleased)
        return;

    // The small box has one action. The first tap only opens the strip, so it
    // cannot throw by accident.
    if (!sOpen)
    {
        sOpen = TRUE;
        UiMarkDirty();
        return;
    }

    if (UiHit(t, QB_HIDE_X, QB_HIDE_Y, QB_HIDE_W, QB_HIDE_H))
    {
        sOpen = FALSE;
        UiMarkDirty();
        return;
    }

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

// Called once each frame by the shell, with the strip up or down.
//
// The override, the message and the open strip belong to one encounter, and
// nothing else clears them. The strip is down for most of a battle, so it cannot clear its state
// when it closes. A message from a full box would show again on the next mon.
// Use gMain.inBattle, not gBattleOutcome, because it stays TRUE through the
// catch and the nickname prompt, when this must not reset.
void UiQuickBallTick(void)
{
    if (!gMain.inBattle && (sOverride != ITEM_NONE || sMessage != QB_MSG_NONE || sOpen))
    {
        sOverride = ITEM_NONE;
        sMessage = QB_MSG_NONE;
        sOpen = FALSE;
    }
}
