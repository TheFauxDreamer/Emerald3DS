// BAG tab: pockets and item list on the left, details and Use on the right.
//
// Tapping a row only HIGHLIGHTS it; using an item takes a second, deliberate
// tap on Use. On a resistive panel that matters: a single-tap-to-use list makes
// a mis-touch cost you a Full Restore.
//
// This is the only place in the bottom-screen UI that MUTATES game state, so
// the gates in CanUseItemNow() and the battle branch of TryUseItem() are the
// design, not a detail. Everything else here is a read through the game's own
// accessors.
//
// What is deliberately NOT used: GetItemFieldFunc(). The game's field-use flows
// are coupled to the bag menu's task and callback context and render onto the
// top screen; driving one from here would fight the overworld for BG layers.

#include "global.h"
#include "main.h"
#include "item.h"
#include "pokemon.h"
#include "overworld.h"
#include "script.h"
#include "battle.h"              // struct DisableStruct, for the header below
#include "battle_controllers.h"
#include "constants/items.h"
#include "constants/species.h"

#include "../bridge.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"

#define POCKET_COUNT  5
#define POCKET_BAR_H  22

// Two panels side by side, both on the player's chosen window frame. 19 + 21
// tiles exactly fills the 40-tile width.
#define PANEL_Y       24
#define PANEL_TY      (PANEL_Y / 8)
#define PANEL_TH      ((UI_CONTENT_H - PANEL_Y) / 8)
#define LEFT_TW       19
#define RIGHT_TX      LEFT_TW
#define RIGHT_TW      ((CTR_BOTTOM_WIDTH / 8) - LEFT_TW)

#define LEFT_X        10
#define LEFT_W        (LEFT_TW * 8 - 20)
#define LIST_Y        (PANEL_Y + 10)
#define ROW_H         24
#define VISIBLE_ROWS  5

#define PAGE_Y        (LIST_Y + VISIBLE_ROWS * ROW_H + 2)
#define PAGE_W        56
#define PAGE_H        20

#define RIGHT_X       (RIGHT_TX * 8 + 10)
#define RIGHT_W       (RIGHT_TW * 8 - 20)

#define USE_W         88
#define USE_H         26
#define USE_X         (RIGHT_X + (RIGHT_W - USE_W) / 2)
#define USE_Y         (UI_CONTENT_H - USE_H - 12)

static u8  sPocket = POCKET_ITEMS;   // pocket ids are 1-based
static u16 sScroll;
static u16 sCursor;                  // highlighted row, absolute

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

static u16 CursorItem(void)
{
    if (sCursor >= PocketCount(sPocket))
        return ITEM_NONE;

    return BagGetItemIdByPocketPosition(sPocket, sCursor);
}

// ------------------------------------------------------------- using it ----
//
// Item use is only safe out in the overworld with no script holding the
// player: mutating party data mid-script can contradict whatever the script is
// about to do. CtrBottomUpdate itself runs between frames (from
// Rp2350PresentFrame, after VBlankIntr), so the frame's own callbacks have
// already finished.
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

    if (item == ITEM_NONE)
        return;

    if (GetMonData(mon, MON_DATA_SPECIES) == SPECIES_NONE)
    {
        sMessage = MSG_NO_TARGET;
        return;
    }

    // In battle the effect must NOT be applied directly. It has to go through
    // the engine's action queue so that it costs a turn and the opponent gets
    // to respond, which is what the d-pad route does.
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

        // The list may have shrunk under the cursor.
        if (sCursor > 0 && sCursor >= PocketCount(sPocket))
            sCursor--;
    }
    else
    {
        sMessage = MSG_NO_EFFECT;
    }
}

// --------------------------------------------------------------- drawing ---

static void DrawPocketBar(void)
{
    const int w = CTR_BOTTOM_WIDTH / POCKET_COUNT;
    u8 label[8];

    for (int i = 0; i < POCKET_COUNT; i++)
    {
        int active = (sPocket == i + 1);

        UiAscii(label, sPocketNames[i], sizeof(label));
        UiText(i * w + (w - UiTextWidth(label)) / 2, 3, label,
               active ? UiThemeText() : UI_COL_DIM, UiThemeShadow());

        if (active)
            UiFillRect(i * w + 6, POCKET_BAR_H - 3, w - 12, 2, UI_COL_ACCENT);
    }
}

static void DrawList(void)
{
    u16 count = PocketCount(sPocket);
    u8 label[24];

    UiWindowFrame(0, PANEL_TY, LEFT_TW, PANEL_TH);

    if (count == 0)
    {
        UiText(LEFT_X, LIST_Y, UiAscii(label, "Empty.", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
        return;
    }

    for (u32 row = 0; row < VISIBLE_ROWS; row++)
    {
        u16 pos = sScroll + (u16)row;
        int y = LIST_Y + (int)row * ROW_H;
        u16 item;

        if (pos >= count)
            break;

        item = BagGetItemIdByPocketPosition(sPocket, pos);

        // The highlight is what the Use button acts on, so it has to be
        // unmistakable rather than a subtle tint.
        if (pos == sCursor)
            UiRect(LEFT_X - 2, y - 3, LEFT_W + 4, ROW_H - 2, UI_COL_ACCENT);

        UiText(LEFT_X, y, GetItemName(item), UiThemeText(), UiThemeShadow());
        UiNumRight(LEFT_X + LEFT_W, y,
                   (s32)BagGetQuantityByPocketPosition(sPocket, pos),
                   UI_COL_DIM, UiThemeShadow());
    }

    // Paging lives below the list rather than beside it: the column is too
    // narrow to give up width to arrows.
    if (sScroll > 0)
    {
        UiRect(LEFT_X, PAGE_Y, PAGE_W, PAGE_H, UI_COL_DIM);
        UiText(LEFT_X + 18, PAGE_Y + 3, UiAscii(label, "UP", sizeof(label)),
               UI_COL_ACCENT, UiThemeShadow());
    }

    if (sScroll + VISIBLE_ROWS < count)
    {
        UiRect(LEFT_X + PAGE_W + 8, PAGE_Y, PAGE_W, PAGE_H, UI_COL_DIM);
        UiText(LEFT_X + PAGE_W + 26, PAGE_Y + 3, UiAscii(label, "DN", sizeof(label)),
               UI_COL_ACCENT, UiThemeShadow());
    }
}

static void DrawDetails(void)
{
    u16 item = CursorItem();
    u8 label[32];

    UiWindowFrame(RIGHT_TX, PANEL_TY, RIGHT_TW, PANEL_TH);

    if (item == ITEM_NONE)
        return;

    UiText(RIGHT_X, PANEL_Y + 8, GetItemName(item), UiThemeText(), UiThemeShadow());

    // Carries its own line breaks, which UiText honours.
    UiText(RIGHT_X, PANEL_Y + 34, GetItemDescription(item),
           UiThemeText(), UiThemeShadow());

    if (sMessage != MSG_NONE)
    {
        static const char *const text[] = {
            [MSG_USED]      = "Used it.",
            [MSG_NO_EFFECT] = "It had no effect.",
            [MSG_NOT_NOW]   = "Not right now.",
            [MSG_NO_TARGET] = "Pick a Pokemon first.",
            [MSG_QUEUED]    = "Using it this turn.",
        };

        UiText(RIGHT_X, USE_Y - 20, UiAscii(label, text[sMessage], sizeof(label)),
               UI_COL_ACCENT, UiThemeShadow());
    }

    UiRect(USE_X, USE_Y, USE_W, USE_H, UI_COL_DIM);
    UiAscii(label, "USE", sizeof(label));
    UiText(USE_X + (USE_W - UiTextWidth(label)) / 2, USE_Y + 5, label,
           UiThemeText(), UiThemeShadow());
}

void UiBagDraw(void)
{
    DrawPocketBar();
    DrawList();
    DrawDetails();
}

// --------------------------------------------------------------- input -----

void UiBagTouch(const CtrTouchState *t)
{
    u16 count;

    if (!t->justReleased)
        return;

    if (t->y < POCKET_BAR_H)
    {
        int i = t->x / (CTR_BOTTOM_WIDTH / POCKET_COUNT);

        if (i >= 0 && i < POCKET_COUNT && sPocket != i + 1)
        {
            sPocket = (u8)(i + 1);
            sScroll = 0;
            sCursor = 0;
            sMessage = MSG_NONE;
            UiMarkDirty();
        }
        return;
    }

    count = PocketCount(sPocket);

    if (UiHit(t, USE_X, USE_Y, USE_W, USE_H))
    {
        TryUseItem(CursorItem());
        UiMarkDirty();
        return;
    }

    if (UiHit(t, LEFT_X, PAGE_Y, PAGE_W, PAGE_H) && sScroll > 0)
    {
        sScroll--;
        UiMarkDirty();
        return;
    }

    if (UiHit(t, LEFT_X + PAGE_W + 8, PAGE_Y, PAGE_W, PAGE_H)
     && sScroll + VISIBLE_ROWS < count)
    {
        sScroll++;
        UiMarkDirty();
        return;
    }

    // Selecting only moves the highlight. Using takes a second tap on USE.
    if (t->x < LEFT_TW * 8 && t->y >= LIST_Y && t->y < LIST_Y + VISIBLE_ROWS * ROW_H)
    {
        u16 pos = sScroll + (u16)((t->y - LIST_Y) / ROW_H);

        if (pos < count && pos != sCursor)
        {
            sCursor = pos;
            sMessage = MSG_NONE;
            UiMarkDirty();
        }
    }
}
