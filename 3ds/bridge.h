// The seam between the two worlds of this port.
//
// include/gba/types.h (game) and <3ds.h> (libctru) both typedef u8/u16/u32 to
// different-but-compatible things, and the game's include/ shadows libc headers
// (string.h, strings.h). A translation unit that includes both will not build.
// So:
//
//   game-side TUs : src/**, rp2350/{bios,asm_stubs,m4a_1}.c, 3ds/gba_mem.c,
//                   3ds/ui/**            -- game headers only, never <3ds.h>
//   host-side TUs : 3ds/host/**, rp2350/ppu.c
//                                        -- libctru only, never game headers
//
// Everything they say to each other is declared here, in stdint types only.
// This header includes neither side's headers and must stay that way.

#ifndef CTR_BRIDGE_H
#define CTR_BRIDGE_H

#include <stdint.h>

#define CTR_GBA_WIDTH   240
#define CTR_GBA_HEIGHT  160

#define CTR_BOTTOM_WIDTH   320
#define CTR_BOTTOM_HEIGHT  240

// ---------------------------------------------------------------- game side --

// Allocate and clear the GBA memory regions. MUST run before any other game
// code: every VRAM/palette/OAM/register access derives from gGbaMem.
void Ctr3dsInitGbaMemory(void);

// Region bases for ppu_set_memory(). Valid only after Ctr3dsInitGbaMemory().
void CtrGetGbaRegions(const void **reg, const void **pal,
                      const void **vram, const void **oam);

// Write the frame's button state into REG_KEYINPUT. GBA keys are ACTIVE-LOW:
// a 0 bit means pressed. Bit order is A,B,Select,Start,Right,Left,Up,Down,R,L.
void CtrSetKeyInput(uint16_t keysActiveLow);

// The game's superloop (src/main.c). Never returns.
void AgbMain(void);

// ---- bottom screen (game side: it draws with Emerald's own fonts and gfx) ----

typedef struct {
    int16_t x, y;          // touch position, 0..319 / 0..239; valid if touching
    uint8_t touching;
    uint8_t justPressed;
    uint8_t justReleased;
} CtrTouchState;

void CtrBottomInit(void);
void CtrBottomUpdate(const CtrTouchState *touch);

// Non-zero when the framebuffer changed since the host last uploaded it.
// The bottom screen is mostly static, so this gates the texture upload.
int  CtrBottomIsDirty(void);
void CtrBottomClearDirty(void);

// 320x240 RGB565, row-major. Stable pointer, valid after CtrBottomInit().
const uint16_t *CtrBottomFramebuffer(void);

// ---------------------------------------------------------------- host side --

// Per-frame hook called at the end of every game frame from AgbMain's loop
// (src/main.c, under #if RP2350). Renders, presents, samples input, feeds
// audio, and paces the game to the GBA frame rate.
void Rp2350PresentFrame(void);

// Save flash write hooks, called from src/agb_flash*.c. Reads go straight
// through FLASH_BASE (gCtrSaveFlash); only writes come through here.
// Return 0 on success, 0x80FF on failure.
uint16_t Rp2350SaveEraseChip(void);
uint16_t Rp2350SaveEraseSector(uint16_t sectorNum);
uint16_t Rp2350SaveProgramSector(uint16_t sectorNum, uint8_t *src);
uint16_t Rp2350SaveProgramByte(uint16_t sectorNum, uint32_t offset, uint8_t data);
void     Rp2350SaveSync(void);

// The console's real-time clock, standing in for the cartridge RTC. The GBA
// carts carried an S-3511A; a 3DS has no cart, so src/siirtc.c is backed by
// this instead (under PLATFORM_3DS). That file is game-side and includes this
// header directly, the way 3ds/ui/*.c already do.
//
// Fields are PLAIN BINARY. The driver applies the BCD encoding the real chip
// would have used, because that is what src/rtc.c reads back.
typedef struct {
    uint8_t year;       // years since 2000, 0..99: the chip stores two digits
    uint8_t month;      // 1..12
    uint8_t day;        // 1..31
    uint8_t dayOfWeek;  // 0..6, Sunday = 0
    uint8_t hour;       // 0..23, always 24-hour
    uint8_t minute;     // 0..59
    uint8_t second;     // 0..59
} CtrClock;

void Ctr3dsGetClock(CtrClock *out);

// Mix one frame of PCM. Implemented game-side in rp2350/m4a_1.c.
int Rp2350MixFrame(int8_t *out, int n);

#endif // CTR_BRIDGE_H
