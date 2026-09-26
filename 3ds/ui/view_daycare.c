// DAY CARE: the Pokemon in the Day Care on Route 117. See view_home.h.
//
// GetDaycareState (src/daycare.c) gives the headline. For each Pokemon, the
// level it had when the player left it and the level it has now, from
// Ctr3dsDaycareLevelNow. With two, the old man's words for how they get
// along, from Ctr3dsDaycareCompatibilityText. Both wrappers are next to the
// game's own static functions, which they call.

#include "global.h"
#include "daycare.h"
#include "pokemon.h"
#include "constants/daycare.h"
#include "constants/species.h"

#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "view_home.h"

#define HEAD_Y       UI_PAGE_TOP
#define MON_Y(i)     (UI_PAGE_TOP + 22 + (i) * 40)
#define ICON_X       UI_PAGE_LEFT
#define TEXT_X       (UI_PAGE_LEFT + 40)
#define WORDS_Y      (MON_Y(2) + 4)
#define WORDS_W      (CTR_BOTTOM_WIDTH - 2 * UI_PAGE_LEFT)

static struct BoxPokemon *DaycareMon(u8 slot)
{
    return &gSaveBlock1Ptr->daycare.mons[slot].mon;
}

static void DrawMon(u8 slot)
{
    struct BoxPokemon *mon = DaycareMon(slot);
    u16 species = GetBoxMonData(mon, MON_DATA_SPECIES);
    u8 name[POKEMON_NAME_LENGTH + 2];
    u8 label[16];
    u8 before = GetLevelFromBoxMonExp(mon);
    u8 now = Ctr3dsDaycareLevelNow(slot);
    int y = MON_Y(slot);
    int x;

    UiMonIcon(ICON_X, y, species, GetBoxMonData(mon, MON_DATA_PERSONALITY));

    GetBoxMonNickname(mon, name);
    UiTextClipped(TEXT_X, y, WORDS_W - (TEXT_X - UI_PAGE_LEFT), name,
                  UiThemeText(), UiThemeShadow());

    // "Lv 12, now Lv 15 (+3)": the level when left, then the level the
    // player gets back.
    x = TEXT_X;
    y += 16;
    x += UiText(x, y, UiAscii(label, "Lv", sizeof(label)), UI_COL_DIM, UiThemeShadow()) + 2;
    x += UiNum(x, y, before, UiThemeText(), UiThemeShadow()) + 8;
    x += UiText(x, y, UiAscii(label, "now Lv", sizeof(label)), UI_COL_DIM, UiThemeShadow()) + 2;
    x += UiNum(x, y, now, UiThemeText(), UiThemeShadow()) + 6;

    if (now > before)
    {
        x += UiText(x, y, UiAscii(label, "+", sizeof(label)), UI_COL_ACCENT, UiThemeShadow());
        UiNum(x, y, now - before, UI_COL_ACCENT, UiThemeShadow());
    }
}

void UiDaycarePageDraw(void)
{
    u8 label[48];
    u8 state = GetDaycareState();
    const char *head;

    switch (state)
    {
    case DAYCARE_EGG_WAITING: head = "An EGG is waiting for you!"; break;
    case DAYCARE_ONE_MON:     head = "One POKéMON is in the DAY CARE."; break;
    case DAYCARE_TWO_MONS:    head = "Two POKéMON are in the DAY CARE."; break;
    default:                  head = "No POKéMON are in the DAY CARE."; break;
    }

    UiText(UI_PAGE_LEFT, HEAD_Y, UiAscii(label, head, sizeof(label)),
           state == DAYCARE_EGG_WAITING ? UI_COL_ACCENT : UiThemeText(), UiThemeShadow());

    for (u8 i = 0; i < DAYCARE_MON_COUNT; i++)
        if (GetBoxMonData(DaycareMon(i), MON_DATA_SPECIES) != SPECIES_NONE)
            DrawMon(i);

    // The old man's words need two Pokemon. An egg can wait with only one
    // left, so test the count and not the state.
    if (CountPokemonInDaycare(&gSaveBlock1Ptr->daycare) == DAYCARE_MON_COUNT)
        UiTextWrapped(UI_PAGE_LEFT, WORDS_Y, WORDS_W, 2, Ctr3dsDaycareCompatibilityText(),
                      UiThemeText(), UiThemeShadow());
}

u32 UiDaycarePageKey(void)
{
    struct DayCare *daycare = &gSaveBlock1Ptr->daycare;
    u32 key = GetDaycareState();

    // The steps give the levels. They change as the player walks.
    for (u32 i = 0; i < DAYCARE_MON_COUNT; i++)
        key ^= (GetBoxMonData(&daycare->mons[i].mon, MON_DATA_SPECIES)
                ^ (daycare->mons[i].steps << 9)) * (2654435761u + 2 * i);

    return key;
}
