// TRAINER: the player's own trainer card, and the records behind it. See
// view_home.h.
//
// The card comes from TrainerCard_GenerateCardForLinkPlayer
// (src/trainer_card.c), which writes only the struct it gets. It is the card
// that the game sends over a link, so it is the card the game shows, and
// ui_card.c draws it with the game's own art. A tap on the card flips it.

#include "global.h"
#include "money.h"                    // GetMoney
#include "overworld.h"                // GetGameStat
#include "trainer_card.h"
#include "constants/game_stat.h"

#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "ui_card.h"
#include "ui_view.h"
#include "view_home.h"

// The card at the place of the LINK page's card view, with a column of
// buttons on its right.
#define CARD_X       4
#define CARD_Y       16
#define COL_X        252
#define COL_W        60
#define BTN_H        22
#define FLIP_Y       16
#define REC_Y        44
#define BACK_Y       150

// The records: one row for each value, the label at the left and the value
// right-aligned to the column's edge.
#define REC_TITLE_Y  16
#define REC_ROW_Y0   40
#define REC_ROW_H    18
#define REC_LABEL_X  16
#define REC_VALUE_R  232

static struct TrainerCard sCard;
static bool8 sBack;
static bool8 sRecords;

void UiTrainerPageOpen(void)
{
    sBack = FALSE;
    sRecords = FALSE;
}

static void DrawButton(int x, int y, int w, const char *text, bool8 on)
{
    u8 label[16];

    UiRect(x, y, w, BTN_H, UI_COL_DIM);
    if (on)
        UiRect(x + 2, y + 2, w - 4, BTN_H - 4, UI_COL_ACCENT);

    UiAscii(label, text, sizeof(label));
    UiText(x + (w - UiTextWidth(label)) / 2, y + (BTN_H - UI_GLYPH_H) / 2,
           label, on ? UI_COL_ACCENT : UiThemeText(), UiThemeShadow());
}

static void RecordRow(int row, const char *text, u32 value)
{
    u8 label[24];
    int y = REC_ROW_Y0 + row * REC_ROW_H;

    UiText(REC_LABEL_X, y, UiAscii(label, text, sizeof(label)),
           UiThemeText(), UiThemeShadow());
    UiNumRight(REC_VALUE_R, y, (s32)value, UiThemeText(), UiThemeShadow());
}

static void DrawRecords(void)
{
    u8 label[24];
    int y = REC_ROW_Y0 + 6 * REC_ROW_H;
    int x;

    UiWindowFrame(0, 0, COL_X / 8 - 1, UI_CONTENT_H / 8);
    UiText(REC_LABEL_X, REC_TITLE_Y, UiAscii(label, "RECORDS", sizeof(label)),
           UI_COL_DIM, UiThemeShadow());

    // The game's own counters (GetGameStat). The game shows none of these to
    // the player, except the trades on a link card.
    RecordRow(0, "STEPS TAKEN", GetGameStat(GAME_STAT_STEPS));
    RecordRow(1, "BATTLES", GetGameStat(GAME_STAT_TOTAL_BATTLES));
    RecordRow(2, "POKEMON CAUGHT", GetGameStat(GAME_STAT_POKEMON_CAPTURES));
    RecordRow(3, "EGGS HATCHED", GetGameStat(GAME_STAT_HATCHED_EGGS));
    RecordRow(4, "TRADES", GetGameStat(GAME_STAT_POKEMON_TRADES));
    RecordRow(5, "SHOPPING TRIPS", GetGameStat(GAME_STAT_SHOPPED));

    // The play time as the card shows it, hours and two digits of minutes.
    UiText(REC_LABEL_X, y, UiAscii(label, "PLAY TIME", sizeof(label)),
           UiThemeText(), UiThemeShadow());
    x = REC_VALUE_R - UiNumWidth(gSaveBlock2Ptr->playTimeMinutes / 10)
      - UiNumWidth(gSaveBlock2Ptr->playTimeMinutes % 10);
    UiNum(x, y, gSaveBlock2Ptr->playTimeMinutes / 10, UiThemeText(), UiThemeShadow());
    UiNum(x + UiNumWidth(gSaveBlock2Ptr->playTimeMinutes / 10), y,
          gSaveBlock2Ptr->playTimeMinutes % 10, UiThemeText(), UiThemeShadow());
    x -= UiTextWidth(UiAscii(label, ":", sizeof(label)));
    UiText(x, y, label, UiThemeText(), UiThemeShadow());
    UiNumRight(x, y, gSaveBlock2Ptr->playTimeHours, UiThemeText(), UiThemeShadow());
}

void UiTrainerPageDraw(void)
{
    // No window frame around the card: the card is the panel, as on the LINK
    // page.
    UiClear(UI_COL_BG);

    if (sRecords)
    {
        DrawRecords();
    }
    else
    {
        TrainerCard_GenerateCardForLinkPlayer(&sCard);
        UiCardDrawCard(CARD_X, CARD_Y, &sCard, gSaveBlock2Ptr->playerName, sBack);
    }

    DrawButton(COL_X, FLIP_Y, COL_W, sRecords ? "CARD" : "FLIP", FALSE);
    DrawButton(COL_X, REC_Y, COL_W, "RECORDS", sRecords);
    DrawButton(COL_X, BACK_Y, COL_W, "BACK", FALSE);
}

void UiTrainerPageTouch(const CtrTouchState *t)
{
    if (UiHit(t, COL_X, BACK_Y, COL_W, BTN_H))
    {
        UiViewPop();
        return;
    }

    if (UiHit(t, COL_X, REC_Y, COL_W, BTN_H))
        sRecords = !sRecords;
    else if (UiHit(t, COL_X, FLIP_Y, COL_W, BTN_H))
    {
        // From the records, this button goes back to the card as it was.
        if (sRecords)
            sRecords = FALSE;
        else
            sBack = !sBack;
    }
    else if (!sRecords && UiHit(t, CARD_X, CARD_Y, UI_CARD_W, UI_CARD_H))
        sBack = !sBack;
    else
        return;

    UiMarkDirty();
}

u32 UiTrainerPageKey(void)
{
    // The card shows the money and the play time, and the records show the
    // steps. All of them change as the player plays.
    return ((u32)sBack | ((u32)sRecords << 1))
         ^ (GetMoney(&gSaveBlock1Ptr->money) << 2)
         ^ ((u32)gSaveBlock2Ptr->playTimeHours << 20)
         ^ ((u32)gSaveBlock2Ptr->playTimeMinutes << 12)
         ^ (sRecords ? GetGameStat(GAME_STAT_STEPS) * 2654435761u : 0);
}
