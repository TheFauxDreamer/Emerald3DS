// GBA memory regions for the 3DS port.
//
// The game writes EWRAM/IWRAM/VRAM/palette/OAM/IO at fixed addresses. On real
// GBA those are hardware; on RP2350 they were carved out of SRAM by the linker
// script. The 3DS cannot hand out arbitrary virtual addresses, so the regions
// are one contiguous heap block instead and gba/defines.h derives every base
// from gGbaMem at runtime.
//
// The block preserves RP2350's exact region ORDER and spacing, and that is
// load-bearing, not cosmetic: IsTileMapOutsideWram() (src/bg.c) decides whether
// a tilemap pointer lives in VRAM by testing `ptr > IWRAM_END`. Keeping
// EWRAM < IWRAM < VRAM contiguous makes that test answer exactly as it does on
// the RP2350 port, which is known-good.
//
// The block is a static array rather than a heap allocation so that every
// region base is a link-time constant, which the static initialisers in
// src/field_screen_effect.c and src/pokenav_menu_handler_gfx.c require.
// The cost is that .bss ordering against the game's own EWRAM_DATA variables is
// link-order dependent -- see the PLATFORM_3DS branch in IsTileMapOutsideWram
// (src/bg.c), the one place that ever compared these addresses.

#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "gba/io_reg.h"

// The contiguous GBA region block. 32-byte aligned: the regions are memcpy/DMA
// targets and the ARM11 has 32-byte cache lines, so this keeps region starts
// off shared lines.
ALIGNED(32) u8 gGbaMem[CTR_GBA_MEM_SIZE];

// The GBA cart's 128 KB save flash, backing FLASH_BASE (gba/flash_internal.h).
// Erased flash reads as 0xFF; the save layer relies on that for a blank cart.
u8 gCtrSaveFlash[CTR_SAVE_FLASH_SIZE];

void Ctr3dsInitGbaMemory(void)
{
    // .bss is already zero, but the GBA powers on with these regions cleared
    // and saying so explicitly makes a re-entry (or a future soft reset) safe.
    memset(gGbaMem, 0, sizeof(gGbaMem));

    // Blank cart = erased flash. The save layer's checksums then decide there
    // is no save and offer a new game.
    memset(gCtrSaveFlash, 0xFF, sizeof(gCtrSaveFlash));

    // A GBA powers on with every key RELEASED, and KEYINPUT is active-low: a
    // CLEAR bit means pressed. The memset above leaves it 0, i.e. all ten
    // buttons held -- and CtrSetKeyInput() cannot correct that in time,
    // because it only runs from Rp2350PresentFrame() at the END of a frame,
    // while ReadKeys() samples at the START of one.
    //
    // The first frame therefore read A+B+START+SELECT, the soft-reset combo,
    // and src/main.c dived into rfu_REQ_stopMode() -> AgbRFU_SoftReset(),
    // which dereferences gSTWIStatus. That is NULL here because InitRFU() is
    // GBA-only (it sits in the #else branch RP2350 skips), so the port crashed
    // on boot reading null+0xA, the offset of STWIStatus::timerSelect.
    *(vu16 *)(REG_BASE + REG_OFFSET_KEYINPUT) = KEYS_MASK;
}

// Hand the PPU its four region bases. rp2350/ppu.c is host-side and
// address-agnostic by design (see ppu.h), which is what lets the same
// byte-exact rasteriser run against a heap block here and against SRAM on
// RP2350.
void CtrGetGbaRegions(const void **reg, const void **pal,
                      const void **vram, const void **oam)
{
    *reg  = (const void *)REG_BASE;
    *pal  = (const void *)PLTT;
    *vram = (const void *)VRAM;
    *oam  = (const void *)OAM;
}

// REG_KEYINPUT lives in the register backing store; ReadKeys() (src/main.c)
// reads it at the top of the next frame.
void CtrSetKeyInput(u16 keysActiveLow)
{
    *(vu16 *)(REG_BASE + REG_OFFSET_KEYINPUT) = keysActiveLow;
}
