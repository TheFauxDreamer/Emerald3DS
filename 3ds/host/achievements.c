// The achievements store, stored next to settings.bin on the SD card.
//
// Host side, and simple. The game side (3ds/achievements.c) decides what an
// achievement is and when it unlocks, with the game's own accessors. This file
// keeps the bits that it gets, one record for each playthrough, and writes them
// to the card away from the frame.
//
// It follows the write rules of settings.c. A change marks the table dirty.
// CtrAchFlush() waits for one second of quiet and gives a snapshot to the I/O
// thread (io_thread.c). The card write occurs while the main thread waits for
// VBlank. An unlock occurs during a catch, a badge or the end of a battle,
// where a direct write would be visible.
//
// Like settings.c, this is not save data. A missing, short or wrong-version
// file is normal. The table then starts empty, and the game side finds again
// every achievement that it can from the save (the backfill in
// 3ds/achievements.c). Nothing here blocks boot.
//
// It writes in place, like settings.c, not with a temp file and a rename. The
// file is one fixed 460-byte image in one sector, so a write cannot tear. The
// magic and version checks turn a bad file into "no records".
//
// Version 1 (332 bytes) has no place bits. It is still read, with every place
// unvisited. Not all of its unlocks can be found again: the shiny catch leaves
// nothing in the save.

#include <3ds.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "../bridge.h"
#include "io_thread.h"
#include "trace.h"                    // CtrLog

#define ACH_DIR   "sdmc:/3ds/emerald3ds"
#define ACH_PATH  ACH_DIR "/achievements.bin"

#define ACH_MAGIC    0x43413345u      // 'E3AC' little endian
#define ACH_VERSION  2

// One playthrough. A fixed size with every byte in use, so the layout on the
// card does not depend on the compiler's alignment.
struct CtrAchRecord
{
    uint32_t playerId;
    // The table's clock when this record was last loaded or saved. When all
    // CTR_ACH_RECORDS are in use, a new playthrough replaces the lowest.
    uint32_t lastUsed;
    uint8_t  unlocked[CTR_ACH_BYTES];
    uint8_t  unseen[CTR_ACH_BYTES];
    uint8_t  places[CTR_ACH_PLACE_BYTES];
};

struct CtrAchFile
{
    uint32_t magic;
    uint16_t version;
    uint8_t  count;                   // records in use, from rec[0]
    uint8_t  pad;                     // explicit, always written as 0
    uint32_t clock;                   // the next lastUsed value
    struct CtrAchRecord rec[CTR_ACH_RECORDS];
};

// Version 1, only for reading: the same header, and records with no places.
struct CtrAchRecordV1
{
    uint32_t playerId;
    uint32_t lastUsed;
    uint8_t  unlocked[16];
    uint8_t  unseen[16];
};

struct CtrAchFileV1
{
    uint32_t magic;
    uint16_t version;
    uint8_t  count;
    uint8_t  pad;
    uint32_t clock;
    struct CtrAchRecordV1 rec[8];
};

// If one of these fails, the compiler added padding, and ach_put() would write
// uninitialized bytes to the card. Add explicit padding.
_Static_assert(sizeof(struct CtrAchRecord)
                   == 8 + 2 * CTR_ACH_BYTES + CTR_ACH_PLACE_BYTES,
               "CtrAchRecord has implicit padding");
_Static_assert(sizeof(struct CtrAchFile)
                   == 12 + CTR_ACH_RECORDS * sizeof(struct CtrAchRecord),
               "CtrAchFile has implicit padding");

// Version 1's layout must stay the 332 bytes that were written. A later change
// to the live sizes must not change it.
_Static_assert(sizeof(struct CtrAchFileV1) == 332,
               "CtrAchFileV1 must match the version 1 file");
_Static_assert(CTR_ACH_BYTES >= 16 && CTR_ACH_RECORDS >= 8,
               "a version 1 record must still fit a live one");

// The same one second of quiet as settings.c's CTR_SETTINGS_QUIET_MS. A
// backfill or a burst of unlocks (a battle can complete several) costs one
// write.
#define CTR_ACH_QUIET_MS 1000

// The live table, for the main thread only. The game side loads and saves
// through it. The I/O thread sees only the copy in sPending.
static struct CtrAchFile sTable;

static int      sDirty;
static uint64_t sLastChangeMs;

// The file, open for the full session and never closed, like the files of
// settings.c and log.c. The call to fflush() pushes the bytes, and the process
// exit closes the file. One open attempt for each boot, so a read-only card
// costs one failed open.
static FILE *sFile;
static int   sOpenTried;

static void table_reset(void)
{
    memset(&sTable, 0, sizeof(sTable));
    sTable.magic   = ACH_MAGIC;
    sTable.version = ACH_VERSION;
}

// Make the directory once for each boot. The code in settings.c makes the same
// directory. Each file keeps its own flag, so neither depends on the other.
static void ensure_dir(void)
{
    static int done;
    unsigned int t0;

    if (done)
        return;

    done = 1;
    t0 = CtrTimeNowMs();

    mkdir("sdmc:/3ds", 0777);
    mkdir(ACH_DIR, 0777);

    CtrLogSlow("ach.mkdir", t0);
}

// Try "r+b" first, so an existing file opens without truncation. Use "w+b" only
// on a first run. See settings_open() for why the order matters.
static FILE *ach_open(void)
{
    unsigned int t0;

    if (sOpenTried)
        return sFile;

    sOpenTried = 1;

    ensure_dir();

    t0 = CtrTimeNowMs();

    sFile = fopen(ACH_PATH, "r+b");
    if (sFile == NULL)
        sFile = fopen(ACH_PATH, "w+b");

    CtrLogSlow("ach.open", t0);

    return sFile;
}

// Load a version 1 file into the reset table: keep every record, with every
// place unvisited. The clock carries over, so the same record is still the
// least recently used.
static void table_from_v1(const struct CtrAchFileV1 *v1)
{
    sTable.count = v1->count;
    sTable.clock = v1->clock;

    for (unsigned i = 0; i < v1->count; i++)
    {
        struct CtrAchRecord *r = &sTable.rec[i];
        const struct CtrAchRecordV1 *old = &v1->rec[i];

        r->playerId = old->playerId;
        r->lastUsed = old->lastUsed;
        memcpy(r->unlocked, old->unlocked, sizeof(old->unlocked));
        memcpy(r->unseen, old->unseen, sizeof(old->unseen));
        // The places array stays as table_reset() set it: zero, no place
        // visited.
    }
}

// At boot, read the full table once. After this, everything comes from memory.
// A bad file leaves the table empty.
void CtrAchStoreInit(void)
{
    // The image of either version, from one read. Both have the same header.
    union
    {
        struct CtrAchFile   now;
        struct CtrAchFileV1 v1;
    } f;
    FILE *fp;
    size_t n;

    table_reset();

    fp = ach_open();
    if (fp == NULL)
        return;                       // read-only or full card: start empty

    if (fseek(fp, 0, SEEK_SET) != 0)
        return;

    memset(&f, 0, sizeof(f));
    n = fread(&f, 1, sizeof(f), fp);

    // At least the size of version 1, not exactly that size. The write is in
    // place and never truncates. Thus an old build's version 1 write over a
    // newer file keeps the newer tail.
    if (n >= sizeof(f.v1) && f.v1.magic == ACH_MAGIC && f.v1.version == 1
        && f.v1.count <= sizeof(f.v1.rec) / sizeof(f.v1.rec[0]))
    {
        table_from_v1(&f.v1);
        CtrLog("emerald3ds: achievements: version 1 file, %u playthrough%s carried over\n",
               (unsigned)sTable.count, sTable.count == 1 ? "" : "s");
        return;
    }

    // Always log which path loaded, like the other such lines. Without it, "no
    // file yet" and "a file that was discarded" give the same log.
    if (n != sizeof(f.now) || f.now.magic != ACH_MAGIC
        || f.now.version != ACH_VERSION || f.now.count > CTR_ACH_RECORDS)
    {
        CtrLog("emerald3ds: achievements: no usable file (%u bytes), starting empty\n",
               (unsigned)n);
        return;
    }

    sTable = f.now;
    // Only the records that the header counts. An unused slot is written before
    // it is read.
    sTable.pad = 0;

    CtrLog("emerald3ds: achievements: %u playthrough%s loaded\n",
           (unsigned)sTable.count, sTable.count == 1 ? "" : "s");
}

static struct CtrAchRecord *find_record(uint32_t playerId)
{
    for (unsigned i = 0; i < sTable.count; i++)
        if (sTable.rec[i].playerId == playerId)
            return &sTable.rec[i];

    return NULL;
}

// A slot for a new playthrough: the next free slot or, when all are in use, the
// least recently used. Clear it and set its ID.
static struct CtrAchRecord *claim_record(uint32_t playerId)
{
    struct CtrAchRecord *r;

    if (sTable.count < CTR_ACH_RECORDS)
    {
        r = &sTable.rec[sTable.count++];
    }
    else
    {
        r = &sTable.rec[0];
        for (unsigned i = 1; i < CTR_ACH_RECORDS; i++)
            if (sTable.rec[i].lastUsed < r->lastUsed)
                r = &sTable.rec[i];
    }

    memset(r, 0, sizeof(*r));
    r->playerId = playerId;
    return r;
}

static void mark_dirty(void)
{
    sDirty = 1;
    sLastChangeMs = osGetTime();
}

int CtrAchStoreLoad(uint32_t playerId, uint8_t *unlocked, uint8_t *unseen,
                    uint8_t *places)
{
    struct CtrAchRecord *r = find_record(playerId);

    if (r == NULL)
    {
        memset(unlocked, 0, CTR_ACH_BYTES);
        memset(unseen, 0, CTR_ACH_BYTES);
        memset(places, 0, CTR_ACH_PLACE_BYTES);
        return 0;
    }

    memcpy(unlocked, r->unlocked, CTR_ACH_BYTES);
    memcpy(unseen, r->unseen, CTR_ACH_BYTES);
    memcpy(places, r->places, CTR_ACH_PLACE_BYTES);

    // The playthrough in play is in use. Do not mark the table dirty. The new
    // clock matters only when a record must be replaced, which occurs in
    // memory. It reaches the card with the next unlock.
    r->lastUsed = ++sTable.clock;
    return 1;
}

void CtrAchStoreSave(uint32_t playerId, const uint8_t *unlocked,
                     const uint8_t *unseen, const uint8_t *places)
{
    struct CtrAchRecord *r = find_record(playerId);

    if (r == NULL)
        r = claim_record(playerId);

    memcpy(r->unlocked, unlocked, CTR_ACH_BYTES);
    memcpy(r->unseen, unseen, CTR_ACH_BYTES);
    memcpy(r->places, places, CTR_ACH_PLACE_BYTES);
    r->lastUsed = ++sTable.clock;

    mark_dirty();
}

// The card part. It runs on the I/O thread during play, and on the main thread
// only when there is no I/O thread or the game closes. It always holds
// sFileLock. Report a slow write with CtrLog, not CtrLogSlow, whose table is
// for the main thread only.
static void ach_put(const struct CtrAchFile *f)
{
    unsigned int t0, elapsed;
    FILE *fp = ach_open();

    if (fp == NULL)
        return;                       // read-only card or full card: not fatal

    t0 = CtrTimeNowMs();

    if (fseek(fp, 0, SEEK_SET) == 0)
    {
        size_t n = fwrite(f, 1, sizeof(*f), fp);

        if (n != sizeof(*f) || fflush(fp) != 0)
            CtrLog("emerald3ds: achievements write failed (%u/%u bytes)\n",
                   (unsigned)n, (unsigned)sizeof(*f));
    }

    elapsed = CtrTimeNowMs() - t0;
    if (elapsed >= 50)
        CtrLog("emerald3ds: slow achievements.write %u ms\n", elapsed);
}

// The write that waits for the I/O thread, and its two locks, as in settings.c.
// The main thread holds sPendingLock only for a struct copy, so it never waits
// on the card. The writer holds sFileLock for the full write, so an older table
// never overwrites a newer one. Both start as 1, what LightLock_Init() writes.
static LightLock         sPendingLock = 1;
static LightLock         sFileLock = 1;
static struct CtrAchFile sPending;
static int               sHavePending;

void CtrAchDrain(void)
{
    static struct CtrAchFile f;       // 460 bytes: not on the writer's stack
    int have;

    LightLock_Lock(&sFileLock);

    LightLock_Lock(&sPendingLock);
    have = sHavePending;
    if (have)
    {
        f = sPending;
        sHavePending = 0;
    }
    LightLock_Unlock(&sPendingLock);

    if (have)
        ach_put(&f);

    LightLock_Unlock(&sFileLock);
}

// Rp2350PresentFrame() calls this next to CtrSettingsFlush(), and the close
// paths call it with force. It gives the table to the I/O thread one second
// after the last unlock. It writes directly only when forced or when there is
// no I/O thread.
//
// The flag sDirty clears even if the write fails, as in settings.c. There is
// one attempt for each change, so a read-only card does not cause an FS attempt
// on every frame. The next unlock tries again.
void CtrAchFlush(int force)
{
    unsigned int t0;
    int queued = 0;

    if (sDirty && (force || osGetTime() - sLastChangeMs >= CTR_ACH_QUIET_MS))
    {
        sDirty = 0;

        LightLock_Lock(&sPendingLock);
        sPending = sTable;
        sHavePending = 1;
        LightLock_Unlock(&sPendingLock);

        queued = 1;
    }

    // Check this even when nothing new was queued. A forced flush must also
    // complete a write that the I/O thread has not reached yet.
    if (force || !CtrIoRunning())
    {
        t0 = CtrTimeNowMs();
        CtrAchDrain();
        CtrLogSlow("achievements", t0);
    }
    else if (queued)
    {
        CtrIoWake();
    }
}
