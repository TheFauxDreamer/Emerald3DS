// BAG tab: pockets and item list on the left, details and Use on the right,
// and a party target picker that opens over both when an item needs one.
//
// Tapping a row only MOVES THE CURSOR to it; using an item takes a second,
// deliberate tap on Use. On a resistive panel that matters: a single-tap-to-use
// list makes a mis-touch cost you a Full Restore.
//
// This is the only place in the bottom-screen UI that MUTATES game state, so
// the gates in CanUseItemNow(), ItemTargeting() and Ctr3dsQueueBattleItem() are
// the design, not a detail. Everything else here is a read through the game's
// own accessors.
//
// What is deliberately NOT used: GetItemFieldFunc(). The game's field-use flows
// are coupled to the bag menu's task and callback context and render onto the
// top screen; driving one from here would fight the overworld for BG layers.
// ItemTargeting() is what keeps that promise honest -- see its comment for the
// two item classes that would otherwise reach the top screen through the back
// door.

#include "global.h"
#include "main.h"
#include "item.h"
#include "pokemon.h"
#include "overworld.h"
#include "script.h"
#include "battle.h"              // struct DisableStruct, for the header below
#include "battle_controllers.h"
#include "party_menu.h"          // GetItemEffectType, GetMonAilment
#include "constants/items.h"
#include "constants/item_effects.h"
#include "constants/species.h"

#include "../bridge.h"
#include "../tweaks.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"

#define POCKET_COUNT  5
#define POCKET_BAR_H  22

// Two panels side by side, both on the player's chosen window frame. 24 + 16
// tiles exactly fills the 40-tile width, a 60/40 split in the list's favour.
//
// 24 is the largest the list can take. It leaves the details column 108px of
// text width, and the widest line in any item description ("raises FARFETCH'D's",
// src/data/text/item_descriptions.h) is exactly 108px. One tile further and
// descriptions start wrapping into the border.
#define PANEL_Y       24
#define PANEL_TY      (PANEL_Y / 8)
#define PANEL_TH      ((UI_CONTENT_H - PANEL_Y) / 8)
#define LEFT_TW       24
#define RIGHT_TX      LEFT_TW
#define RIGHT_TW      ((CTR_BOTTOM_WIDTH / 8) - LEFT_TW)

#define LEFT_X        10
#define LEFT_W        (LEFT_TW * 8 - 20)

// The selected row is marked the way the game's own menus mark one: a cursor in
// a reserved column, not a box drawn round the text. The column is the glyph
// plus a 4px gap, and every row's text starts after it whether it is selected
// or not, so the list does not shuffle sideways as the cursor moves.
#define LIST_CURSOR_X (LEFT_X)
#define LIST_TEXT_X   (LEFT_X + UI_CHEVRON_W + 4)
#define LIST_Y        (PANEL_Y + 10)
#define ROW_H         24
#define VISIBLE_ROWS  5

#define PAGE_Y        (LIST_Y + VISIBLE_ROWS * ROW_H + 2)
#define PAGE_W        56
#define PAGE_H        20

#define RIGHT_X       (RIGHT_TX * 8 + 10)
#define RIGHT_W       (RIGHT_TW * 8 - 20)

// The narrower column stacks the icon above the name instead of setting them
// side by side: at 108px a 32px icon plus the longest item name (72px) does not
// fit on one line.
#define ICON_X        (RIGHT_X + (RIGHT_W - 32) / 2)
#define ICON_Y        (PANEL_Y + 6)
#define NAME_Y        (PANEL_Y + 40)
#define DESC_Y        (PANEL_Y + 58)

#define USE_W         88
#define USE_H         26
#define USE_X         (RIGHT_X + (RIGHT_W - USE_W) / 2)
#define USE_Y         (UI_CONTENT_H - USE_H - 12)

// Target picker: a prompt band over a 2x3 grid of the team. The three bands
// have to add up to UI_CONTENT_H exactly and land on 8px tile boundaries, or
// the window frames inside them do not: 24 + 3 * 56 = 192, i.e. 3 + 3 * 7 tiles.
#define PICK_HEAD_H   24
#define PICK_COLS     2
#define PICK_ROWS     3
#define PICK_CELL_W   (CTR_BOTTOM_WIDTH / PICK_COLS)
#define PICK_CELL_H   ((UI_CONTENT_H - PICK_HEAD_H) / PICK_ROWS)

// Same three-column split as the PARTY tab's cells, and for the same reason:
// cursor, then icon and status, then everything textual.
#define PICK_CURSOR_X 8
#define PICK_ICON_X   18
#define PICK_TEXT_X   54

#define PICK_CANCEL_W 56
#define PICK_CANCEL_H 20
#define PICK_CANCEL_X (CTR_BOTTOM_WIDTH - PICK_CANCEL_W - 8)
#define PICK_CANCEL_Y 2

static u8  sPocket = POCKET_ITEMS;   // pocket ids are 1-based
static u16 sScroll;
static u16 sCursor;                  // row the cursor is on, absolute

// The picker is modal over the whole content area, the same shape the PARTY
// tab's detail view uses.
enum { VIEW_LIST, VIEW_PICK_MON };
static u8  sView;
static u16 sPickItem;                // the item USE was tapped for

enum { MSG_NONE, MSG_USED, MSG_NO_EFFECT, MSG_NOT_NOW, MSG_QUEUED, MSG_USE_IN_MENU };
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

// Ether restores one move and Elixir restores all four. The party menu tells
// them apart on this bit and opens a move list for the first kind
// (ItemUseCB_PPRecovery, src/party_menu.c); this UI has no move list, so it
// makes the same split and refuses that half.
static bool8 PpItemNeedsMoveChoice(u16 item)
{
    const u8 *effect;

    if (item == ITEM_ENIGMA_BERRY)
        effect = gSaveBlock1Ptr->enigmaBerry.itemEffect;
    else
        effect = gItemEffectTable[item - ITEM_POTION];

    return (effect[4] & ITEM4_HEAL_PP_ONE) != 0;
}

// What a USE tap needs before it can do anything. Driven entirely off the
// game's own tables rather than a list of item ids, so it classifies every item
// of a class the same way and cannot fall behind the data.
enum { TARGET_MON, TARGET_NONE, TARGET_UNSUPPORTED };

static u8 ItemTargeting(u16 item)
{
    u8 effect = GetItemEffectType(item);

    // Needs a MOVE chosen as well as a mon. Defaulting to the first move slot
    // would silently top up the wrong move, so these are refused outright.
    if (effect == ITEM_EFFECT_PP_UP || effect == ITEM_EFFECT_PP_MAX)
        return TARGET_UNSUPPORTED;
    if (effect == ITEM_EFFECT_HEAL_PP && PpItemNeedsMoveChoice(item))
        return TARGET_UNSUPPORTED;

    // In battle the engine's own table already says which items are offerable
    // and whether they need a party choice: MEDICINE is the class the vanilla
    // bag follows with a party menu, OTHER is balls, X items and the escape
    // items, all of which act without one.
    if (gMain.inBattle)
    {
        switch (GetItemBattleUsage(item))
        {
        case ITEM_B_USE_MEDICINE: return TARGET_MON;
        case ITEM_B_USE_OTHER:    return TARGET_NONE;
        default:                  return TARGET_UNSUPPORTED;
        }
    }

    switch (effect)
    {
    case ITEM_EFFECT_HEAL_HP:
    case ITEM_EFFECT_HEAL_PP:
    case ITEM_EFFECT_CURE_POISON:
    case ITEM_EFFECT_CURE_SLEEP:
    case ITEM_EFFECT_CURE_BURN:
    case ITEM_EFFECT_CURE_FREEZE:
    case ITEM_EFFECT_CURE_PARALYSIS:
    case ITEM_EFFECT_CURE_CONFUSION:
    case ITEM_EFFECT_CURE_INFATUATION:
    case ITEM_EFFECT_CURE_ALL_STATUS:
    case ITEM_EFFECT_ATK_EV:
    case ITEM_EFFECT_HP_EV:
    case ITEM_EFFECT_SPATK_EV:
    case ITEM_EFFECT_SPDEF_EV:
    case ITEM_EFFECT_SPEED_EV:
    case ITEM_EFFECT_DEF_EV:
        return TARGET_MON;

    // Everything else out of battle is refused, and two of those are refused
    // for correctness rather than tidiness:
    //
    //   EVO_STONE -- PokemonUseItemEffects() calls BeginEvolutionScene()
    //   directly (src/pokemon.c), which would seize gMain.callback2 from inside
    //   CtrBottomUpdate, mid-frame. That is exactly the top-screen fight this
    //   file's header promises not to pick.
    //
    //   RAISE_LEVEL -- the table effect is only the level-up. The new-move
    //   check runs afterwards in ItemUseCB_RareCandy (src/party_menu.c), so
    //   applying the bare effect walks a mon straight past a learnset move.
    //
    // X_ITEM does nothing outside battle, SACRED_ASH needs a whole-party sweep,
    // and NONE has no effect table at all.
    default:
        return TARGET_UNSUPPORTED;
    }
}

// The gate both routes share, checked before the picker opens and again when it
// commits: a turn can pass, or a script can start, while the picker is up.
static bool8 CanStartUse(void)
{
    return gMain.inBattle ? Ctr3dsPlayerIsChoosingAction() : CanUseItemNow();
}

// `slot` is ignored for TARGET_NONE items: a ball targets nobody and an X item
// targets whichever mon is out, which the battle controller resolves itself.
static void UseItemOn(u16 item, u8 slot)
{
    // In battle this does not apply the effect itself: the controller does,
    // and then registers B_ACTION_USE_ITEM so the item costs a turn and the
    // opponent gets to respond, which is what the d-pad route does.
    if (gMain.inBattle)
    {
        switch (Ctr3dsQueueBattleItem(item, slot))
        {
        case CTR3DS_ITEM_QUEUED:    sMessage = MSG_QUEUED;    break;
        case CTR3DS_ITEM_NO_EFFECT: sMessage = MSG_NO_EFFECT; break;
        default:                    sMessage = MSG_NOT_NOW;   break;
        }
    }
    else if (!CanUseItemNow())
    {
        sMessage = MSG_NOT_NOW;
    }
    // Inverted return, matching the game: FALSE means the item DID something.
    else if (PokemonUseItemEffects(&gPlayerParty[slot], item, slot, 0, FALSE) == FALSE)
    {
        RemoveBagItem(item, 1);
        sMessage = MSG_USED;
    }
    else
    {
        sMessage = MSG_NO_EFFECT;
    }

    // The list may have shrunk under the cursor. Both routes consume the item --
    // the battle one inside Ctr3dsQueueBattleItem -- so this is checked once
    // here rather than in the branch that happens to be looking.
    if (sCursor > 0 && sCursor >= PocketCount(sPocket))
        sCursor--;
}

// Which slots the picker will accept. Empty slots are obvious; eggs match the
// party menu, which refuses to use an item on one (IsSelectedMonNotEgg).
static bool8 IsPickable(u8 slot)
{
    struct Pokemon *mon = &gPlayerParty[slot];

    if (GetMonData(mon, MON_DATA_SPECIES) == SPECIES_NONE)
        return FALSE;

    return GetMonData(mon, MON_DATA_IS_EGG) == 0;
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
        UiText(LIST_TEXT_X, LIST_Y, UiAscii(label, "Empty.", sizeof(label)),
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

        // The cursor is what the Use button acts on, so it has to be
        // unmistakable rather than a subtle tint.
        if (pos == sCursor)
            UiChevron(LIST_CURSOR_X, y + (UI_GLYPH_H - UI_CHEVRON_H) / 2);

        UiText(LIST_TEXT_X, y, GetItemName(item), UiThemeText(), UiThemeShadow());
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
    const u8 *name;
    u8 label[32];

    UiWindowFrame(RIGHT_TX, PANEL_TY, RIGHT_TW, PANEL_TH);

    if (item == ITEM_NONE)
        return;

    // Icon and name centred. Left-aligning a 32px icon in a column this narrow
    // reads as though it slipped rather than as a layout.
    UiItemIcon(ICON_X, ICON_Y, item);

    name = GetItemName(item);
    UiText(RIGHT_X + (RIGHT_W - UiTextWidth(name)) / 2, NAME_Y, name,
           UiThemeText(), UiThemeShadow());

    // Carries its own line breaks, which UiText honours. Left-aligned, unlike
    // the two above it: this is prose, and three centred lines read as ragged.
    UiText(RIGHT_X, DESC_Y, GetItemDescription(item),
           UiThemeText(), UiThemeShadow());

    if (sMessage != MSG_NONE)
    {
        // Kept inside the 108px column: at 6px a glyph that is 18 characters.
        static const char *const text[] = {
            [MSG_USED]        = "Used it.",
            [MSG_NO_EFFECT]   = "It had no effect.",
            [MSG_NOT_NOW]     = "Not right now.",
            [MSG_QUEUED]      = "Use this turn.",
            [MSG_USE_IN_MENU] = "Use from the menu.",
        };

        UiText(RIGHT_X, USE_Y - 20, UiAscii(label, text[sMessage], sizeof(label)),
               UI_COL_ACCENT, UiThemeShadow());
    }

    UiRect(USE_X, USE_Y, USE_W, USE_H, UI_COL_DIM);
    UiAscii(label, "USE", sizeof(label));
    UiText(USE_X + (USE_W - UiTextWidth(label)) / 2, USE_Y + 5, label,
           UiThemeText(), UiThemeShadow());
}

// One target cell. A 56px-tall relative of the PARTY tab's grid cell: same
// icon-left, text-right split, eight pixels shorter, so the two read as the
// same object in two places rather than as two designs.
static void DrawPickCell(u8 slot)
{
    struct Pokemon *mon = &gPlayerParty[slot];
    int cx = (slot % PICK_COLS) * PICK_CELL_W;
    int cy = PICK_HEAD_H + (slot / PICK_COLS) * PICK_CELL_H;
    u32 species = GetMonData(mon, MON_DATA_SPECIES);
    u8 name[POKEMON_NAME_LENGTH + 1];
    u8 label[8];
    u32 hp, maxHp;

    UiWindowFrame(cx / 8, cy / 8, PICK_CELL_W / 8, PICK_CELL_H / 8);

    // Drawn before the checks below, so an empty or egg slot still shows where
    // the cursor is rather than looking like the selection vanished.
    if (slot == UiSelectedMon())
        UiChevron(cx + PICK_CURSOR_X, cy + (PICK_CELL_H - UI_CHEVRON_H) / 2);

    if (species == SPECIES_NONE)
        return;

    // An egg's species would spoil what is inside it, so it gets neither its
    // icon nor its stats -- just enough to show the slot is taken.
    if (GetMonData(mon, MON_DATA_IS_EGG))
    {
        UiText(cx + PICK_ICON_X, cy + 16, UiAscii(label, "EGG", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
        return;
    }

    UiMonIcon(cx + PICK_ICON_X, cy + 8, (u16)species,
              GetMonData(mon, MON_DATA_PERSONALITY));
    UiStatusIcon(cx + PICK_ICON_X, cy + 40, GetMonAilment(mon));

    GetMonData(mon, MON_DATA_NICKNAME, name);
    UiText(cx + PICK_TEXT_X, cy + 8, name, UiThemeText(), UiThemeShadow());

    UiAscii(label, "Lv", sizeof(label));
    UiText(cx + PICK_TEXT_X, cy + 24, label, UI_COL_DIM, UiThemeShadow());
    UiNum(cx + PICK_TEXT_X + 18, cy + 24, (s32)GetMonData(mon, MON_DATA_LEVEL),
          UiThemeText(), UiThemeShadow());

    // The true value, not the PARTY tab's animated one: choosing who to heal
    // should be answered by what the mon's HP actually is right now.
    hp    = GetMonData(mon, MON_DATA_HP);
    maxHp = GetMonData(mon, MON_DATA_MAX_HP);

    // Labelled to match the PARTY cell this is a relative of. Same reasoning
    // there: beside "Lv 42" a bare number reads as another stat rather than as
    // health. Measured the same way too, off maxHp, even though nothing here
    // animates -- one rule for both cells is what keeps them looking alike.
    UiAscii(label, "HP", sizeof(label));
    UiText(cx + PICK_CELL_W - 10 - UiNumWidth((s32)maxHp) - UiTextWidth(label) - 2,
           cy + 24, label, UI_COL_DIM, UiThemeShadow());
    UiNumRight(cx + PICK_CELL_W - 10, cy + 24, (s32)hp,
               UiThemeText(), UiThemeShadow());
    UiHpBar(cx + PICK_TEXT_X, cy + 40, PICK_CELL_W - PICK_TEXT_X - 10, hp, maxHp);
}

static void DrawPicker(void)
{
    u8 label[24];

    UiText(8, (PICK_HEAD_H - UI_GLYPH_H) / 2,
           UiAscii(label, "Use on which?", sizeof(label)),
           UiThemeText(), UiThemeShadow());

    UiRect(PICK_CANCEL_X, PICK_CANCEL_Y, PICK_CANCEL_W, PICK_CANCEL_H, UI_COL_DIM);
    UiAscii(label, "CANCEL", sizeof(label));
    UiText(PICK_CANCEL_X + (PICK_CANCEL_W - UiTextWidth(label)) / 2,
           PICK_CANCEL_Y + 3, label, UI_COL_ACCENT, UiThemeShadow());

    for (u8 i = 0; i < PARTY_SIZE; i++)
        DrawPickCell(i);
}

void UiBagDraw(void)
{
    if (sView == VIEW_PICK_MON)
    {
        DrawPicker();
        return;
    }

    DrawPocketBar();
    DrawList();
    DrawDetails();
}

// --------------------------------------------------------------- input -----

static void UseTapped(void)
{
    u16 item = CursorItem();

    if (item == ITEM_NONE)
        return;

    // Refuse before the picker rather than after it. Making the player choose a
    // target and only then saying no is worse than saying no immediately.
    if (!CanStartUse())
    {
        sMessage = MSG_NOT_NOW;
        return;
    }

    switch (ItemTargeting(item))
    {
    case TARGET_MON:
        sPickItem = item;
        sMessage = MSG_NONE;
        sView = VIEW_PICK_MON;
        break;

    case TARGET_NONE:
        UseItemOn(item, 0);
        break;

    default:
        sMessage = MSG_USE_IN_MENU;
        break;
    }
}

static void PickerTouch(const CtrTouchState *t)
{
    if (UiHit(t, PICK_CANCEL_X, PICK_CANCEL_Y, PICK_CANCEL_W, PICK_CANCEL_H))
    {
        sView = VIEW_LIST;
        UiMarkDirty();
        return;
    }

    for (u8 i = 0; i < PARTY_SIZE; i++)
    {
        int cx = (i % PICK_COLS) * PICK_CELL_W;
        int cy = PICK_HEAD_H + (i / PICK_COLS) * PICK_CELL_H;

        if (!UiHit(t, cx, cy, PICK_CELL_W, PICK_CELL_H))
            continue;

        if (!IsPickable(i))
            return;

        // A single tap commits here. The USE button was already the deliberate
        // second tap this file's header is about, and a 160x56 cell is not a
        // 24px list row that a resistive panel can slip onto.
        UiSetSelectedMon(i);
        UseItemOn(sPickItem, i);
        sView = VIEW_LIST;
        UiMarkDirty();
        return;
    }
}

void UiBagTouch(const CtrTouchState *t)
{
    u16 count;

    if (!t->justReleased)
        return;

    if (sView == VIEW_PICK_MON)
    {
        PickerTouch(t);
        return;
    }

    if (t->y < POCKET_BAR_H)
    {
        int i = t->x / (CTR_BOTTOM_WIDTH / POCKET_COUNT);

        if (i >= 0 && i < POCKET_COUNT && sPocket != i + 1)
        {
            sPocket = (u8)(i + 1);

            // Keep the list in the order the player asked for. Cheap enough
            // here (it runs on a pocket tap, never on a repaint) and it is the
            // only place this tab can pick up items gained out in the field,
            // which never go through the in-game bag's own sort.
            Ctr3dsSortBagNow();
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
        UseTapped();
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

    // Selecting only moves the cursor. Using takes a second tap on USE.
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
