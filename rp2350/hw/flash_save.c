// Persistent game saves: the GBA cart's 128 KB save flash, emulated in the
// last 128 KB of the QSPI flash the game itself executes from.
//
// Mapping is exact -- 4 KB sectors, erased state 0xFF, programming only
// clears bits (both chips AND data in) -- so the game's agb_flash layer maps
// 1:1: reads go straight through the XIP window (FLASH_BASE = 0x10FE0000 in
// include/gba/flash_internal.h) and the write entry points below reprogram
// the QSPI chip. The save region sits far above the ~13.4 MB firmware image,
// so saves survive reflashes.
//
// Two hard constraints shape this file:
//
// 1. QSPI program/erase suspends XIP, and BOTH cores normally execute from
//    it. Every operation runs under flash_safe_execute(): core 0 IRQs off,
//    core 1 parked in the SDK's RAM-resident lockout handler (core 1 calls
//    flash_safe_execute_core_init() at startup). The HSTX scanout is immune
//    by design -- its control-block DMA ring and line buffers are all SRAM,
//    and its IRQ handler lives in SCRATCH_X, so the display stays alive even
//    mid-erase. After every operation the XIP cache is invalidated for the
//    touched range so FLASH_BASE reads see fresh data.
//
// 2. The save code (src/save.c HandleReplaceSector) writes sectors as ~4080
//    single-byte ProgramFlashByte calls. One QSPI page program per byte
//    would take seconds per sector, so byte writes coalesce into a 256-byte
//    page buffer (initialised to 0xFF = program-nothing) and flush when the
//    stream crosses a page boundary, when any other flash op happens, or
//    when Rp2350SaveSync() runs (called by the agb_flash read/verify paths
//    and once per frame). Net cost per sector: 1 erase + 16 page programs,
//    ~55 ms -- about what the real cart took.
//
// NOTE: program sources must be in RAM (flash is unreadable mid-op). The
// game always programs from EWRAM buffers, and the page buffer is .bss.

#include <string.h>
#include <stdio.h>

#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/xip_cache.h"

#define SAVE_SIZE   (128 * 1024)
#define SAVE_OFFSET (PICO_FLASH_SIZE_BYTES - SAVE_SIZE)
#define GBA_SECTOR  4096u

static_assert(SAVE_OFFSET % FLASH_SECTOR_SIZE == 0, "save region alignment");
static_assert(GBA_SECTOR == FLASH_SECTOR_SIZE, "GBA/QSPI sector mismatch");

// agb_flash error code for a failed write (see WaitForFlashWrite users).
#define FLASH_ERR 0x80FF

struct flash_op {
    uint32_t offset, size;
    const uint8_t *src;   // NULL = erase
};

static void do_flash_op(void *param) {
    const struct flash_op *op = param;
    if (op->src)
        flash_range_program(op->offset, op->src, op->size);
    else
        flash_range_erase(op->offset, op->size);
}

static uint16_t run_op(struct flash_op *op) {
    int rc = flash_safe_execute(do_flash_op, op, 4000);
    // FLASH_BASE reads are XIP-cached; drop stale lines for the changed range.
    xip_cache_invalidate_range(op->offset, op->size);
    if (rc != PICO_OK) {
        printf("!! save flash %s failed rc=%d off=%08lx\n",
               op->src ? "program" : "erase", rc, (unsigned long)op->offset);
        return FLASH_ERR;
    }
    return 0;
}

// ---- byte-write page coalescing ---------------------------------------------
// (page_buf lives in EWRAM slack; the SDK RAM region is full.)
static uint8_t  page_buf[FLASH_PAGE_SIZE] __attribute__((section(".ewram_top.flashsave")));
static uint32_t page_off = UINT32_MAX;   // save-region offset of open page
static bool     page_dirty;

static uint16_t flush_page(void) {
    if (!page_dirty)
        return 0;
    struct flash_op op = { SAVE_OFFSET + page_off, FLASH_PAGE_SIZE, page_buf };
    page_dirty = false;
    page_off = UINT32_MAX;
    return run_op(&op);
}

void Rp2350SaveSync(void) {
    flush_page();
}

// ---- agb_flash entry points (declared in include/gba/flash_internal.h) ------
uint16_t Rp2350SaveEraseChip(void) {
    page_dirty = false;   // pending bytes are moot; everything goes to 0xFF
    page_off = UINT32_MAX;
    struct flash_op op = { SAVE_OFFSET, SAVE_SIZE, NULL };
    return run_op(&op);
}

uint16_t Rp2350SaveEraseSector(uint16_t sectorNum) {
    uint16_t rc = flush_page();
    if (rc) return rc;
    struct flash_op op = { SAVE_OFFSET + sectorNum * GBA_SECTOR, GBA_SECTOR, NULL };
    return run_op(&op);
}

uint16_t Rp2350SaveProgramSector(uint16_t sectorNum, uint8_t *src) {
    uint16_t rc = flush_page();
    if (rc) return rc;
    struct flash_op op = { SAVE_OFFSET + sectorNum * GBA_SECTOR, GBA_SECTOR, src };
    return run_op(&op);
}

uint16_t Rp2350SaveProgramByte(uint16_t sectorNum, uint32_t offset, uint8_t data) {
    uint32_t addr = sectorNum * GBA_SECTOR + offset;
    uint32_t page = addr & ~(uint32_t)(FLASH_PAGE_SIZE - 1);
    if (page != page_off) {
        uint16_t rc = flush_page();
        if (rc) return rc;
        memset(page_buf, 0xFF, sizeof page_buf);   // 0xFF programs nothing
        page_off = page;
    }
    page_buf[addr & (FLASH_PAGE_SIZE - 1)] &= data;
    page_dirty = true;
    return 0;
}
