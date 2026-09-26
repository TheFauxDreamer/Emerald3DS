// MAP tab: the Hoenn region map at 1:1, with a marker at the player's position.
//
// The art is the game's own: the PokeNav's tiles, tilemap and palette. The
// accessors are in the PLATFORM_3DS block of src/region_map.c, because the data
// is file-static there.
//
// The map only reads. The fly and escape controls write. They follow the rules
// of BAG's item use: the game's own checks, a separate tap to commit, and a new
// check at that moment. See FlyState and EscState.
//
// Three facts about the art:
// - The background is 8bpp, not 4bpp. UiBlit4bppTile cannot draw it.
// - The tilemap is affine: BG2 has BG_ATTR_SCREENSIZE 2 and BG_ATTR_PALETTEMODE
//   1. That is 64x64 tiles at one byte each, the 4096 bytes of map.bin. There
//   are no flip bits and no palette bank. The byte is the tile id.
// - The tile bytes are absolute palette indices in 112..143, because the game
//   loads the map's 32 colors at BG_PLTT_ID(7). Thus the 256-entry palette
//   below has only that part filled.
//
// Do not use InitRegionMap() or LoadRegionMapGfx(). They use BG layers, sprites
// and tasks on the top screen. The player's position is the only value used
// from src/region_map.c, because it is only arithmetic
// (Ctr3dsGetRegionMapPlayerPos).

#include "global.h"
#include "main.h"
#include "decompress.h"
#include "region_map.h"
#include "landmark.h"
#include "pokemon.h"
#include "event_data.h"
#include "overworld.h"
#include "script.h"
#include "field_effect.h"
#include "field_weather.h"            // PlayRainStoppingSoundEffect
#include "palette.h"                  // gPaletteFade
#include "battle.h"                   // struct DisableStruct
#include "party_menu.h"               // gPartyMenu.slotId selects the flyer
#include "item.h"                     // CheckBagHasItem, RemoveBagItem
#include "item_use.h"                 // CanUseDigOrEscapeRopeOnCurMap
#include "event_scripts.h"            // EventScript_UseDig
#include "constants/items.h"
#include "constants/region_map_sections.h"
#include "constants/flags.h"
#include "constants/moves.h"
#include "constants/species.h"

#include "../bridge.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "view_encounters.h"

// The real area of the art in the 64x64 tilemap. Everything outside it is the
// blank tile. These values come from map.bin. The MAPSEC grid (28x15 at 1,2) is
// smaller than the picture.
#define MAP_TW        31
#define MAP_TH        19
#define TILEMAP_STRIDE 64

// The map is 31 tiles (248px) wide in a 320px panel, so it has 36px on each
// side. It is 19 tiles (152px) tall, which leaves 40px for the caption. A 2x
// scale does not fit. The 1x scale is also the only pixel-perfect scale.
#define MAP_PX        ((CTR_BOTTOM_WIDTH - MAP_TW * 8) / 2)
#define MAP_PY        0

// The caption uses the rest of the content area. Both bands are whole tiles:
// 19 + 5 = 24 tiles = 192px.
#define CAP_TY        MAP_TH
#define CAP_TH        ((UI_CONTENT_H / 8) - MAP_TH)
#define CAP_Y         (CAP_TY * 8)
#define CAP_TEXT_Y    (CAP_Y + 12)
#define CAP_MARGIN    10

// The fly controls share the one interior row of the caption band. The band is
// at y 152..192 and its frame leaves y 160..184. A 22px button at y 161 fits,
// and its text lines up with CAP_TEXT_Y.
#define FLY_BTN_H     22
#define FLY_BTN_Y     (CAP_Y + 9)
#define FLY_BTN_W     54
#define FLY_BTN_X     (CTR_BOTTOM_WIDTH - CAP_MARGIN - FLY_BTN_W)

// The confirm step replaces that button with three items on the same row. The
// question right-aligns at 202, NO is at 210..256 and YES is at 264..310.
//
// The longest place name, EVER GRANDE CITY, is 90px and ends at x 100. The
// widest refusal, "can't FLY from here", is 100px and right-aligns to 210. Both
// are clear.
#define CFM_W         46
#define CFM_YES_X     (CTR_BOTTOM_WIDTH - CAP_MARGIN - CFM_W)
#define CFM_NO_X      (CFM_YES_X - 8 - CFM_W)
#define CFM_ASK_X     (CFM_NO_X - 8)

// WILD PKMN opens the encounter list for the place in the caption. It shares
// FLY's row. The right end of the caption holds [FLY] [WILD PKMN], one of them,
// or neither. ESCAPE uses FLY's slot, and the two never show together: FLY is
// for a selected town, ESCAPE for the player's own location.
//
// WILD PKMN has a fixed slot at the right, and FLY moves inward when both show.
// Most places have wild Pokemon, but FLY shows only on the towns that the
// player has reached. Thus the common button is always in the same place. FLY
// right-aligns when WILD PKMN is absent.
//
// The button is 66px, because the label is 51px. That leaves 7px on each side.
// Its left edge is x 236. The widest landmark (FOSSIL MANIAC'S HOUSE, 117px)
// then starts at 119, clear of the longest place name. The refusals are 100px
// or less.
//
// WildBtnX and FlyBtnX are the only places that decide the positions.
#define WILD_BTN_W    66
#define WILD_BTN_X    (CTR_BOTTOM_WIDTH - CAP_MARGIN - WILD_BTN_W)
#define FLY_INNER_X   (WILD_BTN_X - 8 - FLY_BTN_W)

#define MAP_TILE_COUNT 233
#define MAP_PAL_BASE   112
#define MAP_PAL_COUNT  32

// Decompressed once and kept, 19KB. Nothing in 3ds/ui/ uses the game's heap.
static u8    sTiles[MAP_TILE_COUNT * 64];
static u8    sTilemap[TILEMAP_STRIDE * TILEMAP_STRIDE];
static u16   sPal[256];
static u8    sIconGfx[0x80];        // 16x16 4bpp, 4 tiles, stored uncompressed
static u16   sIconPal[16];
static bool8 sLoaded;

// The tile that the player tapped last, or -1 to follow the player.
static s8 sPickX = -1;
static s8 sPickY = -1;

// TRUE when the selected destination waits for YES. Cleared when the selection
// moves, so a confirm always belongs to the current town.
static bool8 sConfirm;

// TRUE when ESCAPE waits for YES. It belongs to the player's own location, so
// any tap on the map clears it.
static bool8 sEscConfirm;

// ---------------------------------------------------------------- loading ---
//
// The map never changes, so it loads once. Check the size of both destinations
// before each decompress. LZDecompressWram uses only the size word in its
// input, so an overrun writes into the statics of this file. See the note above
// UiMonPic in ui_draw.c.
static void EnsureLoaded(void)
{
    const u32 *gfxLZ;
    const u32 *tilemapLZ;
    const u16 *gbaPal;
    const u8 *iconGfx;
    const u16 *iconPal;

    if (sLoaded)
        return;

    Ctr3dsGetRegionMapGfx(&gfxLZ, &tilemapLZ, &gbaPal);

    if (GetDecompressedDataSize(gfxLZ) > sizeof(sTiles))
        return;
    if (GetDecompressedDataSize(tilemapLZ) > sizeof(sTilemap))
        return;

    LZDecompressWram(gfxLZ, sTiles);
    LZDecompressWram(tilemapLZ, sTilemap);

    // Only the part that the art uses. The other entries stay 0. No tile in the
    // tilemap uses a byte outside this range.
    UiLoadPal(&sPal[MAP_PAL_BASE], gbaPal, MAP_PAL_COUNT);

    Ctr3dsGetRegionMapPlayerIcon(&iconGfx, &iconPal);
    for (u32 i = 0; i < sizeof(sIconGfx); i++)
        sIconGfx[i] = iconGfx[i];
    UiLoadPal(sIconPal, iconPal, 16);

    sLoaded = TRUE;
}

// --------------------------------------------------------------- helpers ----

// TRUE when a tap is on a place with a name. It uses the same absolute map-tile
// coordinates as GetRegionMapSecIdAt, which is the touch position divided by 8.
static bool8 PickIsSet(void)
{
    return sPickX >= 0 && sPickY >= 0;
}

// The mapsec in the caption, and where its box is. This always writes
// `mapSecId`. It writes the rest only when the caption describes the player.
static mapsec_u16_t CaptionMapSec(u8 *posWithinMapSec)
{
    u16 x, y;
    mapsec_u16_t mapSecId;
    bool8 inCave;

    if (PickIsSet())
    {
        *posWithinMapSec = 0;
        return GetRegionMapSecIdAt((u16)sPickX, (u16)sPickY);
    }

    Ctr3dsGetRegionMapPlayerPos(&x, &y, &mapSecId, posWithinMapSec, &inCave);
    return mapSecId;
}

// ------------------------------------------------------------------ fly -----
//
// All the game's gates for a fly, in one place. Four are about the player and
// one is about the destination. All are the game's own tests:
//
//   engine state   the same four conditions as BAG's item use
//   the place      SetUpFieldMove_Fly            (src/party_menu.c)
//   the badge      CursorCb_FieldMove            (src/party_menu.c)
//   a flyer        SetPartyMonFieldSelectionActions, which puts FLY in
//                  the party menu                (src/party_menu.c)
//   the target     CB_HandleFlyMapInput's A press (src/region_map.c)
enum {
    FLY_READY,
    FLY_NOT_A_DEST,   // a route or the sea: no fly control here
    FLY_UNVISITED,    // a town that the player has not reached on foot
    FLY_BUSY,         // in battle, in a script, or not in the overworld
    FLY_BAD_PLACE,    // indoors, in a cave, or underwater
    FLY_NO_BADGE,
    FLY_NO_MON,
};

// The party slot of the mon that flies, or PARTY_SIZE if no mon can.
//
// Skip eggs, as GetPartyMenuActionsType does: an egg offers only SWITCH. Do not
// skip fainted mons, because the party menu does not skip them. The original
// game lets a fainted team fly.
static u8 FlyerSlot(void)
{
    u8 i, j;

    for (i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *mon = &gPlayerParty[i];

        if (GetMonData(mon, MON_DATA_SPECIES) == SPECIES_NONE)
            continue;
        if (GetMonData(mon, MON_DATA_IS_EGG))
            continue;

        for (j = 0; j < MAX_MON_MOVES; j++)
            if (GetMonData(mon, MON_DATA_MOVE1 + j) == MOVE_FLY)
                return i;
    }

    return PARTY_SIZE;
}

// The engine-state gate for FLY and ESCAPE: the same four conditions as the
// BAG tab, and no fade. This runs from CtrBottomUpdate, which does not know
// what the frame did. Both controls leave the map, so this gate is even more
// important here.
static bool8 OverworldIdle(void)
{
    if (gMain.inBattle)
        return FALSE;
    if (gMain.callback2 != CB2_Overworld)
        return FALSE;
    if (ArePlayerFieldControlsLocked())
        return FALSE;
    if (ScriptContext_IsEnabled())
        return FALSE;

    // Each exit from the overworld in the game waits for this: the start menu,
    // item use and the PC all check `if (!gPaletteFade.active)`. A fade means
    // that a transition has started. A second transition breaks the map.
    if (gPaletteFade.active)
        return FALSE;

    return TRUE;
}

static u8 FlyState(mapsec_u16_t dest)
{
    u8 type;

    if (dest >= MAPSEC_NONE)
        return FLY_NOT_A_DEST;

    type = Ctr3dsGetMapSecType(dest);

    // Routes, the sea, and the landmarks with no fly icon. There is no control
    // here.
    if (type != MAPSECTYPE_CITY_CANFLY
     && type != MAPSECTYPE_CITY_CANTFLY
     && type != MAPSECTYPE_BATTLE_FRONTIER)
        return FLY_NOT_A_DEST;

    // Check this before the player's gates. A tap on an unvisited town then
    // says so, and does not blame a missing badge. This is the FLAG_VISITED_*
    // test, through GetMapsecType.
    if (type == MAPSECTYPE_CITY_CANTFLY)
        return FLY_UNVISITED;

    if (!OverworldIdle())
        return FLY_BUSY;

    // SetUpFieldMove_Fly. The party menu refuses indoors, in a cave or
    // underwater.
    if (Overworld_MapTypeAllowsTeleportAndFly(gMapHeader.mapType) != TRUE)
        return FLY_BAD_PLACE;

    // CursorCb_FieldMove tests FlagGet(FLAG_BADGE01_GET + fieldMove), and
    // FIELD_MOVE_FLY is index 5. This names the badge directly, because that
    // enum is private to src/party_menu.c.
    if (!FlagGet(FLAG_BADGE06_GET))
        return FLY_NO_BADGE;

    if (FlyerSlot() >= PARTY_SIZE)
        return FLY_NO_MON;

    return FLY_READY;
}

// The reason that the button does not show. Show the reason, not a gray button.
// The player has no other way to find out, and can act on each reason.
static const char *FlyRefusal(u8 state)
{
    switch (state)
    {
    case FLY_UNVISITED: return "never been there";
    case FLY_BUSY:      return "not right now";
    case FLY_BAD_PLACE: return "can't FLY from here";
    case FLY_NO_BADGE:  return "no FEATHER BADGE";
    default:            return "nobody knows FLY";
    }
}

static void DoFly(mapsec_u16_t mapSecId)
{
    // FlyState checked this, so it is never PARTY_SIZE.
    u8 slot = FlyerSlot();

    // Refuse, and do not fly to the last warp point. FlyState already refuses
    // each destination that this can reject, so this is only a second guard.
    if (!Ctr3dsSetFlyWarpDestination(mapSecId, (u16)sPickX, (u16)sPickY))
        return;

    // Task_UseFly asks the party menu's cursor which mon flies
    // (GetCursorSelectionMonId). No party menu opened, so set the cursor to the
    // mon that FlyState found. Otherwise it animates the last highlighted mon
    // or slot 0.
    gPartyMenu.slotId = (s8)slot;

    // Release the overworld's graphics before you leave it.
    //
    // This is necessary. The overworld frees these when it starts, not when it
    // ends. Thus the code that takes the main callback from CB2_Overworld must
    // free them first. All twenty exits from the overworld in the game call
    // this.
    //
    // Without it, each fly leaks 0x2D80 bytes of heap and nothing looks wrong.
    // After fewer than ten flies, the 0x1C000 heap is full. Then InitWindows
    // gets NULL, and the next window draw writes to address 0.
    //
    // Stop the rain sound for the same reason as the start menu: nothing later
    // stops it.
    PlayRainStoppingSoundEffect();
    CleanupOverworldWindowsAndTilemaps();

    // The same steps as CB_ExitFlyMap after it sets the destination.
    //
    // It is safe to replace the main callback here. Rp2350PresentFrame runs at
    // the end of a frame, after CallCallbacks and VBlankIntr (src/main.c), so
    // the old callback has finished.
    ReturnToFieldFromFlyMapSelect();

    // Go back to following the player. The tab then does not show an old
    // selection box over the town that the player left.
    sPickX = -1;
    sPickY = -1;
    sConfirm = FALSE;
    sEscConfirm = FALSE;
    UiMarkDirty();
}

// ---------------------------------------------------------------- escape ----
//
// ESCAPE takes the player out of a cave or a building to the place they went
// in, as DIG and the ESCAPE ROPE do. It is for the player's own location, so it
// shows only while no place is selected. The gates are the game's own:
//
//   engine state   OverworldIdle, as for FLY
//   the place      CanUseDigOrEscapeRopeOnCurMap (both DIG and the rope)
//   the means      a mon that knows DIG, as the party menu lists it, or an
//                  ESCAPE ROPE in the bag
//
// DIG has no badge gate: only HMs have one (CursorCb_FieldMove). DIG comes
// first, because the rope is used up and DIG is free.
enum {
    ESC_DIG,
    ESC_ROPE,
    ESC_NONE,       // no control: the place, the engine or the means says no
};

// The party slot of the mon that digs, or PARTY_SIZE. The same rules as
// FlyerSlot.
static u8 DiggerSlot(void)
{
    u8 i, j;

    for (i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *mon = &gPlayerParty[i];

        if (GetMonData(mon, MON_DATA_SPECIES) == SPECIES_NONE)
            continue;
        if (GetMonData(mon, MON_DATA_IS_EGG))
            continue;

        for (j = 0; j < MAX_MON_MOVES; j++)
            if (GetMonData(mon, MON_DATA_MOVE1 + j) == MOVE_DIG)
                return i;
    }

    return PARTY_SIZE;
}

// There is no refusal text, unlike FLY. The player did not ask about this
// place, and "can't escape" on every route is noise. The button shows or it
// does not.
static u8 EscState(void)
{
    if (!OverworldIdle())
        return ESC_NONE;
    if (CanUseDigOrEscapeRopeOnCurMap() != TRUE)
        return ESC_NONE;
    if (DiggerSlot() < PARTY_SIZE)
        return ESC_DIG;
    if (CheckBagHasItem(ITEM_ESCAPE_ROPE, 1))
        return ESC_ROPE;
    return ESC_NONE;
}

// TRUE while the player is between two tiles.
//
// The game uses DIG and the rope from a menu, so the player always stands
// still. This screen does not stop the d-pad. StartEscapeRopeFieldEffect
// freezes every object and then waits for the player's held movement to end. A
// walk that is frozen never ends, so the screen fades to black and stays
// there. DIG's script waits for the player (lockall), but check it for both.
//
// Only the commit checks this. The button does not hide during a walk, or it
// would flicker at each step.
static bool8 PlayerIsMoving(void)
{
    struct ObjectEvent *player = &gObjectEvents[gPlayerAvatar.objectEventId];

    if (gPlayerAvatar.tileTransitionState != T_NOT_MOVING)
        return TRUE;

    return player->heldMovementActive && !player->heldMovementFinished;
}

static void DoEscape(u8 state)
{
    if (state == ESC_DIG)
    {
        // DiggerSlot is never PARTY_SIZE here: EscState found the mon.
        u8 slot = DiggerSlot();

        // FieldCallback_Dig (src/fldeff_dig.c), the step after the party menu
        // closes, without the menu. It reads the mon from the party menu's
        // cursor, so set the cursor, as DoFly does.
        gPartyMenu.slotId = (s8)slot;
        Overworld_ResetStateAfterDigEscRope();
        gFieldEffectArguments[0] = slot;
        ScriptContext_SetupScript(EventScript_UseDig);
    }
    else
    {
        // ItemUseOnFieldCB_EscapeRope, then Task_UseDigEscapeRopeOnField
        // (src/item_use.c), without the message box. The spin is the feedback.
        Overworld_ResetStateAfterDigEscRope();
        RemoveBagItem(ITEM_ESCAPE_ROPE, 1);
        ResetInitialPlayerAvatarState();
        StartEscapeRopeFieldEffect();
    }

    sEscConfirm = FALSE;
    UiMarkDirty();
}

// --------------------------------------------------------------- drawing ----

static void DrawMap(void)
{
    for (int ty = 0; ty < MAP_TH; ty++)
    {
        for (int tx = 0; tx < MAP_TW; tx++)
        {
            u32 tileId = sTilemap[ty * TILEMAP_STRIDE + tx];

            // Opaque: index 0 is in no tile of this map, so there is nothing to
            // test per pixel.
            UiBlit8bppTile(MAP_PX + tx * 8, MAP_PY + ty * 8,
                           &sTiles[tileId * 64], sPal, FALSE);
        }
    }
}

// The 16x16 icon, at the position that CreateRegionMapPlayerIcon uses. That
// sets the sprite's center to tile*8 + 4, so the top-left corner is at tile*8 -
// 4.
static void DrawPlayerIcon(void)
{
    u16 x, y;
    mapsec_u16_t mapSecId;
    u8 posWithinMapSec;
    bool8 inCave;
    int px, py;

    // Birth Island, Faraway Island and Navel Rock are not on the Hoenn map. The
    // game shows no icon there either.
    if (IsEventIslandMapSecId(gMapHeader.regionMapSectionId))
        return;

    Ctr3dsGetRegionMapPlayerPos(&x, &y, &mapSecId, &posWithinMapSec, &inCave);

    px = MAP_PX + (int)x * 8 - 4;
    py = MAP_PY + (int)y * 8 - 4;

    for (int t = 0; t < 4; t++)
        UiBlit4bppTile(px + (t % 2) * 8, py + (t / 2) * 8,
                       sIconGfx + t * 32, sIconPal, TRUE);
}

// Put a box around the full mapsec, not only the tapped tile. A city of two
// tiles is one place.
static void DrawPick(void)
{
    mapsec_u16_t mapSecId;
    const struct RegionMapLocation *entry;

    if (!PickIsSet())
        return;

    mapSecId = GetRegionMapSecIdAt((u16)sPickX, (u16)sPickY);
    if (mapSecId >= MAPSEC_NONE)
        return;

    entry = &gRegionMapEntries[mapSecId];

    UiRect(MAP_PX + (entry->x + CTR_MAPCURSOR_X_MIN) * 8,
           MAP_PY + (entry->y + CTR_MAPCURSOR_Y_MIN) * 8,
           entry->width * 8, entry->height * 8, UI_COL_ACCENT);
}

// The same shape as the EXTRA buttons: a 1px border, with the accent doubled on
// the button that commits. Thus a button looks the same on both tabs. Local,
// because only two buttons outside tab_extra.c use it.
static void DrawBtn(int x, int w, const char *text, int accent)
{
    // Larger than the longest label. UiAscii cuts a long label without a
    // warning.
    u8 label[16];

    UiRect(x, FLY_BTN_Y, w, FLY_BTN_H, UI_COL_DIM);

    if (accent)
    {
        UiRect(x + 2, FLY_BTN_Y + 2, w - 4, FLY_BTN_H - 4, UI_COL_ACCENT);
        UiRect(x + 3, FLY_BTN_Y + 3, w - 6, FLY_BTN_H - 6, UI_COL_ACCENT);
    }

    UiAscii(label, text, sizeof(label));
    UiText(x + (w - UiTextWidth(label)) / 2,
           FLY_BTN_Y + (FLY_BTN_H - UI_GLYPH_H) / 2,
           label, accent ? UI_COL_ACCENT : UiThemeText(), UiThemeShadow());
}

// ------------------------------------------------------------ encounters ---
//
// The lookup that the encounter list uses for the place in the caption. A
// tapped tile is a mapsec. The player's location is a map, because the region
// map has no cave interiors. See the note on UI_ENC_SRC_PLAYER.
static u8 EncSource(void)
{
    return PickIsSet() ? UI_ENC_SRC_MAPSEC : UI_ENC_SRC_PLAYER;
}

// The x of the WILD button on the caption row, or -1 when it is not there.
//
// The draw and the hit test both use this, so a button that does not show
// cannot work. The row can hold four different items, and one function must
// decide.
static int WildBtnX(mapsec_u16_t mapSecId)
{
    if (mapSecId >= MAPSEC_NONE)
        return -1;

    // A place with no encounters gets no button, not a button that opens an
    // empty panel.
    if (!UiEncountersAvailable(EncSource(), mapSecId))
        return -1;

    // A confirm uses the full row: the question right-aligns at CFM_ASK_X, then
    // NO and YES fill it to the margin. There is no space for this button, and
    // a third button next to YES causes wrong taps.
    if (PickIsSet() && sConfirm && FlyState(mapSecId) == FLY_READY)
        return -1;
    if (!PickIsSet() && sEscConfirm && EscState() != ESC_NONE)
        return -1;

    // Otherwise, always the same slot. See the note on WILD_BTN_X.
    return WILD_BTN_X;
}

// The x of the FLY button: the right-aligned slot, or one step inward when WILD
// uses that slot. The draw and the hit test both use this, as for WildBtnX.
//
// This applies only while a FLY button shows: FLY_READY with no confirm. The
// refusal text and the YES/NO pair place themselves.
static int FlyBtnX(mapsec_u16_t mapSecId)
{
    return WildBtnX(mapSecId) >= 0 ? FLY_INNER_X : FLY_BTN_X;
}

// ESCAPE takes FLY's slot, by the same rule.
static int EscBtnX(mapsec_u16_t mapSecId)
{
    return FlyBtnX(mapSecId);
}

// The ESCAPE confirm: the same row shape as FLY's. The question names the
// means, because DIG is free and the rope is used up.
static void DrawEscConfirm(u8 state)
{
    u8 label[24];

    UiTextRight(CFM_ASK_X, CAP_TEXT_Y,
                UiAscii(label, state == ESC_DIG ? "DIG?" : "ESCAPE ROPE?",
                        sizeof(label)),
                UiThemeText(), UiThemeShadow());
    DrawBtn(CFM_NO_X, CFM_W, "NO", FALSE);
    DrawBtn(CFM_YES_X, CFM_W, "YES", TRUE);
}

// The right end of the caption row: a FLY button, the reason for no button, or
// the confirm that replaces it.
//
// `textRight` is the right limit of a refusal string. It is the caption margin,
// or the left of the WILD button when that button is there.
//
// Nothing here is cached. Read FlyState on each repaint, so the row stays
// correct when the player goes indoors, faints the last flyer or starts a
// script. The touch handler's check then always agrees with the screen.
static void DrawFlyControls(mapsec_u16_t mapSecId, int textRight)
{
    u8 label[24];
    u8 state = FlyState(mapSecId);

    if (state == FLY_NOT_A_DEST)
        return;

    if (state != FLY_READY)
    {
        UiTextRight(textRight, CAP_TEXT_Y,
                    UiAscii(label, FlyRefusal(state), sizeof(label)),
                    UI_COL_DIM, UiThemeShadow());
        return;
    }

    if (!sConfirm)
    {
        DrawBtn(FlyBtnX(mapSecId), FLY_BTN_W, "FLY", FALSE);
        return;
    }

    // The place name is already on the left of this row, so the question does
    // not repeat it. "SLATEPORT CITY ... FLY? NO YES" reads as one sentence.
    UiTextRight(CFM_ASK_X, CAP_TEXT_Y, UiAscii(label, "FLY?", sizeof(label)),
                UiThemeText(), UiThemeShadow());
    DrawBtn(CFM_NO_X, CFM_W, "NO", FALSE);
    DrawBtn(CFM_YES_X, CFM_W, "YES", TRUE);
}

static void DrawCaption(void)
{
    u8 posWithinMapSec = 0;
    mapsec_u16_t mapSecId = CaptionMapSec(&posWithinMapSec);
    const u8 *landmark;
    int wildX;
    int textRight;
    // Not MAP_NAME_LENGTH (16). The game's struct RegionMap uses 20 for this
    // field, and GetMapName's empty-name fallback writes 18 and a terminator.
    u8 name[32];

    UiWindowFrame(0, CAP_TY, CTR_BOTTOM_WIDTH / 8, CAP_TH);

    if (mapSecId >= MAPSEC_NONE)
    {
        u8 label[16];
        UiText(CAP_MARGIN, CAP_TEXT_Y, UiAscii(label, "Sea", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
        return;
    }

    // This is already in the game's encoding, so it goes directly to UiText.
    GetMapNameGeneric(name, mapSecId);
    UiText(CAP_MARGIN, CAP_TEXT_Y, name, UiThemeText(), UiThemeShadow());

    // When the WILD button is at the end of this row, the other text stops
    // before it.
    //
    // Both possible strings clear the place name in every real pair. The widest
    // refusal is about 100px and runs 148..248. The longest landmark is about
    // 118px and runs 130..248; it belongs to Route 114, which has a short name.
    // The longest place name ends at 100.
    wildX = WildBtnX(mapSecId);
    textRight = (wildX >= 0) ? WILD_BTN_X - 8 : CTR_BOTTOM_WIDTH - CAP_MARGIN;

    // Landmarks are only for the player's own location. They depend on which
    // tile of a mapsec the player stands on, and a tapped tile has no such
    // position. A tapped tile has a fly control instead. The two never show
    // together.
    if (PickIsSet())
    {
        DrawFlyControls(mapSecId, textRight);
    }
    else
    {
        u8 esc = EscState();

        // A confirm uses the full row, as FLY's does, so no landmark.
        if (esc != ESC_NONE && sEscConfirm)
        {
            DrawEscConfirm(esc);
        }
        else
        {
            if (esc != ESC_NONE)
            {
                int escX = EscBtnX(mapSecId);

                DrawBtn(escX, FLY_BTN_W, "ESCAPE", FALSE);
                textRight = escX - 8;
            }

            // With ESCAPE on the row, a long landmark can reach the place
            // name. Then leave the landmark out. The name matters more.
            landmark = GetLandmarkName((mapsec_u8_t)mapSecId, posWithinMapSec, 0);
            if (landmark != NULL
             && textRight - UiTextWidth(landmark)
                >= CAP_MARGIN + UiTextWidth(name) + 8)
                UiTextRight(textRight, CAP_TEXT_Y, landmark,
                            UI_COL_DIM, UiThemeShadow());
        }
    }

    if (wildX >= 0)
        DrawBtn(wildX, WILD_BTN_W, "WILD PKMN", FALSE);
}

void UiMapDraw(void)
{
    // The encounter list replaces the full tab while it is up, like the DEX
    // entry screen. It is not an overlay, because the map has nothing useful
    // behind it.
    if (UiEncountersIsOpen())
    {
        UiEncountersDraw();
        return;
    }

    EnsureLoaded();

    if (!sLoaded)
    {
        u8 label[24];
        UiWindowFrame(0, 0, CTR_BOTTOM_WIDTH / 8, UI_CONTENT_H / 8);
        UiText(16, 16, UiAscii(label, "Map unavailable.", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
        return;
    }

    DrawMap();
    DrawPick();
    DrawPlayerIcon();
    DrawCaption();
}

// ----------------------------------------------------------- repaint key ----
//
// Without this key, the map is stale while the player walks. Nothing else in
// UiStateHash tracks the position. The cursor moves only when the player
// crosses a band in a mapsec, so a walk along a route costs few repaints.
u32 UiMapStateKey(void)
{
    u16 x, y;
    mapsec_u16_t mapSecId;
    u8 posWithinMapSec;
    bool8 inCave;

    u32 key;

    Ctr3dsGetRegionMapPlayerPos(&x, &y, &mapSecId, &posWithinMapSec, &inCave);

    key = ((u32)x << 24) ^ ((u32)y << 16) ^ ((u32)mapSecId << 4)
        ^ (u32)posWithinMapSec ^ ((u32)inCave << 3);

    // The inputs of the fly row, which the fields above do not track. Examples
    // are a script that ends, a battle that starts, or the last flyer put in
    // the PC. None of them move the player.
    //
    // Multiply, do not XOR the raw value. The fields above use most of the
    // word, and two values that cancel stop the row from updating.
    if (PickIsSet())
    {
        u32 fly = (u32)FlyState(GetRegionMapSecIdAt((u16)sPickX, (u16)sPickY))
                | ((u32)(sConfirm != 0) << 3)
                | ((u32)(u8)sPickX << 4)
                | ((u32)(u8)sPickY << 12);

        key ^= fly * 2654435761u;
    }
    else
    {
        // The inputs of the ESCAPE control, for the same reasons as the fly
        // row: a script that ends, a DIG mon put in the PC, the last rope
        // used. Its own multiplier, so it cannot cancel the others.
        u32 esc = (u32)EscState() | ((u32)(sEscConfirm != 0) << 2);

        key ^= esc * 0xC2B2AE35u;
    }

    // The inputs of the encounter panel: the page, the map, and whether each
    // mon on the page is seen. Zero while it is closed.
    //
    // It has its own multiplier. Two values folded through the same constant
    // can cancel.
    key ^= UiEncountersStateKey() * 0x85EBCA6Bu;

    return key;
}

// --------------------------------------------------------------- input -----

void UiMapLeave(void)
{
    // The selected place stays: it is where the player was looking, not a
    // question waiting for an answer.
    sConfirm = FALSE;
    sEscConfirm = FALSE;
}

// The controls of the caption band. Returns TRUE when it used the tap.
static bool8 HandleFlyTouch(const CtrTouchState *t)
{
    mapsec_u16_t mapSecId = GetRegionMapSecIdAt((u16)sPickX, (u16)sPickY);

    // Check again at the moment of the tap. The player can walk, start a script
    // or enter a battle while this screen is open, and an old confirm must not
    // survive that. DrawFlyControls hides the row in the same case, so no tap
    // reaches a button that does not show.
    if (FlyState(mapSecId) != FLY_READY)
    {
        sConfirm = FALSE;
        return FALSE;
    }

    if (!sConfirm)
    {
        if (!UiHit(t, FlyBtnX(mapSecId), FLY_BTN_Y, FLY_BTN_W, FLY_BTN_H))
            return FALSE;

        sConfirm = TRUE;
        UiMarkDirty();
        return TRUE;
    }

    if (UiHit(t, CFM_NO_X, FLY_BTN_Y, CFM_W, FLY_BTN_H))
    {
        sConfirm = FALSE;
        UiMarkDirty();
        return TRUE;
    }

    if (UiHit(t, CFM_YES_X, FLY_BTN_Y, CFM_W, FLY_BTN_H))
    {
        DoFly(mapSecId);
        return TRUE;
    }

    return FALSE;
}

// The ESCAPE control. Returns TRUE when it used the tap. Only while no place is
// selected.
static bool8 HandleEscTouch(const CtrTouchState *t)
{
    u8 posWithinMapSec = 0;
    mapsec_u16_t mapSecId;
    u8 state = EscState();

    // Check again at the moment of the tap, as HandleFlyTouch does.
    if (state == ESC_NONE)
    {
        sEscConfirm = FALSE;
        return FALSE;
    }

    if (!sEscConfirm)
    {
        mapSecId = CaptionMapSec(&posWithinMapSec);
        if (!UiHit(t, EscBtnX(mapSecId), FLY_BTN_Y, FLY_BTN_W, FLY_BTN_H))
            return FALSE;

        sEscConfirm = TRUE;
        UiMarkDirty();
        return TRUE;
    }

    if (UiHit(t, CFM_NO_X, FLY_BTN_Y, CFM_W, FLY_BTN_H))
    {
        sEscConfirm = FALSE;
        UiMarkDirty();
        return TRUE;
    }

    if (UiHit(t, CFM_YES_X, FLY_BTN_Y, CFM_W, FLY_BTN_H))
    {
        // Keep the confirm up. The next tap, after the step ends, acts. See
        // PlayerIsMoving.
        if (!PlayerIsMoving())
            DoEscape(state);
        return TRUE;
    }

    return FALSE;
}

void UiMapTouch(const CtrTouchState *t)
{
    int tx, ty;

    // The panel covers the full tab, so it takes every touch (presses, drags
    // and releases) until its BACK closes it.
    if (UiEncountersIsOpen())
    {
        UiEncountersTouch(t);
        return;
    }

    if (!t->justReleased)
        return;

    // Test the two controls of the caption band before the deselect below,
    // which takes every tap outside the map. Other taps in the band deselect,
    // so a tap away from a confirm cancels it.
    if (t->y >= CAP_Y)
    {
        u8 posWithinMapSec = 0;
        mapsec_u16_t capMapSec;
        int wildX;

        // Fly first: it can be in a confirm, and its own check decides if its
        // buttons show.
        if (PickIsSet() && HandleFlyTouch(t))
            return;
        if (!PickIsSet() && HandleEscTouch(t))
            return;

        capMapSec = CaptionMapSec(&posWithinMapSec);
        wildX = WildBtnX(capMapSec);

        if (wildX >= 0 && UiHit(t, wildX, FLY_BTN_Y, WILD_BTN_W, FLY_BTN_H))
        {
            UiEncountersOpen(EncSource(), capMapSec);
            return;
        }
    }

    // A tap outside the map, the caption band too, drops the selection. The
    // caption then shows the player's location again.
    if (t->y < MAP_PY || t->y >= MAP_PY + MAP_TH * 8
     || t->x < MAP_PX || t->x >= MAP_PX + MAP_TW * 8)
    {
        if (PickIsSet() || sEscConfirm)
        {
            sPickX = -1;
            sPickY = -1;
            sConfirm = FALSE;
            sEscConfirm = FALSE;
            UiMarkDirty();
        }
        return;
    }

    tx = (t->x - MAP_PX) / 8;
    ty = (t->y - MAP_PY) / 8;

    // The open sea has no mapsec. Treat a tap there as a deselect, so there is
    // always a way back to the player.
    if (GetRegionMapSecIdAt((u16)tx, (u16)ty) >= MAPSEC_NONE)
    {
        sPickX = -1;
        sPickY = -1;
    }
    else
    {
        sPickX = (s8)tx;
        sPickY = (s8)ty;
    }

    // A new selection starts without a confirm. Otherwise a YES from one town
    // moves to the next, and the next tap flies to a place that the player did
    // not confirm.
    sConfirm = FALSE;
    sEscConfirm = FALSE;
    UiMarkDirty();
}
