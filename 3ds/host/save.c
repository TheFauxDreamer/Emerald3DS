// Cartridge save flash, stored in a file on the SD card.
//
// On the 3DS, the 128 KB flash is a RAM array (gCtrSaveFlash, 3ds/gba_mem.c).
// The game reads it through FLASH_BASE directly, so only the write hooks are
// here.
//
// Writes to the array are free. The question is when to write the SD card. A
// write marks the image dirty. Two paths then flush it.
//
// The main path is CtrSaveCommit(). The game (src/save.c) calls it when a save
// finishes, so the file is current before the "saved the game" message clears.
// That makes the save durable. The process does not always exit cleanly. A
// close of the emulator window kills it, and aptMainLoop() does not report
// that. Thus the exit flush does not run.
//
// The debounce is the second path, for writes outside a save (the special
// sectors, a chunked link save between steps). It turns a burst into one file
// write after CTR_SAVE_QUIET_MS of quiet.
//
// The burst is small: ProgramFlashSector_MX writes a full sector at once on
// this port (src/agb_flash_mx.c), so a full save is 28 hook calls.

#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "../bridge.h"

#define SAVE_SIZE     (128 * 1024)
#define GBA_SECTOR    4096u
#define FLASH_ERR     0x80FF

// The time after the last write before the file write. It is short because it
// is only the second path. The important saves commit directly.
#define CTR_SAVE_QUIET_MS 100

#define SAVE_DIR  "sdmc:/3ds/emerald3ds"
#define SAVE_PATH SAVE_DIR "/pokeemerald.sav"
#define SAVE_TMP  SAVE_PATH ".tmp"
#define SAVE_BAK  SAVE_PATH ".bak"

extern unsigned char gCtrSaveFlash[];   // game side, SAVE_SIZE bytes

static int      sDirty;
static uint64_t sLastWriteMs;

static uint64_t now_ms(void) { return osGetTime(); }

// The bounds check for all hooks: a bad sector number must not write over the
// heap. Returns 0 if the range is inside the image.
static int range_ok(uint32_t off, uint32_t len)
{
    return off <= SAVE_SIZE && len <= SAVE_SIZE - off;
}

void CtrSaveLoad(void)
{
    // Ctr3dsInitGbaMemory filled gCtrSaveFlash with 0xFF (an erased cart), so a
    // missing file reads as a blank cart, and the game offers "new game".
    FILE *f = fopen(SAVE_PATH, "rb");

    if (f == NULL) {
        // No save, but maybe a save that a swap moved aside and did not finish
        // (between its two renames). Recover it here. Thus the flush can move
        // the old file, not delete it. Without this step, a crash in that
        // window looks like a blank cart. The player then sees a new game on
        // top of a real save.
        if (rename(SAVE_BAK, SAVE_PATH) == 0) {
            printf("save: recovered " SAVE_BAK "\n");
            f = fopen(SAVE_PATH, "rb");
        }
    } else {
        // An old .bak means that the last swap finished. The copy is not
        // needed.
        remove(SAVE_BAK);
    }

    if (f == NULL)
        return;

    size_t n = fread(gCtrSaveFlash, 1, SAVE_SIZE, f);
    fclose(f);

    // A short read leaves the tail at 0xFF, which is what an erased region
    // reads as. The save layer's checksums decide what is valid.
    if (n != SAVE_SIZE)
        printf("save: short read (%u/%u bytes)\n", (unsigned)n, SAVE_SIZE);
}

// Write the image. It writes a temp file and swaps it in, so an interrupted
// flush (battery pull, crash, emulator close) cannot leave a half-written save.
//
// The sDirty flag clears only on success, so the next frame retries any failure
// below.
void CtrSaveFlush(int force)
{
    unsigned int t0;

    if (!sDirty)
        return;
    if (!force && now_ms() - sLastWriteMs < CTR_SAVE_QUIET_MS)
        return;

    // This writes 128 KB and does a three-way rename, in the same part of the
    // frame as the settings write. It runs from the frame loop, so measure it.
    // If it is slow, the player sees the game stop and nothing else tells why.
    t0 = CtrTimeNowMs();

    mkdir("sdmc:/3ds", 0777);
    mkdir(SAVE_DIR, 0777);

    FILE *f = fopen(SAVE_TMP, "wb");
    if (f == NULL) {
        printf("save: cannot open " SAVE_TMP "\n");
        CtrLogSlow("saveflush", t0);
        return;
    }

    size_t n = fwrite(gCtrSaveFlash, 1, SAVE_SIZE, f);
    int flushed = (fflush(f) == 0);
    // Check fclose. On the 3DS newlib, the close reaches the FS service, so a
    // failure here means that the bytes did not land.
    int closed = (fclose(f) == 0);

    if (n != SAVE_SIZE || !flushed || !closed) {
        printf("save: write failed (%u/%u, flush %d, close %d)\n",
               (unsigned)n, SAVE_SIZE, flushed, closed);
        remove(SAVE_TMP);
        CtrLogSlow("saveflush", t0);
        return;
    }

    // Swap the new file in; do not delete the old one first. FAT does not
    // rename onto an existing name, so the old save must move first. The move
    // keeps a complete save on the card at all times. A failed second rename
    // puts the old file back, and a crash between the two leaves .bak for
    // CtrSaveLoad to recover at boot.
    //
    // Remove an old copy first. A .bak from an earlier failure would block the
    // move below, and every later save would fail.
    remove(SAVE_BAK);

    int hadOld = (rename(SAVE_PATH, SAVE_BAK) == 0);

    if (rename(SAVE_TMP, SAVE_PATH) != 0) {
        printf("save: rename failed\n");
        if (hadOld)
            rename(SAVE_BAK, SAVE_PATH);
        remove(SAVE_TMP);
        CtrLogSlow("saveflush", t0);
        return;
    }

    if (hadOld)
        remove(SAVE_BAK);

    sDirty = 0;
    CtrLogSlow("saveflush", t0);
}

// Write the image now, with no debounce.
//
// The game (src/save.c) calls this when a save completes. Thus the save does
// not depend on the process living long enough for a timer or for the exit
// path.
void CtrSaveCommit(void)
{
    CtrSaveFlush(1);
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

// Flash programming can only clear bits (1 -> 0), and an erase sets them. The
// game always erases before it programs, so AND and a plain store give the same
// result. AND is what the hardware does, so a double-programmed byte acts as on
// a cart.
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

// Writes are already visible through FLASH_BASE, so reads need nothing. This is
// only a chance to flush.
//
// Do not force a flush. VerifyFlashSector calls this after each sector
// (src/agb_flash.c), so a forced flush would write the 128 KB file fourteen
// times for each save. CtrSaveCommit() forces once, at the end.
void Rp2350SaveSync(void)
{
    CtrSaveFlush(0);
}
