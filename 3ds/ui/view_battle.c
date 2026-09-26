// The battle panel. See view_battle.h.
//
// This file only reads and asks. The two writes, a move and a switch, are
// Ctr3dsQueueBattleMove and Ctr3dsQueueBattleSwitch in
// src/battle_controller_player.c, which hold every gate. A tap that the
// controller refuses leaves the turn as it was and says why.

#include "global.h"
#include "battle.h"                   // gBattleMons, gBattlerPartyIndexes
#include "battle_anim.h"              // GetBattlerSide
#include "data.h"                     // gMoveNames
#include "battle_controllers.h"       // Ctr3dsQueueBattleMove and the rest
#include "main.h"
#include "pokemon.h"
#include "constants/battle.h"
#include "constants/moves.h"
#include "constants/species.h"

#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "ui_team.h"
#include "ui_view.h"
#include "matchup.h"
#include "status_tags.h"
#include "view_battle.h"

// ---------------------------------------------------------------- layout ---
//
//    0..88    the moves, or the selected Pokemon's card, in one frame
//    88..152  the party row, in its own frame
//    152..192 a message line, under the quick-throw strip when it is up
//
// All three frames are whole 8px tiles: 11, 8 and 5 tiles tall.
#define TOP_TH       11
#define ROW_TY       11
#define ROW_TH       8
#define MSG_TY       19
#define MSG_TH       5
#define MSG_TEXT_Y   (MSG_TY * 8 + 12)
#define MSG_X        12

// The moves, 2x2 in the order of the game's FIGHT menu: 0 top left, 1 top
// right, 2 bottom left, 3 bottom right. The interior is x 8..311 and y 8..80.
#define MOVE_W       149
#define MOVE_H       34
#define MOVE_X(i)    (8 + ((i) & 1) * (MOVE_W + 5))
#define MOVE_Y(i)    (8 + ((i) >> 1) * (MOVE_H + 4))
#define MOVE_ICON_DX 4
#define MOVE_NAME_DX (MOVE_ICON_DX + UI_TYPE_ICON_W + 4)
#define MOVE_TEXT_DY 2
#define MOVE_PP_DY   18
// The effectiveness goes on the PP line, right-aligned: the exact multiplier
// with its arrow (UiMultiplierRight), one for each opponent, the right
// opponent at the right. It shows even when neutral, as a dim "x1", so the
// player always sees an answer. "PP 35/35" ends near x+60, and two values with
// their arrows take about 80px, so they fit in the 149px button.
#define MOVE_MUL_RIGHT (MOVE_W - 4)
#define MOVE_MUL_GAP  6

// The party row: six 50px cells from x 10. The interior is y 96..144: the icon,
// then the HP bar.
#define CELL_W       50
#define CELL_X(i)    (10 + (i) * CELL_W)
#define CELL_Y       96
#define CELL_H       48
#define CELL_ICON_DX ((CELL_W - 32) / 2)
#define CELL_ICON_DY 2
#define CELL_BAR_DX  5
#define CELL_BAR_W   (CELL_W - 2 * CELL_BAR_DX)
#define CELL_BAR_DY  38

// The selected Pokemon's card, in the moves' frame. Its buttons are on the
// right, so a mis-tap on the party row cannot reach SWITCH IN.
#define CARD_ICON_X  12
#define CARD_ICON_Y  12
#define CARD_TEXT_X  52
#define CARD_NAME_Y  10
#define CARD_INFO_Y  28
#define CARD_TYPE_Y  50
#define CARD_BTN_H   22
#define SWITCH_X     204
#define SWITCH_Y     10
#define SWITCH_W     104
#define INFO_X       204
#define INFO_Y       38
#define INFO_W       50
#define BACK_X       258
#define BACK_W       50
// Why SWITCH IN cannot work, on the card's last line, under the types. It is
// here and not on the message line, because in a wild battle the quick-throw
// strip covers the message line (y 152..192).
#define CARD_WHY_Y   64
#define CARD_WHY_W   (CTR_BOTTOM_WIDTH - 12 - CARD_TEXT_X)

// ------------------------------------------------------------------ state ---

#define NO_SLOT 0xFF

enum
{
    MSG_HINT,           // what the panel is for
    MSG_NOT_NOW,        // the controller said no: the turn moved on
    MSG_BACK_OUT,       // a switch while the game's move menu is up
    MSG_SWITCH_REASON,  // why the selected Pokemon cannot switch in
};

static u8 sSel = NO_SLOT;         // the selected party slot, field order
static u8 sMsg = MSG_HINT;

// The panel stays up for the whole battle, not only while the player chooses.
// A panel that went back to the grid for every turn's animations and came back
// for the next choice flickered between two screens all battle.
//
// It starts at the battle's first choice, not when gMain.inBattle goes TRUE:
// until the intro loads them, gBattleMons still holds the last battle's mons
// (the same reason status_tags.c waits). It ends when the battle has an
// outcome, so the grid is back for the experience and the catch.
//
// sShown is the battler whose moves show: the one choosing, or between choices
// the one that chose last.
static bool8 sLive;
static u8 sShown = MAX_BATTLERS_COUNT;

static void Refresh(void)
{
    u8 battler;

    if (!gMain.inBattle || gBattleOutcome != 0)
    {
        sLive = FALSE;
        sShown = MAX_BATTLERS_COUNT;
        sSel = NO_SLOT;
        sMsg = MSG_HINT;
        return;
    }

    battler = Ctr3dsBattleChoosingBattler();
    if (battler >= MAX_BATTLERS_COUNT)
        return;

    sLive = TRUE;

    // The other battler of a double starts clean, so a card or a refusal from
    // one never shows for the other.
    if (battler != sShown)
    {
        sShown = battler;
        sSel = NO_SLOT;
        sMsg = MSG_HINT;
    }
}

// TRUE while the shown battler is the one choosing: the only time a move or a
// switch can be tapped.
static bool8 Choosing(void)
{
    return sLive && Ctr3dsBattleChoosingBattler() == sShown;
}

bool8 UiBattlePanelActive(void)
{
    Refresh();
    return sLive && sShown < MAX_BATTLERS_COUNT;
}

void UiBattlePanelLeave(void)
{
    sSel = NO_SLOT;
    sMsg = MSG_HINT;
}

// The party mon that the choosing battler is. gBattlerPartyIndexes holds field
// ids, the same order as UiPartyMon.
static struct Pokemon *BattlerMon(u8 battler)
{
    return &gPlayerParty[gBattlerPartyIndexes[battler]];
}

// ---------------------------------------------------------------- drawing ---

static void DrawButton(int x, int y, int w, int h, const char *text, bool8 on)
{
    u8 label[16];

    UiRect(x, y, w, h, UI_COL_DIM);
    UiAscii(label, text, sizeof(label));
    UiText(x + (w - UiTextWidth(label)) / 2, y + (h - UI_GLYPH_H) / 2, label,
           on ? UI_COL_ACCENT : UI_COL_DIM, UiThemeShadow());
}

static void DrawMove(u8 battler, u8 i)
{
    int x = MOVE_X(i), y = MOVE_Y(i);
    u16 move = gBattleMons[battler].moves[i];
    struct Pokemon *mon = BattlerMon(battler);
    u8 label[16];
    int px;
    u8 pp, maxPp;
    bool8 usable;

    if (move == MOVE_NONE)
    {
        UiRect(x, y, MOVE_W, MOVE_H, UI_COL_DIM);
        UiText(x + MOVE_NAME_DX, y + MOVE_TEXT_DY, UiAscii(label, "-", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
        return;
    }

    pp = gBattleMons[battler].pp[i];
    maxPp = CalculatePPWithBonus(move, gBattleMons[battler].ppBonuses, i);
    usable = pp > 0;

    // The accent inset of the EXTRA and MAP buttons marks a move the player can
    // tap now: only while choosing. Between choices the buttons stay, as they
    // are, without it. One with no PP still takes the tap, and the engine says
    // why it cannot be used, but it is drawn dim so the player knows first.
    UiRect(x, y, MOVE_W, MOVE_H, UI_COL_DIM);
    if (usable && Choosing() && Ctr3dsBattleCanTapMoves())
        UiRect(x + 2, y + 2, MOVE_W - 4, MOVE_H - 4, UI_COL_ACCENT);

    UiTypeIcon(x + MOVE_ICON_DX, y + MOVE_TEXT_DY, UiMatchupMoveType(mon, move));
    UiText(x + MOVE_NAME_DX, y + MOVE_TEXT_DY, gMoveNames[move],
           usable ? UiThemeText() : UI_COL_DIM, UiThemeShadow());

    px = UiText(x + MOVE_ICON_DX, y + MOVE_PP_DY, UiAscii(label, "PP", sizeof(label)),
                UI_COL_DIM, UiThemeShadow());
    px = x + MOVE_ICON_DX + px + 4;
    px += UiNum(px, y + MOVE_PP_DY, pp, pp > 0 ? UiThemeText() : UI_COL_HP_LOW,
                UiThemeShadow());
    px += UiText(px, y + MOVE_PP_DY, UiAscii(label, "/", sizeof(label)),
                 UI_COL_DIM, UiThemeShadow());
    UiNum(px, y + MOVE_PP_DY, maxPp, UI_COL_DIM, UiThemeShadow());

    // One value for each opponent on the field, the right one at the right,
    // so the pair reads in field order. A status move has none.
    px = x + MOVE_MUL_RIGHT;
    if (UiMatchupFoePresent(B_POSITION_OPPONENT_RIGHT))
        px = UiMultiplierRight(px, y + MOVE_PP_DY,
                               UiMatchupMove(mon, move, B_POSITION_OPPONENT_RIGHT))
           - MOVE_MUL_GAP;
    if (UiMatchupFoePresent(B_POSITION_OPPONENT_LEFT))
        UiMultiplierRight(px, y + MOVE_PP_DY,
                          UiMatchupMove(mon, move, B_POSITION_OPPONENT_LEFT));
}

static bool8 SlotIsOut(u8 slot)
{
    for (u32 i = 0; i < gBattlersCount; i++)
        if (GetBattlerSide(i) == B_SIDE_PLAYER && gBattlerPartyIndexes[i] == slot)
            return TRUE;

    return FALSE;
}

static void DrawPartyRow(u8 battler)
{
    UiWindowFrame(0, ROW_TY, CTR_BOTTOM_WIDTH / 8, ROW_TH);

    for (u8 i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *mon = UiPartyMon(i);
        u16 species = GetMonData(mon, MON_DATA_SPECIES);
        int cx = CELL_X(i);

        if (species == SPECIES_NONE)
            continue;

        // The one choosing now in accent, any other battler that is out in
        // dim, and the selection doubled.
        if (gBattlerPartyIndexes[battler] == i)
            UiRect(cx, CELL_Y, CELL_W, CELL_H, UI_COL_ACCENT);
        else if (SlotIsOut(i))
            UiRect(cx, CELL_Y, CELL_W, CELL_H, UI_COL_DIM);

        if (sSel == i)
        {
            UiRect(cx + 1, CELL_Y + 1, CELL_W - 2, CELL_H - 2, UI_COL_ACCENT);
            UiRect(cx + 2, CELL_Y + 2, CELL_W - 4, CELL_H - 4, UI_COL_ACCENT);
        }

        if (UiAllySlot(i))
            UiAllyFrameGround(cx + 3, CELL_Y + 3, CELL_W - 6, CELL_H - 6);

        if (GetMonData(mon, MON_DATA_IS_EGG) || GetMonData(mon, MON_DATA_HP) == 0)
            UiMonIconSilhouette(cx + CELL_ICON_DX, CELL_Y + CELL_ICON_DY, species,
                                GetMonData(mon, MON_DATA_PERSONALITY), UiThemeShadow());
        else
            UiMonIcon(cx + CELL_ICON_DX, CELL_Y + CELL_ICON_DY, species,
                      GetMonData(mon, MON_DATA_PERSONALITY));

        if (!GetMonData(mon, MON_DATA_IS_EGG))
            UiHpBar(cx + CELL_BAR_DX, CELL_Y + CELL_BAR_DY, CELL_BAR_W,
                    GetMonData(mon, MON_DATA_HP), GetMonData(mon, MON_DATA_MAX_HP));
    }
}

static const char *SwitchReason(u8 reason)
{
    switch (reason)
    {
    case CTR3DS_SWITCH_PARTNER:   return "That is your partner's.";
    case CTR3DS_SWITCH_FAINTED:   return "It has no energy left.";
    case CTR3DS_SWITCH_IN_BATTLE: return "It is already in battle.";
    case CTR3DS_SWITCH_EGG:       return "An EGG can't battle.";
    case CTR3DS_SWITCH_CHOSEN:    return "It is already chosen.";
    default:                      return "Wait for your turn to choose.";
    }
}

static void DrawCard(u8 slot)
{
    struct Pokemon *mon = UiPartyMon(slot);
    u16 species = GetMonData(mon, MON_DATA_SPECIES);
    u8 name[POKEMON_NAME_LENGTH + 1];
    u8 label[40];
    int x;
    bool8 canSwitch = Ctr3dsCanSwitchTo(slot) == CTR3DS_SWITCH_OK;

    UiMonIcon(CARD_ICON_X, CARD_ICON_Y, species, GetMonData(mon, MON_DATA_PERSONALITY));

    // A nickname is player text, so it is cut to the space before the
    // buttons.
    GetMonData(mon, MON_DATA_NICKNAME, name);
    UiTextClipped(CARD_TEXT_X, CARD_NAME_Y, SWITCH_X - 8 - CARD_TEXT_X, name,
                  UiThemeText(), UiThemeShadow());

    x = CARD_TEXT_X;
    x += UiText(x, CARD_INFO_Y, UiAscii(label, "Lv", sizeof(label)),
                UI_COL_DIM, UiThemeShadow());
    x += UiNum(x + 2, CARD_INFO_Y, GetMonData(mon, MON_DATA_LEVEL),
               UiThemeText(), UiThemeShadow()) + 10;
    x += UiText(x, CARD_INFO_Y, UiAscii(label, "HP", sizeof(label)),
                UI_COL_DIM, UiThemeShadow()) + 4;
    x += UiNum(x, CARD_INFO_Y, GetMonData(mon, MON_DATA_HP), UiThemeText(), UiThemeShadow());
    x += UiText(x, CARD_INFO_Y, UiAscii(label, "/", sizeof(label)),
                UI_COL_DIM, UiThemeShadow());
    UiNum(x, CARD_INFO_Y, GetMonData(mon, MON_DATA_MAX_HP), UI_COL_DIM, UiThemeShadow());

    if (!GetMonData(mon, MON_DATA_IS_EGG))
    {
        UiTypeIcon(CARD_TEXT_X, CARD_TYPE_Y, gSpeciesInfo[species].types[0]);
        if (gSpeciesInfo[species].types[1] != gSpeciesInfo[species].types[0])
            UiTypeIcon(CARD_TEXT_X + UI_TYPE_ICON_W + 2, CARD_TYPE_Y,
                       gSpeciesInfo[species].types[1]);
        UiStatusIcon(CARD_TEXT_X + 2 * (UI_TYPE_ICON_W + 2), CARD_TYPE_Y + 4,
                     UiStatusTag(slot));
    }

    // SWITCH IN in accent only when it would work. The reason, when it would
    // not, is on the message line.
    DrawButton(SWITCH_X, SWITCH_Y, SWITCH_W, CARD_BTN_H, "SWITCH IN", canSwitch);
    if (canSwitch)
        UiRect(SWITCH_X + 2, SWITCH_Y + 2, SWITCH_W - 4, CARD_BTN_H - 4, UI_COL_ACCENT);
    DrawButton(INFO_X, INFO_Y, INFO_W, CARD_BTN_H, "INFO", TRUE);
    DrawButton(BACK_X, INFO_Y, BACK_W, CARD_BTN_H, "BACK", TRUE);

    // Always the reason when there is one, not only after a tap: the player
    // sees why before trying.
    if (!canSwitch)
        UiTextClipped(CARD_TEXT_X, CARD_WHY_Y, CARD_WHY_W,
                      UiAscii(label, SwitchReason(Ctr3dsCanSwitchTo(slot)), sizeof(label)),
                      UiThemeText(), UiThemeShadow());
    else if (sMsg == MSG_BACK_OUT)
        UiTextClipped(CARD_TEXT_X, CARD_WHY_Y, CARD_WHY_W,
                      UiAscii(label, "Press B on the top screen first.", sizeof(label)),
                      UiThemeText(), UiThemeShadow());
}

static void DrawMessage(void)
{
    u8 label[40];
    const char *text;

    // The card shows why a switch cannot happen (DrawCard), so this line only
    // has the hint and a refused move. In a wild battle the quick-throw strip
    // covers it, which loses nothing the player needs.
    switch (sMsg)
    {
    case MSG_NOT_NOW:
        text = "Not right now.";
        break;
    default:
        text = !Choosing()     ? "Waiting for your turn."
             : sSel != NO_SLOT ? "SWITCH IN sends it out."
             : Ctr3dsBattleCanTapMoves() ? "Tap a move, or a POKEMON to switch."
                                         : "Tap a POKEMON to switch.";
        break;
    }

    UiWindowFrame(0, MSG_TY, CTR_BOTTOM_WIDTH / 8, MSG_TH);
    UiText(MSG_X, MSG_TEXT_Y - 4, UiAscii(label, text, sizeof(label)),
           sMsg == MSG_HINT ? UI_COL_DIM : UiThemeText(), UiThemeShadow());
}

void UiBattlePanelDraw(void)
{
    u8 battler;

    Refresh();
    battler = sShown;

    if (!sLive || battler >= MAX_BATTLERS_COUNT)
        return;

    UiWindowFrame(0, 0, CTR_BOTTOM_WIDTH / 8, TOP_TH);

    if (sSel != NO_SLOT)
        DrawCard(sSel);
    else
        for (u8 i = 0; i < MAX_MON_MOVES; i++)
            DrawMove(battler, i);

    DrawPartyRow(battler);
    DrawMessage();
}

// ------------------------------------------------------------------ input ---

static void TouchCard(const CtrTouchState *t)
{
    if (UiHit(t, SWITCH_X, SWITCH_Y, SWITCH_W, CARD_BTN_H))
    {
        u8 reason = Ctr3dsCanSwitchTo(sSel);

        if (reason != CTR3DS_SWITCH_OK)
            sMsg = MSG_SWITCH_REASON;
        else if (Ctr3dsQueueBattleSwitch(sSel) != CTR3DS_ITEM_QUEUED)
            // The d-pad cannot switch from the move menu either.
            sMsg = MSG_BACK_OUT;
        else
            sSel = NO_SLOT;

        UiMarkDirty();
        return;
    }

    if (UiHit(t, INFO_X, INFO_Y, INFO_W, CARD_BTN_H))
    {
        // The PARTY tab's own detail view. Its BACK returns here.
        UiSetSelectedMon(sSel);
        UiViewPush(UI_VIEW_PARTY_DETAIL, 0);
        return;
    }

    if (UiHit(t, BACK_X, INFO_Y, BACK_W, CARD_BTN_H))
    {
        sSel = NO_SLOT;
        sMsg = MSG_HINT;
        UiMarkDirty();
    }
}

static void TouchMoves(const CtrTouchState *t)
{
    u8 battler = sShown;

    // Between choices the buttons show the moves but do nothing.
    if (!Choosing())
        return;

    for (u8 i = 0; i < MAX_MON_MOVES; i++)
    {
        if (!UiHit(t, MOVE_X(i), MOVE_Y(i), MOVE_W, MOVE_H))
            continue;

        if (gBattleMons[battler].moves[i] == MOVE_NONE)
            return;

        // One tap uses it, as on the DS. The controller makes the same choice
        // as the d-pad, and the engine refuses what it would refuse there.
        if (Ctr3dsQueueBattleMove(i) != CTR3DS_ITEM_QUEUED)
            sMsg = MSG_NOT_NOW;

        UiMarkDirty();
        return;
    }
}

void UiBattlePanelTouch(const CtrTouchState *t)
{
    if (!t->justReleased)
        return;

    Refresh();

    if (!sLive || sShown >= MAX_BATTLERS_COUNT)
        return;

    // The party row first: it is the same in both states. A card opens between
    // choices too, to read it or open INFO; SWITCH IN says to wait.
    for (u8 i = 0; i < PARTY_SIZE; i++)
    {
        if (!UiHit(t, CELL_X(i), CELL_Y, CELL_W, CELL_H))
            continue;

        if (GetMonData(UiPartyMon(i), MON_DATA_SPECIES) == SPECIES_NONE)
            return;

        // A second tap on the selection closes the card.
        sSel = (sSel == i) ? NO_SLOT : i;
        sMsg = MSG_HINT;
        UiMarkDirty();
        return;
    }

    if (sSel != NO_SLOT)
        TouchCard(t);
    else
        TouchMoves(t);
}

// ----------------------------------------------------------- repaint key ----

u32 UiBattlePanelKey(void)
{
    u8 battler;
    u32 key;

    Refresh();
    battler = sShown;

    if (!sLive || battler >= MAX_BATTLERS_COUNT)
        return 0;

    // Choosing changes the move buttons' inset and the hint, with no touch.
    key = 1u | ((u32)battler << 1) | ((u32)sSel << 4) | ((u32)sMsg << 12)
        | ((u32)Choosing() << 16);

    // PP drops after a move, and the battler's moves change with Transform or
    // Mimic, with no touch here. The party's HP and status are already in the
    // shell's hash on this tab.
    for (u32 i = 0; i < MAX_MON_MOVES; i++)
        key ^= (((u32)gBattleMons[battler].moves[i] << 8) | gBattleMons[battler].pp[i])
               * (2654435761u + 2 * i);

    // The partner's choice changes whether SWITCH IN works.
    if (sSel != NO_SLOT)
        key ^= (u32)Ctr3dsCanSwitchTo(sSel) << 20;

    return key;
}
