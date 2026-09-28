#include "global.h"
#include "berry.h"
#include "bike.h"
#include "field_camera.h"
#include "field_player_avatar.h"
#include "fieldmap.h"
#include "event_object_movement.h"
#include "gpu_regs.h"
#include "menu.h"
#include "overworld.h"
#include "rotating_gate.h"
#include "sprite.h"
#include "text.h"
#if PLATFORM_3DS
#include "main.h"
#include "battle_transition.h"
#include "../3ds/bridge.h"
#endif

EWRAM_DATA bool8 gUnusedBikeCameraAheadPanback = FALSE;

struct FieldCameraOffset
{
    u8 xPixelOffset;
    u8 yPixelOffset;
    u8 xTileOffset;
    u8 yTileOffset;
    bool8 copyBGToVRAM;
};

static void RedrawMapSliceNorth(struct FieldCameraOffset *, const struct MapLayout *);
static void RedrawMapSliceSouth(struct FieldCameraOffset *, const struct MapLayout *);
static void RedrawMapSliceEast(struct FieldCameraOffset *, const struct MapLayout *);
static void RedrawMapSliceWest(struct FieldCameraOffset *, const struct MapLayout *);
static s32 MapPosToBgTilemapOffset(struct FieldCameraOffset *, s32, s32);
static void DrawWholeMapViewInternal(int, int, const struct MapLayout *);
static void DrawMetatileAt(const struct MapLayout *, u16, int, int);
static void DrawMetatile(s32, const u16 *, u16);
static void CameraPanningCB_PanAhead(void);
#if PLATFORM_3DS
static void CtrWideReset(void);
static void CtrWideAddTileX(u32 xOffset);
static void CtrWideAddPixelX(u32 xOffset);
static void CtrWideDrawMetatile(s32 metatileLayerType, const u16 *tiles, int x, int y);
static bool8 CtrWideDrawMapMetatileAt(const struct MapLayout *mapLayout, int x, int y);
static void CtrWideDrawColumn(const struct MapLayout *mapLayout, int dx);
static void CtrWideDrawRow(const struct MapLayout *mapLayout, int dy);
#endif

static struct FieldCameraOffset sFieldCameraOffset;
static s16 sHorizontalCameraPan;
static s16 sVerticalCameraPan;
static bool8 sBikeCameraPanFlag;
static void (*sFieldCameraPanningCallback)(void);

COMMON_DATA struct CameraObject gFieldCamera = {0};
COMMON_DATA u16 gTotalCameraPixelOffsetY = 0;
COMMON_DATA u16 gTotalCameraPixelOffsetX = 0;

static void ResetCameraOffset(struct FieldCameraOffset *cameraOffset)
{
    cameraOffset->xTileOffset = 0;
    cameraOffset->yTileOffset = 0;
    cameraOffset->xPixelOffset = 0;
    cameraOffset->yPixelOffset = 0;
    cameraOffset->copyBGToVRAM = TRUE;
#if PLATFORM_3DS
    CtrWideReset();
#endif
}

static void AddCameraTileOffset(struct FieldCameraOffset *cameraOffset, u32 xOffset, u32 yOffset)
{
    cameraOffset->xTileOffset += xOffset;
    cameraOffset->xTileOffset %= 32;
    cameraOffset->yTileOffset += yOffset;
    cameraOffset->yTileOffset %= 32;
#if PLATFORM_3DS
    CtrWideAddTileX(xOffset);
#endif
}

static void AddCameraPixelOffset(struct FieldCameraOffset *cameraOffset, u32 xOffset, u32 yOffset)
{
    cameraOffset->xPixelOffset += xOffset;
    cameraOffset->yPixelOffset += yOffset;
#if PLATFORM_3DS
    CtrWideAddPixelX(xOffset);
#endif
}

void ResetFieldCamera(void)
{
    ResetCameraOffset(&sFieldCameraOffset);
}

void FieldUpdateBgTilemapScroll(void)
{
    u32 r4, r5;
    r5 = sFieldCameraOffset.xPixelOffset + sHorizontalCameraPan;
    r4 = sVerticalCameraPan + sFieldCameraOffset.yPixelOffset + 8;

    SetGpuReg(REG_OFFSET_BG1HOFS, r5);
    SetGpuReg(REG_OFFSET_BG1VOFS, r4);
    SetGpuReg(REG_OFFSET_BG2HOFS, r5);
    SetGpuReg(REG_OFFSET_BG2VOFS, r4);
    SetGpuReg(REG_OFFSET_BG3HOFS, r5);
    SetGpuReg(REG_OFFSET_BG3VOFS, r4);
}

void GetCameraOffsetWithPan(s16 *x, s16 *y)
{
    *x = sFieldCameraOffset.xPixelOffset + sHorizontalCameraPan;
    *y = sFieldCameraOffset.yPixelOffset + sVerticalCameraPan + 8;
}

void DrawWholeMapView(void)
{
    DrawWholeMapViewInternal(gSaveBlock1Ptr->pos.x, gSaveBlock1Ptr->pos.y, gMapHeader.mapLayout);
#if PLATFORM_3DS
    // The metatiles of the wide view that the 16x16 above does not hold.
    CtrWideDrawColumn(gMapHeader.mapLayout, -2);
    CtrWideDrawColumn(gMapHeader.mapLayout, -1);
    CtrWideDrawColumn(gMapHeader.mapLayout, 16);
#endif
    sFieldCameraOffset.copyBGToVRAM = TRUE;
}

static void DrawWholeMapViewInternal(int x, int y, const struct MapLayout *mapLayout)
{
    u8 i;
    u8 j;
    u32 r6;
    u8 temp;

    for (i = 0; i < 32; i += 2)
    {
        temp = sFieldCameraOffset.yTileOffset + i;
        if (temp >= 32)
            temp -= 32;
        r6 = temp * 32;
        for (j = 0; j < 32; j += 2)
        {
            temp = sFieldCameraOffset.xTileOffset + j;
            if (temp >= 32)
                temp -= 32;
            DrawMetatileAt(mapLayout, r6 + temp, x + j / 2, y + i / 2);
        }
    }
}

static void RedrawMapSlicesForCameraUpdate(struct FieldCameraOffset *cameraOffset, int x, int y)
{
    const struct MapLayout *mapLayout = gMapHeader.mapLayout;

    if (x > 0)
        RedrawMapSliceWest(cameraOffset, mapLayout);
    if (x < 0)
        RedrawMapSliceEast(cameraOffset, mapLayout);
    if (y > 0)
        RedrawMapSliceNorth(cameraOffset, mapLayout);
    if (y < 0)
        RedrawMapSliceSouth(cameraOffset, mapLayout);
#if PLATFORM_3DS
    // The wide view's new edges. A step east (x > 0) brings in the metatile
    // column 16 to the right of pos.x, a step west the column 2 to the left.
    // A new row gets its margin metatiles here; the slices above drew the rest
    // of it.
    if (x > 0)
        CtrWideDrawColumn(mapLayout, 16);
    if (x < 0)
        CtrWideDrawColumn(mapLayout, -2);
    if (y > 0)
        CtrWideDrawRow(mapLayout, 14);
    if (y < 0)
        CtrWideDrawRow(mapLayout, 0);
#endif
    cameraOffset->copyBGToVRAM = TRUE;
}

static void RedrawMapSliceNorth(struct FieldCameraOffset *cameraOffset, const struct MapLayout *mapLayout)
{
    u8 i;
    u8 temp;
    u32 r7;

    temp = cameraOffset->yTileOffset + 28;
    if (temp >= 32)
        temp -= 32;
    r7 = temp * 32;
    for (i = 0; i < 32; i += 2)
    {
        temp = cameraOffset->xTileOffset + i;
        if (temp >= 32)
            temp -= 32;
        DrawMetatileAt(mapLayout, r7 + temp, gSaveBlock1Ptr->pos.x + i / 2, gSaveBlock1Ptr->pos.y + 14);
    }
}

static void RedrawMapSliceSouth(struct FieldCameraOffset *cameraOffset, const struct MapLayout *mapLayout)
{
    u8 i;
    u8 temp;
    u32 r7 = cameraOffset->yTileOffset * 32;

    for (i = 0; i < 32; i += 2)
    {
        temp = cameraOffset->xTileOffset + i;
        if (temp >= 32)
            temp -= 32;
        DrawMetatileAt(mapLayout, r7 + temp, gSaveBlock1Ptr->pos.x + i / 2, gSaveBlock1Ptr->pos.y);
    }
}

static void RedrawMapSliceEast(struct FieldCameraOffset *cameraOffset, const struct MapLayout *mapLayout)
{
    u8 i;
    u8 temp;
    u32 r6 = cameraOffset->xTileOffset;

    for (i = 0; i < 32; i += 2)
    {
        temp = cameraOffset->yTileOffset + i;
        if (temp >= 32)
            temp -= 32;
        DrawMetatileAt(mapLayout, temp * 32 + r6, gSaveBlock1Ptr->pos.x, gSaveBlock1Ptr->pos.y + i / 2);
    }
}

static void RedrawMapSliceWest(struct FieldCameraOffset *cameraOffset, const struct MapLayout *mapLayout)
{
    u8 i;
    u8 temp;
    u8 r5 = cameraOffset->xTileOffset + 28;

    if (r5 >= 32)
        r5 -= 32;
    for (i = 0; i < 32; i += 2)
    {
        temp = cameraOffset->yTileOffset + i;
        if (temp >= 32)
            temp -= 32;
        DrawMetatileAt(mapLayout, temp * 32 + r5, gSaveBlock1Ptr->pos.x + 14, gSaveBlock1Ptr->pos.y + i / 2);
    }
}

void CurrentMapDrawMetatileAt(int x, int y)
{
    int offset = MapPosToBgTilemapOffset(&sFieldCameraOffset, x, y);

    if (offset >= 0)
    {
        DrawMetatileAt(gMapHeader.mapLayout, offset, x, y);
        sFieldCameraOffset.copyBGToVRAM = TRUE;
    }
#if PLATFORM_3DS
    else
    {
        CtrWideDrawMapMetatileAt(gMapHeader.mapLayout, x, y);
    }
#endif
}

void DrawDoorMetatileAt(int x, int y, u16 *tiles)
{
    int offset = MapPosToBgTilemapOffset(&sFieldCameraOffset, x, y);

#if PLATFORM_3DS
    CtrWideDrawMetatile(METATILE_LAYER_TYPE_COVERED, tiles, x, y);
#endif
    if (offset >= 0)
    {
        DrawMetatile(METATILE_LAYER_TYPE_COVERED, tiles, offset);
        sFieldCameraOffset.copyBGToVRAM = TRUE;
    }
}

static void DrawMetatileAt(const struct MapLayout *mapLayout, u16 offset, int x, int y)
{
    u16 metatileId = MapGridGetMetatileIdAt(x, y);
    const u16 *metatiles;

    if (metatileId > NUM_METATILES_TOTAL)
        metatileId = 0;
    if (metatileId < NUM_METATILES_IN_PRIMARY)
    {
        metatiles = mapLayout->primaryTileset->metatiles;
    }
    else
    {
        metatiles = mapLayout->secondaryTileset->metatiles;
        metatileId -= NUM_METATILES_IN_PRIMARY;
    }
    DrawMetatile(MapGridGetMetatileLayerTypeAt(x, y), metatiles + metatileId * NUM_TILES_PER_METATILE, offset);
#if PLATFORM_3DS
    CtrWideDrawMetatile(MapGridGetMetatileLayerTypeAt(x, y), metatiles + metatileId * NUM_TILES_PER_METATILE, x, y);
#endif
}

static void DrawMetatile(s32 metatileLayerType, const u16 *tiles, u16 offset)
{
    switch (metatileLayerType)
    {
    case METATILE_LAYER_TYPE_SPLIT:
        // Draw metatile's bottom layer to the bottom background layer.
        gOverworldTilemapBuffer_Bg3[offset] = tiles[0];
        gOverworldTilemapBuffer_Bg3[offset + 1] = tiles[1];
        gOverworldTilemapBuffer_Bg3[offset + 0x20] = tiles[2];
        gOverworldTilemapBuffer_Bg3[offset + 0x21] = tiles[3];

        // Draw transparent tiles to the middle background layer.
        gOverworldTilemapBuffer_Bg2[offset] = 0;
        gOverworldTilemapBuffer_Bg2[offset + 1] = 0;
        gOverworldTilemapBuffer_Bg2[offset + 0x20] = 0;
        gOverworldTilemapBuffer_Bg2[offset + 0x21] = 0;

        // Draw metatile's top layer to the top background layer.
        gOverworldTilemapBuffer_Bg1[offset] = tiles[4];
        gOverworldTilemapBuffer_Bg1[offset + 1] = tiles[5];
        gOverworldTilemapBuffer_Bg1[offset + 0x20] = tiles[6];
        gOverworldTilemapBuffer_Bg1[offset + 0x21] = tiles[7];
        break;
    case METATILE_LAYER_TYPE_COVERED:
        // Draw metatile's bottom layer to the bottom background layer.
        gOverworldTilemapBuffer_Bg3[offset] = tiles[0];
        gOverworldTilemapBuffer_Bg3[offset + 1] = tiles[1];
        gOverworldTilemapBuffer_Bg3[offset + 0x20] = tiles[2];
        gOverworldTilemapBuffer_Bg3[offset + 0x21] = tiles[3];

        // Draw metatile's top layer to the middle background layer.
        gOverworldTilemapBuffer_Bg2[offset] = tiles[4];
        gOverworldTilemapBuffer_Bg2[offset + 1] = tiles[5];
        gOverworldTilemapBuffer_Bg2[offset + 0x20] = tiles[6];
        gOverworldTilemapBuffer_Bg2[offset + 0x21] = tiles[7];

        // Draw transparent tiles to the top background layer.
        gOverworldTilemapBuffer_Bg1[offset] = 0;
        gOverworldTilemapBuffer_Bg1[offset + 1] = 0;
        gOverworldTilemapBuffer_Bg1[offset + 0x20] = 0;
        gOverworldTilemapBuffer_Bg1[offset + 0x21] = 0;
        break;
    case METATILE_LAYER_TYPE_NORMAL:
        // Draw garbage to the bottom background layer.
        gOverworldTilemapBuffer_Bg3[offset] = 0x3014;
        gOverworldTilemapBuffer_Bg3[offset + 1] = 0x3014;
        gOverworldTilemapBuffer_Bg3[offset + 0x20] = 0x3014;
        gOverworldTilemapBuffer_Bg3[offset + 0x21] = 0x3014;

        // Draw metatile's bottom layer to the middle background layer.
        gOverworldTilemapBuffer_Bg2[offset] = tiles[0];
        gOverworldTilemapBuffer_Bg2[offset + 1] = tiles[1];
        gOverworldTilemapBuffer_Bg2[offset + 0x20] = tiles[2];
        gOverworldTilemapBuffer_Bg2[offset + 0x21] = tiles[3];

        // Draw metatile's top layer to the top background layer, which covers object event sprites.
        gOverworldTilemapBuffer_Bg1[offset] = tiles[4];
        gOverworldTilemapBuffer_Bg1[offset + 1] = tiles[5];
        gOverworldTilemapBuffer_Bg1[offset + 0x20] = tiles[6];
        gOverworldTilemapBuffer_Bg1[offset + 0x21] = tiles[7];
        break;
    }
    ScheduleBgCopyTilemapToVram(1);
    ScheduleBgCopyTilemapToVram(2);
    ScheduleBgCopyTilemapToVram(3);
}

static s32 MapPosToBgTilemapOffset(struct FieldCameraOffset *cameraOffset, s32 x, s32 y)
{
    x -= gSaveBlock1Ptr->pos.x;
    x *= 2;
    if (x >= 32 || x < 0)
        return -1;
    x = x + cameraOffset->xTileOffset;
    if (x >= 32)
        x -= 32;

    y = (y - gSaveBlock1Ptr->pos.y) * 2;
    if (y >= 32 || y < 0)
        return -1;
    y = y + cameraOffset->yTileOffset;
    if (y >= 32)
        y -= 32;

    return y * 32 + x;
}

static void CameraUpdateCallback(struct CameraObject *fieldCamera)
{
    if (fieldCamera->spriteId != 0)
    {
        fieldCamera->movementSpeedX = gSprites[fieldCamera->spriteId].sCamera_MoveX;
        fieldCamera->movementSpeedY = gSprites[fieldCamera->spriteId].sCamera_MoveY;
    }
}

void ResetCameraUpdateInfo(void)
{
    gFieldCamera.movementSpeedX = 0;
    gFieldCamera.movementSpeedY = 0;
    gFieldCamera.x = 0;
    gFieldCamera.y = 0;
    gFieldCamera.spriteId = 0;
    gFieldCamera.callback = NULL;
}

u32 InitCameraUpdateCallback(u8 trackedSpriteId)
{
    if (gFieldCamera.spriteId != 0)
        DestroySprite(&gSprites[gFieldCamera.spriteId]);
    gFieldCamera.spriteId = AddCameraObject(trackedSpriteId);
    gFieldCamera.callback = CameraUpdateCallback;
    return 0;
}

void CameraUpdate(void)
{
    int deltaX;
    int deltaY;
    int curMovementOffsetY;
    int curMovementOffsetX;
    int movementSpeedX;
    int movementSpeedY;

    if (gFieldCamera.callback != NULL)
        gFieldCamera.callback(&gFieldCamera);
    movementSpeedX = gFieldCamera.movementSpeedX;
    movementSpeedY = gFieldCamera.movementSpeedY;
    deltaX = 0;
    deltaY = 0;
    curMovementOffsetX = gFieldCamera.x;
    curMovementOffsetY = gFieldCamera.y;


    if (curMovementOffsetX == 0 && movementSpeedX != 0)
    {
        if (movementSpeedX > 0)
            deltaX = 1;
        else
            deltaX = -1;
    }
    if (curMovementOffsetY == 0 && movementSpeedY != 0)
    {
        if (movementSpeedY > 0)
            deltaY = 1;
        else
            deltaY = -1;
    }
    if (curMovementOffsetX != 0 && curMovementOffsetX == -movementSpeedX)
    {
        if (movementSpeedX > 0)
            deltaX = 1;
        else
            deltaX = -1;
    }
    if (curMovementOffsetY != 0 && curMovementOffsetY == -movementSpeedY)
    {
        if (movementSpeedY > 0)
            deltaX = 1;
        else
            deltaX = -1;
    }

    gFieldCamera.x += movementSpeedX;
    gFieldCamera.x %= 16;
    gFieldCamera.y += movementSpeedY;
    gFieldCamera.y %= 16;

    if (deltaX != 0 || deltaY != 0)
    {
        CameraMove(deltaX, deltaY);
        UpdateObjectEventsForCameraUpdate(deltaX, deltaY);
        RotatingGatePuzzleCameraUpdate(deltaX, deltaY);
        SetBerryTreesSeen();
        AddCameraTileOffset(&sFieldCameraOffset, deltaX * 2, deltaY * 2);
        RedrawMapSlicesForCameraUpdate(&sFieldCameraOffset, deltaX * 2, deltaY * 2);
    }

    AddCameraPixelOffset(&sFieldCameraOffset, movementSpeedX, movementSpeedY);
    gTotalCameraPixelOffsetX -= movementSpeedX;
    gTotalCameraPixelOffsetY -= movementSpeedY;
}

void MoveCameraAndRedrawMap(int deltaX, int deltaY) //unused
{
    CameraMove(deltaX, deltaY);
    UpdateObjectEventsForCameraUpdate(deltaX, deltaY);
    DrawWholeMapView();
    gTotalCameraPixelOffsetX -= deltaX * 16;
    gTotalCameraPixelOffsetY -= deltaY * 16;
}

void SetCameraPanningCallback(void (*callback)(void))
{
    sFieldCameraPanningCallback = callback;
}

void SetCameraPanning(s16 horizontal, s16 vertical)
{
    sHorizontalCameraPan = horizontal;
    sVerticalCameraPan = vertical + 32;
}

void InstallCameraPanAheadCallback(void)
{
    sFieldCameraPanningCallback = CameraPanningCB_PanAhead;
    sBikeCameraPanFlag = FALSE;
    sHorizontalCameraPan = 0;
    sVerticalCameraPan = 32;
}

void UpdateCameraPanning(void)
{
    if (sFieldCameraPanningCallback != NULL)
        sFieldCameraPanningCallback();
    //Update sprite offset of overworld objects
    gSpriteCoordOffsetX = gTotalCameraPixelOffsetX - sHorizontalCameraPan;
    gSpriteCoordOffsetY = gTotalCameraPixelOffsetY - sVerticalCameraPan - 8;
}

static void CameraPanningCB_PanAhead(void)
{
    u8 var;

    if (gUnusedBikeCameraAheadPanback == FALSE)
    {
        InstallCameraPanAheadCallback();
    }
    else
    {
        // this code is never reached
        if (gPlayerAvatar.tileTransitionState == T_TILE_TRANSITION)
        {
            sBikeCameraPanFlag ^= 1;
            if (sBikeCameraPanFlag == FALSE)
                return;
        }
        else
        {
            sBikeCameraPanFlag = FALSE;
        }

        var = GetPlayerMovementDirection();
        if (var == 2)
        {
            if (sVerticalCameraPan > -8)
                sVerticalCameraPan -= 2;
        }
        else if (var == 1)
        {
            if (sVerticalCameraPan < 72)
                sVerticalCameraPan += 2;
        }
        else if (sVerticalCameraPan < 32)
        {
            sVerticalCameraPan += 2;
        }
        else if (sVerticalCameraPan > 32)
        {
            sVerticalCameraPan -= 2;
        }
    }
}

#if PLATFORM_3DS
// ---- the wide overworld (3DS) ----------------------------------------------
//
// The WIDE top-screen scale shows 16 more pixels on each side of the field
// (3ds/bridge.h, CTR_WIDE_MARGIN). The 32x32 tile rings of BG1-3 above hold 16
// metatiles across, and the view then needs 19: from 2 left of pos.x to 16
// right of it. Thus each ring has a copy here that is 64 tiles across, the
// "side map". The PPU reads it only for the margins (ppu_set_wide in
// rp2350/ppu.h). The 240 pixels in the middle still come from VRAM, so nothing
// the GBA shows changes.
//
// The side map keeps the ring's rows, and its columns follow the same rule
// with a period of 64: the column of a metatile is sCtrWideTileX + 2 * (x -
// pos.x), where sCtrWideTileX is xTileOffset counted mod 64. Every metatile
// that the code above draws is also drawn here. The margin metatiles are drawn
// here only.

#define CTR_WIDE_COLS   64
#define CTR_WIDE_LEFT   (-2)    // metatiles, relative to pos.x
#define CTR_WIDE_RIGHT  16

static u16 sCtrWideMap[3][32 * CTR_WIDE_COLS];   // BG1, BG2, BG3
static u8  sCtrWideTileX;    // xTileOffset, mod 64
static u16 sCtrWidePixelX;   // xPixelOffset, mod 512

static void CtrWideReset(void)
{
    sCtrWideTileX = 0;
    sCtrWidePixelX = 0;
}

// The offsets are u32 because the callers pass a negative step as u32. The
// masks keep the result right, as 2^32 is a multiple of 64 and of 512.
static void CtrWideAddTileX(u32 xOffset)
{
    sCtrWideTileX = (sCtrWideTileX + xOffset) & (CTR_WIDE_COLS - 1);
}

static void CtrWideAddPixelX(u32 xOffset)
{
    sCtrWidePixelX = (sCtrWidePixelX + xOffset) & 0x1FF;
}

// The side-map index of the metatile at map position (x, y), or -1 when the
// wide view does not hold it.
static s32 CtrWideOffset(int x, int y)
{
    int dx = x - gSaveBlock1Ptr->pos.x;
    int dy = y - gSaveBlock1Ptr->pos.y;
    int col, row;

    if (dx < CTR_WIDE_LEFT || dx > CTR_WIDE_RIGHT || dy < 0 || dy > 15)
        return -1;

    col = (sCtrWideTileX + dx * 2) & (CTR_WIDE_COLS - 1);
    row = (sFieldCameraOffset.yTileOffset + dy * 2) & 31;
    return row * CTR_WIDE_COLS + col;
}

// The same layer rules as DrawMetatile, into the side map.
static void CtrWideDrawMetatile(s32 metatileLayerType, const u16 *tiles, int x, int y)
{
    static const u16 sBlank[4] = {0};
    static const u16 sGarbage[4] = {0x3014, 0x3014, 0x3014, 0x3014};
    const u16 *bg1, *bg2, *bg3;
    s32 offset = CtrWideOffset(x, y);
    int i;

    if (offset < 0)
        return;

    switch (metatileLayerType)
    {
    case METATILE_LAYER_TYPE_SPLIT:
        bg3 = tiles;
        bg2 = sBlank;
        bg1 = tiles + 4;
        break;
    case METATILE_LAYER_TYPE_COVERED:
        bg3 = tiles;
        bg2 = tiles + 4;
        bg1 = sBlank;
        break;
    case METATILE_LAYER_TYPE_NORMAL:
        bg3 = sGarbage;
        bg2 = tiles;
        bg1 = tiles + 4;
        break;
    default:
        return;
    }

    for (i = 0; i < 4; i++)
    {
        s32 at = offset + (i & 1) + (i >> 1) * CTR_WIDE_COLS;

        sCtrWideMap[0][at] = bg1[i];
        sCtrWideMap[1][at] = bg2[i];
        sCtrWideMap[2][at] = bg3[i];
    }
}

// DrawMetatileAt for the side map only.
static bool8 CtrWideDrawMapMetatileAt(const struct MapLayout *mapLayout, int x, int y)
{
    u16 metatileId;
    const u16 *metatiles;

    if (CtrWideOffset(x, y) < 0)
        return FALSE;

    metatileId = MapGridGetMetatileIdAt(x, y);
    if (metatileId > NUM_METATILES_TOTAL)
        metatileId = 0;
    if (metatileId < NUM_METATILES_IN_PRIMARY)
    {
        metatiles = mapLayout->primaryTileset->metatiles;
    }
    else
    {
        metatiles = mapLayout->secondaryTileset->metatiles;
        metatileId -= NUM_METATILES_IN_PRIMARY;
    }
    CtrWideDrawMetatile(MapGridGetMetatileLayerTypeAt(x, y),
                        metatiles + metatileId * NUM_TILES_PER_METATILE, x, y);
    return TRUE;
}

// One metatile column, dx metatiles right of pos.x, over the 16 rows.
static void CtrWideDrawColumn(const struct MapLayout *mapLayout, int dx)
{
    int dy;

    for (dy = 0; dy < 16; dy++)
        CtrWideDrawMapMetatileAt(mapLayout, gSaveBlock1Ptr->pos.x + dx,
                                 gSaveBlock1Ptr->pos.y + dy);
}

// The margin metatiles of one row, dy metatiles below pos.y. The GBA ring's
// slice drew the others, and those reached the side map through DrawMetatileAt.
static void CtrWideDrawRow(const struct MapLayout *mapLayout, int dy)
{
    int dx;

    for (dx = CTR_WIDE_LEFT; dx <= CTR_WIDE_RIGHT; dx++)
    {
        if (dx == 0)
            dx = 16;   // 0 to 15 are the GBA ring's
        CtrWideDrawMapMetatileAt(mapLayout, gSaveBlock1Ptr->pos.x + dx,
                                 gSaveBlock1Ptr->pos.y + dy);
    }
}

// Is this frame the field with the layout the side maps belong to? Menus and
// cut scenes over the field keep it, because they draw on BG0 or on sprites.
// A screen that takes over BG1-3 changes their map bases, and a battle
// transition stays 240 wide, because it covers only the GBA screen.
static bool8 CtrWideFieldShows(void)
{
    static const u8 sMapBase[4] = {0, 29, 28, 30};   // sOverworldBgTemplates
    int bg;

    if (gMain.callback2 != CB2_Overworld && gMain.callback2 != CB2_OverworldBasic)
        return FALSE;
    if (CtrBattleTransitionActive())
        return FALSE;
    if ((GetGpuReg(REG_OFFSET_DISPCNT) & 7) != DISPCNT_MODE_0)
        return FALSE;

    for (bg = 1; bg <= 3; bg++)
    {
        u16 cnt = GetGpuReg(REG_OFFSET_BG0CNT + bg * 2);

        // 256x256, and the map base of the overworld template.
        if ((cnt >> 14) != 0 || ((cnt >> 8) & 31) != sMapBase[bg])
            return FALSE;
    }
    return TRUE;
}

void CtrWideFieldGet(CtrWideField *out)
{
    int bg;

    out->active = Ctr3dsGetTopScale() == CTR_TOP_SCALE_WIDE && CtrWideFieldShows();
    out->sideMap[0] = NULL;
    for (bg = 1; bg <= 3; bg++)
        out->sideMap[bg] = sCtrWideMap[bg - 1];
    // A multiple of 256: the two counters move together, and the u8 one wraps
    // at 256. See ppu_set_wide.
    out->sideDelta = (u16)((sCtrWidePixelX - sFieldCameraOffset.xPixelOffset) & 0x1FF);
}
#endif
