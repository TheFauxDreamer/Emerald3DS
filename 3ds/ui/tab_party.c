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
#include "battle.h"             // struct DisableStruct, for the headers below
#include "battle_main.h"
#include "party_menu.h"         // GetMonAilment
#include "pokemon_summary_screen.h"
#include "constants/species.h"
#include "constants/party_menu.h"

#include "../bridge.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "matchup.h"

#define COLS      2
#define ROWS      3
#define CELL_W    (CTR_BOTTOM_WIDTH / COLS)     // 160
#define CELL_H    (UI_CONTENT_H / ROWS)         // 64

// A cell is laid out as three columns: the selection cursor, the mon's icon and
// status badge, then everything textual. The cursor column is always reserved,
// selected or not, so the cards do not shuffle sideways as the selection moves.
// The first two sit at the frame's interior edge rather than over its border.
#define CELL_CURSOR_X   8
#define CELL_ICON_X     18
#define CELL_TEXT_X     54

// Detail view: the stats block and the moves list share this row band, so their
// widths are one decision, not two. A stat cell is an 18px label plus an 18px
// three-digit value, and the longest move name is 72px (THUNDERSHOCK), which
// leaves the moves column ending at 268 inside a 312px interior.
#define STAT_X(col)     (12 + (col) * 60)
#define STAT_Y          72
#define STAT_ROW_H      18
#define STAT_VALUE_DX   34
#define MOVES_X         196
#define MOVES_Y         72

// The detail view's way out. One definition, because the drawn rect and the hit
// test in UiPartyTouch have to agree; the dex entry screen names its own the
// same way.
#define BACK_X          (CTR_BOTTOM_WIDTH - 46)
#define BACK_Y          8
#define BACK_W          38
#define BACK_H          22

static bool8 sDetailOpen;

// ------------------------------------------------------- HP bar animation --
//
// The game slides its battle HP bar rather than snapping it, and the number
// counts along with it. That bar is 48 px wide and moves about a pixel a frame,
// so stepping maxHp/48 per frame gives the same pace here. Its own logic
// (CalcNewBarValue, src/battle_interface.c) is static and tied to healthbox
// sprites, so it cannot be called; this matches the feel instead.
#define HP_ANIM_FRAMES 48

static u32 sShownHp[PARTY_SIZE];
static u32 sShownMax[PARTY_SIZE];
static u32 sShownSpecies[PARTY_SIZE];

bool8 UiPartyTick(void)
{
    bool8 moving = FALSE;

    for (u32 i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *mon = &gPlayerParty[i];
        u32 species = GetMonData(mon, MON_DATA_SPECIES);
        u32 hp      = GetMonData(mon, MON_DATA_HP);
        u32 maxHp   = GetMonData(mon, MON_DATA_MAX_HP);
        u32 step;

        // A different mon in the slot, or a changed maximum from a level-up or
        // evolution, has no relationship to the bar currently drawn. Snap,
        // rather than sliding from a value that measured something else.
        if (species != sShownSpecies[i] || maxHp != sShownMax[i])
        {
            sShownSpecies[i] = species;
            sShownMax[i] = maxHp;
            sShownHp[i] = hp;
            continue;
        }

        if (sShownHp[i] == hp)
            continue;

        step = maxHp / HP_ANIM_FRAMES;
        if (step == 0)
            step = 1;   // tiny maximums must still move

        // Land exactly on the target rather than overshooting past it.
        if (sShownHp[i] > hp)
            sShownHp[i] = (sShownHp[i] - hp <= step) ? hp : sShownHp[i] - step;
        else
            sShownHp[i] = (hp - sShownHp[i] <= step) ? hp : sShownHp[i] + step;

        moving = TRUE;
    }

    return moving;
}

#define ARROW_GAP 3
#define ARROW_PAIR_W (UI_ARROW_W * 2 + ARROW_GAP)

// Two arrows, drawn only when the matchup is actually worth noticing. Neutral
// is the common case and marking it would just add noise to five other cells.
//
// Direction carries the meaning and colour reinforces it: up and green is good
// for you, down and red is bad for you. That reads at a glance in a way a flat
// coloured square does not. The left arrow is offence (can this one hit it
// hard), the right is risk (how hard does it hit back).
//
// `x` is where the nickname ended, so the pair follows the name rather than
// floating in the corner. `xLimit` is the rightmost pixel they may occupy.
static void DrawMatchupArrows(int x, int y, int xLimit, struct Pokemon *mon)
{
    u16 off, risk;

    if (!UiMatchupActive())
        return;

    // A long nickname would otherwise push these through the window frame.
    if (x + ARROW_PAIR_W > xLimit)
        x = xLimit - ARROW_PAIR_W;

    off  = UiMatchupOffence(mon);
    risk = UiMatchupRisk(mon);

    if (off != UI_MATCHUP_NA && off != TYPE_MUL_NORMAL)
    {
        // Amber for a resisted move, red only for one that does nothing at all:
        // the difference between "weak" and "pointless" is worth keeping.
        if (off > TYPE_MUL_NORMAL)
            UiArrow(x, y, TRUE, UI_COL_HP_HIGH);
        else
            UiArrow(x, y, FALSE, (off == 0) ? UI_COL_HP_LOW : UI_COL_HP_MID);
    }

    // Risk reads the other way round: a large multiplier against us is bad.
    if (risk != UI_MATCHUP_NA && risk != TYPE_MUL_NORMAL)
        UiArrow(x + UI_ARROW_W + ARROW_GAP, y,
                risk < TYPE_MUL_NORMAL,
                (risk > TYPE_MUL_NORMAL) ? UI_COL_HP_LOW : UI_COL_HP_HIGH);
}

static void DrawCell(int index)
{
    struct Pokemon *mon = &gPlayerParty[index];
    int cx = (index % COLS) * CELL_W;
    int cy = (index / COLS) * CELL_H;
    u32 species, hp, maxHp, level;
    u8 name[POKEMON_NAME_LENGTH + 1];
    u8 label[8];
    int nameW;

    UiWindowFrame(cx / 8, cy / 8, CELL_W / 8, CELL_H / 8);

    // Before the empty-slot check below, so an empty slot still shows which one
    // the player is on. This slot is what the detail view opens on and what the
    // BAG tab's target picker starts from, so it has to be readable either way.
    if (index == UiSelectedMon())
        UiChevron(cx + CELL_CURSOR_X, cy + (CELL_H - UI_CHEVRON_H) / 2);

    species = GetMonData(mon, MON_DATA_SPECIES);
    if (species == SPECIES_NONE)
        return;

    UiMonIcon(cx + CELL_ICON_X, cy + 16, (u16)species,
              GetMonData(mon, MON_DATA_PERSONALITY));

    // The 32x8 strip under the mon icon is otherwise empty, and the badge is
    // 32x8, so status lands next to the mon it belongs to without disturbing
    // anything. The HP bar starts at CELL_TEXT_X, well clear of it.
    UiStatusIcon(cx + CELL_ICON_X, cy + 48, GetMonAilment(mon));

    GetMonData(mon, MON_DATA_NICKNAME, name);
    nameW = UiText(cx + CELL_TEXT_X, cy + 8, name, UiThemeText(), UiThemeShadow());

    // Centred in the 15px name row, immediately after the name.
    DrawMatchupArrows(cx + CELL_TEXT_X + nameW + 4,
                      cy + 8 + (UI_GLYPH_H - UI_ARROW_H) / 2,
                      cx + CELL_W - 8, mon);

    level = GetMonData(mon, MON_DATA_LEVEL);
    UiAscii(label, "Lv", sizeof(label));
    UiText(cx + CELL_TEXT_X, cy + 26, label, UI_COL_DIM, UiThemeShadow());
    UiNum(cx + CELL_TEXT_X + 18, cy + 26, (s32)level, UiThemeText(), UiThemeShadow());

    // The animated value, not the raw one: bar and number slide together.
    hp    = sShownHp[index];
    maxHp = GetMonData(mon, MON_DATA_MAX_HP);

    UiNumRight(cx + CELL_W - 10, cy + 26, (s32)hp, UiThemeText(), UiThemeShadow());
    UiHpBar(cx + CELL_TEXT_X, cy + 46, CELL_W - CELL_TEXT_X - 10, hp, maxHp);
}

static void DrawDetail(void)
{
    struct Pokemon *mon = &gPlayerParty[UiSelectedMon()];
    u32 species = GetMonData(mon, MON_DATA_SPECIES);
    u8 name[POKEMON_NAME_LENGTH + 1];
    u8 label[16];
    int y, nameW;

    UiWindowFrame(0, 0, CTR_BOTTOM_WIDTH / 8, UI_CONTENT_H / 8);

    // Drawn first, before any content that might bail. The way out of a modal
    // must not depend on what is inside it: an empty slot used to return below
    // without ever reaching this, leaving a view whose only exit was an
    // invisible rectangle the player had no reason to touch.
    UiRect(BACK_X, BACK_Y, BACK_W, BACK_H, UI_COL_DIM);
    UiText(BACK_X + 6, BACK_Y + 4, UiAscii(label, "BACK", sizeof(label)),
           UI_COL_ACCENT, UiThemeShadow());

    if (species == SPECIES_NONE)
    {
        UiText(16, 16, UiAscii(label, "Empty slot", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
        return;
    }

    UiMonIcon(12, 12, (u16)species, GetMonData(mon, MON_DATA_PERSONALITY));

    GetMonData(mon, MON_DATA_NICKNAME, name);
    nameW = UiText(52, 12, name, UiThemeText(), UiThemeShadow());

    // Same treatment as the grid: arrows follow the name. The limit is the BACK
    // target's left edge rather than the window frame.
    DrawMatchupArrows(52 + nameW + 4, 12 + (UI_GLYPH_H - UI_ARROW_H) / 2,
                      BACK_X - 6, mon);

    UiText(52, 30, UiAscii(label, "Lv", sizeof(label)), UI_COL_DIM, UiThemeShadow());
    UiNum(70, 30, (s32)GetMonData(mon, MON_DATA_LEVEL), UiThemeText(), UiThemeShadow());

    UiText(110, 30, gSpeciesNames[species], UI_COL_DIM, UiThemeShadow());

    // HP
    y = 52;
    UiText(12, y, UiAscii(label, "HP", sizeof(label)), UI_COL_DIM, UiThemeShadow());
    UiNum(44, y, (s32)sShownHp[UiSelectedMon()], UiThemeText(), UiThemeShadow());
    UiText(76, y, UiAscii(label, "/", sizeof(label)), UI_COL_DIM, UiThemeShadow());
    UiNum(86, y, (s32)GetMonData(mon, MON_DATA_MAX_HP), UiThemeText(), UiThemeShadow());

    // Status belongs on the HP row; the bar gives up 20px to make room for it.
    UiStatusIcon(120, y + 2, GetMonAilment(mon));
    UiHpBar(160, y + 4, 140, sShownHp[UiSelectedMon()],
            GetMonData(mon, MON_DATA_MAX_HP));

    // Stats, two rows of three, and the moves list beside them.
    //
    // The pitch and the moves column are tied together: the third stat column's
    // value sits at STAT_X(2) + STAT_VALUE_DX and runs 18px (three digits), so
    // it must finish before MOVES_X. At the original 66px pitch it ended at 196
    // against a moves column at 180, and SPA printed straight through the first
    // move name.
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
            int sx = STAT_X(i % 3);
            int sy = STAT_Y + (i / 3) * STAT_ROW_H;
            UiText(sx, sy, UiAscii(label, names[i], sizeof(label)),
                   UI_COL_DIM, UiThemeShadow());
            UiNum(sx + STAT_VALUE_DX, sy, fields[i], UiThemeText(), UiThemeShadow());
        }
    }

    // Ability / nature / held item
    y = 112;
    UiText(12, y, UiAscii(label, "ABILITY", sizeof(label)), UI_COL_DIM, UiThemeShadow());
    {
        // Deliberately NOT GetMonAbility(): it routes through
        // GetAbilityBySpecies(), which writes gLastUsedAbility -- a global the
        // battle message system reads (src/battle_message.c:2511). Displaying
        // an ability must not perturb that. This is the same lookup, read-only.
        u8 abilityNum = (u8)GetMonData(mon, MON_DATA_ABILITY_NUM);
        u8 ability = gSpeciesInfo[species].abilities[abilityNum ? 1 : 0];
        UiText(80, y, gAbilityNames[ability], UiThemeText(), UiThemeShadow());
    }

    y += 16;
    UiText(12, y, UiAscii(label, "NATURE", sizeof(label)), UI_COL_DIM, UiThemeShadow());
    UiText(80, y, gNatureNamePointers[GetNature(mon)], UiThemeText(), UiThemeShadow());

    y += 16;
    {
        u16 item = (u16)GetMonData(mon, MON_DATA_HELD_ITEM);
        UiText(12, y, UiAscii(label, "ITEM", sizeof(label)), UI_COL_DIM, UiThemeShadow());
        if (item != ITEM_NONE)
            UiText(80, y, GetItemName(item), UiThemeText(), UiThemeShadow());
        else
            UiText(80, y, UiAscii(label, "none", sizeof(label)),
                   UI_COL_DIM, UiThemeShadow());
    }

    // Moves, two columns
    for (int i = 0; i < MAX_MON_MOVES; i++)
    {
        u16 move = (u16)GetMonData(mon, MON_DATA_MOVE1 + i);
        if (move == MOVE_NONE)
            continue;

        UiText(MOVES_X, MOVES_Y + i * UI_LINE_H, gMoveNames[move],
               UiThemeText(), UiThemeShadow());
    }
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
        if (UiHit(t, BACK_X, BACK_Y, BACK_W, BACK_H))
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
