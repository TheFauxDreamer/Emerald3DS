// The battle panel. See view_battle.h.
//
// This file only reads and asks. The writes, a move, a switch, a send-out and
// RUN, are the Ctr3dsQueueBattle* functions in src/battle_controller_player.c,
// which hold every gate. A tap that the controller refuses leaves the turn as
// it was and says why.

#include "global.h"
#include "battle.h"                   // gBattleMons, gBattlerPartyIndexes
#include "battle_anim.h"              // GetBattlerSide
#include "data.h"                     // gMoveNames
#include "battle_controllers.h"       // Ctr3dsQueueBattleMove and the rest
#include "main.h"
#include "party_menu.h"               // GetAilmentFromStatus
#include "pokemon.h"
#include "constants/battle.h"
#include "constants/characters.h"   // EOS
#include "constants/moves.h"
#include "constants/pokemon.h"
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
//    0..96    the foe header and the moves, or a card, in one frame
//    96..152  the party row and the BAG / RUN column, in their own frame
//    152..192 a message line, under the quick-throw strip when it is up
//
// All three frames are whole 8px tiles: 12, 7 and 5 tiles tall.
#define TOP_TH       12
#define ROW_TY       12
#define ROW_TH       7
#define MSG_TY       19
#define MSG_TH       5
#define MSG_TEXT_Y   (MSG_TY * 8 + 12)
#define MSG_X        12

// The foe header: one line for each opponent, in field order. In a double
// battle each foe has half of the line. A tap opens that foe's card.
#define HEAD_X       8
#define HEAD_Y       8
#define HEAD_W       304
#define HEAD_H       17
#define HEAD_BAR_W   40
#define HEAD_LV_W    34

// The moves, 2x2 in the order of the game's FIGHT menu: 0 top left, 1 top
// right, 2 bottom left, 3 bottom right. They fill y 26..88.
#define MOVE_W       149
#define MOVE_H       30
#define MOVE_X(i)    (8 + ((i) & 1) * (MOVE_W + 5))
#define MOVE_Y(i)    (26 + ((i) >> 1) * (MOVE_H + 2))
#define MOVE_ICON_DX 4
#define MOVE_NAME_DX (MOVE_ICON_DX + UI_TYPE_ICON_W + 4)
#define MOVE_TEXT_DY 1
#define MOVE_PP_DY   15
// The effectiveness goes on the PP line, right-aligned: the exact multiplier
// with its arrow (UiMultiplierRight), one for each opponent, the right
// opponent at the right. It shows even when neutral, as a dim "x1", so the
// player always sees an answer.
#define MOVE_MUL_RIGHT (MOVE_W - 4)
#define MOVE_MUL_GAP  6

// The party row: six 42px cells from x 8, in y 104..144. Each has the icon,
// then the HP bar. The icon starts 1px high, on its blank top row.
#define CELL_W       42
#define CELL_X(i)    (8 + (i) * CELL_W)
#define CELL_Y       104
#define CELL_H       40
#define CELL_ICON_DX ((CELL_W - 32) / 2)
#define CELL_ICON_DY (-1)
#define CELL_BAR_DX  4
#define CELL_BAR_W   (CELL_W - 2 * CELL_BAR_DX)
#define CELL_BAR_DY  31

// BAG and RUN, one above the other, right of the party row.
#define COL_X        264
#define COL_W        46
#define COL_BTN_H    18
#define BAG_Y        104
#define RUN_Y        126

// The selected Pokemon's card, in the whole top frame. Its buttons are on the
// right, so a mis-tap on the party row cannot reach SWITCH IN.
#define CARD_ICON_X  12
#define CARD_ICON_Y  12
#define CARD_TEXT_X  52
#define CARD_NAME_Y  10
#define CARD_INFO_Y  28
#define CARD_TYPE_Y  48
#define CARD_BTN_H   22
#define SWITCH_X     204
#define SWITCH_Y     10
#define SWITCH_W     104
#define INFO_X       204
#define INFO_Y       38
#define INFO_W       50
#define BACK_X       258
#define BACK_W       50
// Why SWITCH IN cannot work, or the stat stages of a Pokemon that is out, on
// the card's last line. It is here and not on the message line, because in a
// wild battle the quick-throw strip covers the message line (y 152..192).
#define CARD_WHY_Y   68
#define CARD_WHY_W   (CTR_BOTTOM_WIDTH - 12 - CARD_TEXT_X)

// A foe's card, under the foe header, in the moves' place.
#define FOE_X        12
#define FOE_ROW1_Y   30
#define FOE_ROW2_Y   50
#define FOE_ROW3_Y   68
#define FOE_BAR_X    124
#define FOE_BAR_W    120
#define FOE_BACK_X   258
#define FOE_BACK_Y   27
#define FOE_BACK_H   20
#define FOE_TEXT_W   (CTR_BOTTOM_WIDTH - 12 - FOE_X)

// ------------------------------------------------------------------ state ---

#define NO_SLOT 0xFF
#define NO_FOE  0xFF

enum
{
    MSG_HINT,           // what the panel is for
    MSG_NOT_NOW,        // the controller said no: the turn moved on
    MSG_BACK_OUT,       // a switch or RUN while the game's move menu is up
    MSG_SWITCH_REASON,  // why the selected Pokemon cannot switch in
};

static const u8 sFoePositions[] = { B_POSITION_OPPONENT_LEFT, B_POSITION_OPPONENT_RIGHT };

static u8 sSel = NO_SLOT;         // the selected party slot, field order
static u8 sFoe = NO_FOE;          // the open foe card, in sFoePositions
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

static bool8 FoeShown(u8 index)
{
    if (index >= ARRAY_COUNT(sFoePositions))
        return FALSE;

    return UiMatchupFoePresent(sFoePositions[index]);
}

static u8 FoeBattler(u8 index)
{
    return GetBattlerAtPosition(sFoePositions[index]);
}

static void Refresh(void)
{
    u8 battler;

    if (!gMain.inBattle || gBattleOutcome != 0)
    {
        sLive = FALSE;
        sShown = MAX_BATTLERS_COUNT;
        sSel = NO_SLOT;
        sFoe = NO_FOE;
        sMsg = MSG_HINT;
        return;
    }

    // A foe that faints or leaves takes its card with it.
    if (sFoe != NO_FOE && !FoeShown(sFoe))
        sFoe = NO_FOE;

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

// TRUE while the shown battler waits for a Pokemon to send out. Its moves do
// not show then.
static bool8 SendingOut(void)
{
    return Choosing() && Ctr3dsBattleSendingOut();
}

bool8 UiBattlePanelActive(void)
{
    Refresh();
    return sLive && sShown < MAX_BATTLERS_COUNT;
}

void UiBattlePanelLeave(void)
{
    sSel = NO_SLOT;
    sFoe = NO_FOE;
    sMsg = MSG_HINT;
}

// The party mon that the choosing battler is. gBattlerPartyIndexes holds field
// ids, the same order as UiPartyMon.
static struct Pokemon *BattlerMon(u8 battler)
{
    return &gPlayerParty[gBattlerPartyIndexes[battler]];
}

// The player battler that party slot `slot` is, or MAX_BATTLERS_COUNT when it
// is not out.
static u8 SlotBattler(u8 slot)
{
    for (u32 i = 0; i < gBattlersCount; i++)
        if (GetBattlerSide(i) == B_SIDE_PLAYER && gBattlerPartyIndexes[i] == slot)
            return i;

    return MAX_BATTLERS_COUNT;
}

// ------------------------------------------------------------- read outs ---

// The stat stages that are not neutral, as "ATK +1  SPE -2", in ASCII. An
// empty string when all are neutral. The order is the game's: attack to
// evasion.
static void StagesText(u8 battler, char *out, int size)
{
    static const char *const names[NUM_BATTLE_STATS] =
    {
        [STAT_ATK] = "ATK", [STAT_DEF] = "DEF", [STAT_SPEED] = "SPE",
        [STAT_SPATK] = "SPA", [STAT_SPDEF] = "SPD", [STAT_ACC] = "ACC",
        [STAT_EVASION] = "EVA",
    };
    int o = 0;

    out[0] = '\0';

    for (u32 i = STAT_ATK; i < NUM_BATTLE_STATS; i++)
    {
        s8 stage = gBattleMons[battler].statStages[i] - DEFAULT_STAT_STAGE;
        const char *n = names[i];

        if (stage == 0 || o + 9 >= size)
            continue;

        if (o != 0)
        {
            out[o++] = ' ';
            out[o++] = ' ';
        }
        while (*n != '\0')
            out[o++] = *n++;
        out[o++] = ' ';
        out[o++] = stage > 0 ? '+' : '-';
        out[o++] = '0' + (stage > 0 ? stage : -stage);
        out[o] = '\0';
    }
}

// The conditions the game keeps on a battler, as words, in ASCII. Each is a
// flag or a timer in gBattleMons[].status2, gStatuses3 or gDisableStructs.
static void ConditionsText(u8 battler, char *out, int size)
{
    u32 s2 = gBattleMons[battler].status2;
    u32 s3 = gStatuses3[battler];
    const struct DisableStruct *d = &gDisableStructs[battler];
    const char *words[16];
    u32 n = 0;
    int o = 0;

    if (s2 & STATUS2_CONFUSION)                         words[n++] = "confused";
    if (s2 & STATUS2_INFATUATION)                       words[n++] = "in love";
    if (s2 & (STATUS2_WRAPPED | STATUS2_ESCAPE_PREVENTION)) words[n++] = "trapped";
    if (s2 & STATUS2_SUBSTITUTE)                        words[n++] = "substitute";
    if (s3 & STATUS3_LEECHSEED)                         words[n++] = "seeded";
    if (s2 & STATUS2_CURSED)                            words[n++] = "cursed";
    if (s2 & STATUS2_NIGHTMARE)                         words[n++] = "nightmare";
    if (s3 & STATUS3_PERISH_SONG)                       words[n++] = "perish song";
    if (s3 & STATUS3_YAWN)                              words[n++] = "drowsy";
    if (s3 & STATUS3_ROOTED)                            words[n++] = "rooted";
    if (s2 & STATUS2_FOCUS_ENERGY)                      words[n++] = "pumped";
    if (d->tauntTimer != 0)                             words[n++] = "taunted";
    if (d->disabledMove != MOVE_NONE)                   words[n++] = "disabled";
    if (d->encoredMove != MOVE_NONE)                    words[n++] = "encore";
    if (s2 & STATUS2_TRANSFORMED)                       words[n++] = "transformed";

    out[0] = '\0';

    for (u32 i = 0; i < n; i++)
    {
        const char *w = words[i];

        if (o != 0 && o + 2 < size)
        {
            out[o++] = ',';
            out[o++] = ' ';
        }
        while (*w != '\0' && o + 1 < size)
            out[o++] = *w++;
        out[o] = '\0';
    }
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

// The part of the header line that foe `index` has.
static void FoeHeadRect(u8 index, int *x, int *w)
{
    if (gBattleTypeFlags & BATTLE_TYPE_DOUBLE)
    {
        *w = HEAD_W / 2 - 2;
        *x = HEAD_X + index * (HEAD_W / 2 + 2);
    }
    else
    {
        *w = HEAD_W;
        *x = HEAD_X;
    }
}

static void DrawFoeHead(u8 index)
{
    u8 foe = FoeBattler(index);
    u8 name[POKEMON_NAME_LENGTH + 1];
    u8 label[8];
    int x, w, px;

    FoeHeadRect(index, &x, &w);

    if (sFoe == index)
        UiRect(x, HEAD_Y, w, HEAD_H, UI_COL_ACCENT);

    // The name the healthbox shows. It is player text for a link foe, so it
    // is cut.
    memcpy(name, gBattleMons[foe].nickname, POKEMON_NAME_LENGTH);
    name[POKEMON_NAME_LENGTH] = EOS;
    UiTextClipped(x + 3, HEAD_Y + 1, w - 6 - HEAD_LV_W - HEAD_BAR_W - 4, name,
                  UiThemeText(), UiThemeShadow());

    px = x + w - 3 - HEAD_BAR_W - HEAD_LV_W;
    px += UiText(px, HEAD_Y + 1, UiAscii(label, "Lv", sizeof(label)),
                 UI_COL_DIM, UiThemeShadow());
    UiNum(px + 1, HEAD_Y + 1, gBattleMons[foe].level, UiThemeText(), UiThemeShadow());

    // No numbers, as the game shows a foe.
    UiHpBar(x + w - 3 - HEAD_BAR_W, HEAD_Y + 5, HEAD_BAR_W,
            gBattleMons[foe].hp, gBattleMons[foe].maxHP);
}

static void DrawFoeHeader(void)
{
    for (u8 i = 0; i < ARRAY_COUNT(sFoePositions); i++)
        if (FoeShown(i))
            DrawFoeHead(i);
}

static void DrawFoeCard(u8 index)
{
    u8 foe = FoeBattler(index);
    char ascii[80];
    u8 text[80];
    u8 label[8];
    int x;

    UiTypeIcon(FOE_X, FOE_ROW1_Y, gBattleMons[foe].types[0]);
    x = FOE_X + UI_TYPE_ICON_W + 2;
    if (gBattleMons[foe].types[1] != gBattleMons[foe].types[0])
    {
        UiTypeIcon(x, FOE_ROW1_Y, gBattleMons[foe].types[1]);
        x += UI_TYPE_ICON_W + 2;
    }
    UiStatusIcon(x, FOE_ROW1_Y + 4, GetAilmentFromStatus(gBattleMons[foe].status1));

    UiText(FOE_BAR_X - 18, FOE_ROW1_Y, UiAscii(label, "HP", sizeof(label)),
           UI_COL_DIM, UiThemeShadow());
    UiHpBar(FOE_BAR_X, FOE_ROW1_Y + 4, FOE_BAR_W, gBattleMons[foe].hp, gBattleMons[foe].maxHP);

    DrawButton(FOE_BACK_X, FOE_BACK_Y, BACK_W, FOE_BACK_H, "BACK", TRUE);

    StagesText(foe, ascii, sizeof(ascii));
    if (ascii[0] != '\0')
        UiText(FOE_X, FOE_ROW2_Y, UiAscii(text, ascii, sizeof(text)),
               UiThemeText(), UiThemeShadow());
    else
        UiText(FOE_X, FOE_ROW2_Y, UiAscii(text, "No stat changes.", sizeof(text)),
               UI_COL_DIM, UiThemeShadow());

    ConditionsText(foe, ascii, sizeof(ascii));
    if (ascii[0] != '\0')
        UiTextClipped(FOE_X, FOE_ROW3_Y, FOE_TEXT_W, UiAscii(text, ascii, sizeof(text)),
                      UiThemeText(), UiThemeShadow());
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

    // The accent inset of the HOME and MAP buttons marks a move the player can
    // tap now: only while choosing. Between choices the buttons stay, as they
    // are, without it. One with no PP still takes the tap, and the engine says
    // why it cannot be used, but it is drawn dim so the player knows first.
    UiRect(x, y, MOVE_W, MOVE_H, UI_COL_DIM);
    if (usable && Choosing() && Ctr3dsBattleCanTapMoves())
        UiRect(x + 2, y + 2, MOVE_W - 4, MOVE_H - 4, UI_COL_ACCENT);

    UiTypeIcon(x + MOVE_ICON_DX, y + (MOVE_H - UI_TYPE_ICON_H) / 2,
               UiMatchupMoveType(mon, move));
    UiText(x + MOVE_NAME_DX, y + MOVE_TEXT_DY, gMoveNames[move],
           usable ? UiThemeText() : UI_COL_DIM, UiThemeShadow());

    // The PP line starts under the name, clear of the type icon.
    px = x + MOVE_NAME_DX;
    px += UiText(px, y + MOVE_PP_DY, UiAscii(label, "PP", sizeof(label)),
                 UI_COL_DIM, UiThemeShadow()) + 4;
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

// In place of the moves while a Pokemon must go out.
static void DrawSendOutPrompt(void)
{
    u8 text[40];

    UiTextWrapped(FOE_X, FOE_ROW1_Y, FOE_TEXT_W, 2,
                  UiAscii(text, "Choose a POKEMON to send out.", sizeof(text)),
                  UiThemeText(), UiThemeShadow());
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
        else if (SlotBattler(i) < MAX_BATTLERS_COUNT)
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

    // BAG goes to the BAG tab at any time. RUN is in accent while the player
    // chooses an action, and the engine answers it with its own words.
    DrawButton(COL_X, BAG_Y, COL_W, COL_BTN_H, "BAG", TRUE);
    DrawButton(COL_X, RUN_Y, COL_W, COL_BTN_H, "RUN", Choosing() && !SendingOut());
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
    char stages[40];
    u8 out = SlotBattler(slot);
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

    // SWITCH IN in accent only when it would work. For a send-out the same
    // button says SEND OUT.
    DrawButton(SWITCH_X, SWITCH_Y, SWITCH_W, CARD_BTN_H,
               SendingOut() ? "SEND OUT" : "SWITCH IN", canSwitch);
    if (canSwitch)
        UiRect(SWITCH_X + 2, SWITCH_Y + 2, SWITCH_W - 4, CARD_BTN_H - 4, UI_COL_ACCENT);
    DrawButton(INFO_X, INFO_Y, INFO_W, CARD_BTN_H, "INFO", TRUE);
    DrawButton(BACK_X, INFO_Y, BACK_W, CARD_BTN_H, "BACK", TRUE);

    // A Pokemon that is out shows its stat stages here. Why it cannot switch
    // in is clear from the party row.
    stages[0] = '\0';
    if (out < MAX_BATTLERS_COUNT && GetMonData(mon, MON_DATA_HP) != 0)
        StagesText(out, stages, sizeof(stages));

    // Otherwise always the reason when there is one, not only after a tap: the
    // player sees why before trying.
    if (stages[0] != '\0')
        UiTextClipped(CARD_TEXT_X, CARD_WHY_Y, CARD_WHY_W,
                      UiAscii(label, stages, sizeof(label)),
                      UiThemeText(), UiThemeShadow());
    else if (!canSwitch)
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
    u8 label[48];
    const char *text;

    // The card shows why a switch cannot happen (DrawCard), so this line only
    // has the hint and a refused tap. In a wild battle the quick-throw strip
    // covers it while the player chooses an action. A refusal that shows here
    // comes from the move menu, when the strip is down.
    switch (sMsg)
    {
    case MSG_NOT_NOW:
        text = "Not right now.";
        break;
    case MSG_BACK_OUT:
        text = "Press B on the top screen first.";
        break;
    default:
        text = !Choosing()     ? "Waiting for your turn."
             : SendingOut()    ? (sSel != NO_SLOT ? "SEND OUT sends it to battle."
                                                  : "Tap a POKEMON, or press A for the menu.")
             : sFoe != NO_FOE  ? "Tap the foe again to close."
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
    {
        DrawCard(sSel);
    }
    else
    {
        DrawFoeHeader();

        if (sFoe != NO_FOE)
            DrawFoeCard(sFoe);
        else if (SendingOut())
            DrawSendOutPrompt();
        else
            for (u8 i = 0; i < MAX_MON_MOVES; i++)
                DrawMove(battler, i);
    }

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
    if (!Choosing() || SendingOut())
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

// TRUE when the tap was on the foe header. A tap on a foe opens its card, and
// a second tap closes it.
static bool8 TouchFoeHeader(const CtrTouchState *t)
{
    for (u8 i = 0; i < ARRAY_COUNT(sFoePositions); i++)
    {
        int x, w;

        if (!FoeShown(i))
            continue;

        FoeHeadRect(i, &x, &w);
        if (!UiHit(t, x, HEAD_Y, w, HEAD_H))
            continue;

        sFoe = (sFoe == i) ? NO_FOE : i;
        sMsg = MSG_HINT;
        UiMarkDirty();
        return TRUE;
    }

    return FALSE;
}

// TRUE when the tap was on BAG or RUN.
static bool8 TouchColumn(const CtrTouchState *t)
{
    if (UiHit(t, COL_X, BAG_Y, COL_W, COL_BTN_H))
    {
        // The BAG tab uses battle items itself, with its own gates.
        UiSetTab(UI_TAB_BAG);
        return TRUE;
    }

    if (UiHit(t, COL_X, RUN_Y, COL_W, COL_BTN_H))
    {
        if (!Choosing() || SendingOut())
            sMsg = MSG_HINT;
        else if (Ctr3dsQueueBattleRun() != CTR3DS_ITEM_QUEUED)
            // The d-pad cannot run from the move menu either.
            sMsg = MSG_BACK_OUT;

        UiMarkDirty();
        return TRUE;
    }

    return FALSE;
}

void UiBattlePanelTouch(const CtrTouchState *t)
{
    if (!t->justReleased)
        return;

    Refresh();

    if (!sLive || sShown >= MAX_BATTLERS_COUNT)
        return;

    if (TouchColumn(t))
        return;

    // The party row next: it is the same in all states. A card opens between
    // choices too, to read it or open INFO; SWITCH IN says to wait.
    for (u8 i = 0; i < PARTY_SIZE; i++)
    {
        if (!UiHit(t, CELL_X(i), CELL_Y, CELL_W, CELL_H))
            continue;

        if (GetMonData(UiPartyMon(i), MON_DATA_SPECIES) == SPECIES_NONE)
            return;

        // A second tap on the selection closes the card.
        sSel = (sSel == i) ? NO_SLOT : i;
        sFoe = NO_FOE;
        sMsg = MSG_HINT;
        UiMarkDirty();
        return;
    }

    if (sSel != NO_SLOT)
    {
        TouchCard(t);
        return;
    }

    if (TouchFoeHeader(t))
        return;

    if (sFoe != NO_FOE)
    {
        if (UiHit(t, FOE_BACK_X, FOE_BACK_Y, BACK_W, FOE_BACK_H))
        {
            sFoe = NO_FOE;
            UiMarkDirty();
        }
        return;
    }

    TouchMoves(t);
}

// ----------------------------------------------------------- repaint key ----

// What a foe's header and card show. HP, status and the conditions change with
// no touch here.
static u32 FoeKey(u8 index)
{
    u8 foe = FoeBattler(index);
    u32 key;

    if (!FoeShown(index))
        return 0;

    key = gBattleMons[foe].species ^ ((u32)gBattleMons[foe].hp << 9)
        ^ ((u32)gBattleMons[foe].level << 25) ^ gBattleMons[foe].status1 * 31u;

    if (sFoe == index)
    {
        for (u32 i = 0; i < NUM_BATTLE_STATS; i++)
            key ^= (u32)(u8)gBattleMons[foe].statStages[i] << (i * 4);
        key ^= gBattleMons[foe].status2 * 2654435761u;
        key ^= gStatuses3[foe] * 0x9E3779B1u;
        key ^= ((u32)gDisableStructs[foe].disabledMove << 16)
             ^ gDisableStructs[foe].encoredMove ^ gDisableStructs[foe].tauntTimer;
        key ^= ((u32)gBattleMons[foe].types[0] << 8) ^ ((u32)gBattleMons[foe].types[1] << 20);
    }

    return key;
}

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
        | ((u32)Choosing() << 16) | ((u32)SendingOut() << 17) | ((u32)sFoe << 24);

    // PP drops after a move, and the battler's moves change with Transform or
    // Mimic, with no touch here. The party's HP and status are already in the
    // shell's hash on this tab.
    for (u32 i = 0; i < MAX_MON_MOVES; i++)
        key ^= (((u32)gBattleMons[battler].moves[i] << 8) | gBattleMons[battler].pp[i])
               * (2654435761u + 2 * i);

    for (u8 i = 0; i < ARRAY_COUNT(sFoePositions); i++)
        key ^= FoeKey(i) * (0x85EBCA6Bu + 2 * i);

    if (sSel != NO_SLOT)
    {
        u8 out = SlotBattler(sSel);

        // The partner's choice changes whether SWITCH IN works.
        key ^= (u32)Ctr3dsCanSwitchTo(sSel) << 20;

        // The stages of a Pokemon that is out change with no touch.
        if (out < MAX_BATTLERS_COUNT)
            for (u32 i = 0; i < NUM_BATTLE_STATS; i++)
                key ^= (u32)(u8)gBattleMons[out].statStages[i] << (i * 4 + 1);
    }

    return key;
}
