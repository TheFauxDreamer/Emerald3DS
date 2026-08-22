// Cart save flash, backed by a file on the SD card.
//
// The RP2350 port had to emulate the cart's flash inside the QSPI chip it was
// executing from, with core lockout and page coalescing. On the 3DS the whole
// 128 KB is just a RAM array (gCtrSaveFlash, 3ds/gba_mem.c) that the game reads
// through FLASH_BASE directly, so only the WRITE hooks land here.
//
// Writes are therefore free, and the only real question is when to touch the SD
// card. src/save.c writes a sector as ~4080 single-byte programs; flushing per
// write would mean thousands of 128 KB file writes per save. Instead any write
// marks the image dirty and the flush is deferred until the writes stop
// (CTR_SAVE_QUIET_MS of quiet), plus an unconditional flush on exit.

#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "../bridge.h"

#define SAVE_SIZE     (128 * 1024)
#define GBA_SECTOR    4096u
#define FLASH_ERR     0x80FF

// Wait this long after the last write before writing the file out. One save in
// Emerald is a burst of sector writes; this coalesces the whole burst into one.
#define CTR_SAVE_QUIET_MS 400

#define SAVE_DIR  "sdmc:/3ds/emerald3ds"
#define SAVE_PATH SAVE_DIR "/pokeemerald.sav"

extern unsigned char gCtrSaveFlash[];   // game-side, SAVE_SIZE bytes

static int      sDirty;
static uint64_t sLastWriteMs;

static uint64_t now_ms(void) { return osGetTime(); }

// Bounds check shared by every hook: a bad sector number must not scribble
// over the heap. Returns 0 if the range is inside the image.
static int range_ok(uint32_t off, uint32_t len)
{
    return off <= SAVE_SIZE && len <= SAVE_SIZE - off;
}

void CtrSaveLoad(void)
{
    // gCtrSaveFlash was filled with 0xFF (erased cart) by Ctr3dsInitGbaMemory,
    // so a missing file correctly reads as a blank cart -> "new game".
    FILE *f = fopen(SAVE_PATH, "rb");
    if (f == NULL)
        return;

    size_t n = fread(gCtrSaveFlash, 1, SAVE_SIZE, f);
    fclose(f);

    // A short read leaves the tail at 0xFF, which is what an erased region
    // reads as anyway -- the save layer's checksums decide what is valid.
    if (n != SAVE_SIZE)
        printf("save: short read (%u/%u bytes)\n", (unsigned)n, SAVE_SIZE);
}

// Write the image out. Writes to a temp file and renames, so an interrupted
// flush (battery pull, crash) cannot leave a half-written save behind.
void CtrSaveFlush(int force)
{
    if (!sDirty)
        return;
    if (!force && now_ms() - sLastWriteMs < CTR_SAVE_QUIET_MS)
        return;

    mkdir("sdmc:/3ds", 0777);
    mkdir(SAVE_DIR, 0777);

    FILE *f = fopen(SAVE_PATH ".tmp", "wb");
    if (f == NULL) {
        printf("save: cannot open " SAVE_PATH ".tmp\n");
        return;
    }

    size_t n = fwrite(gCtrSaveFlash, 1, SAVE_SIZE, f);
    int flushed = (fflush(f) == 0);
    fclose(f);

    if (n != SAVE_SIZE || !flushed) {
        printf("save: write failed (%u/%u)\n", (unsigned)n, SAVE_SIZE);
        remove(SAVE_PATH ".tmp");
        return;
    }

    remove(SAVE_PATH);
    if (rename(SAVE_PATH ".tmp", SAVE_PATH) != 0) {
        printf("save: rename failed\n");
        return;
    }

    sDirty = 0;
}

static void mark_dirty(void)
{
    sDirty = 1;
    sLastWriteMs = now_ms();
}

// ---- hooks called from src/agb_flash*.c -------------------------------------

uint16_t Rp2350SaveEraseChip(void)
{
    memset(gCtrSaveFlash, 0xFF, SAVE_SIZE);
    mark_dirty();
    return 0;
}

uint16_t Rp2350SaveEraseSector(uint16_t sectorNum)
{
    uint32_t off = (uint32_t)sectorNum * GBA_SECTOR;
    if (!range_ok(off, GBA_SECTOR))
        return FLASH_ERR;

    memset(gCtrSaveFlash + off, 0xFF, GBA_SECTOR);
    mark_dirty();
    return 0;
}

// Flash programming can only clear bits (1 -> 0); an erase is what sets them
// back. The game always erases before programming, so AND and plain assignment
// agree in practice -- but AND is what the hardware does, and matching it means
// a double-programmed byte behaves the same here as on a cart.
uint16_t Rp2350SaveProgramSector(uint16_t sectorNum, uint8_t *src)
{
    uint32_t off = (uint32_t)sectorNum * GBA_SECTOR;
    if (!range_ok(off, GBA_SECTOR) || src == NULL)
        return FLASH_ERR;

    for (uint32_t i = 0; i < GBA_SECTOR; i++)
        gCtrSaveFlash[off + i] &= src[i];

    mark_dirty();
    return 0;
}

uint16_t Rp2350SaveProgramByte(uint16_t sectorNum, uint32_t offset, uint8_t data)
{
    uint32_t off = (uint32_t)sectorNum * GBA_SECTOR + offset;
    if (offset >= GBA_SECTOR || !range_ok(off, 1))
        return FLASH_ERR;

    gCtrSaveFlash[off] &= data;
    mark_dirty();
    return 0;
}

// On RP2350 this committed a pending QSPI page. Here writes are already visible
// through FLASH_BASE, so reads need nothing -- it is only a flush opportunity.
void Rp2350SaveSync(void)
{
    CtrSaveFlush(0);
}
