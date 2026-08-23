// PARTY tab: a 2x3 grid of the player's team, and a detail view per mon.
//
// Everything shown here comes from the game's own accessors -- GetMonData,
// GetMonAbility, gMoveNames, gAbilityNames -- so it cannot drift out of sync
// with what Emerald's own party and summary screens report. Nothing in this
// file writes game state.

#include "global.h"
#include "pokemon.h"
#include "item.h"
#include "data.h"
#include "battle_main.h"
#include "pokemon_summary_screen.h"
#include "constants/species.h"

#include "../bridge.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"

#define COLS      2
#define ROWS      3
#define CELL_W    (CTR_BOTTOM_WIDTH / COLS)     // 160
#define CELL_H    (UI_CONTENT_H / ROWS)         // 64

static bool8 sDetailOpen;

static void HpBar(int x, int y, int w, u32 hp, u32 maxHp)
{
    u16 color = UI_COL_HP_HIGH;
    u32 filled;

    UiFillRect(x, y, w, 8, UI_COL_HP_BACK);

    if (maxHp == 0)
        return;

    filled = (hp * (u32)w) / maxHp;
    // Any surviving HP should show at least a sliver rather than reading as 0.
    if (filled == 0 && hp > 0)
        filled = 1;

    if (hp * 2 <= maxHp) color = UI_COL_HP_MID;
    if (hp * 5 <= maxHp) color = UI_COL_HP_LOW;

    UiFillRect(x, y, (int)filled, 8, color);
}

static void DrawCell(int index)
{
    struct Pokemon *mon = &gPlayerParty[index];
    int cx = (index % COLS) * CELL_W;
    int cy = (index / COLS) * CELL_H;
    u32 species, hp, maxHp, level;
    u8 name[POKEMON_NAME_LENGTH + 1];
    u8 label[8];

    UiWindowFrame(cx / 8, cy / 8, CELL_W / 8, CELL_H / 8);

    species = GetMonData(mon, MON_DATA_SPECIES);
    if (species == SPECIES_NONE)
        return;

    UiMonIcon(cx + 6, cy + 16, (u16)species, GetMonData(mon, MON_DATA_PERSONALITY));

    GetMonData(mon, MON_DATA_NICKNAME, name);
    UiText(cx + 42, cy + 8, name, UI_COL_TEXT, UI_COL_SHADOW);

    level = GetMonData(mon, MON_DATA_LEVEL);
    UiAscii(label, "Lv", sizeof(label));
    UiText(cx + 42, cy + 26, label, UI_COL_DIM, UI_COL_SHADOW);
    UiNum(cx + 60, cy + 26, (s32)level, UI_COL_TEXT, UI_COL_SHADOW);

    hp    = GetMonData(mon, MON_DATA_HP);
    maxHp = GetMonData(mon, MON_DATA_MAX_HP);

    UiNumRight(cx + CELL_W - 10, cy + 26, (s32)hp, UI_COL_TEXT, UI_COL_SHADOW);
    HpBar(cx + 42, cy + 46, CELL_W - 52, hp, maxHp);

    // The selected slot is what the BAG tab will act on, so it needs to be
    // unmistakable.
    if (index == UiSelectedMon())
        UiRect(cx + 2, cy + 2, CELL_W - 4, CELL_H - 4, UI_COL_ACCENT);
}

static void DrawDetail(void)
{
    struct Pokemon *mon = &gPlayerParty[UiSelectedMon()];
    u32 species = GetMonData(mon, MON_DATA_SPECIES);
    u8 name[POKEMON_NAME_LENGTH + 1];
    u8 label[16];
    int y;

    UiWindowFrame(0, 0, CTR_BOTTOM_WIDTH / 8, UI_CONTENT_H / 8);

    if (species == SPECIES_NONE)
    {
        UiText(16, 16, UiAscii(label, "Empty slot", sizeof(label)),
               UI_COL_DIM, UI_COL_SHADOW);
        return;
    }

    UiMonIcon(12, 12, (u16)species, GetMonData(mon, MON_DATA_PERSONALITY));

    GetMonData(mon, MON_DATA_NICKNAME, name);
    UiText(52, 12, name, UI_COL_TEXT, UI_COL_SHADOW);

    UiText(52, 30, UiAscii(label, "Lv", sizeof(label)), UI_COL_DIM, UI_COL_SHADOW);
    UiNum(70, 30, (s32)GetMonData(mon, MON_DATA_LEVEL), UI_COL_TEXT, UI_COL_SHADOW);

    UiText(110, 30, gSpeciesNames[species], UI_COL_DIM, UI_COL_SHADOW);

    // HP
    y = 52;
    UiText(12, y, UiAscii(label, "HP", sizeof(label)), UI_COL_DIM, UI_COL_SHADOW);
    UiNum(44, y, (s32)GetMonData(mon, MON_DATA_HP), UI_COL_TEXT, UI_COL_SHADOW);
    UiText(76, y, UiAscii(label, "/", sizeof(label)), UI_COL_DIM, UI_COL_SHADOW);
    UiNum(86, y, (s32)GetMonData(mon, MON_DATA_MAX_HP), UI_COL_TEXT, UI_COL_SHADOW);
    HpBar(140, y + 4, 160, GetMonData(mon, MON_DATA_HP),
          GetMonData(mon, MON_DATA_MAX_HP));

    // Stats, two columns
    {
        static const char *const names[5] = { "ATK", "DEF", "SPA", "SPD", "SPE" };
        const s32 fields[5] = {
            (s32)GetMonData(mon, MON_DATA_ATK),
            (s32)GetMonData(mon, MON_DATA_DEF),
            (s32)GetMonData(mon, MON_DATA_SPATK),
            (s32)GetMonData(mon, MON_DATA_SPDEF),
            (s32)GetMonData(mon, MON_DATA_SPEED),
        };

        for (int i = 0; i < 5; i++)
        {
            int sx = 12 + (i % 3) * 66;
            int sy = 72 + (i / 3) * 18;
            UiText(sx, sy, UiAscii(label, names[i], sizeof(label)),
                   UI_COL_DIM, UI_COL_SHADOW);
            UiNum(sx + 34, sy, fields[i], UI_COL_TEXT, UI_COL_SHADOW);
        }
    }

    // Ability / nature / held item
    y = 112;
    UiText(12, y, UiAscii(label, "ABILITY", sizeof(label)), UI_COL_DIM, UI_COL_SHADOW);
    UiText(80, y, gAbilityNames[GetMonAbility(mon)], UI_COL_TEXT, UI_COL_SHADOW);

    y += 16;
    UiText(12, y, UiAscii(label, "NATURE", sizeof(label)), UI_COL_DIM, UI_COL_SHADOW);
    UiText(80, y, gNatureNamePointers[GetNature(mon)], UI_COL_TEXT, UI_COL_SHADOW);

    y += 16;
    {
        u16 item = (u16)GetMonData(mon, MON_DATA_HELD_ITEM);
        UiText(12, y, UiAscii(label, "ITEM", sizeof(label)), UI_COL_DIM, UI_COL_SHADOW);
        if (item != ITEM_NONE)
            UiText(80, y, GetItemName(item), UI_COL_TEXT, UI_COL_SHADOW);
        else
            UiText(80, y, UiAscii(label, "none", sizeof(label)),
                   UI_COL_DIM, UI_COL_SHADOW);
    }

    // Moves, two columns
    for (int i = 0; i < MAX_MON_MOVES; i++)
    {
        u16 move = (u16)GetMonData(mon, MON_DATA_MOVE1 + i);
        if (move == MOVE_NONE)
            continue;

        UiText(180, 72 + i * 16, gMoveNames[move], UI_COL_TEXT, UI_COL_SHADOW);
    }

    // Back target, top-right.
    UiRect(CTR_BOTTOM_WIDTH - 46, 8, 38, 22, UI_COL_DIM);
    UiText(CTR_BOTTOM_WIDTH - 40, 12, UiAscii(label, "BACK", sizeof(label)),
           UI_COL_ACCENT, UI_COL_SHADOW);
}

void UiPartyDraw(void)
{
    if (sDetailOpen)
        DrawDetail();
    else
        for (int i = 0; i < PARTY_SIZE; i++)
            DrawCell(i);
}

void UiPartyTouch(const CtrTouchState *t)
{
    if (!t->justReleased)
        return;

    if (sDetailOpen)
    {
        if (UiHit(t, CTR_BOTTOM_WIDTH - 46, 8, 38, 22))
        {
            sDetailOpen = FALSE;
            UiMarkDirty();
        }
        return;
    }

    for (int i = 0; i < PARTY_SIZE; i++)
    {
        int cx = (i % COLS) * CELL_W;
        int cy = (i / COLS) * CELL_H;

        if (!UiHit(t, cx, cy, CELL_W, CELL_H))
            continue;

        // First tap selects, a tap on the already-selected slot opens detail.
        // That keeps selection (which BAG needs) reachable without a long press
        // on a resistive panel.
        if (UiSelectedMon() == i)
            sDetailOpen = TRUE;
        else
            UiSetSelectedMon((u8)i);

        UiMarkDirty();
        return;
    }
}
