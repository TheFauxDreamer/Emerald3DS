// MAP tab: Hoenn's region map, drawn at 1:1, with a marker where the player is.
//
// The art is the game's own -- the same tiles, tilemap and palette the PokeNav
// draws -- reached through the accessors in the PLATFORM_3DS block of
// src/region_map.c, because they are file-static there.
//
// Drawing the map writes nothing. Flying from it does, and that half of the
// file is built the way the BAG tab's item use is built: every check the game
// makes is made here too, the tap that commits is a separate deliberate tap
// from the tap that selects, and the checks are re-run at the moment of
// commit rather than trusted from when the button was drawn. See FlyState.
//
// Three things about that art are not obvious and are worth stating once, since
// getting any of them wrong produces a plausible-looking wrong picture:
//
//   1. The background is 8bpp, not 4bpp. UiBlit4bppTile cannot draw it.
//
//   2. Its tilemap is an AFFINE one: BG2 is set up with BG_ATTR_SCREENSIZE 2 and
//      BG_ATTR_PALETTEMODE 1 (src/region_map.c), and affine screen size 2 is
//      64x64 tiles at ONE BYTE per entry, which is exactly the 4096 bytes
//      map.bin decompresses to. So there are no 16-bit screen entries, no flip
//      bits and no palette-bank field: the tile id is simply the byte.
//
//   3. Those tile bytes are ABSOLUTE palette indices in the 112..143 range,
//      because the game loads the map's 32 colours at BG_PLTT_ID(7). Hence the
//      256-entry palette below with only that slice filled.
//
// What is deliberately NOT used: InitRegionMap() and LoadRegionMapGfx(). They
// drive BG layers, sprites and the task system on the top screen. The one piece
// of src/region_map.c this file does lean on is the player's position, and only
// because it is pure arithmetic -- see Ctr3dsGetRegionMapPlayerPos.

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
#include "battle.h"                   // struct DisableStruct, for the header below
#include "party_menu.h"               // gPartyMenu, whose slotId picks the flyer
#include "constants/region_map_sections.h"
#include "constants/flags.h"
#include "constants/moves.h"
#include "constants/species.h"

#include "../bridge.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"

// The drawn artwork's real extent inside the 64x64 tilemap: everything outside
// this is the blank tile. Measured from map.bin rather than assumed, because the
// MAPSEC grid (28x15 at offset 1,2) is smaller than the picture around it -- the
// northern coast, Dewford's islands and the eastern edge all sit outside it.
#define MAP_TW        31
#define MAP_TH        19
#define TILEMAP_STRIDE 64

// 31 tiles is 248px in a 320px panel, so it centres with 36px each side, and 19
// tiles is 152px, leaving exactly 40px for the caption. 2x would be 496x304 and
// does not fit in either dimension, so 1x is the only scale -- which is also the
// only pixel-perfect one.
#define MAP_PX        ((CTR_BOTTOM_WIDTH - MAP_TW * 8) / 2)
#define MAP_PY        0

// The caption takes the rest of the content area. Both bands are whole tiles so
// the window frame lands on tile boundaries: 19 + 5 = 24 tiles = 192px.
#define CAP_TY        MAP_TH
#define CAP_TH        ((UI_CONTENT_H / 8) - MAP_TH)
#define CAP_Y         (CAP_TY * 8)
#define CAP_TEXT_Y    (CAP_Y + 12)
#define CAP_MARGIN    10

// The fly controls share the caption band's one interior row. The band is 5
// tiles at 152..192 and its 8px frame leaves y 160..184, so a 22px button at
// 161 sits inside it with a pixel to spare, and its glyph row lands on
// CAP_TEXT_Y -- the button and the place name read as a single line.
#define FLY_BTN_H     22
#define FLY_BTN_Y     (CAP_Y + 9)
#define FLY_BTN_W     54
#define FLY_BTN_X     (CTR_BOTTOM_WIDTH - CAP_MARGIN - FLY_BTN_W)

// Confirming replaces that one button with three things on the same row: the
// ask right-aligned at 202, then NO at 210..256 and YES at 264..310.
//
// 202 is chosen against the widest thing that can share the row. The longest
// place name is EVER GRANDE CITY at 90px, which from x=10 ends at 100, so there
// are 78px to spare whatever is selected. The widest refusal that replaces all
// three, "can't FLY from here" at 100px, right-aligns to 210 and clears it too.
#define CFM_W         46
#define CFM_YES_X     (CTR_BOTTOM_WIDTH - CAP_MARGIN - CFM_W)
#define CFM_NO_X      (CFM_YES_X - 8 - CFM_W)
#define CFM_ASK_X     (CFM_NO_X - 8)

#define MAP_TILE_COUNT 233
#define MAP_PAL_BASE   112
#define MAP_PAL_COUNT  32

// Decompressed once and kept. 19KB against a 150KB framebuffer in the same
// directory, and nothing in 3ds/ui/ ever touches the game's heap.
static u8    sTiles[MAP_TILE_COUNT * 64];
static u8    sTilemap[TILEMAP_STRIDE * TILEMAP_STRIDE];
static u16   sPal[256];
static u8    sIconGfx[0x80];        // 16x16 4bpp, 4 tiles, stored uncompressed
static u16   sIconPal[16];
static bool8 sLoaded;

// The tile the player last tapped, or -1 to follow the player instead.
static s8 sPickX = -1;
static s8 sPickY = -1;

// Whether the selected destination is waiting on a YES. Cleared whenever the
// selection moves, so a raised confirm can never belong to a different town.
static bool8 sConfirm;

// ---------------------------------------------------------------- loading ---
//
// The map never changes, so this is a one-shot load rather than a keyed cache.
// Both destinations are size-checked before either decompress: LZDecompressWram
// is bounded only by the size word in its own input, and an overrun here lands
// in the neighbouring statics of this file. That exact bug has been hit in
// ui_draw.c before -- see the comment above UiMonPic.
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

    // Only the slice the art uses. Every other entry stays 0 and is never
    // indexed: no tile the tilemap references contains a byte outside this range.
    UiLoadPal(&sPal[MAP_PAL_BASE], gbaPal, MAP_PAL_COUNT);

    Ctr3dsGetRegionMapPlayerIcon(&iconGfx, &iconPal);
    for (u32 i = 0; i < sizeof(sIconGfx); i++)
        sIconGfx[i] = iconGfx[i];
    UiLoadPal(sIconPal, iconPal, 16);

    sLoaded = TRUE;
}

// --------------------------------------------------------------- helpers ----

// Whether a tap landed on something nameable. Takes the same absolute map-tile
// coordinates GetRegionMapSecIdAt does, which is what dividing a touch position
// by 8 gives directly.
static bool8 PickIsSet(void)
{
    return sPickX >= 0 && sPickY >= 0;
}

// The mapsec the caption is describing, and where its box sits. `mapSecId` is
// always written; the rest only when the player is what is being described.
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
// Every gate the game puts in front of a fly, in one place. Four of them are
// about the player and one is about the destination, and they are all the
// game's own tests rather than reimplementations:
//
//   engine state   the same four conditions the BAG tab's item use checks
//   the place      SetUpFieldMove_Fly            (src/party_menu.c)
//   the badge      CursorCb_FieldMove            (src/party_menu.c)
//   a flyer        SetPartyMonFieldSelectionActions, which is what puts FLY in
//                  the party menu at all         (src/party_menu.c)
//   the target     CB_HandleFlyMapInput's A press (src/region_map.c)
enum {
    FLY_READY,
    FLY_NOT_A_DEST,   // a route or the sea: no fly control belongs here at all
    FLY_UNVISITED,    // a town, but not one the player has reached on foot
    FLY_BUSY,         // mid-battle, mid-script, or not in the overworld
    FLY_BAD_PLACE,    // indoors, in a cave, underwater
    FLY_NO_BADGE,
    FLY_NO_MON,
};

// The party slot that will perform the fly, or PARTY_SIZE if nobody can.
//
// Eggs are skipped to match GetPartyMenuActionsType, which offers an egg
// SWITCH and nothing else, so its stored moves are never usable as field moves.
// Fainted mons are NOT skipped, because the party menu does not skip them
// either: flying with a whited-out team is allowed in the original game and
// removing that here would be a change, not a check.
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

static u8 FlyState(mapsec_u16_t dest)
{
    u8 type;

    if (dest >= MAPSEC_NONE)
        return FLY_NOT_A_DEST;

    type = Ctr3dsGetMapSecType(dest);

    // Routes, the sea and the landmarks the fly map draws no icon over. Nothing
    // to explain: the control simply does not exist there.
    if (type != MAPSECTYPE_CITY_CANFLY
     && type != MAPSECTYPE_CITY_CANTFLY
     && type != MAPSECTYPE_BATTLE_FRONTIER)
        return FLY_NOT_A_DEST;

    // Checked before the player-side gates so that tapping a town you have
    // never seen says so, rather than blaming a badge you also happen to lack.
    // This is the FLAG_VISITED_* test, reached through GetMapsecType.
    if (type == MAPSECTYPE_CITY_CANTFLY)
        return FLY_UNVISITED;

    // The gate the BAG tab carries, for the same reason: this runs from
    // CtrBottomUpdate, which has no idea what the frame it follows was doing.
    // Flying is far more invasive than using a Potion -- it replaces the main
    // callback -- so if anything it matters more here.
    if (gMain.inBattle)
        return FLY_BUSY;
    if (gMain.callback2 != CB2_Overworld)
        return FLY_BUSY;
    if (ArePlayerFieldControlsLocked())
        return FLY_BUSY;
    if (ScriptContext_IsEnabled())
        return FLY_BUSY;

    // Every exit from the overworld in the game waits on this before tearing
    // anything down -- the start menu, item use, the PC, all of them open with
    // `if (!gPaletteFade.active)`. A fade means a transition is already in
    // flight, and starting a second one on top of it hands the callback it was
    // heading for a half-dismantled map.
    if (gPaletteFade.active)
        return FLY_BUSY;

    // SetUpFieldMove_Fly. Indoors, in a cave or underwater the party menu
    // refuses before the fly map is ever opened.
    if (Overworld_MapTypeAllowsTeleportAndFly(gMapHeader.mapType) != TRUE)
        return FLY_BAD_PLACE;

    // CursorCb_FieldMove tests FlagGet(FLAG_BADGE01_GET + fieldMove), and
    // FIELD_MOVE_FLY is index 5. Spelled as the badge itself because that
    // arithmetic, and the enum it indexes, are private to src/party_menu.c.
    if (!FlagGet(FLAG_BADGE06_GET))
        return FLY_NO_BADGE;

    if (FlyerSlot() >= PARTY_SIZE)
        return FLY_NO_MON;

    return FLY_READY;
}

// Why the button is not there. Shown rather than greyed out: on a screen this
// size there is nowhere else for the player to find out, and every one of these
// is something they can act on.
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
    // Vetted by FlyState, so this cannot come back as PARTY_SIZE.
    u8 slot = FlyerSlot();

    // Refuses rather than flying to wherever the last warp happened to point.
    // FlyState has already ruled out every destination this can reject, so this
    // is the belt to that brace, not an expected path.
    if (!Ctr3dsSetFlyWarpDestination(mapSecId, (u16)sPickX, (u16)sPickY))
        return;

    // Task_UseFly asks the party menu's cursor which mon performs the take-off
    // (GetCursorSelectionMonId, src/party_menu.c). No party menu was opened to
    // get here, so without this it would animate whichever mon was last
    // highlighted, or fall back to slot 0 through its own clamp. Point it at
    // the mon this fly was actually authorised on.
    gPartyMenu.slotId = (s8)slot;

    // Hand the overworld's graphics back BEFORE leaving it.
    //
    // This is not optional and it is not tidiness. The overworld does not free
    // these on the way out, it frees them on the way IN, so whatever takes the
    // main callback away from CB2_Overworld has to do it first. Every one of
    // the twenty places in the game that leaves the overworld calls this: the
    // start menu, item use, the PC, egg hatching, battle setup, the lot.
    //
    // Skipping it does not fail visibly, which is what made it dangerous. The
    // first fly looks perfect. ReturnToFieldLocal still reaches
    // InitOverworldBgs, and it leaks 0x2D80 bytes every time:
    //
    //   3 x BG_SCREEN_SIZE   fresh Bg1/2/3 tilemap buffers allocated straight
    //                        over the live pointers
    //   1 x BG_SCREEN_SIZE   InitWindows' first loop sets
    //                        gWindowBgTilemapBuffers[0] to NULL without freeing
    //   27 * 4 * 32          its second loop NULLs every gWindows[].tileData,
    //                        also without freeing
    //
    // That is under ten flies before a 0x1C000 heap has nothing left, sooner
    // with fragmentation. Then an allocation returns NULL, nobody checks
    // InitWindows' return value, and the next thing to draw into a window
    // writes through a null tileData -- which is a write to address 0.
    //
    // The rain SFX is stopped for the same reason the start menu stops it: the
    // map is being left, and nothing downstream of here will silence it.
    PlayRainStoppingSoundEffect();
    CleanupOverworldWindowsAndTilemaps();

    // Exactly what CB_ExitFlyMap does once it has set the destination.
    //
    // Replacing the main callback from here is safe for one specific reason:
    // Rp2350PresentFrame runs at the END of a frame, after CallCallbacks and
    // after VBlankIntr (src/main.c), so the callback being replaced has already
    // finished its turn. This is no different from a callback setting the next
    // one as its own last act, which is what CB_ExitFlyMap is doing.
    ReturnToFieldFromFlyMapSelect();

    // Back to following the player, so landing and reopening this tab does not
    // present a stale selection box over the town just left.
    sPickX = -1;
    sPickY = -1;
    sConfirm = FALSE;
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

            // Opaque: index 0 never appears in a tile this map references, so
            // there is nothing to see through and nothing to test per pixel.
            UiBlit8bppTile(MAP_PX + tx * 8, MAP_PY + ty * 8,
                           &sTiles[tileId * 64], sPal, FALSE);
        }
    }
}

// The 16x16 icon, positioned the way CreateRegionMapPlayerIcon positions its
// sprite: that sets the sprite's CENTRE to tile*8 + 4, so the top-left corner is
// tile*8 - 4.
static void DrawPlayerIcon(void)
{
    u16 x, y;
    mapsec_u16_t mapSecId;
    u8 posWithinMapSec;
    bool8 inCave;
    int px, py;

    // Birth Island, Faraway Island and Navel Rock are not on the Hoenn map at
    // all, and the game draws no icon there either.
    if (IsEventIslandMapSecId(gMapHeader.regionMapSectionId))
        return;

    Ctr3dsGetRegionMapPlayerPos(&x, &y, &mapSecId, &posWithinMapSec, &inCave);

    px = MAP_PX + (int)x * 8 - 4;
    py = MAP_PY + (int)y * 8 - 4;

    for (int t = 0; t < 4; t++)
        UiBlit4bppTile(px + (t % 2) * 8, py + (t / 2) * 8,
                       sIconGfx + t * 32, sIconPal, TRUE);
}

// Outlines the whole mapsec, not the tile that was tapped: a two-tile city reads
// as one place, and boxing half of it would look like a mis-hit.
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

// The same shape the EXTRA tab's buttons use -- 1px border, accent doubled
// inset on the one that commits -- so a button means the same thing on both
// tabs. Local rather than shared because these two are the only ones outside
// tab_extra.c, and hoisting a widget for a second user is premature.
static void DrawBtn(int x, int w, const char *text, int accent)
{
    u8 label[8];

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

// Right-hand end of the caption row: a FLY button, or the reason there is not
// one, or the confirmation that replaces it.
//
// Nothing here is cached. FlyState is re-read on every repaint so the row keeps
// up with a player who walks indoors, faints their last flyer or starts a
// script while this tab is open, and so the touch handler's own re-check can
// never disagree with what is on screen.
static void DrawFlyControls(mapsec_u16_t mapSecId)
{
    u8 label[24];
    u8 state = FlyState(mapSecId);

    if (state == FLY_NOT_A_DEST)
        return;

    if (state != FLY_READY)
    {
        UiTextRight(CTR_BOTTOM_WIDTH - CAP_MARGIN, CAP_TEXT_Y,
                    UiAscii(label, FlyRefusal(state), sizeof(label)),
                    UI_COL_DIM, UiThemeShadow());
        return;
    }

    if (!sConfirm)
    {
        DrawBtn(FLY_BTN_X, FLY_BTN_W, "FLY", FALSE);
        return;
    }

    // The place name is already on the left of this row, so the prompt does not
    // repeat it: "SLATEPORT CITY ... FLY? NO YES" reads as one sentence and
    // leaves room for the longest name there is.
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
    // Not MAP_NAME_LENGTH (16): the game's own struct RegionMap sizes this field
    // at 20, and GetMapName's empty-name fallback fills 18 plus a terminator.
    u8 name[32];

    UiWindowFrame(0, CAP_TY, CTR_BOTTOM_WIDTH / 8, CAP_TH);

    if (mapSecId >= MAPSEC_NONE)
    {
        u8 label[16];
        UiText(CAP_MARGIN, CAP_TEXT_Y, UiAscii(label, "Sea", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
        return;
    }

    // Already in the game's own encoding, so it goes straight to UiText.
    GetMapNameGeneric(name, mapSecId);
    UiText(CAP_MARGIN, CAP_TEXT_Y, name, UiThemeText(), UiThemeShadow());

    // Landmarks are only for the player's own location: they are keyed on which
    // tile of a multi-tile mapsec you are standing in, and a tapped tile has no
    // such position to offer. A tapped tile has a fly control instead, and the
    // two share this end of the row precisely because they never coexist.
    if (PickIsSet())
    {
        DrawFlyControls(mapSecId);
        return;
    }

    landmark = GetLandmarkName((mapsec_u8_t)mapSecId, posWithinMapSec, 0);
    if (landmark != NULL)
        UiTextRight(CTR_BOTTOM_WIDTH - CAP_MARGIN, CAP_TEXT_Y, landmark,
                    UI_COL_DIM, UiThemeShadow());
}

void UiMapDraw(void)
{
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
// Without this the map would go stale while the player walks: nothing else in
// UiStateHash tracks where they are. It is naturally cheap and naturally coarse
// -- the cursor position only moves when the player crosses a band boundary
// within a mapsec, so walking the length of one stretch of route costs no
// repaints at all.
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

    // The fly row's own inputs, which nothing above tracks. A script ending, a
    // battle starting, the last flyer being deposited in the PC: each changes
    // what that row should say, and none of them move the player.
    //
    // Scattered rather than XORed in raw. The fields above already use most of
    // the word, and two contributions that happen to cancel would show up as a
    // row that stops updating, which is the one failure this key exists to
    // prevent.
    if (PickIsSet())
    {
        u32 fly = (u32)FlyState(GetRegionMapSecIdAt((u16)sPickX, (u16)sPickY))
                | ((u32)(sConfirm != 0) << 3)
                | ((u32)(u8)sPickX << 4)
                | ((u32)(u8)sPickY << 12);

        key ^= fly * 2654435761u;
    }

    return key;
}

// --------------------------------------------------------------- input -----

// The caption band's own controls. Returns TRUE when it consumed the tap.
static bool8 HandleFlyTouch(const CtrTouchState *t)
{
    mapsec_u16_t mapSecId = GetRegionMapSecIdAt((u16)sPickX, (u16)sPickY);

    // Re-checked at the moment of the tap rather than trusted from when the
    // button was drawn. The player can still walk, be handed a script or be
    // pulled into a battle with this screen open, and a confirm raised before
    // any of that must not survive it. DrawFlyControls hides the row in exactly
    // the same case, so falling through here is also what guarantees no tap
    // ever lands on a button that is not on the screen.
    if (FlyState(mapSecId) != FLY_READY)
    {
        sConfirm = FALSE;
        return FALSE;
    }

    if (!sConfirm)
    {
        if (!UiHit(t, FLY_BTN_X, FLY_BTN_Y, FLY_BTN_W, FLY_BTN_H))
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

void UiMapTouch(const CtrTouchState *t)
{
    int tx, ty;

    if (!t->justReleased)
        return;

    // Tested before the deselect below, which would otherwise claim every tap
    // outside the map -- the fly controls among them, since they sit in the
    // caption band. Anything in that band it does not want falls through and
    // deselects, so tapping away from a raised confirm cancels it.
    if (PickIsSet() && t->y >= CAP_Y && HandleFlyTouch(t))
        return;

    // Anywhere off the map, the caption band included, drops the selection and
    // goes back to reporting where the player actually is.
    if (t->y < MAP_PY || t->y >= MAP_PY + MAP_TH * 8
     || t->x < MAP_PX || t->x >= MAP_PX + MAP_TW * 8)
    {
        if (PickIsSet())
        {
            sPickX = -1;
            sPickY = -1;
            sConfirm = FALSE;
            UiMarkDirty();
        }
        return;
    }

    tx = (t->x - MAP_PX) / 8;
    ty = (t->y - MAP_PY) / 8;

    // Open sea has no mapsec. Treat it as a deselect rather than ignoring it, so
    // there is always an obvious way back to following the player.
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

    // A fresh selection is never pre-confirmed. Without this, tapping from one
    // town to another would carry the first one's raised YES across, and the
    // next tap would fly to somewhere the player never confirmed.
    sConfirm = FALSE;
    UiMarkDirty();
}
