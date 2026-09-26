// PARTY tab: a 2x3 grid of the player's team, and a detail view for each mon.
//
// The left column of the detail view shows one of these:
// - the stats, with the ability, nature and item
// - the details of the tapped move
// - the IV/EV spread
// The moves list on the right always shows.
//
// All data comes from the game's own accessors (GetMonData, gMoveNames,
// gAbilityNames), so it agrees with the game's own screens. Nothing in this
// file writes game state.

#include "global.h"
#include "pokemon.h"
#include "item.h"
#include "data.h"
#include "battle.h"             // struct DisableStruct
#include "main.h"              // gMain.inBattle, for the battle-anim switch
#include "battle_main.h"
#include "pokemon_summary_screen.h"
#include "constants/species.h"
#include "constants/pokemon.h"

#include "../bridge.h"
#include "../tweaks.h"      // Ctr3dsCurrentLevelCap
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "matchup.h"
#include "status_tags.h"
#include "ui_team.h"
#include "ui_view.h"
#include "view_battle.h"

#define COLS      2
#define ROWS      3
#define CELL_W    (CTR_BOTTOM_WIDTH / COLS)     // 160

// Cheat tags. EXP All, a level cap and the randomizer change how the game
// plays, and a player can forget them. The team screen shows their effects, so
// the reminder goes here.
//
// The strip shows only while a tweak is on (or a battle partner is present).
// Without it, the grid keeps its original layout. Both cell heights are whole
// tiles, as UiWindowFrame requires: 64 is 8 tiles and 56 is 7.
#define TAG_STRIP_H   24
#define CELL_H_FULL   (UI_CONTENT_H / ROWS)                  // 64, no strip
#define CELL_H_TIGHT  ((UI_CONTENT_H - TAG_STRIP_H) / ROWS)  // 56, strip up

#define TAG_X0        8
#define TAG_Y         2
#define TAG_H         20
#define TAG_PAD       5
#define TAG_GAP       6

// A cell has three columns: the cursor, the icon with its status badge, and the
// text. The cursor column is always there, so the cells do not move when the
// selection changes.
#define CELL_CURSOR_X   8
#define CELL_ICON_X     18
#define CELL_TEXT_X     54

// Detail view: the stats block and the moves list share this row band. A stat
// cell is an 18px label and an 18px value. The longest move name is 72px
// (THUNDERSHOCK).
#define STAT_X(col)     (12 + (col) * 60)
#define STAT_Y          72
#define STAT_ROW_H      18
#define STAT_VALUE_DX   34
// The moves list, and the panel that opens when the player taps a move.
//
// Four rows of 28px fill y 72..184, the interior below the HP row. Each row has
// the game's own 32x16 type icon and the name. The longest name ends at 306,
// inside the 312px interior.
//
// PP, power and accuracy do not fit on a row, so a tap opens a panel.
#define MOVES_X         196
#define MOVES_Y         72
#define MOVE_ROW_H      28
#define MOVE_ROW_Y(i)   (MOVES_Y + (i) * MOVE_ROW_H)
#define MOVE_ICON_X     (MOVES_X + 2)
#define MOVE_NAME_X     (MOVE_ICON_X + UI_TYPE_ICON_W + 4)
#define MOVE_ROW_W      (CTR_BOTTOM_WIDTH - 8 - MOVES_X)

// The tapped move's details replace the stats block in the left column. The
// moves list stays, so the selected move stays visible.
#define MOVEINFO_X      12
#define MOVEINFO_W      (MOVES_X - MOVEINFO_X - 8)
#define MOVEINFO_LABEL_X MOVEINFO_X
#define MOVEINFO_VALUE_X (MOVEINFO_X + 52)

// Aligned with the top of the moves list. Icon at 72..88, three 16px lines to
// 141, and two description lines at 148..179, inside the 184px floor.
#define MOVEINFO_Y      MOVES_Y
#define MOVEINFO_DESC_Y 148

// The way out of the detail view. The drawn rect and the hit test in
// UiPartyTouch must agree, so there is one definition.
#define BACK_X          (CTR_BOTTOM_WIDTH - 46)
#define BACK_Y          8
#define BACK_W          38
#define BACK_H          22

// The status badge of the detail view, on the HP row. DrawDetail and
// UiPartyRedrawAnimated both paint it, at the same 32x8.
#define DETAIL_STATUS_X 120
#define DETAIL_STATUS_Y 54

// The IV/EV toggle, on the level line. The longest species name ends near x 170
// and BACK starts at 274, so 196..252 has space on both sides.
#define SPREAD_X        196
#define SPREAD_Y        26
#define SPREAD_W        56
#define SPREAD_H        22

// The spread table, in the left column that the move panel uses. Seven 16px
// rows (one header, six stats) fill y 72..183. There is no row for a total, so
// the EV total goes in the empty label cell of the header.
//
// The columns are right-aligned, so the digits line up. There are at least 14px
// between adjacent columns.
#define SPREAD_ROW_H    16
#define SPREAD_LABEL_X  MOVEINFO_X            // 12
#define SPREAD_STAT_R   88
#define SPREAD_BASE_R   126
#define SPREAD_IV_R     156
#define SPREAD_EV_R     188

// The detail view is UI_VIEW_PARTY_DETAIL on the view stack, not a flag here,
// so a tab switch closes it (ui_view.h).
static bool8 DetailOpen(void)
{
    return UiViewIsOpen(UI_VIEW_PARTY_DETAIL);
}

// TRUE when the left column shows the IV/EV spread, not the stats. This does
// not reset with sMoveSel. It is a preference, so it stays when the player
// compares the team.
static bool8 sSpreadOpen;

// The move that the detail view describes, or -1 for none. Reset when the
// detail view opens or closes, so it never shows a move of the previous mon.
static s8 sMoveSel = -1;

// ------------------------------------------------------- HP bar animation --
//
// The battle HP bar slides, and its number counts with it. That bar is 48px
// wide and moves about 1px each frame, so a step of maxHp/48 each frame gives
// the same pace. The game's own code (CalcNewBarValue) is static and uses the
// healthbox sprites, so this copies the pace.
#define HP_ANIM_FRAMES 48

// ------------------------------------------------------- icon animation ----
//
// A mon icon has two frames. Every graphics/pokemon/*/icon.png is 32x64.
//
// The pace comes from the shell's shared clock (UiAnimStepped). With a second
// core, it is 6 frames, as in the game's party menu. On one core, it is 12
// frames, because each repaint there costs a VBlank.
static u32   sShownHp[PARTY_SIZE];
static u32   sShownMax[PARTY_SIZE];
static u32   sShownSpecies[PARTY_SIZE];
static u8    sIconFrame;
static bool8 sTabVisible;
static bool8 sIconStepped;   // the icon frame flipped on this tick
static bool8 sHpMoving;      // a bar moved on this tick
static bool8 sTagFlipped;    // a badge on the screen changed tag on this tick

// Take the real party values, with no animation. Use this when the tab comes on
// screen. A slide at that moment would show damage from an earlier time.
// GetMonData decrypts in place, so do this only on arrival, not on each hidden
// frame.
static void SnapBars(void)
{
    for (u32 i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *mon = UiPartyMon((u8)i);

        sShownSpecies[i] = GetMonData(mon, MON_DATA_SPECIES);
        sShownMax[i]     = GetMonData(mon, MON_DATA_MAX_HP);
        sShownHp[i]      = GetMonData(mon, MON_DATA_HP);
    }
}

// TRUE when this frame's tag flip changes the screen. In the detail view, the
// shown mon must have two tags. On the grid, any mon with two tags counts.
static bool8 TagFlipVisible(void)
{
    if (!UiStatusTagsFlipped())
        return FALSE;

    if (DetailOpen())
        return UiStatusTagCycles(UiSelectedMon());

    for (u8 i = 0; i < PARTY_SIZE; i++)
        if (UiStatusTagCycles(i))
            return TRUE;

    return FALSE;
}

bool8 UiPartyTick(bool8 visible)
{
    bool8 moving = FALSE;

    sHpMoving = FALSE;
    sTagFlipped = FALSE;

    // No other tab shows these. A tick that returns TRUE off screen causes a
    // full repaint for nothing. This gate also stops a sliding HP bar from
    // repainting the MAP tab.
    if (!visible)
    {
        sTabVisible = FALSE;
        return FALSE;
    }

    if (!sTabVisible)
    {
        sTabVisible = TRUE;
        SnapBars();
        // Do not ask for a repaint. The tab switch already asked for one.
        return FALSE;
    }

    // The EXTRA tab's BATTLE ANIM switch is off.
    //
    // In battle, the top screen needs the frame time most. Thus the bars show
    // their true values, the icons stay still, and nothing here asks for a
    // repaint. The state hash still sees an HP change and repaints once.
    //
    // This applies only in battle. In the field, the animation costs nothing.
    //
    // A status badge flip still asks for a repaint. A mon that is poisoned and
    // confused shows the two tags in turn. Without the flip, one tag stays
    // hidden for the full battle.
    if (gMain.inBattle && Ctr3dsGetBattleAnimOff())
    {
        SnapBars();
        sIconStepped = FALSE;
        sTagFlipped = TagFlipVisible();
        return sTagFlipped;
    }

    // Keep this separate. The HP loop below can also set `moving`. The shell
    // must know which one it was, to select the cheap path or a full repaint.
    sIconStepped = UiAnimStepped();

    if (sIconStepped)
    {
        sIconFrame ^= 1;
        moving = TRUE;
    }

    // This is always on a step frame (UiStatusTagsTick), so it uses the same
    // repaint as the icons.
    sTagFlipped = TagFlipVisible();
    if (sTagFlipped)
        moving = TRUE;

    for (u32 i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *mon = UiPartyMon((u8)i);
        u32 species = GetMonData(mon, MON_DATA_SPECIES);
        u32 hp      = GetMonData(mon, MON_DATA_HP);
        u32 maxHp   = GetMonData(mon, MON_DATA_MAX_HP);
        u32 step;

        // A different mon in the slot, or a new maximum after a level-up or
        // evolution, makes the drawn bar meaningless. Snap to the new value.
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
            step = 1;   // small maximums must move too

        // Stop exactly on the target.
        if (sShownHp[i] > hp)
            sShownHp[i] = (sShownHp[i] - hp <= step) ? hp : sShownHp[i] - step;
        else
            sShownHp[i] = (hp - sShownHp[i] <= step) ? hp : sShownHp[i] + step;

        moving = TRUE;
        sHpMoving = TRUE;
    }

    return moving;
}

// TRUE when all changes on this tick are on the animated layer: the icon frame,
// a sliding bar or a badge flip. The shell then restores a few rects and does
// not repaint the full screen.
bool8 UiPartyAnimOnly(void)
{
    // The HP readout of the detail view is not on the animated layer, so a
    // slide there needs a full repaint.
    if (DetailOpen() && sHpMoving)
        return FALSE;

    return sIconStepped || sHpMoving || sTagFlipped;
}

// The icon frame that DrawCell and DrawDetail use while an overlay owns the
// animated layer.
//
// On the second-core path, use the live frame. The shell repaints fully for
// each step while an overlay is up, so the icons keep their pace. On the
// single-core path, use frame 0, because nothing repaints for a step there.
static u8 OverlayIconFrame(void)
{
    return Ctr3dsRasteriserOnOwnCore() ? sIconFrame : 0;
}

#define ARROW_GAP 3
#define ARROW_PAIR_W (UI_ARROW_W * 2 + ARROW_GAP)

// One offence arrow, for a multiplier on the x10 scale. Up and green is super
// effective. Down is weaker: amber for a resisted move, red only for a move
// that has no effect. Nothing for neutral or UI_MATCHUP_NA, the usual cases.
//
// The party grid, the move list and the battle panel (view_battle.c) all use
// this, so a mark means the same on each.
void UiOffenceArrow(int x, int y, u16 mul)
{
    if (mul == UI_MATCHUP_NA || mul == TYPE_MUL_NORMAL)
        return;

    if (mul > TYPE_MUL_NORMAL)
        UiArrow(x, y, TRUE, UI_COL_HP_HIGH);
    else
        UiArrow(x, y, FALSE, (mul == 0) ? UI_COL_HP_LOW : UI_COL_HP_MID);
}

// Two arrows, only when the matchup is not neutral. Neutral is the usual case.
//
// The direction gives the meaning and the color supports it: up and green is
// good for the player, down and red is bad. The left arrow is offence: can this
// mon hit the opponent hard? The right arrow is risk: how hard can the opponent
// hit back?
//
// `x` is where the nickname ends, so the arrows follow the name. `xLimit` is
// the rightmost pixel they can use.
static void DrawMatchupArrows(int x, int y, int xLimit, struct Pokemon *mon)
{
    u16 off, risk;

    if (!UiMatchupActive())
        return;

    // Stop a long nickname from pushing the arrows through the window frame.
    if (x + ARROW_PAIR_W > xLimit)
        x = xLimit - ARROW_PAIR_W;

    off  = UiMatchupOffence(mon);
    risk = UiMatchupRisk(mon);

    UiOffenceArrow(x, y, off);

    // Risk is the opposite: a large multiplier against the player is bad.
    if (risk != UI_MATCHUP_NA && risk != TYPE_MUL_NORMAL)
        UiArrow(x + UI_ARROW_W + ARROW_GAP, y,
                risk < TYPE_MUL_NORMAL,
                (risk > TYPE_MUL_NORMAL) ? UI_COL_HP_LOW : UI_COL_HP_HIGH);
}

// The rows of a cell, which move when the strip takes 8px from the height.
// There are two sets, not arithmetic. The full layout must stay as it shipped.
// In the tight layout, the icon moves up to the top of the interior and the
// status badge moves onto the HP bar row.
struct CellRows { int iconY, lvY, hpY, statusY; };

// Defined below with DrawCell. UiPartyRedrawAnimated uses it too, because the
// animated layer draws the same HP block.
static int  HpLabelX(int cx, u32 maxHp);
static void DrawCellHp(int index, int cx, int cy, const struct CellRows *rows);
static const struct CellRows sRowsFull  = { 16, 26, 46, 48 };
static const struct CellRows sRowsTight = {  8, 24, 40, 40 };

static bool8 CapOn(void)
{
    return Ctr3dsGetLevelCap() != CTR_CAP_OFF;
}

static bool8 AnyTweakOn(void)
{
    return Ctr3dsGetExpAll() || CapOn() || Ctr3dsGetRandomizer();
}

// The strip also shows the key to the partner's colored cells: the partner's
// name, in the same color (ui_team.h). Thus the strip shows during a partner
// battle too.
static bool8 StripOn(void)
{
    return AnyTweakOn() || UiAllyPresent();
}

static int TagStripH(void) { return StripOn() ? TAG_STRIP_H : 0; }
static int CellH(void)     { return StripOn() ? CELL_H_TIGHT : CELL_H_FULL; }
static int CellTop(int i)  { return TagStripH() + (i / COLS) * CellH(); }

// The moving parts: restore what the last full paint had under them and draw
// them again. These are the icons, the HP blocks and the status badges. The
// snapshot already has the rest of the cell.
void UiPartyRedrawAnimated(void)
{
    const struct CellRows *rows = StripOn() ? &sRowsTight : &sRowsFull;

    // The detail view covers the grid. It has one icon and one badge.
    if (DetailOpen())
    {
        struct Pokemon *mon = UiPartyMon(UiSelectedMon());
        u32 species = GetMonData(mon, MON_DATA_SPECIES);

        if (species != SPECIES_NONE)
        {
            UiRestoreRect(12, 12, 32, 32);
            UiMonIconFrame(12, 12, (u16)species,
                           GetMonData(mon, MON_DATA_PERSONALITY), sIconFrame);

            UiRestoreRect(DETAIL_STATUS_X, DETAIL_STATUS_Y, 32, 8);
            UiStatusIcon(DETAIL_STATUS_X, DETAIL_STATUS_Y,
                         UiStatusTag(UiSelectedMon()));
        }
        return;
    }

    for (u32 i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *mon = UiPartyMon((u8)i);
        u32 species = GetMonData(mon, MON_DATA_SPECIES);
        int x, y;

        if (species == SPECIES_NONE)
            continue;

        x = (int)(i % COLS) * CELL_W + CELL_ICON_X;
        y = CellTop((int)i) + rows->iconY;

        UiRestoreRect(x, y, 32, 32);
        UiMonIconFrame(x, y, (u16)species,
                       GetMonData(mon, MON_DATA_PERSONALITY), sIconFrame);

        // Then the HP block, for every slot, not only the sliding ones.
        //
        // This layer is the only place that draws the block. DrawCell leaves it
        // out of the snapshot, so a skipped slot has no bar. Restore before the
        // draw, because the snapshot holds the clean frame ground here.
        //
        // Two rects: the 96x8 bar, and the label with the number on the Lv row.
        // The number's rect starts at hpLabelX. Do not make it wider: the CAP
        // tag at hpLabelX - 4 is not part of this layer.
        {
            int cx = (int)(i % COLS) * CELL_W;
            int cy = CellTop((int)i);
            u32 maxHp = GetMonData(mon, MON_DATA_MAX_HP);
            int hpLabelX = HpLabelX(cx, maxHp);

            UiRestoreRect(cx + CELL_TEXT_X, cy + rows->hpY,
                          CELL_W - CELL_TEXT_X - 10, 8);
            UiRestoreRect(hpLabelX, cy + rows->lvY,
                          cx + CELL_W - 10 - hpLabelX + 1, UI_GLYPH_H + 1);

            DrawCellHp((int)i, cx, cy, rows);
        }

        // Then the status badge, for every slot, as for the HP block. A mon
        // with two tags alternates between them (status_tags.h). A badge in the
        // snapshot would get its old tag back on each restore.
        //
        // The 32x8 badge goes below the icon and does not touch the icon or the
        // HP bar in either layout.
        UiRestoreRect(x, CellTop((int)i) + rows->statusY, 32, 8);
        UiStatusIcon(x, CellTop((int)i) + rows->statusY, UiStatusTag((u8)i));
    }
}

// The color gives the meaning of each tag:
// - green: the tag helps the player (EXP)
// - amber or red: the tag holds the player back (CAP S or CAP H)
// - the accent: the tag changes the world (RND)
// The text and the outline use the same color, like BACK.
static int DrawTag(int x, const char *text, u16 colour)
{
    u8 label[12];
    int w;

    UiAscii(label, text, sizeof(label));
    w = UiTextWidth(label) + TAG_PAD * 2;

    UiRect(x, TAG_Y, w, TAG_H, colour);
    UiText(x + TAG_PAD, TAG_Y + (TAG_H - UI_GLYPH_H) / 2, label,
           colour, UiThemeShadow());

    return w + TAG_GAP;
}

static void DrawTagStrip(void)
{
    int x = TAG_X0;

    if (Ctr3dsGetExpAll())
        x += DrawTag(x, "EXP", UI_COL_HP_HIGH);

    if (CapOn())
    {
        // Show the number and the mode. "Held at 19" is the useful fact. It
        // also shows that the cap follows the badges.
        bool8 hard  = Ctr3dsGetLevelCap() == CTR_CAP_HARD;
        u16   colour = hard ? UI_COL_HP_LOW : UI_COL_HP_MID;
        int   cap   = Ctr3dsCurrentLevelCap();
        int   ty    = TAG_Y + (TAG_H - UI_GLYPH_H) / 2;
        u8    label[12];
        int   w, tx;

        UiAscii(label, hard ? "CAP H" : "CAP S", sizeof(label));
        w = UiTextWidth(label) + 3 + UiNumWidth(cap) + TAG_PAD * 2;

        UiRect(x, TAG_Y, w, TAG_H, colour);
        tx  = x + TAG_PAD;
        tx += UiText(tx, ty, label, colour, UiThemeShadow());
        UiNum(tx + 3, ty, (s32)cap, colour, UiThemeShadow());

        x += w + TAG_GAP;
    }

    if (Ctr3dsGetRandomizer())
        x += DrawTag(x, "RND", UI_COL_ACCENT);

    // The partner's name, right-aligned, as the key to their colored cells. The
    // cheat tags use less than half of the strip, and a name has seven
    // characters at most, so they do not touch.
    if (UiAllyPresent())
    {
        int w = UiAllyTagWidth();

        if (w > 0)
            UiAllyTag(CTR_BOTTOM_WIDTH - TAG_X0 - w, TAG_Y, TAG_H);
    }
}

// The x where the HP label starts. It depends on maxHp, which does not change
// during a slide. Thus one fixed restore rect is correct for the full
// animation.
static int HpLabelX(int cx, u32 maxHp)
{
    u8 label[8];

    UiAscii(label, "HP", sizeof(label));
    return cx + CELL_W - 10 - UiNumWidth((s32)maxHp) - UiTextWidth(label) - 2;
}

// The two parts that move when a bar slides: the label with the number on the
// Lv row, and the bar. The animated layer draws them over a restored snapshot,
// so the shell does not repaint the full screen.
static void DrawCellHp(int index, int cx, int cy, const struct CellRows *rows)
{
    struct Pokemon *mon = UiPartyMon((u8)index);
    u32 maxHp = GetMonData(mon, MON_DATA_MAX_HP);
    u32 hp = sShownHp[index];        // the animated value, not the real one
    int hpLabelX = HpLabelX(cx, maxHp);
    u8 label[8];

    UiAscii(label, "HP", sizeof(label));
    UiText(hpLabelX, cy + rows->lvY, label, UI_COL_DIM, UiThemeShadow());
    UiNumRight(cx + CELL_W - 10, cy + rows->lvY, (s32)hp,
               UiThemeText(), UiThemeShadow());
    UiHpBar(cx + CELL_TEXT_X, cy + rows->hpY, CELL_W - CELL_TEXT_X - 10, hp, maxHp);
}

static void DrawCell(int index)
{
    struct Pokemon *mon = UiPartyMon((u8)index);
    const struct CellRows *rows = StripOn() ? &sRowsTight : &sRowsFull;
    int cellH = CellH();
    int cx = (index % COLS) * CELL_W;
    int cy = CellTop(index);
    bool8 ally = UiAllySlot((u8)index);
    u32 species, maxHp, level;
    u8 name[POKEMON_NAME_LENGTH + 1];
    u8 label[8];
    int nameW, hpLabelX;

    UiWindowFrame(cx / 8, cy / 8, CELL_W / 8, cellH / 8);

    species = GetMonData(mon, MON_DATA_SPECIES);

    // The partner's Pokemon get the partner's color, as in the game's own party
    // menu. The ground goes under everything and into the snapshot, so a
    // restore puts it back. An empty slot stays plain.
    if (ally && species != SPECIES_NONE)
        UiAllyFrameGround(cx, cy, CELL_W, cellH);

    // Draw the cursor before the empty-slot check, so an empty slot still shows
    // the selection. The detail view and the BAG picker start from this slot.
    if (index == UiSelectedMon())
        UiChevron(cx + CELL_CURSOR_X, cy + (cellH - UI_CHEVRON_H) / 2);

    if (species == SPECIES_NONE)
        return;

    // Usually, do not draw the icon here. It is on the animated layer, which
    // paints after the snapshot (see UiPartyRedrawAnimated). An icon in the
    // snapshot shows through the next frame, because index 0 is transparent.
    //
    // The exception is an overlay. The overlay owns the animated layer, and
    // that layer paints over the panel. Thus the icons go into this paint, and
    // the panel covers the icon under it. See OverlayIconFrame for the frame.
    if (UiOverlayActive())
        UiMonIconFrame(cx + CELL_ICON_X, cy + rows->iconY, (u16)species,
                       GetMonData(mon, MON_DATA_PERSONALITY), OverlayIconFrame());

    // The status badge goes in the empty 32x8 strip below the icon.
    //
    // It is on the animated layer, like the icon, because a mon with two tags
    // alternates. Draw it here only under an overlay, with the tag that shows.
    if (UiOverlayActive())
        UiStatusIcon(cx + CELL_ICON_X, cy + rows->statusY, UiStatusTag((u8)index));

    GetMonData(mon, MON_DATA_NICKNAME, name);
    nameW = UiText(cx + CELL_TEXT_X, cy + 8, name, UiThemeText(), UiThemeShadow());

    // Centered in the 15px name row, after the name. Not for a partner's
    // Pokemon. The arrows help the player choose which mon to send in, and the
    // game does not let the player switch to a partner's mon
    // (TrySwitchInPokemon).
    if (!ally)
        DrawMatchupArrows(cx + CELL_TEXT_X + nameW + 4,
                          cy + 8 + (UI_GLYPH_H - UI_ARROW_H) / 2,
                          cx + CELL_W - 8, mon);

    level = GetMonData(mon, MON_DATA_LEVEL);
    UiAscii(label, "Lv", sizeof(label));
    UiText(cx + CELL_TEXT_X, cy + rows->lvY, label, UI_COL_DIM, UiThemeShadow());
    UiNum(cx + CELL_TEXT_X + 18, cy + rows->lvY, (s32)level, UiThemeText(), UiThemeShadow());

    maxHp = GetMonData(mon, MON_DATA_MAX_HP);

    // Needed here only to place the CAP tag. DrawCellHp draws the label.
    hpLabelX = HpLabelX(cx, maxHp);

    // Do not draw the HP label, number and bar here. They move during a slide,
    // so they are on the animated layer, like the icon.
    //
    // While an overlay owns that layer, draw the block here, or it does not
    // show. On the second-core path, the bar keeps sliding under the overlay.
    // On the single-core path, it keeps the value of the last full paint.
    if (UiOverlayActive())
        DrawCellHp(index, cx, cy, rows);

    // The mon that reached the cap. The strip shows the cap value. This shows
    // which mon it stops.
    //
    // It goes in the space between the level and the HP block. The level ends
    // by x 90, and the HP block starts at x 118 or later. It uses hpLabelX, so
    // it always stays clear of the label.
    if (CapOn() && level >= Ctr3dsCurrentLevelCap())
        UiTextRight(hpLabelX - 4, cy + rows->lvY,
                    UiAscii(label, "CAP", sizeof(label)),
                    (Ctr3dsGetLevelCap() == CTR_CAP_HARD) ? UI_COL_HP_LOW
                                                          : UI_COL_HP_MID,
                    UiThemeShadow());

}

// In battle, each move row has an offence arrow for each opponent in its
// top-right corner: one in a single battle, two in a double, in the
// opponents' field order (left, then right).
//
// The corner is free space. Move names are capitals, whose ink is 9 to 18px
// below the row's top (the font's rows 3 to 12, drawn at row + 6). The
// selection outline uses the top 2px. Thus rows 2 to 8 are clear, also under
// THUNDERSHOCK, the longest name, which ends at x 306.
#define MOVE_MARK_Y(i)  (MOVE_ROW_Y(i) + 2)
#define MOVE_MARK_X     (MOVES_X + MOVE_ROW_W - 3 - UI_ARROW_W)
#define MOVE_MARK2_X    (MOVE_MARK_X - ARROW_GAP - UI_ARROW_W)

static void DrawMoveMarks(struct Pokemon *mon, u16 move, u8 i)
{
    bool8 left = UiMatchupFoePresent(B_POSITION_OPPONENT_LEFT);
    bool8 right = UiMatchupFoePresent(B_POSITION_OPPONENT_RIGHT);

    // The right opponent's arrow is at the right, so the pair reads in field
    // order. With one opponent left in a double, its arrow keeps its place.
    if (right)
        UiOffenceArrow(MOVE_MARK_X, MOVE_MARK_Y(i),
                         UiMatchupMove(mon, move, B_POSITION_OPPONENT_RIGHT));
    if (left)
        UiOffenceArrow((gBattleTypeFlags & BATTLE_TYPE_DOUBLE) ? MOVE_MARK2_X
                                                                 : MOVE_MARK_X,
                         MOVE_MARK_Y(i),
                         UiMatchupMove(mon, move, B_POSITION_OPPONENT_LEFT));
}

// One move row: the type icon, the name, the matchup marks in battle, and a
// highlight when the row's details show.
//
// The icon is the type the move has for this mon (UiMatchupMoveType), so
// Hidden Power shows its real type.
//
// Empty slots show a dim "-", so the list always has four rows.
static void DrawMoveRow(struct Pokemon *mon, u8 i)
{
    u8 label[16];
    u16 move = (u16)GetMonData(mon, MON_DATA_MOVE1 + i);
    int y = MOVE_ROW_Y(i);

    if (sMoveSel == (s8)i)
    {
        // The same double inset outline as the active EXTRA buttons, so a
        // selection looks the same on both screens.
        UiRect(MOVES_X, y, MOVE_ROW_W, MOVE_ROW_H, UI_COL_ACCENT);
        UiRect(MOVES_X + 1, y + 1, MOVE_ROW_W - 2, MOVE_ROW_H - 2, UI_COL_ACCENT);
    }

    if (move == MOVE_NONE)
    {
        UiText(MOVE_NAME_X, y + (MOVE_ROW_H - UI_GLYPH_H) / 2,
               UiAscii(label, "-", sizeof(label)), UI_COL_DIM, UiThemeShadow());
        return;
    }

    UiTypeIcon(MOVE_ICON_X, y + (MOVE_ROW_H - UI_TYPE_ICON_H) / 2,
               UiMatchupMoveType(mon, move));
    UiText(MOVE_NAME_X, y + (MOVE_ROW_H - UI_GLYPH_H) / 2, gMoveNames[move],
           UiThemeText(), UiThemeShadow());
    DrawMoveMarks(mon, move, i);
}

static void DrawMoveList(struct Pokemon *mon)
{
    for (u8 i = 0; i < MAX_MON_MOVES; i++)
        DrawMoveRow(mon, i);
}

// The exact multiplier against the opponent, right-aligned on the type line,
// after the same arrow as the row. The number is in the theme's text colour,
// because the 20 window frames make a coloured number hard to read on some of
// them; the arrow carries the colour. In a double battle it is the left
// opponent's, or the right one's when the left one is gone.
//
// The values are the game's x10 scale: 40, 20, 10, 5, 2 (a quarter, rounded
// down by the chart walk) and 0.
#define MOVEINFO_MUL_RIGHT (MOVEINFO_X + MOVEINFO_W)

static void DrawMoveInfoMultiplier(struct Pokemon *mon, u16 move)
{
    u8 label[8];
    const char *text;
    u8 position;
    u16 mul;
    int x;

    if (UiMatchupFoePresent(B_POSITION_OPPONENT_LEFT))
        position = B_POSITION_OPPONENT_LEFT;
    else if (UiMatchupFoePresent(B_POSITION_OPPONENT_RIGHT))
        position = B_POSITION_OPPONENT_RIGHT;
    else
        return;

    mul = UiMatchupMove(mon, move, position);

    if (mul == UI_MATCHUP_NA)
        return;
    else if (mul >= 40)
        text = "x4";
    else if (mul >= 20)
        text = "x2";
    else if (mul >= TYPE_MUL_NORMAL)
        text = "x1";
    else if (mul >= 5)
        text = "x0.5";
    else if (mul > 0)
        text = "x0.25";
    else
        text = "x0";

    x = UiTextRight(MOVEINFO_MUL_RIGHT,
                    MOVEINFO_Y + (UI_TYPE_ICON_H - UI_GLYPH_H) / 2,
                    UiAscii(label, text, sizeof(label)),
                    UiThemeText(), UiThemeShadow());

    UiOffenceArrow(MOVEINFO_MUL_RIGHT - x - ARROW_GAP - UI_ARROW_W,
                     MOVEINFO_Y + (UI_TYPE_ICON_H - UI_ARROW_H) / 2, mul);
}

// The tapped move's details, in the space of the stats block.
//
// Power and accuracy are 0 for moves that have none (status moves, and moves
// that never miss). The game shows "---" for these, not 0.
static void DrawMoveInfo(struct Pokemon *mon, u8 i)
{
    u8 label[24];
    u16 move = (u16)GetMonData(mon, MON_DATA_MOVE1 + i);
    const struct BattleMove *info;
    u8 type;
    int y;

    if (move == MOVE_NONE)
        return;

    info = &gBattleMoves[move];
    type = UiMatchupMoveType(mon, move);

    UiTypeIcon(MOVEINFO_X, MOVEINFO_Y, type);
    UiText(MOVEINFO_X + UI_TYPE_ICON_W + 6,
           MOVEINFO_Y + (UI_TYPE_ICON_H - UI_GLYPH_H) / 2,
           gTypeNames[type], UiThemeText(), UiThemeShadow());
    DrawMoveInfoMultiplier(mon, move);

    y = MOVEINFO_Y + UI_TYPE_ICON_H + 6;

    UiText(MOVEINFO_LABEL_X, y, UiAscii(label, "POWER", sizeof(label)),
           UI_COL_DIM, UiThemeShadow());
    if (info->power > 1)
        UiNum(MOVEINFO_VALUE_X, y, info->power, UiThemeText(), UiThemeShadow());
    else
        UiText(MOVEINFO_VALUE_X, y, UiAscii(label, "---", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());

    y += UI_LINE_H;
    UiText(MOVEINFO_LABEL_X, y, UiAscii(label, "ACC", sizeof(label)),
           UI_COL_DIM, UiThemeShadow());
    if (info->accuracy != 0)
        UiNum(MOVEINFO_VALUE_X, y, info->accuracy, UiThemeText(), UiThemeShadow());
    else
        UiText(MOVEINFO_VALUE_X, y, UiAscii(label, "---", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());

    // Max PP comes from the mon, not the move, because PP Ups raise it.
    y += UI_LINE_H;
    {
        u8 ppBonuses = (u8)GetMonData(mon, MON_DATA_PP_BONUSES);
        int cur = (int)GetMonData(mon, MON_DATA_PP1 + i);
        int max = (int)CalculatePPWithBonus(move, ppBonuses, i);
        int x = MOVEINFO_VALUE_X;

        UiText(MOVEINFO_LABEL_X, y, UiAscii(label, "PP", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
        x += UiNum(x, y, cur, UiThemeText(), UiThemeShadow());
        x += UiText(x, y, UiAscii(label, "/", sizeof(label)),
                    UI_COL_DIM, UiThemeShadow());
        UiNum(x, y, max, UiThemeText(), UiThemeShadow());
    }

    // The game's own description, with its own line break, so it wraps as in
    // the summary screen.
    UiText(MOVEINFO_X, MOVEINFO_DESC_Y, gMoveDescriptionPointers[move - 1],
           UiThemeText(), UiThemeShadow());
}

// The IV/EV spread, in the space of the stats block.
//
// The game calculates stats from these values but never shows them: the base
// stat, the IV and the EV. The data comes from the same accessors as
// CalculateMonStats, so the columns agree with the STAT column.
static void DrawSpread(struct Pokemon *mon, u16 species)
{
    // The row order is the summary screen's: HP, ATK, DEF, SPA, SPD, SPE. The
    // tables below use it. The nature table (gNatureStatTable) does not: its
    // columns are ATK, DEF, SPEED, SPATK, SPDEF (src/pokemon.c). Thus the last
    // table maps the columns. The value -1 is HP, which no nature changes.
    static const char *const names[6] = { "HP", "ATK", "DEF", "SPA", "SPD", "SPE" };
    static const u8 statField[6] = {
        MON_DATA_MAX_HP, MON_DATA_ATK, MON_DATA_DEF,
        MON_DATA_SPATK, MON_DATA_SPDEF, MON_DATA_SPEED,
    };
    static const u8 ivField[6] = {
        MON_DATA_HP_IV, MON_DATA_ATK_IV, MON_DATA_DEF_IV,
        MON_DATA_SPATK_IV, MON_DATA_SPDEF_IV, MON_DATA_SPEED_IV,
    };
    static const u8 evField[6] = {
        MON_DATA_HP_EV, MON_DATA_ATK_EV, MON_DATA_DEF_EV,
        MON_DATA_SPATK_EV, MON_DATA_SPDEF_EV, MON_DATA_SPEED_EV,
    };
    static const s8 natureCol[6] = { -1, 0, 1, 3, 4, 2 };

    // The struct SpeciesInfo has six named base stats, not an array, so the row
    // order is written here by hand.
    const u8 base[6] = {
        gSpeciesInfo[species].baseHP,
        gSpeciesInfo[species].baseAttack,
        gSpeciesInfo[species].baseDefense,
        gSpeciesInfo[species].baseSpAttack,
        gSpeciesInfo[species].baseSpDefense,
        gSpeciesInfo[species].baseSpeed,
    };
    u8  nature = GetNature(mon);
    u8  label[16];
    int y = MOVES_Y;
    int x;

    // The EV total, in the empty label cell of the header. The game's limit is
    // 510 (MAX_TOTAL_EVS), and GetMonEVCount is the game's own function.
    x = SPREAD_LABEL_X;
    x += UiNum(x, y, (s32)GetMonEVCount(mon), UiThemeText(), UiThemeShadow());
    UiText(x, y, UiAscii(label, "/510", sizeof(label)), UI_COL_DIM, UiThemeShadow());

    UiTextRight(SPREAD_STAT_R, y, UiAscii(label, "STAT", sizeof(label)),
                UI_COL_DIM, UiThemeShadow());
    UiTextRight(SPREAD_BASE_R, y, UiAscii(label, "BASE", sizeof(label)),
                UI_COL_DIM, UiThemeShadow());
    UiTextRight(SPREAD_IV_R, y, UiAscii(label, "IV", sizeof(label)),
                UI_COL_DIM, UiThemeShadow());
    UiTextRight(SPREAD_EV_R, y, UiAscii(label, "EV", sizeof(label)),
                UI_COL_DIM, UiThemeShadow());

    for (int i = 0; i < 6; i++)
    {
        s32 iv = (s32)GetMonData(mon, ivField[i]);
        s32 ev = (s32)GetMonData(mon, evField[i]);
        s8  mod = (natureCol[i] < 0) ? 0 : gNatureStatTable[nature][natureCol[i]];
        u16 natureColour = (mod > 0) ? UI_COL_HP_HIGH
                         : (mod < 0) ? UI_COL_HP_LOW
                                     : UI_COL_DIM;

        y += SPREAD_ROW_H;

        // The label shows the nature: its color, and a sign next to it. Thus
        // the boosted and hindered rows are clear without color. The nature's
        // name stays on the stats page.
        x = SPREAD_LABEL_X;
        x += UiText(x, y, UiAscii(label, names[i], sizeof(label)),
                    natureColour, UiThemeShadow());

        if (mod != 0)
            UiText(x + 2, y, UiAscii(label, (mod > 0) ? "+" : "-", sizeof(label)),
                   natureColour, UiThemeShadow());

        UiNumRight(SPREAD_STAT_R, y, (s32)GetMonData(mon, statField[i]),
                   UiThemeText(), UiThemeShadow());
        UiNumRight(SPREAD_BASE_R, y, base[i], UiThemeText(), UiThemeShadow());

        // Mark a perfect IV. The EV column marks 252, not 255: Gen 3 uses EV/4,
        // so the last three points do nothing.
        UiNumRight(SPREAD_IV_R, y, iv,
                   (iv == MAX_PER_STAT_IVS) ? UI_COL_HP_HIGH : UiThemeText(),
                   UiThemeShadow());
        UiNumRight(SPREAD_EV_R, y, ev,
                   (ev >= 252) ? UI_COL_HP_HIGH : UiThemeText(),
                   UiThemeShadow());
    }
}

// The IV/EV toggle. Off: a dim frame with accent text, like BACK. On: the
// double inset outline of the active EXTRA buttons.
static void DrawSpreadButton(void)
{
    u8 label[12];

    UiRect(SPREAD_X, SPREAD_Y, SPREAD_W, SPREAD_H,
           sSpreadOpen ? UI_COL_ACCENT : UI_COL_DIM);
    if (sSpreadOpen)
        UiRect(SPREAD_X + 1, SPREAD_Y + 1, SPREAD_W - 2, SPREAD_H - 2,
               UI_COL_ACCENT);

    UiAscii(label, "IV/EV", sizeof(label));
    UiText(SPREAD_X + (SPREAD_W - UiTextWidth(label)) / 2,
           SPREAD_Y + (SPREAD_H - UI_GLYPH_H) / 2,
           label, UI_COL_ACCENT, UiThemeShadow());
}

static void DrawDetail(void)
{
    struct Pokemon *mon = UiPartyMon(UiSelectedMon());
    u32 species = GetMonData(mon, MON_DATA_SPECIES);
    bool8 ally = UiAllySlot(UiSelectedMon()) && species != SPECIES_NONE;
    u8 name[POKEMON_NAME_LENGTH + 1];
    u8 label[16];
    int y, nameW;

    UiWindowFrame(0, 0, CTR_BOTTOM_WIDTH / 8, UI_CONTENT_H / 8);

    // The same color as the partner's grid cell, so the detail view shows the
    // owner too.
    if (ally)
        UiAllyFrameGround(0, 0, CTR_BOTTOM_WIDTH, UI_CONTENT_H);

    // Draw BACK first, before content that can return early. The way out of a
    // modal must always show, even for an empty slot.
    UiRect(BACK_X, BACK_Y, BACK_W, BACK_H, UI_COL_DIM);
    UiText(BACK_X + 6, BACK_Y + 4, UiAscii(label, "BACK", sizeof(label)),
           UI_COL_ACCENT, UiThemeShadow());

    if (species == SPECIES_NONE)
    {
        UiText(16, 16, UiAscii(label, "Empty slot", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
        return;
    }

    // Do not draw the icon here either. The detail icon is animated, and
    // UiPartyRedrawAnimated draws it after the snapshot. The overlay exception
    // applies (see DrawCell).
    if (UiOverlayActive())
        UiMonIconFrame(12, 12, (u16)species,
                       GetMonData(mon, MON_DATA_PERSONALITY), OverlayIconFrame());

    GetMonData(mon, MON_DATA_NICKNAME, name);
    nameW = UiText(52, 12, name, UiThemeText(), UiThemeShadow());

    // As on the grid, the arrows follow the name. Their limit is the left edge
    // of BACK.
    //
    // A partner's Pokemon shows the partner's name tag there instead, because
    // the player cannot send it in. The tag is 2px taller than the name row on
    // each side and clears the Lv line at y 30. It always ends before BACK.
    if (ally)
        UiAllyTag(52 + nameW + 6, 10, UI_GLYPH_H + 4);
    else
        DrawMatchupArrows(52 + nameW + 4, 12 + (UI_GLYPH_H - UI_ARROW_H) / 2,
                          BACK_X - 6, mon);

    UiText(52, 30, UiAscii(label, "Lv", sizeof(label)), UI_COL_DIM, UiThemeShadow());
    UiNum(70, 30, (s32)GetMonData(mon, MON_DATA_LEVEL), UiThemeText(), UiThemeShadow());

    UiText(110, 30, gSpeciesNames[species], UI_COL_DIM, UiThemeShadow());

    DrawSpreadButton();

    // HP
    y = 52;
    UiText(12, y, UiAscii(label, "HP", sizeof(label)), UI_COL_DIM, UiThemeShadow());
    UiNum(44, y, (s32)sShownHp[UiSelectedMon()], UiThemeText(), UiThemeShadow());
    UiText(76, y, UiAscii(label, "/", sizeof(label)), UI_COL_DIM, UiThemeShadow());
    UiNum(86, y, (s32)GetMonData(mon, MON_DATA_MAX_HP), UiThemeText(), UiThemeShadow());

    // The status badge goes on the HP row, and the bar gives up 20px for it.
    // Like the icon, the badge is on the animated layer, and it goes here only
    // under an overlay. See DrawCell.
    if (UiOverlayActive())
        UiStatusIcon(DETAIL_STATUS_X, DETAIL_STATUS_Y, UiStatusTag(UiSelectedMon()));
    UiHpBar(160, y + 4, 140, sShownHp[UiSelectedMon()],
            GetMonData(mon, MON_DATA_MAX_HP));

    // The left column: the stats and the ability, nature and item block, unless
    // a move is selected or the IV/EV spread is on. The moves list always
    // shows.
    //
    // Check the move panel first. It is temporary: one tap opens it and the
    // next tap closes it. The spread stays on until the player turns it off.
    if (sMoveSel >= 0)
    {
        DrawMoveInfo(mon, (u8)sMoveSel);
        DrawMoveList(mon);
        return;
    }

    if (sSpreadOpen)
    {
        DrawSpread(mon, (u16)species);
        DrawMoveList(mon);
        return;
    }

    // The stats, two rows of three, next to the moves list.
    //
    // The third stat value is at STAT_X(2) + STAT_VALUE_DX and is 18px wide. It
    // must end before MOVES_X, or it prints over the first move name.
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

    // Ability, nature, held item
    y = 112;
    UiText(12, y, UiAscii(label, "ABILITY", sizeof(label)), UI_COL_DIM, UiThemeShadow());
    {
        // Do not use GetMonAbility(). It calls GetAbilityBySpecies(), which
        // writes gLastUsedAbility, a global that the battle messages read. This
        // is the same lookup, with no write.
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

    DrawMoveList(mon);
}

// A key for what this tab shows that the shell's hash does not track.
//
// The cheat tags show the current level cap, which changes when the player gets
// a badge. The IV/EV panel shows EVs, which can change after a battle when the
// level, HP and status do not. Thus the EV total goes into the key, but only
// while the panel shows.
//
// IVs and base stats do not change, so they are not in the key.
u32 UiPartyStateKey(void)
{
    u32 key = UiTweakStateKey();

    if (DetailOpen() && sSpreadOpen)
        key ^= (u32)GetMonEVCount(UiPartyMon(UiSelectedMon())) * 2654435761u;

    // The battle panel, while it has this tab: its own multiplier, so it
    // cannot cancel the tweak bits.
    key ^= UiBattlePanelKey() * 0x85EBCA6Bu;

    return key;
}

void UiPartyDraw(void)
{
    if (DetailOpen())
    {
        DrawDetail();
        return;
    }

    // While the player chooses in battle, the panel has this tab (view_battle.c).
    // The detail view above still opens over it, from the panel's INFO.
    if (UiBattlePanelActive())
    {
        UiBattlePanelDraw();
        return;
    }

    DrawTagStrip();

    for (int i = 0; i < PARTY_SIZE; i++)
        DrawCell(i);
}

void UiPartyTouch(const CtrTouchState *t)
{
    if (!t->justReleased)
        return;

    if (DetailOpen())
    {
        if (UiHit(t, BACK_X, BACK_Y, BACK_W, BACK_H))
        {
            UiViewPop();
            sMoveSel = -1;
            UiMarkDirty();
            return;
        }

        // An empty slot does not draw this button. A control that does not show
        // must not work, or the toggle changes without the player seeing it.
        if (UiHit(t, SPREAD_X, SPREAD_Y, SPREAD_W, SPREAD_H)
            && GetMonData(UiPartyMon(UiSelectedMon()), MON_DATA_SPECIES)
               != SPECIES_NONE)
        {
            // A move panel uses the same column. Thus the first tap closes the
            // panel and does not change the mode.
            if (sMoveSel >= 0)
                sMoveSel = -1;
            else
                sSpreadOpen = !sSpreadOpen;

            UiMarkDirty();
            return;
        }

        // A move row. A tap on the open row closes it, so the stats are always
        // one tap away.
        for (u8 i = 0; i < MAX_MON_MOVES; i++)
        {
            if (!UiHit(t, MOVES_X, MOVE_ROW_Y((int)i), MOVE_ROW_W, MOVE_ROW_H))
                continue;

            // An empty move slot has nothing to show. Ignore the tap and keep
            // the selection.
            if (GetMonData(UiPartyMon(UiSelectedMon()),
                           MON_DATA_MOVE1 + i) == MOVE_NONE)
                return;

            sMoveSel = (sMoveSel == (s8)i) ? -1 : (s8)i;
            UiMarkDirty();
            return;
        }

        return;
    }

    if (UiBattlePanelActive())
    {
        UiBattlePanelTouch(t);
        return;
    }

    for (int i = 0; i < PARTY_SIZE; i++)
    {
        int cx = (i % COLS) * CELL_W;
        int cy = CellTop(i);

        // The hit test must use the drawn geometry, with or without the strip.
        // A tap on the strip does nothing, because the strip only shows
        // information.
        if (!UiHit(t, cx, cy, CELL_W, CellH()))
            continue;

        // The first tap selects. A tap on the selected slot opens the detail
        // view. BAG needs the selection, and this gets it without a long press.
        if (UiSelectedMon() == i)
        {
            UiViewPush(UI_VIEW_PARTY_DETAIL, 0);
            sMoveSel = -1;   // never show the previous mon's move
        }
        else
        {
            UiSetSelectedMon((u8)i);
        }

        UiMarkDirty();
        return;
    }
}
