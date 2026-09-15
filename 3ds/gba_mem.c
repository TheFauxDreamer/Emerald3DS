// The GBA memory regions for the 3DS port.
//
// The game writes EWRAM, IWRAM, VRAM, palette, OAM and IO at fixed addresses.
// The 3DS cannot give arbitrary virtual addresses, so the regions are one
// contiguous block, and gba/defines.h calculates every base from gGbaMem at run
// time.
//
// The block keeps the RP2350 port's region order and spacing. This is
// necessary: IsTileMapOutsideWram() (src/bg.c) tests `ptr > IWRAM_END` to find
// if a tilemap pointer is in VRAM. With EWRAM < IWRAM < VRAM contiguous, that
// test gives the same answer as on the RP2350 port.
//
// The block is a static array, not a heap allocation, so every region base is a
// link-time constant. The static initializers in src/field_screen_effect.c and
// src/pokenav_menu_handler_gfx.c need that. The .bss order against the game's
// own EWRAM_DATA variables depends on the link order. See the PLATFORM_3DS
// branch in IsTileMapOutsideWram (src/bg.c), the one place that compared these
// addresses.

#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "gba/io_reg.h"

// The contiguous GBA region block. It is 32-byte aligned: the regions are
// memcpy and DMA targets, and the ARM11 has 32-byte cache lines. Thus no region
// start shares a line.
ALIGNED(32) u8 gGbaMem[CTR_GBA_MEM_SIZE];

// The cartridge's 128 KB save flash, behind FLASH_BASE (gba/flash_internal.h).
// Erased flash reads as 0xFF, and the save layer uses that for a blank cart.
u8 gCtrSaveFlash[CTR_SAVE_FLASH_SIZE];

void Ctr3dsInitGbaMemory(void)
{
    // The .bss section is already zero, but a GBA starts with these regions
    // cleared. The explicit clear makes a second entry (or a future soft reset)
    // safe.
    memset(gGbaMem, 0, sizeof(gGbaMem));

    // A blank cart is erased flash. The save layer's checksums then find no
    // save, and the game offers a new game.
    memset(gCtrSaveFlash, 0xFF, sizeof(gCtrSaveFlash));

    // A GBA starts with every key released, and KEYINPUT is active low: a clear
    // bit means pressed. The memset above leaves it at 0, which means all ten
    // buttons are held. CtrSetKeyInput() cannot correct that in time, because
    // it runs at the end of a frame and ReadKeys() reads at the start of one.
    //
    // Without this, the first frame read A+B+START+SELECT, the soft-reset
    // combo. Then src/main.c called rfu_REQ_stopMode() and AgbRFU_SoftReset(),
    // which reads gSTWIStatus. That pointer is NULL here, because InitRFU() is
    // GBA only. The port crashed on boot at null+0xA.
    *(vu16 *)(REG_BASE + REG_OFFSET_KEYINPUT) = KEYS_MASK;
}

// Give the PPU its four region bases. The PPU (rp2350/ppu.c) is host side and
// does not depend on addresses (see ppu.h). Thus the same rasterizer runs on a
// static block here and on SRAM on the RP2350.
void CtrGetGbaRegions(const void **reg, const void **pal,
                      const void **vram, const void **oam)
{
    *reg  = (const void *)REG_BASE;
    *pal  = (const void *)PLTT;
    *vram = (const void *)VRAM;
    *oam  = (const void *)OAM;
}

// REG_KEYINPUT is in the register store. ReadKeys() (src/main.c) reads it at
// the start of the next frame.
void CtrSetKeyInput(u16 keysActiveLow)
{
    *(vu16 *)(REG_BASE + REG_OFFSET_KEYINPUT) = keysActiveLow;
}
