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
#define SETTINGS_VERSION 7

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
    // alignment itself and settings_write() would write uninitialised stack to
    // the card; v5 said they were somewhere for v6 to go, and this is v6.
    //
    // Stores MUTED rather than enabled, so the zeros a v5 file already has mean
    // "nothing is muted", which is the default and what that file meant.
    uint8_t  audioDbgMuted[CTR_AUDIO_DBG_COUNT];
    // Explicit again for the same reason v5's was: four bytes at offset 17 puts
    // the struct at 21, which the compiler would round to 24 by itself and
    // settings_write() would then write three bytes of uninitialised stack.
    uint8_t  pad[3];
};

// How much of the struct each older layout fills: everything up to the fields
// the next version appended.
#define SETTINGS_V3_SIZE  offsetof(struct CtrSettings, expAll)
#define SETTINGS_V4_SIZE  offsetof(struct CtrSettings, ffAudio)
// v5 and v6 are the same shape as each other: 20 bytes, differing only in what
// the last three mean.
#define SETTINGS_V6_SIZE  (offsetof(struct CtrSettings, audioDbgMuted) + 3)

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

    // Zero for anything older than v6, which is "not muted" for all three.
    for (int i = 0; i < CTR_AUDIO_DBG_COUNT; i++)
        Ctr3dsApplyAudioDbg(i, s.audioDbgMuted[i] == 0);
}

static void settings_write(void)
{
    struct CtrSettings s;
    unsigned int t0;

    // Zeroed first so the padding above is written as zero rather than as
    // whatever the stack held.
    memset(&s, 0, sizeof(s));

    s.magic    = SETTINGS_MAGIC;
    s.version  = SETTINGS_VERSION;
    s.topScale = (uint8_t)Ctr3dsGetTopScale();
    s.showAllTabs = (uint8_t)(Ctr3dsGetShowAllTabs() ? 1 : 0);
    for (int i = 0; i < CTR_TURBO_COUNT; i++)
        s.turbo[i] = (uint8_t)Ctr3dsGetTurboBind(i);
    s.expAll     = (uint8_t)(Ctr3dsGetExpAll() ? 1 : 0);
    s.levelCap   = (uint8_t)Ctr3dsGetLevelCap();
    s.randomizer = (uint8_t)(Ctr3dsGetRandomizer() ? 1 : 0);
    s.bagSort    = (uint8_t)Ctr3dsGetBagSort();
    s.ffAudio    = (uint8_t)Ctr3dsGetFfAudio();

    for (int i = 0; i < CTR_AUDIO_DBG_COUNT; i++)
        s.audioDbgMuted[i] = (uint8_t)(Ctr3dsGetAudioDbg(i) ? 0 : 1);

    FILE *f = settings_open();
    if (f == NULL)
        return;                       // read-only card, full card: not fatal

    t0 = CtrTimeNowMs();

    // In place, over whatever is already there. The struct only ever grows
    // across versions and the version field gates every read, so a shorter
    // older file is simply extended and a longer newer one could only come from
    // a downgrade, where the version check rejects it before the tail matters.
    if (fseek(f, 0, SEEK_SET) == 0) {
        size_t n = fwrite(&s, 1, sizeof(s), f);

        // fflush, not fclose: the handle outlives this call. This is the same
        // thing log.c relies on to get a line onto the card before a crash.
        if (n != sizeof(s) || fflush(f) != 0)
            CtrLog("emerald3ds: settings write failed (%u/%u bytes)\n",
                   (unsigned)n, (unsigned)sizeof(s));
    }

    CtrLogSlow("settings.write", t0);
}

// Write the queued change out, if the player has stopped changing things.
//
// Called from Rp2350PresentFrame() beside CtrSaveFlush(), which is the frame's
// designated point for touching the card, and with force from the close path.
//
// sDirty is cleared whether or not the write succeeded, which is the one place
// this deliberately differs from save.c. Losing a save is worth retrying every
// frame for; losing a display preference is not, and a read-only card would
// otherwise turn one tap into an FS attempt on every frame for the rest of the
// session. This matches what the old write-on-the-tap code did: one attempt.
void CtrSettingsFlush(int force)
{
    unsigned int t0;

    if (!sDirty)
        return;
    if (!force && osGetTime() - sLastChangeMs < CTR_SETTINGS_QUIET_MS)
        return;

    sDirty = 0;

    t0 = CtrTimeNowMs();
    settings_write();
    CtrLogSlow("settings", t0);
}
