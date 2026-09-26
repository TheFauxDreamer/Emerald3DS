// BERRIES: every planted berry tree. See view_home.h.
//
// A tree is gSaveBlock1Ptr->berryTrees[id]. The berry, its stage, the time to
// the next stage and the yield are fields there. The waterings come from
// Ctr3dsBerryTreeStagesWatered (src/berry.c). The yield shows only when the
// game has already put it in the tree: CalcBerryYield uses Random(), so it is
// never called here.
//
// A tree does not know its place. Each tree id is on one object event of one
// map, so the first open walks the map headers once and keeps a table of
// id to map section.

#include "global.h"
#include "berry.h"
#include "overworld.h"                // Overworld_GetMapHeaderByGroupAndId
#include "region_map.h"               // GetMapNameGeneric
#include "constants/berry.h"
#include "constants/event_object_movement.h"
#include "constants/map_groups.h"
#include "constants/region_map_sections.h"

#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "view_home.h"

// Five rows of 30px: the berry and its place, then a small line with the
// stage, the time and the waterings.
#define ROW_H        30
#define ROWS         5
#define ROW_Y(i)     (UI_PAGE_TOP + (i) * ROW_H)
#define ROW_W        256
#define PLACE_X      (UI_PAGE_LEFT + 64)
#define LINE2_DY     16

// The scroll buttons, on the right of the list.
#define SCROLL_X     280
#define SCROLL_W     28
#define SCROLL_H     70
#define UP_Y         UI_PAGE_TOP
#define DOWN_Y       (UI_PAGE_TOP + ROWS * ROW_H - SCROLL_H)

// The trees are all on the towns and routes of map group 0. Its last map is
// MAP_UNDERWATER_ROUTE125, in the order of data/maps/map_groups.json.
#define TREE_GROUP     MAP_GROUP(MAP_ROUTE101)
#define TREE_MAP_LAST  MAP_NUM(MAP_UNDERWATER_ROUTE125)

static u8 sTreeMapSec[BERRY_TREES_COUNT];
static bool8 sTreeMapSecBuilt;
static u8 sTop;

static void BuildPlaces(void)
{
    if (sTreeMapSecBuilt)
        return;

    for (u32 i = 0; i < BERRY_TREES_COUNT; i++)
        sTreeMapSec[i] = MAPSEC_NONE;

    for (u32 num = 0; num <= TREE_MAP_LAST; num++)
    {
        const struct MapHeader *header = Overworld_GetMapHeaderByGroupAndId(TREE_GROUP, num);
        const struct MapEvents *events = header->events;

        if (events == NULL)
            continue;

        // The same test the game makes: a tree is an object with the berry
        // tree movement, and its tree id is in trainerRange_berryTreeId.
        for (u32 i = 0; i < events->objectEventCount; i++)
        {
            const struct ObjectEventTemplate *obj = &events->objectEvents[i];

            if (obj->movementType == MOVEMENT_TYPE_BERRY_TREE_GROWTH
             && obj->trainerRange_berryTreeId < BERRY_TREES_COUNT)
                sTreeMapSec[obj->trainerRange_berryTreeId] = header->regionMapSectionId;
        }
    }

    sTreeMapSecBuilt = TRUE;
}

void UiBerriesPageOpen(void)
{
    sTop = 0;
}

// The planted trees, in id order. Returns their number.
static u32 PlantedTrees(u8 *ids)
{
    u32 n = 0;

    for (u32 i = 0; i < BERRY_TREES_COUNT; i++)
        if (GetStageByBerryTreeId(i) != BERRY_STAGE_NO_BERRY)
            ids[n++] = i;

    return n;
}

// ASCII helpers for the small line, which has no number function of its own.
static char *PutText(char *p, const char *s)
{
    while (*s != '\0')
        *p++ = *s++;
    *p = '\0';
    return p;
}

static char *PutNum(char *p, u32 v)
{
    char digits[10];
    int n = 0;

    do
    {
        digits[n++] = '0' + v % 10;
        v /= 10;
    } while (v != 0);

    while (n > 0)
        *p++ = digits[--n];
    *p = '\0';
    return p;
}

static char *PutTime(char *p, u32 minutes)
{
    if (minutes >= 60)
    {
        p = PutNum(p, minutes / 60);
        p = PutText(p, "h ");
    }
    p = PutNum(p, minutes % 60);
    return PutText(p, "m");
}

static const char *StageWord(u8 stage)
{
    switch (stage)
    {
    case BERRY_STAGE_PLANTED:   return "planted";
    case BERRY_STAGE_SPROUTED:  return "sprouted";
    case BERRY_STAGE_TALLER:    return "taller";
    case BERRY_STAGE_FLOWERING: return "flowering";
    case BERRY_STAGE_BERRIES:   return "berries";
    default:                    return "sparkling";
    }
}

static void DrawTree(int y, u8 id)
{
    struct BerryTree *tree = GetBerryTreeInfo(id);
    u8 name[BERRY_NAME_LENGTH + 1];
    u8 place[32];
    char ascii[64];
    u8 text[64];
    char *p = ascii;

    GetBerryNameByBerryType(tree->berry, name);
    UiText(UI_PAGE_LEFT, y, name, UiThemeText(), UiThemeShadow());

    if (sTreeMapSec[id] != MAPSEC_NONE)
    {
        GetMapNameGeneric(place, sTreeMapSec[id]);
        UiTextClipped(PLACE_X, y, UI_PAGE_LEFT + ROW_W - PLACE_X, place,
                      UiThemeText(), UiThemeShadow());
    }

    p = PutText(p, StageWord(tree->stage));

    if (tree->stage == BERRY_STAGE_BERRIES)
    {
        p = PutText(p, ": ");
        p = PutNum(p, tree->berryYield);
        p = PutText(p, tree->berryYield == 1 ? " berry" : " berries");
    }

    if (tree->stopGrowth)
    {
        p = PutText(p, ", not growing");
    }
    else if (tree->stage >= BERRY_STAGE_PLANTED && tree->stage <= BERRY_STAGE_BERRIES)
    {
        p = PutText(p, tree->stage == BERRY_STAGE_BERRIES ? ", fall in " : ", next in ");
        p = PutTime(p, tree->minutesUntilNextStage);
    }

    if (tree->stage >= BERRY_STAGE_PLANTED && tree->stage <= BERRY_STAGE_BERRIES)
    {
        p = PutText(p, ", watered ");
        p = PutNum(p, Ctr3dsBerryTreeStagesWatered(id));
        PutText(p, "/4");
    }

    UiTextSmall(UI_PAGE_LEFT, y + LINE2_DY, UiAscii(text, ascii, sizeof(text)),
                UI_COL_DIM, UiThemeShadow());
}

void UiBerriesPageDraw(void)
{
    u8 ids[BERRY_TREES_COUNT];
    u8 label[40];
    u32 n;

    BuildPlaces();
    n = PlantedTrees(ids);

    if (n == 0)
    {
        UiText(UI_PAGE_LEFT, UI_PAGE_TOP, UiAscii(label, "No berry trees are planted.", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
        return;
    }

    if (sTop + ROWS > n)
        sTop = n > ROWS ? n - ROWS : 0;

    for (u32 i = 0; i < ROWS && sTop + i < n; i++)
        DrawTree(ROW_Y(i), ids[sTop + i]);

    UiRect(SCROLL_X, UP_Y, SCROLL_W, SCROLL_H, UI_COL_DIM);
    UiRect(SCROLL_X, DOWN_Y, SCROLL_W, SCROLL_H, UI_COL_DIM);
    UiArrow(SCROLL_X + (SCROLL_W - UI_ARROW_W) / 2, UP_Y + (SCROLL_H - UI_ARROW_H) / 2,
            TRUE, sTop > 0 ? UI_COL_ACCENT : UI_COL_DIM);
    UiArrow(SCROLL_X + (SCROLL_W - UI_ARROW_W) / 2, DOWN_Y + (SCROLL_H - UI_ARROW_H) / 2,
            FALSE, sTop + ROWS < n ? UI_COL_ACCENT : UI_COL_DIM);
}

void UiBerriesPageTouch(const CtrTouchState *t)
{
    u8 ids[BERRY_TREES_COUNT];
    u32 n = PlantedTrees(ids);

    if (UiHit(t, SCROLL_X, UP_Y, SCROLL_W, SCROLL_H) && sTop > 0)
        sTop = sTop > ROWS ? sTop - ROWS : 0;
    else if (UiHit(t, SCROLL_X, DOWN_Y, SCROLL_W, SCROLL_H) && sTop + ROWS < n)
        sTop += ROWS;
    else
        return;

    UiMarkDirty();
}

u32 UiBerriesPageKey(void)
{
    u32 key = sTop;

    // A tree grows, is watered or is picked with no touch here. The minutes
    // change as the game's clock runs.
    for (u32 i = 0; i < BERRY_TREES_COUNT; i++)
    {
        struct BerryTree *tree = GetBerryTreeInfo(i);

        if (tree->stage == BERRY_STAGE_NO_BERRY)
            continue;

        key ^= ((u32)tree->berry | ((u32)tree->stage << 8)
                | ((u32)tree->minutesUntilNextStage << 16))
             * (2654435761u + 2 * i);
        key ^= ((u32)tree->berryYield | ((u32)Ctr3dsBerryTreeStagesWatered(i) << 8)
                | ((u32)tree->stopGrowth << 12)) << (i & 15);
    }

    return key;
}
