// The log, for a port with nowhere to print.
//
// Both screens are spoken for -- the game owns the top, the touch UI owns the
// bottom -- so there is no console to consoleInit() to, and printf() goes
// nowhere. Until now CtrLog() went only to svcOutputDebugString(), which an
// emulator prints and a real console DISCARDS. Every "needs a DSP firmware
// dump", "linearAlloc failed", "video init failed" message was therefore
// invisible on exactly the platform where it mattered, and each one presented
// as the port silently not working.
//
// So the same line also goes to a file on the SD card. That file is the only
// way to tell "audio is disabled and here is why" apart from "audio is running
// and produced silence" without a second console and a debugger.
//
// Three properties matter more than anything else here:
//
//   Flushed per line, but not on the frame. The interesting log is the one
//   written immediately before a crash, and a data abort takes the process
//   out with no chance to close the file, so every line is still flushed on
//   its own. What changed is WHO flushes it. A flush is a blocking round trip
//   to the FS process, and the profiler's lines landed on exactly the frames
//   that were already over budget (see 3ds/host/io_thread.c). So once the I/O
//   thread is up, a line is queued in RAM and that thread writes it, a frame
//   later at most. The price is that a crash can lose the last frame's worth
//   of lines. Boot, before the thread exists, is still written synchronously,
//   and a boot hang is where the log matters most.
//
//   Truncated per boot. A log that answers "what happened this run" is worth
//   reading; an append-only one that has to be dated and scrolled is not.
//
//   Bounded. A per-frame log call would otherwise fill the card. After
//   LOG_MAX_LINES the file stops growing and says so, once.
//
// Nothing here may block startup. A read-only card, a full card and a missing
// directory are all ordinary: they cost the file and keep the game.
//
// THE FILE HALF IS COMPILED OUT OF A SHIPPING BUILD, keyed on CTR_DEBUG_MENU
// (3ds/bridge.h) so there stays exactly one switch to throw before sharing a
// build. A build someone else runs should not create files on their card or
// spend an SD write per line, and everything above is written for the person
// developing the port rather than for the person playing it.
//
// svcOutputDebugString stays in every build. It costs nothing, it reaches an
// emulator's log, and a console discards it -- so it is free to leave in.
//
// One message does lose its only destination on a console: the missing
// sdmc:/3ds/dspfirm.cdc warning. That one is about the recipient's SD card
// rather than about this port -- no build can carry a DSP dump -- so its home
// is README.md's Limitations section, not a log they would have to be told to
// go and read.

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "../bridge.h"
#include "io_thread.h"
#include "trace.h"

#if CTR_DEBUG_MENU

#define LOG_DIR   "sdmc:/3ds/emerald3ds"
#define LOG_PATH  LOG_DIR "/log.txt"

// Generous for a boot log (which is a dozen lines) and far below anything that
// could matter to an SD card if a caller ever logs from a frame loop.
#define LOG_MAX_LINES 512

// Lines waiting for the I/O thread. Linear rather than a ring: each pass takes
// the whole of it, so there is never a tail to wrap. 8 KB is thirty-odd of the
// longest lines, far more than one frame produces even when every profiler
// stage reports at once.
#define LOG_QUEUE_SIZE 8192

static FILE *sFile;
static int   sOpened;
static int   sLines;

// Guards the queue and the line count, and is never held across an FS call.
// Initialised statically to 1, which is what LightLock_Init() writes, because
// the first line is logged before anything could call it.
static LightLock sQueueLock = 1;
static char      sQueue[LOG_QUEUE_SIZE];
static int       sQueueLen;
static unsigned  sDropped;

static const char kTruncated[] = "emerald3ds: log truncated (line limit reached)\n";

static void log_open(void)
{
    if (sOpened)
        return;

    sOpened = 1;   // set first: one attempt per boot, whether or not it works

    mkdir("sdmc:/3ds", 0777);
    mkdir(LOG_DIR, 0777);

    // "w", not "a": this file describes this boot.
    sFile = fopen(LOG_PATH, "w");
}

// The only two callers are the main thread while the I/O thread is not running,
// and the I/O thread's pass, so the file is never written from two threads.
static void log_write(const char *buf, int n)
{
    log_open();
    if (sFile == NULL)
        return;

    fwrite(buf, 1, (size_t)n, sFile);
    // The line matters most when the next thing that happens is a data abort,
    // which never returns here to close the file.
    fflush(sFile);
}

static void log_to_file(const char *buf, int n)
{
    // The line limit is decided here, at the moment of logging, whichever path
    // the line then takes. Counting at write time instead would let a burst
    // queued faster than it drains overshoot the cap.
    LightLock_Lock(&sQueueLock);

    if (sLines >= LOG_MAX_LINES) {
        LightLock_Unlock(&sQueueLock);
        return;
    }

    sLines++;
    if (sLines == LOG_MAX_LINES) {
        buf = kTruncated;
        n = (int)sizeof(kTruncated) - 1;
    }

    if (!CtrIoRunning()) {
        LightLock_Unlock(&sQueueLock);
        log_write(buf, n);
        return;
    }

    // Dropped rather than waited for: a caller on the frame must never block on
    // the card, which is the whole reason for the queue. The count is written
    // out with the next pass, so a gap is never silent.
    if (sQueueLen + n > LOG_QUEUE_SIZE) {
        sDropped++;
    } else {
        memcpy(sQueue + sQueueLen, buf, (size_t)n);
        sQueueLen += n;
    }

    LightLock_Unlock(&sQueueLock);
    CtrIoWake();
}

void CtrLogDrain(void)
{
    // Static rather than on the writer's stack. Only the I/O thread calls this
    // while it runs, and the exit path calls it only after joining it.
    static char chunk[LOG_QUEUE_SIZE];
    int n;
    unsigned dropped;

    LightLock_Lock(&sQueueLock);
    n = sQueueLen;
    memcpy(chunk, sQueue, (size_t)n);
    sQueueLen = 0;
    dropped = sDropped;
    sDropped = 0;
    LightLock_Unlock(&sQueueLock);

    if (n == 0 && dropped == 0)
        return;

    log_open();
    if (sFile == NULL)
        return;

    if (n > 0)
        fwrite(chunk, 1, (size_t)n, sFile);
    if (dropped > 0)
        fprintf(sFile, "emerald3ds: %u log lines dropped (queue full)\n", dropped);
    fflush(sFile);
}

#else   // !CTR_DEBUG_MENU: shipping build, nothing reaches the card

static void log_to_file(const char *buf, int n) { (void)buf; (void)n; }
void CtrLogDrain(void) {}

#endif

// Always compiled, unlike CtrTrace: the conditions this reports still reach an
// emulator's log in every build. Whether they also reach the SD card is what
// CTR_DEBUG_MENU decides -- see the note at the top of this file.
void CtrLog(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 0)
        return;
    if (n > (int)sizeof(buf) - 1)
        n = (int)sizeof(buf) - 1;

    // Unconditional, and first: under an emulator this is the live view, and it
    // must not depend on the SD card having been writable -- or, now, on the SD
    // card being written at all.
    svcOutputDebugString(buf, n);

    log_to_file(buf, n);
}

// ---- stall diagnostics ------------------------------------------------------
//
// See the comment on these in 3ds/bridge.h. Two rules shape what is written:
//
//   Only overruns. A line per stage per frame would be the log's whole budget
//   spent on the frames where nothing was wrong, and would itself cost an SD
//   write per frame -- measuring the fault by causing it.
//
//   Capped per stage. One pathological stage must not push every other stage's
//   evidence past LOG_MAX_LINES. Each gets SLOW_MAX lines and then says it has
//   stopped, once, so a missing line is never mistaken for a fast stage.

// Three frames at 60 Hz. Below this a stage is merely expensive; above it the
// player sees the game stop.
#define SLOW_MS      50
#define SLOW_MAX     8
#define SLOW_STAGES  12

unsigned int CtrTimeNowMs(void)
{
    // Truncated to 32 bits on purpose: the callers only ever subtract two of
    // these, and unsigned arithmetic carries that across the wrap correctly.
    return (unsigned int)osGetTime();
}

// ---- stage profiling -------------------------------------------------------
//
// CtrLogSlow above only speaks when a stage exceeds SLOW_MS, which is the right
// shape for a stall and the wrong one for a cost. The bottom screen's repaint
// costs about a whole VBlank and never trips a 50 ms threshold, so it was
// invisible to every tool in this file while setting the frame rate.
//
// Same pointer-keyed table as CtrLogSlow, and for the same reason: every caller
// passes a string literal, so identical stages share an address and this stays
// a handful of compares on a path that runs several times a frame.
//
// Reported every PROFILE_PERIOD samples of a given stage rather than every
// frame: one line per stage per ten seconds keeps the report inside
// LOG_MAX_LINES while still averaging over enough samples to resolve a stage
// far below the clock's own granularity.
//
// MAIN THREAD ONLY, and so is CtrLogSlow below: both keep unlocked static
// tables. The rasteriser worker and the I/O thread measure their own time and
// leave the reporting to the main thread (see ppu.wait in 3ds/host/video.c).
#define PROFILE_STAGES  24
#define PROFILE_PERIOD  600
// ...or this long, whichever comes first. 600 samples is ten seconds for a
// stage that runs every frame and TWO MINUTES for one that runs five times a
// second, which is how `paint` -- the single most important number here --
// managed to be absent from a whole log while every per-frame stage reported
// twelve times. A rare stage has to be able to speak.
#define PROFILE_MAX_TICKS ((unsigned long long)SYSCLOCK_ARM11 * 10)

unsigned long long CtrTicksNow(void)
{
    return svcGetSystemTick();
}

void CtrProfile(const char *stage, unsigned long long startTicks)
{
    static const char        *sName[PROFILE_STAGES];
    static unsigned long long sTotal[PROFILE_STAGES];
    static unsigned long long sWorst[PROFILE_STAGES];
    static unsigned long long sSince[PROFILE_STAGES];   // window opened at
    static unsigned           sSamples[PROFILE_STAGES];
    static int                sUsed;

    unsigned long long now = CtrTicksNow();
    unsigned long long elapsed = now - startTicks;
    int i;

    for (i = 0; i < sUsed; i++)
        if (sName[i] == stage)
            break;

    if (i == sUsed) {
        if (sUsed == PROFILE_STAGES)
            return;             // more stages than expected: drop the newcomer
        sName[sUsed++] = stage;
        sSince[i] = now;
    }

    sTotal[i] += elapsed;
    sSamples[i]++;
    if (elapsed > sWorst[i])
        sWorst[i] = elapsed;

    if (sSamples[i] < PROFILE_PERIOD && now - sSince[i] < PROFILE_MAX_TICKS)
        return;

    {
        // Microseconds, computed in ticks and converted once. SYSCLOCK_ARM11 is
        // 268111856, so dividing by 268 is a microsecond to within 0.04%, and
        // doing it here rather than per sample keeps the accumulator exact.
        unsigned long long meanUs  = sTotal[i] / sSamples[i] / (SYSCLOCK_ARM11 / 1000000);
        unsigned long long worstUs = sWorst[i] / (SYSCLOCK_ARM11 / 1000000);

        CtrLog("emerald3ds: prof %s mean %llu us worst %llu us over %u\n",
               stage, meanUs, worstUs, sSamples[i]);
    }

    sTotal[i] = 0;
    sWorst[i] = 0;
    sSamples[i] = 0;
    sSince[i] = now;
}

void CtrLogSlow(const char *stage, unsigned int startMs)
{
    // Keyed on the POINTER, not on strcmp: every caller passes a string
    // literal, so identical stages share an address and the table stays a
    // handful of compares on a path that runs every frame.
    static const char *sStage[SLOW_STAGES];
    static unsigned    sCount[SLOW_STAGES];
    static int         sUsed;

    unsigned int elapsed = CtrTimeNowMs() - startMs;
    int i;

    if (elapsed < SLOW_MS)
        return;

    for (i = 0; i < sUsed; i++)
        if (sStage[i] == stage)
            break;

    if (i == sUsed) {
        if (sUsed == SLOW_STAGES)
            return;             // more stages than expected: drop the newcomer
        sStage[sUsed++] = stage;
    }

    sCount[i]++;

    if (sCount[i] > SLOW_MAX + 1)
        return;

    if (sCount[i] == SLOW_MAX + 1) {
        CtrLog("emerald3ds: slow %s - further overruns not logged\n", stage);
        return;
    }

    CtrLog("emerald3ds: slow %s %u ms\n", stage, elapsed);
}
