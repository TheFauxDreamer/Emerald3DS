// Cart save flash, backed by a file on the SD card.
//
// The RP2350 port had to emulate the cart's flash inside the QSPI chip it was
// executing from, with core lockout and page coalescing. On the 3DS the whole
// 128 KB is just a RAM array (gCtrSaveFlash, 3ds/gba_mem.c) that the game reads
// through FLASH_BASE directly, so only the WRITE hooks land here.
//
// Writes are therefore free, and the only real question is when to touch the SD
// card. Any write marks the image dirty; the flush is then driven two ways.
//
// The one that matters is CtrSaveCommit(), called from src/save.c the moment a
// save finishes. Committing on the event rather than on a timer is what makes
// the save durable: the file is current before the "saved the game" message
// even clears, so nothing afterwards has to survive for the save to stick. That
// matters because the process does not always get to exit cleanly. Closing the
// emulator window kills it outright, and aptMainLoop() never reports the app is
// going away, so the flush on exit below never runs.
//
// The debounce is the backstop, for writes that arrive outside a save (the
// special sectors, a chunked link save between its steps). It coalesces a burst
// into one file write after CTR_SAVE_QUIET_MS of quiet.
//
// Note the burst here is small. On real hardware a sector is ~4080 single-byte
// programs, but ProgramFlashSector_MX takes its whole-sector branch on this
// port (src/agb_flash_mx.c), so a full save is 28 hook calls, not thousands.

#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "../bridge.h"

#define SAVE_SIZE     (128 * 1024)
#define GBA_SECTOR    4096u
#define FLASH_ERR     0x80FF

// Wait this long after the last write before writing the file out. Short,
// because it is only a backstop now: the saves that matter are committed
// explicitly, and a burst on this port is 28 calls rather than thousands.
#define CTR_SAVE_QUIET_MS 100

#define SAVE_DIR  "sdmc:/3ds/emerald3ds"
#define SAVE_PATH SAVE_DIR "/pokeemerald.sav"
#define SAVE_TMP  SAVE_PATH ".tmp"
#define SAVE_BAK  SAVE_PATH ".bak"

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

    if (f == NULL) {
        // No save, but possibly one moved aside by a swap that was interrupted
        // between its two renames. Recovering it here is what lets the flush
        // move the old file rather than delete it: without this step, dying in
        // that one-rename window would look exactly like a blank cart and the
        // player would be offered a new game on top of a real save.
        if (rename(SAVE_BAK, SAVE_PATH) == 0) {
            printf("save: recovered " SAVE_BAK "\n");
            f = fopen(SAVE_PATH, "rb");
        }
    } else {
        // A stale .bak means the last swap completed; the copy is redundant.
        remove(SAVE_BAK);
    }

    if (f == NULL)
        return;

    size_t n = fread(gCtrSaveFlash, 1, SAVE_SIZE, f);
    fclose(f);

    // A short read leaves the tail at 0xFF, which is what an erased region
    // reads as anyway -- the save layer's checksums decide what is valid.
    if (n != SAVE_SIZE)
        printf("save: short read (%u/%u bytes)\n", (unsigned)n, SAVE_SIZE);
}

// Write the image out. Writes to a temp file and swaps it in, so an interrupted
// flush (battery pull, crash, the emulator window closing) cannot leave a
// half-written save behind.
//
// sDirty is cleared only on the success path, so any failure below is retried
// on the next frame rather than silently dropped.
void CtrSaveFlush(int force)
{
    if (!sDirty)
        return;
    if (!force && now_ms() - sLastWriteMs < CTR_SAVE_QUIET_MS)
        return;

    mkdir("sdmc:/3ds", 0777);
    mkdir(SAVE_DIR, 0777);

    FILE *f = fopen(SAVE_TMP, "wb");
    if (f == NULL) {
        printf("save: cannot open " SAVE_TMP "\n");
        return;
    }

    size_t n = fwrite(gCtrSaveFlash, 1, SAVE_SIZE, f);
    int flushed = (fflush(f) == 0);
    // fclose is checked, not just called: on 3DS newlib it is the close that
    // reaches the FS service, so a failure here means the bytes never landed
    // however well fwrite and fflush went.
    int closed = (fclose(f) == 0);

    if (n != SAVE_SIZE || !flushed || !closed) {
        printf("save: write failed (%u/%u, flush %d, close %d)\n",
               (unsigned)n, SAVE_SIZE, flushed, closed);
        remove(SAVE_TMP);
        return;
    }

    // Swap the new file in, rather than deleting the old one and renaming over
    // the gap. FAT will not rename onto an existing name, so the old save does
    // have to move first, but MOVING it keeps a complete save on the card at
    // every instant: a failed second rename puts the original back, and dying
    // between the two leaves .bak for CtrSaveLoad to recover at boot. Deleting
    // first would open a window where any failure means no save at all.
    //
    // The stale-copy sweep matters for the same reason the move does: a .bak
    // left by an earlier failure in this session would block the move below and
    // wedge every subsequent save.
    remove(SAVE_BAK);

    int hadOld = (rename(SAVE_PATH, SAVE_BAK) == 0);

    if (rename(SAVE_TMP, SAVE_PATH) != 0) {
        printf("save: rename failed\n");
        if (hadOld)
            rename(SAVE_BAK, SAVE_PATH);
        remove(SAVE_TMP);
        return;
    }

    if (hadOld)
        remove(SAVE_BAK);

    sDirty = 0;
}

// Write the image out NOW, whatever the debounce says.
//
// Called from src/save.c the moment a save completes. This is the difference
// between a save that survives the game being killed and one that does not: it
// removes any dependence on the process living long enough for a timer to fire
// or for the exit path to run.
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
//
// Deliberately NOT a forced flush. VerifyFlashSector calls this after every
// sector (src/agb_flash.c), so forcing would mean fourteen 128 KB file writes
// per save. CtrSaveCommit() is the one that forces, once, at the end.
void Rp2350SaveSync(void)
{
    CtrSaveFlush(0);
}
