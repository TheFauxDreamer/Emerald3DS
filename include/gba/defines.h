#ifndef GUARD_GBA_DEFINES_H
#define GUARD_GBA_DEFINES_H

#include <stddef.h>
#include <stdint.h>

#define TRUE  1
#define FALSE 0

#if PLATFORM_3DS
// The 3DS port has no linker-placed EWRAM/IWRAM regions -- gGbaMem is a runtime
// heap block (3ds/gba_mem.c) -- so these variables are ordinary .bss/.data.
// Side benefit over RP2350: common_data keeps its static initialisers instead
// of being discarded by a NOLOAD region (see the IdentifyFlash note in main.c).
#define IWRAM_DATA
#define EWRAM_DATA
#define COMMON_DATA
#else
#define IWRAM_DATA __attribute__((section("iwram_data")))
#define EWRAM_DATA __attribute__((section("ewram_data")))
#define COMMON_DATA __attribute__((section("common_data")))
#endif
#define UNUSED __attribute__((unused))

#if MODERN
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

#define ALIGNED(n) __attribute__((aligned(n)))

// On real GBA / WASM (flat linear memory) the GBA memory map is used verbatim.
// On RP2350 those addresses (0x02-0x07xxxxxx) are unmapped and would bus-fault,
// so the regions are remapped into reserved RP2350 SRAM (0x20000000+). The
// custom linker script (rp2350/memmap_rp2350.ld) reserves matching regions.
#if PLATFORM_3DS
// 3DS: one contiguous heap block, same region order and spacing as RP2350 so
// that IsTileMapOutsideWram()'s `ptr > IWRAM_END` test behaves identically.
// gGbaMem is an ARRAY, not a pointer, and that is deliberate: an array's
// address is a link-time constant, so `&REG_WIN0H` still works in the static
// initialisers that src/field_screen_effect.c and src/pokenav_menu_handler_gfx.c
// build. A `u8 *` would make those "initializer element is not a compile-time
// constant".
#ifndef __ASSEMBLER__
extern unsigned char gGbaMem[];   // u8 is not declared yet at this point
#endif
#define CTR_GBA_MEM_SIZE     0x60C00
#define CTR_SAVE_FLASH_SIZE  (128 * 1024)

#define EWRAM_START ((uintptr_t)gGbaMem + 0x00000)  // 256 KB
#define IWRAM_START ((uintptr_t)gGbaMem + 0x40000)  //  32 KB
#define VRAM        ((uintptr_t)gGbaMem + 0x48000)  //  96 KB
#define PLTT        ((uintptr_t)gGbaMem + 0x60000)  //   1 KB
#define OAM         ((uintptr_t)gGbaMem + 0x60400)  //   1 KB
#define REG_BASE    ((uintptr_t)gGbaMem + 0x60800)  //   1 KB (I/O backing store)
#elif RP2350
#define EWRAM_START 0x20000000  // 256 KB
#define IWRAM_START 0x20040000  //  32 KB
#define VRAM        0x20048000  //  96 KB
#define PLTT        0x20060000  //   1 KB
#define OAM         0x20060400  //   1 KB
#define REG_BASE    0x20060800  //   1 KB (I/O register backing store)
#else
#define EWRAM_START 0x02000000
#define IWRAM_START 0x03000000
#define PLTT        0x5000000
#define VRAM        0x6000000
#define OAM         0x7000000
#endif

#define EWRAM_END   (EWRAM_START + 0x40000)
#define IWRAM_END   (IWRAM_START + 0x8000)

#define SOUND_INFO_PTR (*(struct SoundInfo **)(IWRAM_START + 0x7FF0))
#define INTR_CHECK     (*(u16 *)(IWRAM_START + 0x7FF8))
#define INTR_VECTOR    (*(void **)(IWRAM_START + 0x7FFC))

#define BG_PLTT       PLTT
#define BG_PLTT_SIZE  0x200
#define OBJ_PLTT      (PLTT + BG_PLTT_SIZE)
#define OBJ_PLTT_SIZE 0x200
#define PLTT_SIZE     (BG_PLTT_SIZE + OBJ_PLTT_SIZE)

#define VRAM_SIZE 0x18000

#define BG_VRAM           VRAM
#define BG_VRAM_SIZE      0x10000
#define BG_CHAR_SIZE      0x4000
#define BG_SCREEN_SIZE    0x800
#define BG_CHAR_ADDR(n)   (BG_VRAM + (BG_CHAR_SIZE * (n)))
#define BG_SCREEN_ADDR(n) (BG_VRAM + (BG_SCREEN_SIZE * (n)))

#define BG_TILE_H_FLIP(n) (0x400 + (n))
#define BG_TILE_V_FLIP(n) (0x800 + (n))

#define NUM_BACKGROUNDS 4

// text-mode BG
#define OBJ_VRAM0      (VRAM + 0x10000)
#define OBJ_VRAM0_SIZE 0x8000

// bitmap-mode BG
#define OBJ_VRAM1      (VRAM + 0x14000)
#define OBJ_VRAM1_SIZE 0x4000

#define OAM_SIZE 0x400

#define ROM_HEADER_SIZE   0xC0

// Dimensions of a tile in pixels
#define TILE_WIDTH  8
#define TILE_HEIGHT 8

// Dimensions of the GBA screen in pixels
#define DISPLAY_WIDTH  240
#define DISPLAY_HEIGHT 160

// Dimensions of the GBA screen in tiles
#define DISPLAY_TILE_WIDTH  (DISPLAY_WIDTH / TILE_WIDTH)
#define DISPLAY_TILE_HEIGHT (DISPLAY_HEIGHT / TILE_HEIGHT)

// Size of different tile formats in bytes
#define TILE_SIZE(bpp) ((bpp) * TILE_WIDTH * TILE_HEIGHT / 8)
#define TILE_SIZE_1BPP TILE_SIZE(1) // 8
#define TILE_SIZE_4BPP TILE_SIZE(4) // 32
#define TILE_SIZE_8BPP TILE_SIZE(8) // 64

#define TILE_OFFSET_4BPP(n) ((n) * TILE_SIZE_4BPP)
#define TILE_OFFSET_8BPP(n) ((n) * TILE_SIZE_8BPP)

#define TOTAL_OBJ_TILE_COUNT 1024

#define PLTT_SIZEOF(n) ((n) * sizeof(u16))
#define PLTT_SIZE_4BPP PLTT_SIZEOF(16)
#define PLTT_SIZE_8BPP PLTT_SIZEOF(256)

#define PLTT_OFFSET_4BPP(n) ((n) * PLTT_SIZE_4BPP)

#endif // GUARD_GBA_DEFINES_H
