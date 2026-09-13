// BAG tab: the pockets and the item list on the left, the details and USE on
// the right. A party target picker opens over both when an item needs a target.
//
// A tap on a row only moves the cursor. The player must then tap USE. On a
// resistive panel, a single tap must not use a Full Restore by mistake.
//
// This file writes game state, so the gates in CanUseItemNow(), ItemTargeting()
// and Ctr3dsQueueBattleItem() are important. All other code here only reads.
//
// Do not use GetItemFieldFunc(). The game's field-use flows need the bag menu's
// task and draw on the top screen. ItemTargeting() refuses the two item classes
// that would reach the top screen another way.

#include "global.h"
#include "main.h"
#include "item.h"
#include "pokemon.h"
#include "overworld.h"
#include "script.h"
#include "battle.h"              // struct DisableStruct
#include "battle_controllers.h"
#include "party_menu.h"          // GetItemEffectType
#include "constants/items.h"
#include "constants/item_effects.h"
#include "constants/species.h"

#include "../bridge.h"
#include "../tweaks.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "status_tags.h"
#include "ui_team.h"

#define POCKET_COUNT  5
#define POCKET_BAR_H  22

// Two panels, both on the player's window frame. 24 + 16 tiles fill the 40-tile
// width.
//
// 24 tiles is the maximum for the list. The details column then has 108px of
// text width. The widest line in an item description is exactly 108px.
#define PANEL_Y       24
#define PANEL_TY      (PANEL_Y / 8)
#define PANEL_TH      ((UI_CONTENT_H - PANEL_Y) / 8)
#define LEFT_TW       24
#define RIGHT_TX      LEFT_TW
#define RIGHT_TW      ((CTR_BOTTOM_WIDTH / 8) - LEFT_TW)

#define LEFT_X        10
#define LEFT_W        (LEFT_TW * 8 - 20)

// The selected row gets a cursor in its own column, as in the game's menus. The
// column is the glyph plus a 4px gap. All rows start their text after it, so
// the list does not move when the cursor moves.
#define LIST_CURSOR_X (LEFT_X)
#define LIST_TEXT_X   (LEFT_X + UI_CHEVRON_W + 4)
#define LIST_Y        (PANEL_Y + 10)
#define ROW_H         24
#define VISIBLE_ROWS  5

#define PAGE_Y        (LIST_Y + VISIBLE_ROWS * ROW_H + 2)
#define PAGE_W        56
#define PAGE_H        20
// The same names as in tab_dex.c, because it is the same control.
#define PAGE_UP_X     LEFT_X
#define PAGE_DN_X     (LEFT_X + PAGE_W + 8)

#define RIGHT_X       (RIGHT_TX * 8 + 10)
#define RIGHT_W       (RIGHT_TW * 8 - 20)

// The narrow column puts the icon above the name. A 32px icon and the longest
// item name (72px) do not fit on one line of 108px.
#define ICON_X        (RIGHT_X + (RIGHT_W - 32) / 2)
#define ICON_Y        (PANEL_Y + 6)
#define NAME_Y        (PANEL_Y + 40)
#define DESC_Y        (PANEL_Y + 58)

#define USE_W         88
#define USE_H         26
#define USE_X         (RIGHT_X + (RIGHT_W - USE_W) / 2)
#define USE_Y         (UI_CONTENT_H - USE_H - 12)

// Target picker: a prompt band over a 2x3 grid of the team. The bands must add
// up to UI_CONTENT_H and land on 8px tiles: 24 + 3 * 56 = 192.
#define PICK_HEAD_H   24
#define PICK_COLS     2
#define PICK_ROWS     3
#define PICK_CELL_W   (CTR_BOTTOM_WIDTH / PICK_COLS)
#define PICK_CELL_H   ((UI_CONTENT_H - PICK_HEAD_H) / PICK_ROWS)

// The same three columns as the PARTY cells: the cursor, the icon with the
// status, and the text.
#define PICK_CURSOR_X 8
#define PICK_ICON_X   18
#define PICK_TEXT_X   54

#define PICK_CANCEL_W 56
#define PICK_CANCEL_H 20
#define PICK_CANCEL_X (CTR_BOTTOM_WIDTH - PICK_CANCEL_W - 8)
#define PICK_CANCEL_Y 2

static u8  sPocket = POCKET_ITEMS;   // pocket ids start at 1
static u16 sScroll;
static u16 sCursor;                  // the row of the cursor, absolute
// One counter for each pager, so a held UP or DN scrolls the list. See
// UiHoldRepeat.
static UiHold sHoldUp, sHoldDn;

// The picker is modal over the full content area, like the detail view of the
// PARTY tab.
enum { VIEW_LIST, VIEW_PICK_MON };
static u8  sView;
static u16 sPickItem;                // the item that USE was tapped for

enum { MSG_NONE, MSG_USED, MSG_NO_EFFECT, MSG_NOT_NOW, MSG_QUEUED, MSG_USE_IN_MENU };
static u8 sMessage;

static const char *const sPocketNames[POCKET_COUNT] =
    { "ITEM", "BALL", "TM", "BERRY", "KEY" };

// The game keeps the pockets compact (CompactItemsInBagPocket), so the first
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
// Use an item only in the overworld, with no script in control. A change to the
// party during a script can conflict with the script. CtrBottomUpdate runs
// after the frame's callbacks, so they are complete.
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

// An Ether restores one move and an Elixir restores all four. The party menu
// uses this bit to open a move list for the first kind (ItemUseCB_PPRecovery).
// This UI has no move list, so it refuses that kind.
static bool8 PpItemNeedsMoveChoice(u16 item)
{
    const u8 *effect;

    if (item == ITEM_ENIGMA_BERRY)
        effect = gSaveBlock1Ptr->enigmaBerry.itemEffect;
    else
        effect = gItemEffectTable[item - ITEM_POTION];

    return (effect[4] & ITEM4_HEAL_PP_ONE) != 0;
}

// What a USE tap needs before it can act. It uses the game's own tables, not a
// list of item ids, so it treats all items of a class the same.
enum { TARGET_MON, TARGET_NONE, TARGET_UNSUPPORTED };

static u8 ItemTargeting(u16 item)
{
    u8 effect = GetItemEffectType(item);

    // The item needs a move as well as a mon. The first move slot is not a safe
    // default, so refuse the item.
    if (effect == ITEM_EFFECT_PP_UP || effect == ITEM_EFFECT_PP_MAX)
        return TARGET_UNSUPPORTED;
    if (effect == ITEM_EFFECT_HEAL_PP && PpItemNeedsMoveChoice(item))
        return TARGET_UNSUPPORTED;

    // In battle, the engine's table tells which items are available and if they
    // need a party choice. MEDICINE needs a party menu in the game's bag. OTHER
    // is balls, X items and escape items, which need no choice.
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

    // Refuse all other items out of battle. Two of them must be refused:
    // - EVO_STONE: PokemonUseItemEffects() calls BeginEvolutionScene() directly
    //   (src/pokemon.c). That takes gMain.callback2 during CtrBottomUpdate,
    //   which is in the middle of the frame.
    // - RAISE_LEVEL: the table effect is only the level-up. ItemUseCB_RareCandy
    //   checks for a new move after it (src/party_menu.c), so the bare effect
    //   skips a learnset move.
    //
    // X_ITEM does nothing out of battle. SACRED_ASH needs the full party. NONE
    // has no effect table.
    default:
        return TARGET_UNSUPPORTED;
    }
}

// The gate for both routes. Check it before the picker opens and again when the
// player commits. A turn can pass, or a script can start, while the picker is
// up.
static bool8 CanStartUse(void)
{
    return gMain.inBattle ? Ctr3dsPlayerIsChoosingAction() : CanUseItemNow();
}

// TARGET_NONE items ignore `slot`. A ball has no target, and an X item acts on
// the mon that is out, which the battle controller finds itself.
static void UseItemOn(u16 item, u8 slot)
{
    // In battle, the controller applies the effect. It then registers
    // B_ACTION_USE_ITEM, so the item costs a turn and the opponent can respond,
    // as on the d-pad route.
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
    // The return value is inverted, as in the game: FALSE means that the item
    // had an effect. Use gPlayerParty directly, not UiPartyMon. Out of battle,
    // the party is always in field order, and the effect uses `slot` as an
    // index into it.
    else if (PokemonUseItemEffects(&gPlayerParty[slot], item, slot, 0, FALSE) == FALSE)
    {
        RemoveBagItem(item, 1);
        sMessage = MSG_USED;
    }
    else
    {
        sMessage = MSG_NO_EFFECT;
    }

    // The list can become shorter under the cursor. Both routes use up the
    // item, so check this once here.
    if (sCursor > 0 && sCursor >= PocketCount(sPocket))
        sCursor--;
}

// The slots that the picker accepts. Not empty slots. Not eggs, which the party
// menu also refuses (IsSelectedMonNotEgg).
static bool8 IsPickable(u8 slot)
{
    struct Pokemon *mon = UiPartyMon(slot);

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

        // USE acts on the cursor row, so the cursor must be clear.
        if (pos == sCursor)
            UiChevron(LIST_CURSOR_X, y + (UI_GLYPH_H - UI_CHEVRON_H) / 2);

        UiText(LIST_TEXT_X, y, GetItemName(item), UiThemeText(), UiThemeShadow());
        UiNumRight(LEFT_X + LEFT_W, y,
                   (s32)BagGetQuantityByPocketPosition(sPocket, pos),
                   UI_COL_DIM, UiThemeShadow());
    }

    // The pagers go below the list. The column is too narrow for a pager next
    // to the rows.
    //
    // They use the same UiArrow as the DEX list, centered the same way. The two
    // lists scroll the same, so they look the same.
    if (sScroll > 0)
    {
        UiRect(PAGE_UP_X, PAGE_Y, PAGE_W, PAGE_H, UI_COL_DIM);
        UiArrow(PAGE_UP_X + (PAGE_W - UI_ARROW_W) / 2,
                PAGE_Y + (PAGE_H - UI_ARROW_H) / 2, TRUE, UI_COL_ACCENT);
    }

    if (sScroll + VISIBLE_ROWS < count)
    {
        UiRect(PAGE_DN_X, PAGE_Y, PAGE_W, PAGE_H, UI_COL_DIM);
        UiArrow(PAGE_DN_X + (PAGE_W - UI_ARROW_W) / 2,
                PAGE_Y + (PAGE_H - UI_ARROW_H) / 2, FALSE, UI_COL_ACCENT);
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

    // Center the icon and the name. A 32px icon on the left of a narrow column
    // looks misaligned.
    UiItemIcon(ICON_X, ICON_Y, item);

    name = GetItemName(item);
    UiText(RIGHT_X + (RIGHT_W - UiTextWidth(name)) / 2, NAME_Y, name,
           UiThemeText(), UiThemeShadow());

    // The description has its own line breaks, which UiText follows. It is
    // aligned to the left, because centered lines of text look ragged.
    UiText(RIGHT_X, DESC_Y, GetItemDescription(item),
           UiThemeText(), UiThemeShadow());

    if (sMessage != MSG_NONE)
    {
        // Keep this inside the 108px column: 18 characters at 6px.
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

// One target cell. It is like the PARTY grid cell, 8px shorter, with the icon
// on the left and the text on the right. Thus the two look like the same
// object.
static void DrawPickCell(u8 slot)
{
    struct Pokemon *mon = UiPartyMon(slot);
    int cx = (slot % PICK_COLS) * PICK_CELL_W;
    int cy = PICK_HEAD_H + (slot / PICK_COLS) * PICK_CELL_H;
    u32 species = GetMonData(mon, MON_DATA_SPECIES);
    u8 name[POKEMON_NAME_LENGTH + 1];
    u8 label[8];
    u32 hp, maxHp;

    UiWindowFrame(cx / 8, cy / 8, PICK_CELL_W / 8, PICK_CELL_H / 8);

    // A battle partner's Pokemon gets the partner's color, as on the PARTY tab.
    // It stays pickable, because the game's own bag lets the player use an item
    // on it. The color makes sure that the player sees whose Pokemon it is.
    if (UiAllySlot(slot) && species != SPECIES_NONE)
        UiAllyFrameGround(cx, cy, PICK_CELL_W, PICK_CELL_H);

    // Draw the cursor before the checks below, so an empty slot or an egg still
    // shows the selection.
    if (slot == UiSelectedMon())
        UiChevron(cx + PICK_CURSOR_X, cy + (PICK_CELL_H - UI_CHEVRON_H) / 2);

    if (species == SPECIES_NONE)
        return;

    // An egg shows no icon and no stats, because the species is a secret. It
    // shows only that the slot is full.
    if (GetMonData(mon, MON_DATA_IS_EGG))
    {
        UiText(cx + PICK_ICON_X, cy + 16, UiAscii(label, "EGG", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
        return;
    }

    UiMonIcon(cx + PICK_ICON_X, cy + 8, (u16)species,
              GetMonData(mon, MON_DATA_PERSONALITY));

    // The same tag as the PARTY cell, CNF included. This is where a Persim
    // Berry or a Full Heal gets its target. The picker has no animated layer,
    // so a mon with two tags flips by a full repaint (UiStatusTagsKey in the
    // shell's hash).
    UiStatusIcon(cx + PICK_ICON_X, cy + 40, UiStatusTag(slot));

    GetMonData(mon, MON_DATA_NICKNAME, name);
    UiText(cx + PICK_TEXT_X, cy + 8, name, UiThemeText(), UiThemeShadow());

    UiAscii(label, "Lv", sizeof(label));
    UiText(cx + PICK_TEXT_X, cy + 24, label, UI_COL_DIM, UiThemeShadow());
    UiNum(cx + PICK_TEXT_X + 18, cy + 24, (s32)GetMonData(mon, MON_DATA_LEVEL),
          UiThemeText(), UiThemeShadow());

    // The true HP, not the PARTY tab's animated value. The player chooses who
    // to heal from the real HP.
    hp    = GetMonData(mon, MON_DATA_HP);
    maxHp = GetMonData(mon, MON_DATA_MAX_HP);

    // The label matches the PARTY cell, where a bare number next to "Lv 42"
    // looks like a different stat. The position also comes from maxHp, so both
    // cells look the same.
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

bool8 UiBagPickerOpen(void)
{
    return sView == VIEW_PICK_MON;
}

// --------------------------------------------------------------- input -----

static void UseTapped(void)
{
    u16 item = CursorItem();

    if (item == ITEM_NONE)
        return;

    // Refuse before the picker opens, not after the player chooses a target.
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

        // A single tap commits here. The USE tap was the second, deliberate
        // tap, and a 160x56 cell is too large for a slip.
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

    if (sView == VIEW_PICK_MON)
    {
        if (t->justReleased)
            PickerTouch(t);
        return;
    }

    // Test both pagers before the justReleased guard below, because a held
    // pager acts on frames with no release. A plain tap still acts once, on
    // release. Nothing else uses these rects.
    if (UiHoldRepeat(&sHoldUp, t, PAGE_UP_X, PAGE_Y, PAGE_W, PAGE_H))
    {
        if (sScroll > 0)
        {
            sScroll--;
            UiMarkDirty();
        }
        return;
    }

    if (UiHoldRepeat(&sHoldDn, t, PAGE_DN_X, PAGE_Y, PAGE_W, PAGE_H))
    {
        if (sScroll + VISIBLE_ROWS < PocketCount(sPocket))
        {
            sScroll++;
            UiMarkDirty();
        }
        return;
    }

    if (!t->justReleased)
        return;

    if (t->y < POCKET_BAR_H)
    {
        int i = t->x / (CTR_BOTTOM_WIDTH / POCKET_COUNT);

        if (i >= 0 && i < POCKET_COUNT && sPocket != i + 1)
        {
            sPocket = (u8)(i + 1);

            // Keep the list in the order that the player selected. This runs on
            // a pocket tap, not on a repaint. It is the only place where this
            // tab sorts items that the player got in the field.
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

    // A tap only moves the cursor. USE needs a second tap.
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
