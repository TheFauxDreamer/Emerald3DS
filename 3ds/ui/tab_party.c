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
#include "matchup.h"

#define COLS      2
#define ROWS      3
#define CELL_W    (CTR_BOTTOM_WIDTH / COLS)     // 160
#define CELL_H    (UI_CONTENT_H / ROWS)         // 64

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

// Two small squares, drawn only when the matchup is actually worth noticing.
// Neutral is the common case and marking it would just add noise to five other
// cells. Left square is offence (can this one hit it hard), right is risk (how
// hard does it hit back).
static void DrawMatchupBadges(int cx, int cy, struct Pokemon *mon)
{
    u16 off, risk;

    if (!UiMatchupActive())
        return;

    off  = UiMatchupOffence(mon);
    risk = UiMatchupRisk(mon);

    if (off != UI_MATCHUP_NA && off != TYPE_MUL_NORMAL)
    {
        u16 c = (off > TYPE_MUL_NORMAL) ? UI_COL_HP_HIGH
              : (off == 0)              ? UI_COL_HP_LOW
              :                           UI_COL_HP_MID;

        UiFillRect(cx + CELL_W - 28, cy + 8, 10, 10, c);
        UiRect(cx + CELL_W - 28, cy + 8, 10, 10, UI_COL_DIM);
    }

    // Risk reads the other way round: a large multiplier against us is bad.
    if (risk != UI_MATCHUP_NA && risk != TYPE_MUL_NORMAL)
    {
        u16 c = (risk > TYPE_MUL_NORMAL) ? UI_COL_HP_LOW : UI_COL_HP_HIGH;

        UiFillRect(cx + CELL_W - 15, cy + 8, 10, 10, c);
        UiRect(cx + CELL_W - 15, cy + 8, 10, 10, UI_COL_DIM);
    }
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
    UiText(cx + 42, cy + 8, name, UiThemeText(), UiThemeShadow());

    level = GetMonData(mon, MON_DATA_LEVEL);
    UiAscii(label, "Lv", sizeof(label));
    UiText(cx + 42, cy + 26, label, UI_COL_DIM, UiThemeShadow());
    UiNum(cx + 60, cy + 26, (s32)level, UiThemeText(), UiThemeShadow());

    // The animated value, not the raw one: bar and number slide together.
    hp    = sShownHp[index];
    maxHp = GetMonData(mon, MON_DATA_MAX_HP);

    UiNumRight(cx + CELL_W - 10, cy + 26, (s32)hp, UiThemeText(), UiThemeShadow());
    HpBar(cx + 42, cy + 46, CELL_W - 52, hp, maxHp);

    DrawMatchupBadges(cx, cy, mon);

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
               UI_COL_DIM, UiThemeShadow());
        return;
    }

    UiMonIcon(12, 12, (u16)species, GetMonData(mon, MON_DATA_PERSONALITY));

    GetMonData(mon, MON_DATA_NICKNAME, name);
    UiText(52, 12, name, UiThemeText(), UiThemeShadow());

    UiText(52, 30, UiAscii(label, "Lv", sizeof(label)), UI_COL_DIM, UiThemeShadow());
    UiNum(70, 30, (s32)GetMonData(mon, MON_DATA_LEVEL), UiThemeText(), UiThemeShadow());

    UiText(110, 30, gSpeciesNames[species], UI_COL_DIM, UiThemeShadow());

    // HP
    y = 52;
    UiText(12, y, UiAscii(label, "HP", sizeof(label)), UI_COL_DIM, UiThemeShadow());
    UiNum(44, y, (s32)sShownHp[UiSelectedMon()], UiThemeText(), UiThemeShadow());
    UiText(76, y, UiAscii(label, "/", sizeof(label)), UI_COL_DIM, UiThemeShadow());
    UiNum(86, y, (s32)GetMonData(mon, MON_DATA_MAX_HP), UiThemeText(), UiThemeShadow());
    HpBar(140, y + 4, 160, sShownHp[UiSelectedMon()],
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
                   UI_COL_DIM, UiThemeShadow());
            UiNum(sx + 34, sy, fields[i], UiThemeText(), UiThemeShadow());
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

        UiText(180, 72 + i * 16, gMoveNames[move], UiThemeText(), UiThemeShadow());
    }

    // Back target, top-right.
    UiRect(CTR_BOTTOM_WIDTH - 46, 8, 38, 22, UI_COL_DIM);
    UiText(CTR_BOTTOM_WIDTH - 40, 12, UiAscii(label, "BACK", sizeof(label)),
           UI_COL_ACCENT, UiThemeShadow());
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
