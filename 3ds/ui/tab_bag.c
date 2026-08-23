// BAG tab: the player's pockets, and using an item on the selected party mon.
//
// This is the only place in the bottom-screen UI that MUTATES game state, so
// the gate in CanUseItemNow() is the design, not a detail. Everything else here
// is a read through the game's own accessors.
//
// What is deliberately NOT used: GetItemFieldFunc(). The game's field-use flows
// are coupled to the bag menu's task and callback context and render onto the
// top screen; driving one from here would fight the overworld for BG layers.
// PokemonUseItemEffects() is the primitive underneath them, and is what the
// game's own medicine path ultimately calls.

#include "global.h"
#include "main.h"
#include "item.h"
#include "pokemon.h"
#include "overworld.h"
#include "battle.h"            // struct DisableStruct, for the header below
#include "battle_controllers.h"
#include "script.h"
#include "constants/items.h"
#include "constants/species.h"

#include "../bridge.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"

#define POCKET_COUNT   5
#define POCKET_BAR_H   24
#define LIST_Y         28
#define ROW_H          26
#define VISIBLE_ROWS   5
#define MSG_Y          (LIST_Y + VISIBLE_ROWS * ROW_H + 4)
#define SCROLL_W       28

static u8  sPocket = POCKET_ITEMS;   // pocket ids are 1-based
static u16 sScroll;

enum { MSG_NONE, MSG_USED, MSG_NO_EFFECT, MSG_NOT_NOW, MSG_NO_TARGET, MSG_QUEUED };
static u8 sMessage;

static const char *const sPocketNames[POCKET_COUNT] =
    { "ITEM", "BALL", "TM", "BERRY", "KEY" };

// The game keeps pockets compacted (CompactItemsInBagPocket), so the first
// empty slot is the end of the list.
static u16 PocketCount(u8 pocket)
{
    u16 capacity = gBagPockets[pocket - 1].capacity;
    u16 n = 0;

    while (n < capacity && BagGetItemIdByPocketPosition(pocket, n) != ITEM_NONE)
        n++;

    return n;
}

// The one predicate that matters. Item use is only safe out in the overworld
// with no script holding the player: mutating party data mid-battle corrupts
// battler state, and mid-script it can contradict whatever the script is about
// to do. CtrBottomUpdate itself runs between frames (from Rp2350PresentFrame,
// after VBlankIntr), so the frame's own callbacks have already finished.
static bool8 CanUseItemNow(void)
{
    if (gMain.inBattle)
        return FALSE;
    if (gMain.callback2 != CB2_Overworld)
        return FALSE;
    if (ArePlayerFieldControlsLocked())
        return FALSE;
    if (ScriptContext_IsEnabled())
        return FALSE;

    return TRUE;
}

static void TryUseItem(u16 item)
{
    struct Pokemon *mon = &gPlayerParty[UiSelectedMon()];

    if (GetMonData(mon, MON_DATA_SPECIES) == SPECIES_NONE)
    {
        sMessage = MSG_NO_TARGET;
        return;
    }

    // In battle the effect must NOT be applied directly. It has to go through
    // the engine's action queue so that it costs a turn and the opponent gets
    // to respond, which is what the d-pad route does. Only legal while the
    // player is choosing an action, exactly as with the d-pad.
    if (gMain.inBattle)
    {
        if (!Ctr3dsPlayerIsChoosingAction())
        {
            sMessage = MSG_NOT_NOW;
            return;
        }

        Ctr3dsQueueBattleItem(item, UiSelectedMon());
        sMessage = MSG_QUEUED;
        return;
    }

    if (!CanUseItemNow())
    {
        sMessage = MSG_NOT_NOW;
        return;
    }

    // Inverted return, matching the game: FALSE means the item DID something.
    if (PokemonUseItemEffects(mon, item, UiSelectedMon(), 0, FALSE) == FALSE)
    {
        RemoveBagItem(item, 1);
        sMessage = MSG_USED;
    }
    else
    {
        sMessage = MSG_NO_EFFECT;
    }
}

static void DrawPocketBar(void)
{
    const int w = CTR_BOTTOM_WIDTH / POCKET_COUNT;
    u8 label[8];

    for (int i = 0; i < POCKET_COUNT; i++)
    {
        int active = (sPocket == i + 1);

        UiAscii(label, sPocketNames[i], sizeof(label));
        UiText(i * w + (w - UiTextWidth(label)) / 2, 4, label,
               active ? UiThemeText() : UI_COL_DIM, UiThemeShadow());

        if (active)
            UiFillRect(i * w + 6, POCKET_BAR_H - 4, w - 12, 2, UI_COL_ACCENT);
    }
}

static void DrawMessage(void)
{
    static const char *const text[] = {
        [MSG_USED]      = "Used it.",
        [MSG_NO_EFFECT] = "It had no effect.",
        [MSG_NOT_NOW]   = "Not while busy.",
        [MSG_NO_TARGET] = "Pick a Pokemon first.",
        [MSG_QUEUED]    = "Using it this turn.",
    };
    u8 label[32];

    if (sMessage == MSG_NONE)
        return;

    UiText(12, MSG_Y, UiAscii(label, text[sMessage], sizeof(label)),
           UiThemeText(), UiThemeShadow());
}

void UiBagDraw(void)
{
    u16 count = PocketCount(sPocket);
    u8 label[24];

    UiWindowFrame(0, 0, CTR_BOTTOM_WIDTH / 8, UI_CONTENT_H / 8);
    DrawPocketBar();

    if (count == 0)
    {
        UiText(12, LIST_Y, UiAscii(label, "Empty.", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
        DrawMessage();
        return;
    }

    for (int row = 0; row < VISIBLE_ROWS; row++)
    {
        u16 pos = sScroll + row;
        int y = LIST_Y + row * ROW_H;
        u16 item;

        if (pos >= count)
            break;

        item = BagGetItemIdByPocketPosition(sPocket, pos);

        UiText(14, y, GetItemName(item), UiThemeText(), UiThemeShadow());
        UiNumRight(CTR_BOTTOM_WIDTH - SCROLL_W - 12, y,
                   (s32)BagGetQuantityByPocketPosition(sPocket, pos),
                   UiThemeText(), UiThemeShadow());
    }

    // Scroll targets, only when there is something to scroll to.
    if (sScroll > 0)
        UiText(CTR_BOTTOM_WIDTH - SCROLL_W, LIST_Y,
               UiAscii(label, "UP", sizeof(label)), UI_COL_ACCENT, UiThemeShadow());

    if (sScroll + VISIBLE_ROWS < count)
        UiText(CTR_BOTTOM_WIDTH - SCROLL_W, LIST_Y + (VISIBLE_ROWS - 1) * ROW_H,
               UiAscii(label, "DN", sizeof(label)), UI_COL_ACCENT, UiThemeShadow());

    DrawMessage();
}

void UiBagTouch(const CtrTouchState *t)
{
    u16 count;

    if (!t->justReleased)
        return;

    // Pocket switch
    if (t->y < POCKET_BAR_H)
    {
        int i = t->x / (CTR_BOTTOM_WIDTH / POCKET_COUNT);

        if (i >= 0 && i < POCKET_COUNT && sPocket != i + 1)
        {
            sPocket = (u8)(i + 1);
            sScroll = 0;
            sMessage = MSG_NONE;
            UiMarkDirty();
        }
        return;
    }

    count = PocketCount(sPocket);

    // Scroll
    if (t->x >= CTR_BOTTOM_WIDTH - SCROLL_W)
    {
        if (t->y < LIST_Y + ROW_H && sScroll > 0)
            sScroll--;
        else if (sScroll + VISIBLE_ROWS < count)
            sScroll++;
        else
            return;

        UiMarkDirty();
        return;
    }

    // Use the tapped item on the selected party slot.
    if (t->y >= LIST_Y && t->y < LIST_Y + VISIBLE_ROWS * ROW_H)
    {
        u16 pos = sScroll + (u16)((t->y - LIST_Y) / ROW_H);

        if (pos < count)
        {
            TryUseItem(BagGetItemIdByPocketPosition(sPocket, pos));
            UiMarkDirty();
        }
    }
}
