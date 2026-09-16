// Port settings, stored next to the save on the SD card.
//
// This is not save data. A missing, short or wrong-version file is normal, not
// an error: the defaults apply and the game boots. Nothing here can block
// startup.
//
// Writes follow the rules of save.c. A setter only marks the settings dirty,
// and the frame loop flushes. Each file operation is a blocking call to the FS
// process, and on a console the player sees the game stop. Thus many taps cost
// one write. The flush only gives a snapshot to the I/O thread
// (3ds/host/io_thread.c), which writes while the main thread waits for VBlank.
// See CtrSettingsFlush.
//
// This file does not write to a temp file and rename, as save.c does. The
// payload is 164 bytes in one sector, so a write cannot tear. The magic and
// version checks turn any bad file into "use the defaults". On a console, the
// rename alone costs 51-56 ms. Thus the file opens once and is written in
// place.

#include <3ds.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "../bridge.h"
#include "io_thread.h"
#include "trace.h"                    // CtrLog

#define SETTINGS_DIR  "sdmc:/3ds/emerald3ds"
#define SETTINGS_PATH SETTINGS_DIR "/settings.bin"

#define SETTINGS_MAGIC   0x53443345u   // 'E3DS' little endian
// The history of the versions:
// - v3 changed what a bind value means (CTR_BIND_MOD joined the speeds).
//   Discard a v2 file: it would load as "no modifier" and lose the Y default.
// - v4 added the four gameplay tweaks. A v3 file is the first 12 bytes of a v4
//   file, so migrate it with a short read. The new fields take their defaults.
// - v5 added the fast-forward audio setting, and migrates the same way. Zero is
//   CTR_FFAUDIO_NORMAL, the default.
// - v6 used v5's three padding bytes for the audio A/B switches. Same size, so
//   no migration. They store MUTED, so zero means "the normal mixer".
// - v7 added a fourth switch, which grows the struct. It migrates as a short
//   read. Zero means "not muted".
// - v8 used the first of v7's padding bytes for the phone-call switch. Same
//   size. It stores OFF, so zero means "calls occur".
// - v9 used the last two padding bytes for the quick-throw strip: the last ball
//   thrown, and whether the strip shows. Same size. Zero means "nothing thrown"
//   and (stored as OFF) "the strip shows".
// - v10 grows the struct, because v9 used the last padding byte. A v9 file is
//   the first 24 bytes of a v10 file, so it migrates as a short read. The older
//   checks below compare against SETTINGS_V9_SIZE, not sizeof(s).
// - v11 adds a table of per-save records, keyed on the trainer ID. Six values
//   move into it. An older file has one set of them, and the first save that
//   CtrSettingsAdopt() gets takes that set.
// - v12 uses one padding byte of each record for the FOLLOWER switch. Same
//   size. A v11 record has zero there, which means off.
// - v13 uses the first of the three padding bytes after battleAnimOff for the
//   DAY CARE switch, for each console. Same size. Older files have zero there,
//   which means off.
#define SETTINGS_VERSION 13

// The number of saves with their own record. When all are in use, a new save
// replaces the least recently used record, as in 3ds/host/achievements.c.
#define CTR_SETTINGS_SAVES 8

// The values of one save (v11, v12). A fixed size with every byte in use.
struct CtrSaveSettings {
    uint32_t playerId;                 // the save's full 32-bit trainer ID
    uint32_t lastUsed;                 // the table clock at the last use
    uint8_t  expAll;
    uint8_t  levelCap;                 // CTR_CAP_*
    uint8_t  randomizer;
    uint8_t  bagSort;                  // CTR_BAGSORT_*
    uint8_t  phoneCallsOff;
    uint8_t  lastBall;
    uint8_t  followerOn;               // added in v12
    uint8_t  pad;                      // explicit, always written as 0
};

// A fixed size with every byte in use, so the file layout does not depend on
// the compiler's alignment.
struct CtrSettings {
    uint32_t magic;
    uint16_t version;
    uint8_t  topScale;
    // This was padding, always written as 0. Every v3 file has a zero here,
    // which is the default of this field, so it needs no version change.
    uint8_t  showAllTabs;
    uint8_t  turbo[CTR_TURBO_COUNT];   // CTR_BIND_OFF / a speed / CTR_BIND_MOD
    // Added in v4. The fields above keep their v3 offsets, so the migration
    // below is a plain short read.
    uint8_t  expAll;
    uint8_t  levelCap;                 // CTR_CAP_*
    uint8_t  randomizer;
    uint8_t  bagSort;                  // CTR_BAGSORT_*
    // Added in v5.
    uint8_t  ffAudio;                  // CTR_FFAUDIO_*
    // Added in v6, in the three bytes that v5 kept as padding. The padding
    // stops the compiler from rounding the struct up to 4 bytes, which would
    // make settings_put() write uninitialized stack to the card.
    //
    // It stores MUTED, not enabled, so the zeros in a v5 file mean "nothing is
    // muted", the default.
    uint8_t  audioDbgMuted[CTR_AUDIO_DBG_COUNT];
    // Added in v8, in the first of the three padding bytes of v7. It stores
    // OFF, so the zero in a v7 file means "calls occur", which is what that
    // file meant.
    uint8_t  phoneCallsOff;
    // Added in v9, in the two bytes that v7 kept and v8 left. The lastBall
    // field is a raw item id. It is the one value in this struct with no range
    // check on load. The ball ids are game constants that this side cannot
    // include, so UiQuickBallItem() checks them. The quickBallOff field stores
    // OFF, as phoneCallsOff does.
    //
    // These were the last padding bytes. The size is now 22 + 2 = 24, which is
    // already aligned.
    uint8_t  lastBall;
    uint8_t  quickBallOff;
    // Added in v10, which grows the struct. The compiler would round
    // 24 + 1 = 25 bytes up to 28. Then settings_put() would write three bytes
    // of uninitialized stack to the card. Thus the padding is explicit again,
    // as in v5 and v7. It stores OFF, so zero means "animate".
    uint8_t  battleAnimOff;
    // Added in v13, in the first padding byte of v10. Zero means off: the yard
    // on Route 117 is as in the original game.
    uint8_t  dayCareYard;
    uint8_t  pad[2];
    // v11: the per-save table. The values of expAll, levelCap, randomizer,
    // bagSort, phoneCallsOff and lastBall are now in rec. Their bytes above
    // keep an older file's values until a save takes them. Otherwise they are
    // zero.
    uint32_t clock;                    // the next lastUsed value
    uint8_t  count;                    // records in use, from rec[0]
    uint8_t  pad2[3];
    struct CtrSaveSettings rec[CTR_SETTINGS_SAVES];
};

// The size of the struct in each older layout: all fields before the next
// version's new fields.
#define SETTINGS_V3_SIZE  offsetof(struct CtrSettings, expAll)
#define SETTINGS_V4_SIZE  offsetof(struct CtrSettings, ffAudio)
// Versions 5 and 6 have the same shape: 20 bytes. Only the meaning of the last
// three is different.
#define SETTINGS_V6_SIZE  (offsetof(struct CtrSettings, audioDbgMuted) + 3)
// Versions 7 to 9 are all 24 bytes: all fields before the v10 field.
#define SETTINGS_V9_SIZE  offsetof(struct CtrSettings, battleAnimOff)
// Version 10 is 28 bytes: all fields before the v11 table.
#define SETTINGS_V10_SIZE offsetof(struct CtrSettings, clock)

// If one of these fails, the compiler added padding, and settings_put() would
// write uninitialized bytes to the card. Add explicit padding.
_Static_assert(sizeof(struct CtrSaveSettings) == 16,
               "CtrSaveSettings has implicit padding");
_Static_assert(SETTINGS_V10_SIZE == 28, "the v10 fields must keep their offsets");
_Static_assert(sizeof(struct CtrSettings)
                   == SETTINGS_V10_SIZE + 8
                      + CTR_SETTINGS_SAVES * sizeof(struct CtrSaveSettings),
               "CtrSettings has implicit padding");

// Defined in video.c and main.c, which own the live values.
extern int  Ctr3dsGetTopScale(void);
extern void Ctr3dsApplyTopScale(int mode);
extern int  Ctr3dsGetTurboBind(int button);
extern void Ctr3dsApplyTurboBind(int button, int value);
extern int  Ctr3dsGetShowAllTabs(void);
extern void Ctr3dsApplyShowAllTabs(int on);
extern int  Ctr3dsGetExpAll(void);
extern void Ctr3dsApplyExpAll(int on);
extern int  Ctr3dsGetLevelCap(void);
extern void Ctr3dsApplyLevelCap(int mode);
extern int  Ctr3dsGetRandomizer(void);
extern void Ctr3dsApplyRandomizer(int on);
extern int  Ctr3dsGetBagSort(void);
extern void Ctr3dsApplyBagSort(int mode);
extern int  Ctr3dsGetFfAudio(void);
extern void Ctr3dsApplyFfAudio(int mode);
extern int  Ctr3dsGetPhoneCallsOff(void);
extern void Ctr3dsApplyPhoneCallsOff(int on);
extern int  Ctr3dsGetFollowerOn(void);
extern void Ctr3dsApplyFollowerOn(int on);
extern int  Ctr3dsGetQuickBallOff(void);
extern void Ctr3dsApplyQuickBallOff(int on);
extern int  Ctr3dsGetDayCareYard(void);
extern void Ctr3dsApplyDayCareYard(int on);
extern int  Ctr3dsGetBattleAnimOff(void);
extern void Ctr3dsApplyBattleAnimOff(int on);
extern int  Ctr3dsGetLastBall(void);
extern void Ctr3dsApplyLastBall(int item);

// The time after the last change before the write.
//
// Not save.c's 100 ms. A save is 28 calls a few milliseconds apart. Settings
// come from a finger that moves between buttons, seconds apart, so 100 ms joins
// nothing. One second of quiet makes a visit to the settings one write.
#define CTR_SETTINGS_QUIET_MS 1000

static int      sDirty;
static uint64_t sLastChangeMs;

// The file, open for the full session.
//
// A new open for each write is expensive. Create, close, delete and rename all
// change the directory, and each is a call to the FS process. A write of 164
// bytes at offset 0 of an open file changes no directory entry and touches one
// sector.
//
// It never closes, like the handle in log.c. The fflush() call pushes the
// bytes, and the process exit closes the file.
static FILE *sFile;
static int   sOpenTried;

// Make the directory once for each boot, not before each write.
//
// Two mkdir calls are two FS calls, and after the first time they only report
// "already there". CtrSettingsLoad() calls this during boot, where a pause
// costs nothing. The writer calls it too, so a write works in a build that did
// not load.
static void ensure_dir(void)
{
    static int done;

    if (done)
        return;

    done = 1;

    unsigned int t0 = CtrTimeNowMs();

    mkdir("sdmc:/3ds", 0777);
    mkdir(SETTINGS_DIR, 0777);

    CtrLogSlow("settings.mkdir", t0);
}

// Open the file once, for read and write.
//
// CtrSettingsLoad() calls this, so the open occurs during boot, where a pause
// is not visible.
//
// Try "r+b" first. It opens an existing file without truncating it, so no
// cluster is freed and allocated again. "w+b" is only for the first run. A
// first run that changes no setting leaves an empty settings.bin. The next boot
// sees a bad magic and uses the defaults, as with no file.
//
// One attempt for each boot, so a read-only card costs one failed open.
static FILE *settings_open(void)
{
    unsigned int t0;

    if (sOpenTried)
        return sFile;

    sOpenTried = 1;

    ensure_dir();

    t0 = CtrTimeNowMs();

    sFile = fopen(SETTINGS_PATH, "r+b");
    if (sFile == NULL)
        sFile = fopen(SETTINGS_PATH, "w+b");

    CtrLogSlow("settings.open", t0);

    return sFile;
}

// Queue a write. Every Ctr3dsSetFoo() calls this. Nothing touches the card
// until CtrSettingsFlush() runs from the frame loop.
void CtrSettingsMarkDirty(void)
{
    sDirty = 1;
    sLastChangeMs = osGetTime();
}

// ---- per-save values --------------------------------------------------------
//
// Six gameplay values belong to a save, not to the console. The table keeps
// one record for each save, keyed on its trainer ID. Main thread only: the I/O
// thread sees only the copy in sPending.

static uint32_t               sClock;
static uint8_t                sCount;
static struct CtrSaveSettings sRec[CTR_SETTINGS_SAVES];

// The save in play, from CtrSettingsAdopt().
static uint32_t sPlayerId;
static int      sHavePlayer;

// The values of a file older than v11, which had one set for all saves. The
// first save with no record takes them.
static struct CtrSaveSettings sLegacy;
static int                    sLegacyPending;

static int save_is_default(const struct CtrSaveSettings *v)
{
    return v->expAll == 0 && v->levelCap == CTR_CAP_OFF && v->randomizer == 0
        && v->bagSort == CTR_BAGSORT_OFF && v->phoneCallsOff == 0
        && v->lastBall == 0 && v->followerOn == 0;
}

// Read the seven live values.
static void save_get(struct CtrSaveSettings *v)
{
    v->expAll        = (uint8_t)(Ctr3dsGetExpAll() ? 1 : 0);
    v->levelCap      = (uint8_t)Ctr3dsGetLevelCap();
    v->randomizer    = (uint8_t)(Ctr3dsGetRandomizer() ? 1 : 0);
    v->bagSort       = (uint8_t)Ctr3dsGetBagSort();
    v->phoneCallsOff = (uint8_t)(Ctr3dsGetPhoneCallsOff() ? 1 : 0);
    v->lastBall      = (uint8_t)Ctr3dsGetLastBall();
    v->followerOn    = (uint8_t)(Ctr3dsGetFollowerOn() ? 1 : 0);
}

// Set the seven live values, or the defaults if v is NULL.
//
// Set the defaults first. Apply refuses a bad mode, and a bad byte must give
// the default, not the value of the save before. The lastBall field has no
// range check here: UiQuickBallItem() checks it with the game's constants.
static void save_apply(const struct CtrSaveSettings *v)
{
    Ctr3dsApplyExpAll(0);
    Ctr3dsApplyLevelCap(CTR_CAP_OFF);
    Ctr3dsApplyRandomizer(0);
    Ctr3dsApplyBagSort(CTR_BAGSORT_OFF);
    Ctr3dsApplyPhoneCallsOff(0);
    Ctr3dsApplyLastBall(0);
    Ctr3dsApplyFollowerOn(0);

    if (v == NULL)
        return;

    Ctr3dsApplyExpAll(v->expAll != 0);
    Ctr3dsApplyLevelCap(v->levelCap);
    Ctr3dsApplyRandomizer(v->randomizer != 0);
    Ctr3dsApplyBagSort(v->bagSort);
    Ctr3dsApplyPhoneCallsOff(v->phoneCallsOff != 0);
    Ctr3dsApplyLastBall(v->lastBall);
    Ctr3dsApplyFollowerOn(v->followerOn != 0);
}

static struct CtrSaveSettings *find_record(uint32_t playerId)
{
    for (unsigned i = 0; i < sCount; i++)
        if (sRec[i].playerId == playerId)
            return &sRec[i];

    return NULL;
}

// A slot for a new save: the next free slot or, if all are in use, the least
// recently used.
static struct CtrSaveSettings *claim_record(void)
{
    struct CtrSaveSettings *r;

    if (sCount < CTR_SETTINGS_SAVES)
        return &sRec[sCount++];

    r = &sRec[0];
    for (unsigned i = 1; i < CTR_SETTINGS_SAVES; i++)
        if (sRec[i].lastUsed < r->lastUsed)
            r = &sRec[i];

    return r;
}

// Copy the live values into the record of the save in play.
static void store_current(void)
{
    struct CtrSaveSettings v = { 0 };
    struct CtrSaveSettings *r;

    save_get(&v);

    r = find_record(sPlayerId);
    if (r == NULL)
    {
        // A save with only the defaults needs no record. Thus it does not
        // replace the record of a different save.
        if (save_is_default(&v))
            return;

        r = claim_record();
    }

    v.playerId = sPlayerId;
    v.lastUsed = ++sClock;
    *r = v;
}

// The game side calls this on the first overworld frame of a save (Adopt in
// 3ds/achievements.c). Apply the values of that save.
void CtrSettingsAdopt(uint32_t playerId)
{
    struct CtrSaveSettings *r;
    const char *how;

    if (sHavePlayer && playerId == sPlayerId)
        return;

    // Keep the values of the save before. This cannot occur yet, because
    // SoftReset does nothing on this port, so one boot has one save.
    if (sHavePlayer)
        store_current();

    sPlayerId = playerId;
    sHavePlayer = 1;

    r = find_record(playerId);

    if (r != NULL)
    {
        save_apply(r);
        // In memory only, as in CtrAchStoreLoad(). The next write keeps it.
        r->lastUsed = ++sClock;
        how = "its record";
    }
    else if (sLegacyPending)
    {
        // The first save after an update from v10 or older takes the old
        // values. Write them now, so the old file does not give them again.
        sLegacyPending = 0;
        save_apply(&sLegacy);
        store_current();
        CtrSettingsMarkDirty();
        how = "the values of the old file";
    }
    else
    {
        save_apply(NULL);
        how = "the defaults";
    }

    CtrLog("emerald3ds: settings: save %08lX gets %s\n",
           (unsigned long)playerId, how);
}

void CtrSettingsLoad(void)
{
    struct CtrSettings s;

    FILE *f = settings_open();
    if (f == NULL)
        return;                       // read-only or full card: defaults

    // Clear it first, so a short v3 read leaves the v4 fields at their
    // defaults.
    memset(&s, 0, sizeof(s));

    // The handle stays open, so each access sets its own position.
    if (fseek(f, 0, SEEK_SET) != 0)
        return;

    size_t n = fread(&s, 1, sizeof(s), f);

    if (s.magic != SETTINGS_MAGIC)
        return;                       // anything unexpected: keep the defaults

    // Files v11 and v12 have the same size. Their bytes for the later values
    // are zero.
    if (s.version == SETTINGS_VERSION || s.version == 12 || s.version == 11)
    {
        if (n != sizeof(s) || s.count > CTR_SETTINGS_SAVES)
            return;
    }
    else if (s.version == 10)
    {
        // At least 28 bytes, not exactly 28. The write never truncates, so a
        // v10 build that writes over a v11 file leaves the old table after its
        // bytes. The table is read only from a file of v11 or newer, so the
        // tail is ignored.
        if (n < SETTINGS_V10_SIZE)
            return;
    }
    else if (s.version == 9 || s.version == 8 || s.version == 7)
    {
        // All 24 bytes. Versions v8 and v9 only gave a meaning to bytes that v7
        // wrote as zero. Each of those fields stores the old meaning of zero.
        // Thus the three load the same, and v10 reads them as a short read.
        if (n != SETTINGS_V9_SIZE)
            return;
    }
    else if (s.version == 6 || s.version == 5)
    {
        // Both are 20 bytes. Version 5 wrote its last three bytes as zero
        // padding, and zero is "not muted" in v6. Thus the two load the same.
        if (n != SETTINGS_V6_SIZE)
            return;
    }
    else if (s.version == 4)
    {
        if (n != SETTINGS_V4_SIZE)
            return;                   // says v4, but is not v4 shaped
    }
    else if (s.version == 3)
    {
        if (n != SETTINGS_V3_SIZE)
            return;                   // says v3, but is not v3 shaped
    }
    else
    {
        return;                       // v2 or older, or from a newer version
    }

    // Check the range, and do not trust the file. A bad value would index past
    // the scale table in video.c.
    if (s.topScale < CTR_TOP_SCALE_COUNT)
        Ctr3dsApplyTopScale((int)s.topScale);

    // Any byte that is not zero means on, so a bad value cannot be out of
    // range.
    Ctr3dsApplyShowAllTabs(s.showAllTabs != 0);

    // Ctr3dsApplyTurboBind refuses a value that is not a valid bind. A bad byte
    // leaves that button at its default.
    for (int i = 0; i < CTR_TURBO_COUNT; i++)
        Ctr3dsApplyTurboBind(i, s.turbo[i]);

    // Zero for a migrated v3 or v4 file, which is CTR_FFAUDIO_NORMAL and the
    // default.
    Ctr3dsApplyFfAudio(s.ffAudio);

    // Zero for a file older than v9, which means "the strip shows".
    Ctr3dsApplyQuickBallOff(s.quickBallOff != 0);

    // Zero for a file older than v10 (the short read above keeps it zero),
    // which means "animate".
    Ctr3dsApplyBattleAnimOff(s.battleAnimOff != 0);

    // Zero for a file older than v13, which means "off".
    Ctr3dsApplyDayCareYard(s.dayCareYard != 0);

    // Zero for a file older than v6, which means "not muted" for all three.
    for (int i = 0; i < CTR_AUDIO_DBG_COUNT; i++)
        Ctr3dsApplyAudioDbg(i, s.audioDbgMuted[i] == 0);

    // The per-save values wait for CtrSettingsAdopt(). A short read of an older
    // file leaves a newer field at zero, which is its default.
    if (s.version == SETTINGS_VERSION || s.version == 12 || s.version == 11)
    {
        sClock = s.clock;
        sCount = s.count;
        memcpy(sRec, s.rec, sizeof(sRec));
    }

    sLegacy.expAll        = s.expAll;
    sLegacy.levelCap      = s.levelCap;
    sLegacy.randomizer    = s.randomizer;
    sLegacy.bagSort       = s.bagSort;
    sLegacy.phoneCallsOff = s.phoneCallsOff;
    sLegacy.lastBall      = s.lastBall;
    sLegacyPending = !save_is_default(&sLegacy);
}

// The snapshot of every setting. Take it on the main thread, where the values
// are. Nothing here touches the card.
static void settings_build(struct CtrSettings *s)
{
    // The record of the save in play gets the live values first.
    if (sHavePlayer)
        store_current();

    // Clear it first, so the padding is written as zero, not as old stack data.
    memset(s, 0, sizeof(*s));

    s->magic    = SETTINGS_MAGIC;
    s->version  = SETTINGS_VERSION;
    s->topScale = (uint8_t)Ctr3dsGetTopScale();
    s->showAllTabs = (uint8_t)(Ctr3dsGetShowAllTabs() ? 1 : 0);
    for (int i = 0; i < CTR_TURBO_COUNT; i++)
        s->turbo[i] = (uint8_t)Ctr3dsGetTurboBind(i);
    s->ffAudio    = (uint8_t)Ctr3dsGetFfAudio();
    s->quickBallOff  = (uint8_t)(Ctr3dsGetQuickBallOff() ? 1 : 0);
    s->battleAnimOff = (uint8_t)(Ctr3dsGetBattleAnimOff() ? 1 : 0);
    s->dayCareYard   = (uint8_t)(Ctr3dsGetDayCareYard() ? 1 : 0);

    for (int i = 0; i < CTR_AUDIO_DBG_COUNT; i++)
        s->audioDbgMuted[i] = (uint8_t)(Ctr3dsGetAudioDbg(i) ? 0 : 1);

    // The old per-save values stay until a save takes them. Otherwise these
    // bytes are zero, from the memset.
    if (sLegacyPending)
    {
        s->expAll        = sLegacy.expAll;
        s->levelCap      = sLegacy.levelCap;
        s->randomizer    = sLegacy.randomizer;
        s->bagSort       = sLegacy.bagSort;
        s->phoneCallsOff = sLegacy.phoneCallsOff;
        s->lastBall      = sLegacy.lastBall;
    }

    s->clock = sClock;
    s->count = sCount;
    memcpy(s->rec, sRec, sizeof(s->rec));
}

// The card part. It runs on the I/O thread during play, and on the main thread
// only when there is no I/O thread or the game closes. It always holds
// sFileLock.
//
// It reports a slow write with CtrLog, not CtrLogSlow, because CtrLogSlow's
// table is for the main thread only (3ds/host/log.c). The CtrLogSlow calls in
// settings_open() are safe: they run only on the first call, from
// CtrSettingsLoad() at boot.
static void settings_put(const struct CtrSettings *s)
{
    unsigned int t0, elapsed;

    FILE *f = settings_open();
    if (f == NULL)
        return;                       // read-only card or full card: not fatal

    t0 = CtrTimeNowMs();

    // Write in place, over the old data. The struct only grows between
    // versions, and the version field gates every read. A shorter old file is
    // extended. A longer file can only come from a downgrade, and the version
    // check refuses it.
    if (fseek(f, 0, SEEK_SET) == 0) {
        size_t n = fwrite(s, 1, sizeof(*s), f);

        // This uses fflush, not fclose: the handle stays open after this call.
        // The log (log.c) uses the same method to get a line onto the card
        // before a crash.
        if (n != sizeof(*s) || fflush(f) != 0)
            CtrLog("emerald3ds: settings write failed (%u/%u bytes)\n",
                   (unsigned)n, (unsigned)sizeof(*s));
    }

    elapsed = CtrTimeNowMs() - t0;
    if (elapsed >= 50)
        CtrLog("emerald3ds: slow settings.write %u ms\n", elapsed);
}

// The write that waits for the I/O thread, and its two locks.
//
// The two locks protect different things. The main thread holds sPendingLock
// only for a struct copy, so it never waits on the card. The writer holds
// sFileLock for the full write, the take of the pending struct too. That keeps
// the order correct. A forced write on the close path waits for a background
// write, then writes the newest settings. An older struct never overwrites a
// newer one.
//
// Both start as 1, which is what LightLock_Init() writes.
static LightLock          sPendingLock = 1;
static LightLock          sFileLock = 1;
static struct CtrSettings sPending;
static int                sHavePending;

void CtrSettingsDrain(void)
{
    struct CtrSettings s;
    int have;

    LightLock_Lock(&sFileLock);

    LightLock_Lock(&sPendingLock);
    have = sHavePending;
    if (have) {
        s = sPending;
        sHavePending = 0;
    }
    LightLock_Unlock(&sPendingLock);

    if (have)
        settings_put(&s);

    LightLock_Unlock(&sFileLock);
}

// Write the queued change, if the player has stopped changing things.
//
// Rp2350PresentFrame() calls this next to CtrSaveFlush(). The close path calls
// it with force.
//
// During play, this does not write. It takes a snapshot and gives it to the I/O
// thread (3ds/host/io_thread.c), because a write in the frame loop stops the
// game. For example, the last ball thrown is a setting, and its write would
// occur during the catch animation. The close path is the exception: the
// process can end before a background write, so a forced flush writes now.
//
// Clear sDirty even if the write failed. This is the one difference from
// save.c. A lost save is worth a retry on each frame. A lost display preference
// is not, and a read-only card would cause an FS attempt on every frame.
void CtrSettingsFlush(int force)
{
    unsigned int t0;
    int queued = 0;

    if (sDirty && (force || osGetTime() - sLastChangeMs >= CTR_SETTINGS_QUIET_MS)) {
        struct CtrSettings s;

        sDirty = 0;
        settings_build(&s);

        LightLock_Lock(&sPendingLock);
        sPending = s;
        sHavePending = 1;
        LightLock_Unlock(&sPendingLock);

        queued = 1;
    }

    // Check this even when nothing new was queued. A forced flush must also
    // complete a write that the I/O thread has not reached yet.
    if (force || !CtrIoRunning()) {
        t0 = CtrTimeNowMs();
        CtrSettingsDrain();
        CtrLogSlow("settings", t0);
    } else if (queued) {
        CtrIoWake();
    }
}
