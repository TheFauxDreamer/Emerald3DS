// The log, for a port that has no place to print.
//
// The game uses the top screen and the touch UI uses the bottom screen. Thus
// there is no console, and printf() goes nowhere. The svcOutputDebugString()
// call reaches an emulator's log, but a real console discards it. Thus each
// line also goes to a file on the SD card. Without it, "audio is off, for this
// reason" and "audio runs and makes silence" look the same.
//
// Three rules:
// - Flush each line, but not on the frame. A crash takes the process with no
//   chance to close the file, so each line is flushed. After the I/O thread
//   starts, a line waits in RAM and that thread writes it, at most one frame
//   later (see 3ds/host/io_thread.c). A crash can thus lose the last frame of
//   lines. Boot lines are still written directly.
// - Truncate on each boot. The file tells what occurred in this run.
// - Limit the size. After LOG_MAX_LINES the file stops and says so once. Thus a
//   log call in a frame loop cannot fill the card.
//
// Nothing here can block startup. A read-only card, a full card or a missing
// directory costs the file, not the game.
//
// A release build does not have the file part. CTR_DEBUG_MENU (3ds/bridge.h)
// controls it, so one switch prepares a build to share. A player's build must
// not make files on their card or write to the SD card for each line.
//
// The svcOutputDebugString call stays in every build. It costs nothing and a
// console discards it.
//
// On a console, the missing sdmc:/3ds/dspfirm.cdc warning thus has no
// destination. It is about the player's SD card, and no build can carry a DSP
// dump. The Limitations section of README.md explains it.

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

// Large for a boot log, which is about a dozen lines. Much too small to harm an
// SD card if a caller logs from a frame loop.
#define LOG_MAX_LINES 512

// Lines that wait for the I/O thread. Linear, not a ring: each pass takes all
// of it, so there is no tail to wrap. The 8 KB buffer holds more than thirty of
// the longest lines. That is more than one frame makes, even when every
// profiler stage reports at once.
#define LOG_QUEUE_SIZE 8192

static FILE *sFile;
static int   sOpened;
static int   sLines;

// Protects the queue and the line count. Never held across an FS call. It
// starts as 1 (what LightLock_Init() writes), because the first line comes
// before any call could be made.
static LightLock sQueueLock = 1;
static char      sQueue[LOG_QUEUE_SIZE];
static int       sQueueLen;
static unsigned  sDropped;

static const char kTruncated[] = "emerald3ds: log truncated (line limit reached)\n";

static void log_open(void)
{
    if (sOpened)
        return;

    sOpened = 1;   // set first: one attempt for each boot

    mkdir("sdmc:/3ds", 0777);
    mkdir(LOG_DIR, 0777);

    // "w", not "a": this file describes this boot.
    sFile = fopen(LOG_PATH, "w");
}

// Only two callers: the main thread while the I/O thread is not running, and
// the I/O thread's pass. Thus two threads never write the file at the same
// time.
static void log_write(const char *buf, int n)
{
    log_open();
    if (sFile == NULL)
        return;

    fwrite(buf, 1, (size_t)n, sFile);
    // The line matters most when a data abort comes next, which never returns
    // here to close the file.
    fflush(sFile);
}

static void log_to_file(const char *buf, int n)
{
    // Decide the line limit here, when the line is logged, on either path. A
    // count at write time would let a fast burst go past the limit.
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

    // Drop the line; do not wait. A caller on the frame must never wait for the
    // card, which is why the queue exists. The next pass writes the count of
    // dropped lines, so a gap is always visible.
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
    // Static, not on the writer's stack. Only the I/O thread calls this while
    // it runs, and the exit path calls it only after the join.
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

#else   // !CTR_DEBUG_MENU: release build, nothing goes to the card

static void log_to_file(const char *buf, int n) { (void)buf; (void)n; }
void CtrLogDrain(void) {}

#endif

// Always compiled, unlike CtrTrace. These conditions reach an emulator's log in
// every build. CTR_DEBUG_MENU decides if they also reach the SD card (see the
// note at the top of this file).
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

    // Always, and first. Under an emulator this is the live view. It must not
    // depend on the SD card, or on a write to it.
    svcOutputDebugString(buf, n);

    log_to_file(buf, n);
}

// ---- stall diagnostics ------------------------------------------------------
//
// See the comment on these in 3ds/bridge.h. Two rules:
// - Only overruns. A line for each stage on each frame would fill the log with
//   normal frames, and would cost an SD write on each frame.
// - A limit for each stage. One bad stage must not push the other stages past
//   LOG_MAX_LINES. Each gets SLOW_MAX lines, then says once that it stopped.
//   Thus a missing line never looks like a fast stage.

// Three frames at 60 Hz. Below this, a stage is only expensive. Above it, the
// player sees the game stop.
#define SLOW_MS      50
#define SLOW_MAX     8
#define SLOW_STAGES  12

unsigned int CtrTimeNowMs(void)
{
    // Cut to 32 bits: the callers only subtract two of these, and unsigned
    // arithmetic handles the wrap correctly.
    return (unsigned int)osGetTime();
}

// ---- stage profiling -------------------------------------------------------
//
// CtrLogSlow above reports only a stage over SLOW_MS. That finds a stall, not a
// cost. A cost below 50 ms each frame can still set the frame rate, and only
// this profiler shows it.
//
// The table uses the pointer as the key, like CtrLogSlow. Every caller gives a
// string literal, so the same stage has the same address.
//
// Report every PROFILE_PERIOD samples of a stage, not every frame. One line for
// each stage every ten seconds stays inside LOG_MAX_LINES, and the average is
// finer than the clock's granularity.
//
// Main thread only, like CtrLogSlow below: both use static tables with no lock.
// The rasterizer worker and the I/O thread measure their own time and the main
// thread reports it (see ppu.wait in 3ds/host/video.c).
#define PROFILE_STAGES  24
#define PROFILE_PERIOD  600
// Or this time, whichever comes first. For a stage that runs every frame, 600
// samples is ten seconds. For a stage that runs five times a second, it is two
// minutes. A rare stage must also report.
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
    static unsigned long long sSince[PROFILE_STAGES];   // start of the window
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
            return;             // more stages than expected: drop the new one
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
        // Microseconds, from ticks, converted once. SYSCLOCK_ARM11 is
        // 268111856, so a division by 268 gives microseconds within 0.04%.
        // Here, not for each sample, so the sum stays exact.
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
    // The key is the pointer, not strcmp. Every caller gives a string literal,
    // so the same stage has the same address, and the table search is a few
    // compares.
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
            return;             // more stages than expected: drop the new one
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
