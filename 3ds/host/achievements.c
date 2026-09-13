// The achievements store, persisted beside settings.bin on the SD card.
//
// Host side, and deliberately dumb. What an achievement is and when it unlocks
// lives game-side in 3ds/achievements.c, where the game's own accessors are;
// this file keeps the bits it is handed, one record per playthrough, and gets
// them onto the card without touching the frame.
//
// It copies settings.c's write discipline exactly, for the reasons that file's
// header records: a change marks the table dirty, CtrAchFlush() waits for a
// second of quiet and hands a snapshot to the I/O thread (io_thread.c), and the
// card write happens while the main thread waits for VBlank. An unlock lands in
// the middle of whatever earned it -- a catch, a badge, the end of a battle --
// which is exactly the moment a synchronous write would be seen.
//
// Also like settings.c, it is not save data and must never be treated like it.
// A missing, short or wrong-version file is ordinary, not an error: the table
// starts empty and the game side re-derives every achievement it can from the
// save itself (the backfill in 3ds/achievements.c). Nothing here blocks boot.
//
// And it borrows settings.c's in-place rewrite rather than save.c's
// temp-and-rename. The file is one fixed 460-byte image, a single sector, so
// there is no torn state to protect against, and the magic and version checks
// turn anything unexpected into "no records" rather than into damage.
//
// Version 1 (332 bytes) had no place bits. It is still read, and carried over
// with every place unvisited, because its unlocks are not all re-derivable:
// the shiny catch leaves nothing in the save to find again.

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

#define ACH_MAGIC    0x43413345u      // 'E3AC' little-endian
#define ACH_VERSION  2

// One playthrough. Fixed size with every byte spoken for, so the layout on the
// card does not depend on how the compiler chooses to align it.
struct CtrAchRecord
{
    uint32_t playerId;
    // The table's clock when this record was last loaded or saved. The lowest
    // is the one a new playthrough replaces once all CTR_ACH_RECORDS are used.
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

// Version 1, kept only to read. The same header, and records without places.
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

// If any of these fails the compiler added padding, and ach_put() would write
// uninitialised bytes to the card. Add explicit padding instead.
_Static_assert(sizeof(struct CtrAchRecord)
                   == 8 + 2 * CTR_ACH_BYTES + CTR_ACH_PLACE_BYTES,
               "CtrAchRecord has implicit padding");
_Static_assert(sizeof(struct CtrAchFile)
                   == 12 + CTR_ACH_RECORDS * sizeof(struct CtrAchRecord),
               "CtrAchFile has implicit padding");

// Version 1's layout is history: it must stay the 332 bytes that were written,
// and a later change to the live sizes must not reach back into it.
_Static_assert(sizeof(struct CtrAchFileV1) == 332,
               "CtrAchFileV1 must match the version 1 file");
_Static_assert(CTR_ACH_BYTES >= 16 && CTR_ACH_RECORDS >= 8,
               "a version 1 record must still fit a live one");

// Same second of quiet as settings.c's CTR_SETTINGS_QUIET_MS, for a related
// reason: a backfill or a burst of unlocks (the end of a battle can finish
// several at once) should cost one write, not one per achievement.
#define CTR_ACH_QUIET_MS 1000

// The live table. Main thread only: the game side loads and saves through it,
// and the I/O thread only ever sees the copy handed over in sPending.
static struct CtrAchFile sTable;

static int      sDirty;
static uint64_t sLastChangeMs;

// The file, held open for the session and never closed, the way settings.c
// and log.c hold theirs: fflush() pushes the bytes and process teardown closes
// it. One open attempt per boot, so a read-only card costs one failed open.
static FILE *sFile;
static int   sOpenTried;

static void table_reset(void)
{
    memset(&sTable, 0, sizeof(sTable));
    sTable.magic   = ACH_MAGIC;
    sTable.version = ACH_VERSION;
}

// The directory, made once per boot. settings.c makes the same one; each keeps
// its own flag so neither depends on the other having run first.
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

// "r+b" first so an existing file is opened without being truncated, "w+b" on
// a first run only. See settings_open() for why the order matters.
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

// A version 1 file into the freshly reset table: every record kept, every
// place unvisited. The clock carries over, so "least recently used" still
// picks the same record to replace.
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
        // places stays as table_reset() left it: zero, nowhere visited.
    }
}

// Boot. Reads the whole table once; everything after this is served from
// memory. Anything unexpected in the file leaves the table empty.
void CtrAchStoreInit(void)
{
    // Either version's image, from one read. The header is the same in both.
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

    // At least version 1's size rather than exactly it. The rewrite is in
    // place and never truncates, so a version 1 file that a build from before
    // the change wrote over a newer one keeps the newer one's tail.
    if (n >= sizeof(f.v1) && f.v1.magic == ACH_MAGIC && f.v1.version == 1
        && f.v1.count <= sizeof(f.v1.rec) / sizeof(f.v1.rec[0]))
    {
        table_from_v1(&f.v1);
        CtrLog("emerald3ds: achievements: version 1 file, %u playthrough%s carried over\n",
               (unsigned)sTable.count, sTable.count == 1 ? "" : "s");
        return;
    }

    // Unconditional, like the other "which path did we get" lines: without it
    // "no file yet" and "a file we threw away" leave identical logs.
    if (n != sizeof(f.now) || f.now.magic != ACH_MAGIC
        || f.now.version != ACH_VERSION || f.now.count > CTR_ACH_RECORDS)
    {
        CtrLog("emerald3ds: achievements: no usable file (%u bytes), starting empty\n",
               (unsigned)n);
        return;
    }

    sTable = f.now;
    // Only what the header vouches for. Whatever an unused slot holds is
    // overwritten before it is ever read.
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

// A slot for a playthrough the table has not seen: the next free one, or once
// all are used, the least recently used. Cleared, and keyed to the new ID.
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

    // Being played is being used. Not marked dirty: the new clock only matters
    // when a record has to be replaced, which is decided in memory, and it
    // reaches the card with the next unlock anyway.
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

// The card half. Runs on the I/O thread in play, and on the main thread only
// when there is no I/O thread or the game is closing, always under sFileLock.
// A slow write is reported with CtrLog rather than CtrLogSlow, whose table is
// main-thread only.
static void ach_put(const struct CtrAchFile *f)
{
    unsigned int t0, elapsed;
    FILE *fp = ach_open();

    if (fp == NULL)
        return;                       // read-only card, full card: not fatal

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

// The write waiting for the I/O thread, and the two locks around it, exactly
// as settings.c has them: sPendingLock is only ever held for a struct copy, so
// the main thread never waits on the card, and sFileLock is held across the
// whole write, which keeps an older table from landing on top of a newer one.
// Both start at 1, which is what LightLock_Init() writes.
static LightLock         sPendingLock = 1;
static LightLock         sFileLock = 1;
static struct CtrAchFile sPending;
static int               sHavePending;

void CtrAchDrain(void)
{
    static struct CtrAchFile f;       // 460 bytes: off the writer's stack
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

// Called from Rp2350PresentFrame() beside CtrSettingsFlush(), and with force
// from the close paths. Hands the table to the I/O thread once the unlocks
// have stopped for a second; writes it here and now only when forced or when
// there is no I/O thread.
//
// sDirty clears whether or not the write succeeds, the same choice settings.c
// makes: one attempt per change, so a read-only card does not turn every later
// frame into an FS attempt. The next unlock tries again.
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

    // Checked even when nothing new was queued: a forced flush must also land
    // a write the I/O thread was handed and has not reached yet.
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
