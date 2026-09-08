// PARTY tab: a 2x3 grid of the player's team, and a detail view per mon.
//
// The detail view's left column has three tenants and only ever one at a time:
// the computed stats plus the ability/nature/item block, the tapped move's
// details, or the IV/EV spread. The moves list on the right survives all three.
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
#include "constants/pokemon.h"
#include "constants/party_menu.h"

#include "../bridge.h"
#include "../tweaks.h"      // Ctr3dsCurrentLevelCap
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "matchup.h"

#define COLS      2
#define ROWS      3
#define CELL_W    (CTR_BOTTOM_WIDTH / COLS)     // 160

// Cheat tags. EXP All, a level cap and the randomiser can all be left on and
// forgotten, and every one of them quietly changes how the game plays. The team
// screen is where their effects actually show up, so it is where the reminder
// belongs.
//
// The strip exists only while something is on. With nothing active the grid is
// pixel-for-pixel what it always was, which matters because that is the common
// case: a permanently reserved empty band would tax every player for a feature
// most never enable. Both cell heights are whole tiles, which UiWindowFrame
// requires -- 64 is 8 and 56 is 7.
#define TAG_STRIP_H   24
#define CELL_H_FULL   (UI_CONTENT_H / ROWS)                  // 64, no strip
#define CELL_H_TIGHT  ((UI_CONTENT_H - TAG_STRIP_H) / ROWS)  // 56, strip up

#define TAG_X0        8
#define TAG_Y         2
#define TAG_H         20
#define TAG_PAD       5
#define TAG_GAP       6

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
// The moves list, and the panel that opens when one is tapped.
//
// Four rows of 28px fill y=72..184 exactly, which is the interior below the HP
// row. Each row carries the game's own 32x16 type icon, so FIRE here is the
// FIRE the summary screen shows, and the name beside it: names start at 234 and
// the longest (72px, THUNDERSHOCK) ends at 306, inside the 312px interior.
//
// PP, power and accuracy do NOT fit on a row -- 306 leaves 6px and PP alone is
// 30px -- which is exactly why tapping a move opens a panel instead of the list
// trying to carry everything.
#define MOVES_X         196
#define MOVES_Y         72
#define MOVE_ROW_H      28
#define MOVE_ROW_Y(i)   (MOVES_Y + (i) * MOVE_ROW_H)
#define MOVE_ICON_X     (MOVES_X + 2)
#define MOVE_NAME_X     (MOVE_ICON_X + UI_TYPE_ICON_W + 4)
#define MOVE_ROW_W      (CTR_BOTTOM_WIDTH - 8 - MOVES_X)

// The tapped move's details take over the left column: the stats block and the
// ability/nature/item lines are what get replaced, never the moves list itself,
// so the move you are reading about stays on screen and highlighted.
#define MOVEINFO_X      12
#define MOVEINFO_W      (MOVES_X - MOVEINFO_X - 8)
#define MOVEINFO_LABEL_X MOVEINFO_X
#define MOVEINFO_VALUE_X (MOVEINFO_X + 52)

// Top-aligned with the moves list, so the icon in the panel sits level with the
// icon in the row it came from. Icon 72..88, then three 16px lines to 141, and
// the description gets 148..179 for its two lines, inside the 184px floor.
#define MOVEINFO_Y      MOVES_Y
#define MOVEINFO_DESC_Y 148

// The detail view's way out. One definition, because the drawn rect and the hit
// test in UiPartyTouch have to agree; the dex entry screen names its own the
// same way.
#define BACK_X          (CTR_BOTTOM_WIDTH - 46)
#define BACK_Y          8
#define BACK_W          38
#define BACK_H          22

// The IV/EV toggle, on the level line and clear of everything already there:
// the longest species name ends near x=170 and BACK starts at 274, so 196..252
// has margin on both sides. y=26..48 sits under BACK and above the HP row at 52.
#define SPREAD_X        196
#define SPREAD_Y        26
#define SPREAD_W        56
#define SPREAD_H        22

// The spread table, in the same left column the move panel uses, so its right
// edge is MOVEINFO's. Seven 16px rows -- one header, six stats -- fill y=72..183
// inside a 184px floor, which is what fixes the row count: there is no eighth
// row for a totals line, so the EV total goes in the header's empty label cell.
//
// Columns are right-aligned, because these are numbers read down a column and a
// three-digit value beside a one-digit one has to line up. The four edges below
// leave at least 14px of gutter between adjacent columns at their widest --
// "STAT" is 24px, a three-digit number 18px.
#define SPREAD_ROW_H    16
#define SPREAD_LABEL_X  MOVEINFO_X            // 12
#define SPREAD_STAT_R   88
#define SPREAD_BASE_R   126
#define SPREAD_IV_R     156
#define SPREAD_EV_R     188

static bool8 sDetailOpen;

// Whether the left column is showing the IV/EV spread rather than the computed
// stats. Deliberately NOT reset with sMoveSel below: that one names a specific
// mon's move and goes stale the moment the selection moves, while this is a
// preference about the column itself. Someone comparing spreads across the team
// wants it to stay put.
static bool8 sSpreadOpen;

// Which move the detail view is describing, or -1 for none, in which case the
// left column shows the stats and the ability/nature/item block as before.
// Reset whenever the detail view opens or closes, so it can never describe a
// move belonging to the previously selected mon.
static s8 sMoveSel = -1;

// ------------------------------------------------------- HP bar animation --
//
// The game slides its battle HP bar rather than snapping it, and the number
// counts along with it. That bar is 48 px wide and moves about a pixel a frame,
// so stepping maxHp/48 per frame gives the same pace here. Its own logic
// (CalcNewBarValue, src/battle_interface.c) is static and tied to healthbox
// sprites, so it cannot be called; this matches the feel instead.
#define HP_ANIM_FRAMES 48

// ------------------------------------------------------- icon animation ----
//
// A mon icon in this ROM is two frames, not one, and until now this screen only
// ever drew the first of them: every graphics/pokemon/*/icon.png is 32x64 and
// UiMonIcon blitted the top half. So the grid was showing a still of an
// animation the ROM already had in memory.
//
// The pace is the shell's shared animation clock (UiAnimStepped), not this
// file's own counter and not the game's.
//
// Emerald's own party menu swaps these every six frames, ten times a second,
// and that is what this did first. It cost 7fps: a repaint on this screen costs
// an entire VBlank, so ten of them a second is fps = 3600/(60+10) = 51, and 53
// is what the hardware actually showed. Five steps a second is about 55fps.
// Half the game's pace is the price of those four frames, and the icons are
// visibly slower than the ones in the game's own party menu because of it.
static u32   sShownHp[PARTY_SIZE];
static u32   sShownMax[PARTY_SIZE];
static u32   sShownSpecies[PARTY_SIZE];
static u8    sIconFrame;
static bool8 sTabVisible;
static bool8 sIconStepped;   // the icon frame flipped this tick
static bool8 sHpMoving;      // a bar slid this tick, so the whole cell changed

// Adopt what the party actually holds, with no animation. Used on arrival at
// the tab: a bar that slides on the frame the player switches to PARTY is
// animating damage taken while they were somewhere else, which reports the
// wrong moment. Only the arrival pays for this, not every hidden frame --
// GetMonData decrypts in place, so a per-frame resync across the other four
// tabs would cost more than the repaints the gate is saving.
static void SnapBars(void)
{
    for (u32 i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *mon = &gPlayerParty[i];

        sShownSpecies[i] = GetMonData(mon, MON_DATA_SPECIES);
        sShownMax[i]     = GetMonData(mon, MON_DATA_MAX_HP);
        sShownHp[i]      = GetMonData(mon, MON_DATA_HP);
    }
}

bool8 UiPartyTick(bool8 visible)
{
    bool8 moving = FALSE;

    sHpMoving = FALSE;

    // Nothing on another tab is watching either of these, and a tick that
    // returns TRUE off screen is a full 76,800-pixel repaint of a view nobody
    // can see. This gate is the reason an always-on animation is affordable at
    // all; it also stops a sliding HP bar repainting the MAP tab, which it did
    // before the icons gave anyone a reason to fix it.
    if (!visible)
    {
        sTabVisible = FALSE;
        return FALSE;
    }

    if (!sTabVisible)
    {
        sTabVisible = TRUE;
        SnapBars();
        // No repaint asked for: whatever made this tab visible already did.
        return FALSE;
    }

    // Recorded rather than inferred: the HP loop below can also set `moving`,
    // and the shell needs to know which of the two it was to choose between a
    // six-rect redraw and a whole-screen one.
    sIconStepped = UiAnimStepped();

    if (sIconStepped)
    {
        sIconFrame ^= 1;
        moving = TRUE;
    }

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
        sHpMoving = TRUE;       // outside the icon rects, so this needs it all
    }

    return moving;
}

// Whether the only thing that changed this tick was the icon frame. The shell
// uses it to choose between putting six 32x32 rects back and rebuilding the
// whole 320x240, which measured 4.9 ms against a 5.7 ms frame budget.
bool8 UiPartyIconOnly(void)
{
    return sIconStepped && !sHpMoving;
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

// A cell's rows, which have to shift when the strip takes 8px off the height.
// Two explicit sets rather than arithmetic on the difference: the full layout
// is the one that shipped and has to stay exactly as it was, and the tight one
// is a different arrangement rather than the same one squeezed -- the icon
// moves up to the interior's top edge and the status badge drops onto the HP
// bar's row, which is free because the bar starts well to its right.
struct CellRows { int iconY, lvY, hpY, statusY; };
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

static int TagStripH(void) { return AnyTweakOn() ? TAG_STRIP_H : 0; }
static int CellH(void)     { return AnyTweakOn() ? CELL_H_TIGHT : CELL_H_FULL; }
static int CellTop(int i)  { return TagStripH() + (i / COLS) * CellH(); }

// The icons, and nothing else: restore what the last full paint had under them
// and draw the current frame back. Everything else in the cell -- the window
// frame, the name, the HP bar, the status badge -- is already correct in the
// snapshot and is not touched.
void UiPartyRedrawIcons(void)
{
    const struct CellRows *rows = AnyTweakOn() ? &sRowsTight : &sRowsFull;

    // The detail view covers the grid, and shows one icon of its own.
    if (sDetailOpen)
    {
        struct Pokemon *mon = &gPlayerParty[UiSelectedMon()];
        u32 species = GetMonData(mon, MON_DATA_SPECIES);

        if (species != SPECIES_NONE)
        {
            UiRestoreRect(12, 12, 32, 32);
            UiMonIconFrame(12, 12, (u16)species,
                           GetMonData(mon, MON_DATA_PERSONALITY), sIconFrame);
        }
        return;
    }

    for (u32 i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *mon = &gPlayerParty[i];
        u32 species = GetMonData(mon, MON_DATA_SPECIES);
        int x, y;

        if (species == SPECIES_NONE)
            continue;

        x = (int)(i % COLS) * CELL_W + CELL_ICON_X;
        y = CellTop((int)i) + rows->iconY;

        UiRestoreRect(x, y, 32, 32);
        UiMonIconFrame(x, y, (u16)species,
                       GetMonData(mon, MON_DATA_PERSONALITY), sIconFrame);
    }
}

// Semantic colours, so the tag carries its meaning before the text is read:
// green for the one that helps you, amber and red for the one holding you back
// (matching SOFT and HARD), and the UI's own accent for the one that changes
// the world rather than the numbers. Text and outline share the colour, the way
// the detail view's BACK box does.
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
        // The number as well as the mode: "held at 19" is the useful fact, and
        // showing it is what makes clear the cap tracks your badges rather than
        // being a fixed setting.
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
}

static void DrawCell(int index)
{
    struct Pokemon *mon = &gPlayerParty[index];
    const struct CellRows *rows = AnyTweakOn() ? &sRowsTight : &sRowsFull;
    int cellH = CellH();
    int cx = (index % COLS) * CELL_W;
    int cy = CellTop(index);
    u32 species, hp, maxHp, level;
    u8 name[POKEMON_NAME_LENGTH + 1];
    u8 label[8];
    int nameW, hpLabelX;

    UiWindowFrame(cx / 8, cy / 8, CELL_W / 8, cellH / 8);

    // Before the empty-slot check below, so an empty slot still shows which one
    // the player is on. This slot is what the detail view opens on and what the
    // BAG tab's target picker starts from, so it has to be readable either way.
    if (index == UiSelectedMon())
        UiChevron(cx + CELL_CURSOR_X, cy + (cellH - UI_CHEVRON_H) / 2);

    species = GetMonData(mon, MON_DATA_SPECIES);
    if (species == SPECIES_NONE)
        return;

    // The icon is NOT normally drawn here. It is the animated layer, painted
    // after the shell takes its snapshot, so the snapshot holds the still
    // background beneath it -- see UiPartyRedrawIcons. Drawing it here too would
    // bake a frame into that background, and since a mon icon blits with index 0
    // transparent, the baked frame would show through the next one.
    //
    // The exception is a modal overlay. While one is up it owns the animated
    // layer, so UiPartyRedrawIcons is not called and the icons are not drawn at
    // all -- and they cannot just be added to that layer, because it paints over
    // a snapshot that already contains the panel and they would land on top of
    // it. So they go into the still paint here instead, and the panel covers the
    // one of the six it actually overlaps.
    //
    // Frame 0 rather than sIconFrame: this is a still icon, which is exactly
    // what UiMonIcon is for. There is no ghosting risk in baking it, because the
    // restore-and-redraw that would show through it is the very thing not
    // running while the overlay is up.
    if (UiOverlayActive())
        UiMonIcon(cx + CELL_ICON_X, cy + rows->iconY, (u16)species,
                  GetMonData(mon, MON_DATA_PERSONALITY));

    // The 32x8 strip under the mon icon is otherwise empty, and the badge is
    // 32x8, so status lands next to the mon it belongs to without disturbing
    // anything. The HP bar starts at CELL_TEXT_X, well clear of it.
    UiStatusIcon(cx + CELL_ICON_X, cy + rows->statusY, GetMonAilment(mon));

    GetMonData(mon, MON_DATA_NICKNAME, name);
    nameW = UiText(cx + CELL_TEXT_X, cy + 8, name, UiThemeText(), UiThemeShadow());

    // Centred in the 15px name row, immediately after the name.
    DrawMatchupArrows(cx + CELL_TEXT_X + nameW + 4,
                      cy + 8 + (UI_GLYPH_H - UI_ARROW_H) / 2,
                      cx + CELL_W - 8, mon);

    level = GetMonData(mon, MON_DATA_LEVEL);
    UiAscii(label, "Lv", sizeof(label));
    UiText(cx + CELL_TEXT_X, cy + rows->lvY, label, UI_COL_DIM, UiThemeShadow());
    UiNum(cx + CELL_TEXT_X + 18, cy + rows->lvY, (s32)level, UiThemeText(), UiThemeShadow());

    // The animated value, not the raw one: bar and number slide together.
    hp    = sShownHp[index];
    maxHp = GetMonData(mon, MON_DATA_MAX_HP);

    // Labelled, because beside "Lv 42" a bare number reads as a second stat
    // rather than as health, and the bar below it says how full without ever
    // saying of what.
    //
    // Placed off maxHp's width rather than the animated hp's. maxHp does not
    // move; hp does, and a label measured from a sliding value would step
    // sideways every time that value crossed a digit boundary.
    UiAscii(label, "HP", sizeof(label));
    hpLabelX = cx + CELL_W - 10 - UiNumWidth((s32)maxHp) - UiTextWidth(label) - 2;

    UiText(hpLabelX, cy + rows->lvY, label, UI_COL_DIM, UiThemeShadow());
    UiNumRight(cx + CELL_W - 10, cy + rows->lvY, (s32)hp, UiThemeText(), UiThemeShadow());

    // Which mon has actually hit the ceiling. The strip above says what the cap
    // is; this says who it is holding, which is the thing you want when one mon
    // has stopped growing and the rest have not.
    //
    // It goes in the gutter between the level and the HP block, the one slot in
    // the cell that is free whatever those numbers are: the level is at most 3
    // digits so it ends by 90, and the HP block starts at 118 at its widest.
    // Derived from hpLabelX rather than fixed, so gaining the label cannot
    // leave this sitting under it.
    if (CapOn() && level >= Ctr3dsCurrentLevelCap())
        UiTextRight(hpLabelX - 4, cy + rows->lvY,
                    UiAscii(label, "CAP", sizeof(label)),
                    (Ctr3dsGetLevelCap() == CTR_CAP_HARD) ? UI_COL_HP_LOW
                                                          : UI_COL_HP_MID,
                    UiThemeShadow());

    UiHpBar(cx + CELL_TEXT_X, cy + rows->hpY, CELL_W - CELL_TEXT_X - 10, hp, maxHp);
}

// One move row: the game's own type icon, the name, and a highlight when this
// is the row whose details are showing.
//
// Drawn for empty slots too, as a dimmed "-", so a mon with two moves still
// shows four rows and the list does not change height as moves are learned.
static void DrawMoveRow(struct Pokemon *mon, u8 i)
{
    u8 label[16];
    u16 move = (u16)GetMonData(mon, MON_DATA_MOVE1 + i);
    int y = MOVE_ROW_Y(i);

    if (sMoveSel == (s8)i)
    {
        // The same doubled inset outline the EXTRA tab's buttons use for the
        // active choice, so a selection means the same thing on both screens.
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
               gBattleMoves[move].type);
    UiText(MOVE_NAME_X, y + (MOVE_ROW_H - UI_GLYPH_H) / 2, gMoveNames[move],
           UiThemeText(), UiThemeShadow());
}

static void DrawMoveList(struct Pokemon *mon)
{
    for (u8 i = 0; i < MAX_MON_MOVES; i++)
        DrawMoveRow(mon, i);
}

// The tapped move's details, in the space the stats block otherwise occupies.
//
// Power and accuracy are stored as 0 for moves that have none -- status moves,
// and the ones that never miss -- and the game prints those as "---" rather
// than as a zero, which would read as "this move does nothing".
static void DrawMoveInfo(struct Pokemon *mon, u8 i)
{
    u8 label[24];
    u16 move = (u16)GetMonData(mon, MON_DATA_MOVE1 + i);
    const struct BattleMove *info;
    int y;

    if (move == MOVE_NONE)
        return;

    info = &gBattleMoves[move];

    UiTypeIcon(MOVEINFO_X, MOVEINFO_Y, info->type);
    UiText(MOVEINFO_X + UI_TYPE_ICON_W + 6,
           MOVEINFO_Y + (UI_TYPE_ICON_H - UI_GLYPH_H) / 2,
           gTypeNames[info->type], UiThemeText(), UiThemeShadow());

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

    // Max PP is the mon's, not the move's: PP Ups raise it, and showing the
    // table value would contradict what the game itself reports.
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

    // The game's own description, carrying its own newline, so it wraps exactly
    // where the summary screen wraps it.
    UiText(MOVEINFO_X, MOVEINFO_DESC_Y, gMoveDescriptionPointers[move - 1],
           UiThemeText(), UiThemeShadow());
}

// The IV/EV spread, in the space the stats block otherwise occupies.
//
// Everything Emerald computes a stat from but never shows: the species' base
// value, the mon's IV, and the EVs it has trained. Read-only, and read through
// the same accessors CalculateMonStats uses, so the four columns multiply out
// to the STAT column beside them.
static void DrawSpread(struct Pokemon *mon, u16 species)
{
    // Row order is the summary screen's -- HP, ATK, DEF, SPA, SPD, SPE -- and
    // the parallel tables below all follow it. gNatureStatTable does NOT: its
    // columns run ATK, DEF, SPEED, SPATK, SPDEF (src/pokemon.c:1370), so the
    // last table remaps rather than indexing straight in. -1 is HP, which no
    // nature has ever touched.
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

    // struct SpeciesInfo names its six base stats rather than holding an array,
    // so this is the one place the row order has to be spelled out by hand.
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

    // The EV total, in the header row's label cell, which is the only space in
    // the table not spoken for. 510 is the game's own ceiling (MAX_TOTAL_EVS),
    // and GetMonEVCount is its own summing routine, not a reimplementation.
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

        // The nature is carried by the label: its colour, and a sign beside it
        // so the two boosted-and-hindered rows are still distinguishable
        // without relying on colour alone. The nature's NAME stays on the
        // stats page one tap away, where it always was.
        x = SPREAD_LABEL_X;
        x += UiText(x, y, UiAscii(label, names[i], sizeof(label)),
                    natureColour, UiThemeShadow());

        if (mod != 0)
            UiText(x + 2, y, UiAscii(label, (mod > 0) ? "+" : "-", sizeof(label)),
                   natureColour, UiThemeShadow());

        UiNumRight(SPREAD_STAT_R, y, (s32)GetMonData(mon, statField[i]),
                   UiThemeText(), UiThemeShadow());
        UiNumRight(SPREAD_BASE_R, y, base[i], UiThemeText(), UiThemeShadow());

        // A perfect IV marked, because finding them is the entire point of
        // looking. The EV column marks 252 rather than 255: Gen 3 stats use
        // EV/4, so the last three points buy nothing and 252 is where a trainer
        // actually stops.
        UiNumRight(SPREAD_IV_R, y, iv,
                   (iv == MAX_PER_STAT_IVS) ? UI_COL_HP_HIGH : UiThemeText(),
                   UiThemeShadow());
        UiNumRight(SPREAD_EV_R, y, ev,
                   (ev >= 252) ? UI_COL_HP_HIGH : UiThemeText(),
                   UiThemeShadow());
    }
}

// The IV/EV toggle. Dim frame with accent text when it is off, matching BACK;
// the doubled inset outline of the EXTRA tab's active choices when it is on, so
// "this is the selected one" looks the same everywhere on this screen.
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

    // Likewise not drawn here: the detail view's icon is animated too, and
    // UiPartyRedrawIcons paints it after the snapshot. And likewise the overlay
    // exception -- see the longer note in DrawCell.
    if (UiOverlayActive())
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

    DrawSpreadButton();

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

    // Left column: the stats and the ability/nature/item block, UNLESS a move is
    // selected or the IV/EV spread is up, either of which takes this space
    // instead. The moves list on the right is drawn whichever wins.
    //
    // The move panel is checked first because it is the transient one: it is
    // opened by a tap on a specific row and closed by the next tap, whereas the
    // spread is a mode the player left switched on.
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

    DrawMoveList(mon);
}

// Cheap identity of what this tab is showing that nothing else in the shell's
// hash tracks. Two things, and only one of them is always live.
//
// The cheat tags print the current level cap, which steps up the moment a badge
// is earned and touches nothing else in the hash. The IV/EV panel prints EVs,
// which move on every battle the mon takes part in and can move WITHOUT its
// level, HP or status moving with them -- a full-health mon that lands the last
// hit and does not level. So the EV total joins the key, but only while the
// panel is actually on screen: it is six reads, and paying for them on the
// other four tabs would be paying for a panel nobody is looking at.
//
// IVs and base stats are absent on purpose. Neither can change after the mon
// exists, so neither can go stale.
u32 UiPartyStateKey(void)
{
    u32 key = UiTweakStateKey();

    if (sDetailOpen && sSpreadOpen)
        key ^= (u32)GetMonEVCount(&gPlayerParty[UiSelectedMon()]) * 2654435761u;

    return key;
}

void UiPartyDraw(void)
{
    if (sDetailOpen)
    {
        DrawDetail();
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

    if (sDetailOpen)
    {
        if (UiHit(t, BACK_X, BACK_Y, BACK_W, BACK_H))
        {
            sDetailOpen = FALSE;
            sMoveSel = -1;
            UiMarkDirty();
            return;
        }

        // An empty slot draws neither this button nor anything it would switch
        // between, and a control that is not on screen must not be tappable:
        // the toggle would otherwise carry, silently, into the next mon opened.
        if (UiHit(t, SPREAD_X, SPREAD_Y, SPREAD_W, SPREAD_H)
            && GetMonData(&gPlayerParty[UiSelectedMon()], MON_DATA_SPECIES)
               != SPECIES_NONE)
        {
            // A move panel is borrowing the same column, so the first tap here
            // hands the column back to this button rather than toggling a mode
            // the player cannot currently see the state of.
            if (sMoveSel >= 0)
                sMoveSel = -1;
            else
                sSpreadOpen = !sSpreadOpen;

            UiMarkDirty();
            return;
        }

        // A move row. Tapping the open one closes it rather than doing nothing,
        // so the stats are always one tap away and there is no second control
        // to hunt for.
        for (u8 i = 0; i < MAX_MON_MOVES; i++)
        {
            if (!UiHit(t, MOVES_X, MOVE_ROW_Y((int)i), MOVE_ROW_W, MOVE_ROW_H))
                continue;

            // An empty slot has nothing to describe. Silently ignored rather
            // than clearing the selection, so a mis-tap below the last move
            // does not throw away what you were reading.
            if (GetMonData(&gPlayerParty[UiSelectedMon()],
                           MON_DATA_MOVE1 + i) == MOVE_NONE)
                return;

            sMoveSel = (sMoveSel == (s8)i) ? -1 : (s8)i;
            UiMarkDirty();
            return;
        }

        return;
    }

    for (int i = 0; i < PARTY_SIZE; i++)
    {
        int cx = (i % COLS) * CELL_W;
        int cy = CellTop(i);

        // Has to track the drawn geometry, strip or no strip, or taps land on
        // the wrong mon. A tap on the strip itself falls through and does
        // nothing, which is right: it is a readout, not a control.
        if (!UiHit(t, cx, cy, CELL_W, CellH()))
            continue;

        // First tap selects, a tap on the already-selected slot opens detail.
        // That keeps selection (which BAG needs) reachable without a long press
        // on a resistive panel.
        if (UiSelectedMon() == i)
        {
            sDetailOpen = TRUE;
            sMoveSel = -1;   // never describe the previous mon's move
        }
        else
        {
            UiSetSelectedMon((u8)i);
        }

        UiMarkDirty();
        return;
    }
}
