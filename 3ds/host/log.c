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
//   Flushed per line. The interesting log is the one written immediately
//   before a crash, and a data abort takes the process out with no chance to
//   close the file. Buffered output would lose precisely the line worth having.
//
//   Truncated per boot. A log that answers "what happened this run" is worth
//   reading; an append-only one that has to be dated and scrolled is not.
//
//   Bounded. A per-frame log call would otherwise fill the card. After
//   LOG_MAX_LINES the file stops growing and says so, once.
//
// Nothing here may block startup. A read-only card, a full card and a missing
// directory are all ordinary: they cost the file and keep the game.

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>

#include "../bridge.h"
#include "trace.h"

#define LOG_DIR   "sdmc:/3ds/emerald3ds"
#define LOG_PATH  LOG_DIR "/log.txt"

// Generous for a boot log (which is a dozen lines) and far below anything that
// could matter to an SD card if a caller ever logs from a frame loop.
#define LOG_MAX_LINES 512

static FILE *sFile;
static int   sOpened;
static int   sLines;

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

// Always compiled, unlike CtrTrace. For the handful of conditions a user needs
// to know about even in a release build.
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
    // must not depend on the SD card having been writable.
    svcOutputDebugString(buf, n);

    log_open();
    if (sFile == NULL)
        return;

    if (sLines >= LOG_MAX_LINES)
        return;

    sLines++;
    if (sLines == LOG_MAX_LINES) {
        fputs("emerald3ds: log truncated (line limit reached)\n", sFile);
        fflush(sFile);
        return;
    }

    fwrite(buf, 1, (size_t)n, sFile);
    // The line matters most when the next thing that happens is a data abort,
    // which never returns here to close the file.
    fflush(sFile);
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
