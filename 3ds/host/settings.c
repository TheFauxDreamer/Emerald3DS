// Port settings, persisted beside the save on the SD card.
//
// This is NOT save data and must never be treated like it. A missing, short,
// truncated or wrong-version file is an ordinary situation, not an error: the
// defaults apply and the game boots. Nothing here is allowed to block startup.
//
// Kept separate from save.c because the two have different contents, but they
// now share its WRITE DISCIPLINE, and the reason is worth recording. This used
// to write the file synchronously from the setter, i.e. from inside
// CtrBottomUpdate() in the middle of a game frame, on the grounds that a
// handful of bytes written on a button tap was too rare for a debounce to earn
// its keep. That reasoning counted bytes and not FS calls: the sequence below
// is seven or more blocking round trips to the FS process however small the
// payload, and on a console -- unlike in an emulator -- the player sees the
// game stop for it. So the setters mark dirty and the frame loop flushes,
// exactly as save.c does, and a burst of taps costs one write.
//
// Since then the flush itself moved off the frame as well: the frame loop only
// hands the snapshot to the I/O thread (3ds/host/io_thread.c), which does the
// card write while the main thread waits for VBlank. See CtrSettingsFlush.
//
// It does NOT borrow save.c's write-to-temp-then-rename discipline, and that is
// the second thing measurement changed. That dance exists so an interrupted
// write cannot leave a half-written file, which is worth six FS round trips for
// a 128 KB image spanning hundreds of sectors. This payload is 24 bytes: it
// lands in a single sector, so there is no torn state to protect against, and
// the magic and version checks below turn anything unexpected into "use the
// defaults" rather than into damage. On a console the remove-and-rename half
// measured 51-56 ms on its own -- paid, per tap, to protect a display
// preference from resetting. So the file is opened once and rewritten in place.

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

#define SETTINGS_MAGIC   0x53443345u   // 'E3DS' little-endian
// v3 changed what a binding VALUE means (CTR_BIND_MOD joined the speeds), so a
// v2 file must be discarded rather than reinterpreted: it would otherwise load
// as "nothing is the modifier" and silently lose the Y default.
//
// v4 appended the four gameplay tweaks. Nothing about the v3 fields changed, so
// unlike v2 a v3 file is MIGRATED rather than discarded: it is exactly the
// leading 12 bytes of a v4 file, and throwing it away would silently reset the
// player's top scale and turbo binds as the price of adding an unrelated
// option. The new fields take their defaults, which is what a v3 file means.
//
// v5 appended the fast-forward audio preference, and migrates the same way. A
// zero there is CTR_FFAUDIO_NORMAL, which is the default anyway, so an older
// file loads as exactly what it meant.
//
// v6 claimed v5's three explicit padding bytes for the audio A/B switches, so
// it is the same SIZE as v5 and needs no migration at all: every v5 file has
// zeros there, and the sense is inverted (these store MUTED, not enabled)
// precisely so that zero keeps meaning "the normal mixer".
//
// v7 added a fourth switch, which does grow the struct. It migrates as a short
// read like v3 and v4 did, and the byte it adds means "not muted" when zero, so
// a v6 file still loads as the normal mixer.
//
// v8 claimed the first of v7's three explicit padding bytes for the phone-call
// switch, so like v6 before it it is the same SIZE as its predecessor and needs
// no migration beyond accepting the older version number. Every v7 file has a
// zero there, and the field stores OFF rather than on precisely so that zero
// keeps meaning "calls happen", which is what those files meant.
//
// v9 claimed the LAST TWO padding bytes for the quick-throw strip: the ball the
// player last threw, and whether the strip is shown at all. Same size again, so
// again no migration beyond accepting v8. Both mean the right thing at zero --
// "nothing thrown yet", and (stored as OFF) "the strip is shown".
//
// There is now no padding left. That is not a problem in itself, because 24 is
// already a multiple of the struct's 4-byte alignment and the compiler adds
// nothing, but it does mean the NEXT field to be added grows the struct and
// must bring explicit padding back with it -- see the comment on the fields
// themselves for what that padding is actually protecting against.
// v10 is the first version since v5 to GROW the struct: v9 took the last
// padding byte, so there was nowhere left to claim. That makes it a SHORT-READ
// migration (v3, v4, v7) rather than the same-size accept v6, v8 and v9 used --
// a v9 file is exactly the leading 24 bytes of a v10 one, and the new field
// takes its default. Note the older accepts below now compare against
// SETTINGS_V9_SIZE rather than sizeof(s), which has moved.
#define SETTINGS_VERSION 10

// Fixed-size, with every byte spoken for, so the on-disk layout does not depend
// on how the compiler chooses to align it.
struct CtrSettings {
    uint32_t magic;
    uint16_t version;
    uint8_t  topScale;
    // Was explicit padding, always written as 0. Claiming it costs no version
    // bump precisely because of that: every v3 file in existence has a zero
    // here, which is the same as the default this field wants.
    uint8_t  showAllTabs;
    uint8_t  turbo[CTR_TURBO_COUNT];   // CTR_BIND_OFF / a speed / CTR_BIND_MOD
    // Appended in v4. Everything above keeps its v3 offset, which is what makes
    // the migration below a plain short read rather than a conversion.
    uint8_t  expAll;
    uint8_t  levelCap;                 // CTR_CAP_*
    uint8_t  randomizer;
    uint8_t  bagSort;                  // CTR_BAGSORT_*
    // Appended in v5.
    uint8_t  ffAudio;                  // CTR_FFAUDIO_*
    // v6, in the three bytes v5 reserved as explicit padding. They exist at all
    // because without them the compiler would round the struct to its 4-byte
    // alignment itself and settings_put() would write uninitialised stack to
    // the card; v5 said they were somewhere for v6 to go, and this is v6.
    //
    // Stores MUTED rather than enabled, so the zeros a v5 file already has mean
    // "nothing is muted", which is the default and what that file meant.
    uint8_t  audioDbgMuted[CTR_AUDIO_DBG_COUNT];
    // v8, in the first of the three bytes v7 reserved as explicit padding, the
    // same trick v6 played on v5. Stores OFF rather than on, so the zero a v7
    // file already has means "calls happen" -- the behaviour that file had.
    uint8_t  phoneCallsOff;
    // v9, in the two bytes v7 reserved and v8 left. lastBall is a raw item id
    // and is the one value in this struct NOT range-checked on load: the ball
    // ids are game constants this side may not include, so UiQuickBallItem()
    // checks them where they are defined. quickBallOff stores OFF for the same
    // reason phoneCallsOff does.
    //
    // These were the last of the explicit padding, which existed because
    // without it the compiler would round the struct up to its 4-byte alignment
    // itself and settings_put() would write uninitialised stack to the card.
    // 22 + 2 is 24, which is already aligned, so nothing implicit is added --
    // but a v10 field would take the struct to 25 and that hazard comes
    // straight back. Pad explicitly again when it does.
    uint8_t  lastBall;
    uint8_t  quickBallOff;
    // v10, and the struct grows for it. 24 + 1 is 25, which the compiler would
    // round up to 28 on its own and settings_put() would then put three bytes
    // of uninitialised stack on the card -- so explicit padding comes back,
    // exactly as v5 and v7 kept it. Stores OFF, so zero means "animate".
    uint8_t  battleAnimOff;
    uint8_t  pad[3];
};

// How much of the struct each older layout fills: everything up to the fields
// the next version appended.
#define SETTINGS_V3_SIZE  offsetof(struct CtrSettings, expAll)
#define SETTINGS_V4_SIZE  offsetof(struct CtrSettings, ffAudio)
// v5 and v6 are the same shape as each other: 20 bytes, differing only in what
// the last three mean.
#define SETTINGS_V6_SIZE  (offsetof(struct CtrSettings, audioDbgMuted) + 3)
// v7 through v9 are all 24 bytes: everything up to the field v10 appended.
#define SETTINGS_V9_SIZE  offsetof(struct CtrSettings, battleAnimOff)

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
extern int  Ctr3dsGetQuickBallOff(void);
extern void Ctr3dsApplyQuickBallOff(int on);
extern int  Ctr3dsGetBattleAnimOff(void);
extern void Ctr3dsApplyBattleAnimOff(int on);
extern int  Ctr3dsGetLastBall(void);
extern void Ctr3dsApplyLastBall(int item);

// How long after the last change to write.
//
// Deliberately NOT save.c's 100 ms, though this was matched to it at first. The
// two are debouncing different things: a save burst is 28 mechanical hook calls
// milliseconds apart, so 100 ms coalesces all of them, while settings arrive
// from a finger moving between buttons seconds apart, so 100 ms coalesces
// nothing at all. One pass through the EXTRA tab measured nine separate writes.
// A second of quiet is what actually turns "browsing the settings" into one.
#define CTR_SETTINGS_QUIET_MS 1000

static int      sDirty;
static uint64_t sLastChangeMs;

// The file, held open for the session.
//
// Reopening per write is what made this expensive: creating, closing, deleting
// and renaming all mutate the directory, and each is its own round trip to the
// FS process. Rewriting 24 bytes at offset 0 of an already-open file changes no
// directory entry and touches one sector.
//
// Never closed, exactly as log.c never closes its own handle: fflush() is what
// pushes the bytes, and process teardown is what closes it.
static FILE *sFile;
static int   sOpenTried;

// The directory, made once per boot rather than before every write.
//
// Two mkdir calls is two FS round trips, and after the first one they can only
// ever report "already there". Called from CtrSettingsLoad() so it lands during
// boot where a pause costs nothing, and from the writer as well so a write
// still works in a build that never loaded.
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

// Open the file once, for both reading and writing.
//
// Called from CtrSettingsLoad() so the open lands during boot, where a pause is
// invisible, rather than on the first setting the player touches.
//
// "r+b" first, and the order matters: it opens an existing file WITHOUT
// truncating it, so no cluster is freed and reallocated. "w+b" is the first-run
// path only. On a first run that never changes a setting this leaves a zero-byte
// settings.bin behind, which the next boot reads as a magic mismatch and
// ignores -- the same outcome as no file at all.
//
// One attempt per boot, whether or not it works, so a read-only card costs one
// failed open rather than one per write.
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

// Queue a write. This is what every Ctr3dsSetFoo() calls; nothing touches the
// card until CtrSettingsFlush() runs from the frame loop.
void CtrSettingsMarkDirty(void)
{
    sDirty = 1;
    sLastChangeMs = osGetTime();
}

void CtrSettingsLoad(void)
{
    struct CtrSettings s;

    FILE *f = settings_open();
    if (f == NULL)
        return;                       // read-only or full card: defaults stand

    // Zeroed first so a short v3 read leaves the v4 fields at their defaults
    // rather than at whatever was on the stack.
    memset(&s, 0, sizeof(s));

    // The handle stays open for the session, so every access sets its own
    // position rather than inheriting one.
    if (fseek(f, 0, SEEK_SET) != 0)
        return;

    size_t n = fread(&s, 1, sizeof(s), f);

    if (s.magic != SETTINGS_MAGIC)
        return;                       // anything unexpected: keep the defaults

    if (s.version == SETTINGS_VERSION)
    {
        if (n != sizeof(s))
            return;
    }
    else if (s.version == 9 || s.version == 8 || s.version == 7)
    {
        // All 24 bytes -- v8 and v9 only gave meaning to bytes v7 wrote as zero
        // padding, and each of those fields stores the value zero already meant:
        // "calls happen", "nothing thrown yet", "the strip is shown". So the
        // three load identically, and v10 reads them as a short read.
        if (n != SETTINGS_V9_SIZE)
            return;
    }
    else if (s.version == 6 || s.version == 5)
    {
        // Both are 20 bytes. v5 wrote its last three as padding and v6 gave
        // them meaning, but v5 always wrote zero and zero is "not muted", so
        // the two load identically here.
        if (n != SETTINGS_V6_SIZE)
            return;
    }
    else if (s.version == 4)
    {
        if (n != SETTINGS_V4_SIZE)
            return;                   // claims v4 but is not v4 shaped
    }
    else if (s.version == 3)
    {
        if (n != SETTINGS_V3_SIZE)
            return;                   // claims v3 but is not v3 shaped
    }
    else
    {
        return;                       // v2 or older, or from the future
    }

    // Range-check rather than trust the file: a value out of range would index
    // past the scale table in video.c.
    if (s.topScale < CTR_TOP_SCALE_COUNT)
        Ctr3dsApplyTopScale((int)s.topScale);

    // Any non-zero byte means on, so a corrupt value cannot be out of range.
    Ctr3dsApplyShowAllTabs(s.showAllTabs != 0);

    // Ctr3dsApplyTurboBind rejects anything that is not a valid binding, so a
    // corrupt byte leaves that button on its default rather than being trusted.
    for (int i = 0; i < CTR_TURBO_COUNT; i++)
        Ctr3dsApplyTurboBind(i, s.turbo[i]);

    // Zero for a migrated v3 file, which is the default for all four.
    // Ctr3dsApplyLevelCap and Ctr3dsApplyBagSort reject an out-of-range mode,
    // so a corrupt byte leaves that option off rather than being trusted.
    Ctr3dsApplyExpAll(s.expAll != 0);
    Ctr3dsApplyLevelCap(s.levelCap);
    Ctr3dsApplyRandomizer(s.randomizer != 0);
    Ctr3dsApplyBagSort(s.bagSort);

    // Zero for a migrated v3 or v4 file, which is CTR_FFAUDIO_NORMAL and also
    // the default, so an older file loads as exactly what it meant.
    Ctr3dsApplyFfAudio(s.ffAudio);

    // Zero for anything older than v8, which is "calls happen" -- vanilla, and
    // what every file written before this option existed meant.
    Ctr3dsApplyPhoneCallsOff(s.phoneCallsOff != 0);

    // Zero for anything older than v9: the strip is shown, and no ball has been
    // thrown yet. lastBall is passed through UNCHECKED, which is the one
    // exception to this function's "range-check rather than trust" rule and is
    // deliberate: the range is FIRST_BALL..LAST_BALL in
    // include/constants/items.h, a game header this translation unit may not
    // include, so the check belongs where those constants are. A corrupt byte
    // reaches UiQuickBallItem(), fails its test, and the strip falls back to
    // the first ball in the pocket -- which is what an empty memory does too.
    Ctr3dsApplyQuickBallOff(s.quickBallOff != 0);
    Ctr3dsApplyLastBall(s.lastBall);

    // Zero for anything older than v10 -- the short read above left it that way
    // -- which is "animate", the behaviour those files had.
    Ctr3dsApplyBattleAnimOff(s.battleAnimOff != 0);

    // Zero for anything older than v6, which is "not muted" for all three.
    for (int i = 0; i < CTR_AUDIO_DBG_COUNT; i++)
        Ctr3dsApplyAudioDbg(i, s.audioDbgMuted[i] == 0);
}

// The snapshot of every setting, taken on the main thread because that is where
// all of the values live. Nothing here touches the card.
static void settings_build(struct CtrSettings *s)
{
    // Zeroed first so the padding above is written as zero rather than as
    // whatever the stack held.
    memset(s, 0, sizeof(*s));

    s->magic    = SETTINGS_MAGIC;
    s->version  = SETTINGS_VERSION;
    s->topScale = (uint8_t)Ctr3dsGetTopScale();
    s->showAllTabs = (uint8_t)(Ctr3dsGetShowAllTabs() ? 1 : 0);
    for (int i = 0; i < CTR_TURBO_COUNT; i++)
        s->turbo[i] = (uint8_t)Ctr3dsGetTurboBind(i);
    s->expAll     = (uint8_t)(Ctr3dsGetExpAll() ? 1 : 0);
    s->levelCap   = (uint8_t)Ctr3dsGetLevelCap();
    s->randomizer = (uint8_t)(Ctr3dsGetRandomizer() ? 1 : 0);
    s->bagSort    = (uint8_t)Ctr3dsGetBagSort();
    s->ffAudio    = (uint8_t)Ctr3dsGetFfAudio();
    s->phoneCallsOff = (uint8_t)(Ctr3dsGetPhoneCallsOff() ? 1 : 0);
    s->quickBallOff  = (uint8_t)(Ctr3dsGetQuickBallOff() ? 1 : 0);
    s->battleAnimOff = (uint8_t)(Ctr3dsGetBattleAnimOff() ? 1 : 0);
    s->lastBall      = (uint8_t)Ctr3dsGetLastBall();

    for (int i = 0; i < CTR_AUDIO_DBG_COUNT; i++)
        s->audioDbgMuted[i] = (uint8_t)(Ctr3dsGetAudioDbg(i) ? 0 : 1);
}

// The card half. Runs on the I/O thread in play and on the main thread only when
// there is no I/O thread or the game is closing, always under sFileLock.
//
// It reports a slow write with a plain CtrLog rather than CtrLogSlow, because
// CtrLogSlow's table is main-thread only (3ds/host/log.c). settings_open()'s own
// CtrLogSlow calls are safe here: they run only on its first call, which is
// CtrSettingsLoad() at boot.
static void settings_put(const struct CtrSettings *s)
{
    unsigned int t0, elapsed;

    FILE *f = settings_open();
    if (f == NULL)
        return;                       // read-only card, full card: not fatal

    t0 = CtrTimeNowMs();

    // In place, over whatever is already there. The struct only ever grows
    // across versions and the version field gates every read, so a shorter
    // older file is simply extended and a longer newer one could only come from
    // a downgrade, where the version check rejects it before the tail matters.
    if (fseek(f, 0, SEEK_SET) == 0) {
        size_t n = fwrite(s, 1, sizeof(*s), f);

        // fflush, not fclose: the handle outlives this call. This is the same
        // thing log.c relies on to get a line onto the card before a crash.
        if (n != sizeof(*s) || fflush(f) != 0)
            CtrLog("emerald3ds: settings write failed (%u/%u bytes)\n",
                   (unsigned)n, (unsigned)sizeof(*s));
    }

    elapsed = CtrTimeNowMs() - t0;
    if (elapsed >= 50)
        CtrLog("emerald3ds: slow settings.write %u ms\n", elapsed);
}

// The write waiting for the I/O thread, and the two locks around it.
//
// Two locks, because they protect against different things. sPendingLock is
// only ever held for a struct copy, so the main thread can hand over a write
// without ever waiting on the card. sFileLock is held across the whole write,
// taking the pending struct included, which is what keeps the order right: a
// forced write on the closing path waits for a background write already in
// flight, then writes the newest settings, and an older struct can never land
// on top of a newer one.
//
// Both are initialised statically to 1, which is what LightLock_Init() writes.
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

// Write the queued change out, if the player has stopped changing things.
//
// Called from Rp2350PresentFrame() beside CtrSaveFlush(), and with force from
// the close path.
//
// In play this no longer writes anything itself. It snapshots the settings and
// hands them to the I/O thread (3ds/host/io_thread.c), because this call sits
// in the frame loop and a settings write lands at the worst moment there is:
// the last ball thrown is a setting, so it fell about a second after the throw,
// in the middle of the catch animation. The close path is the exception, since
// the process may be gone before a background write happens, so a forced flush
// writes here and now.
//
// sDirty is cleared whether or not the write succeeded, which is the one place
// this deliberately differs from save.c. Losing a save is worth retrying every
// frame for; losing a display preference is not, and a read-only card would
// otherwise turn one tap into an FS attempt on every frame for the rest of the
// session. This matches what the old write-on-the-tap code did: one attempt.
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

    // Checked even when nothing new was queued: a forced flush must also land a
    // write the I/O thread was handed and has not reached yet.
    if (force || !CtrIoRunning()) {
        t0 = CtrTimeNowMs();
        CtrSettingsDrain();
        CtrLogSlow("settings", t0);
    } else if (queued) {
        CtrIoWake();
    }
}
