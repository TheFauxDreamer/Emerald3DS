// The background SD-card writer.
//
// On a console, each fflush() is a blocking call to the FS process. A write in
// the frame loop stops the game for that time:
// - The profiler writes a line when a stage's window closes. A rare stage
//   closes its window on the repaint that an overlay just caused, so the write
//   lands on a frame that is already slow.
// - The settings file: the last ball thrown is a setting, and its write
//   occurred while the ball shook on the screen.
//
// Thus those writes are queued and done here. The thread runs one priority step
// below the main thread, on the same core. Thus it runs only while the main
// thread waits (for VBlank or for the rasterizer). A card write then costs the
// frame nothing.
//
// This thread does not write the save file. The file save.c commits a save
// directly when the game finishes it, because only that survives a close of the
// emulator window. A save already has the game's own "SAVING" pause.

#include <3ds.h>

#include "io_thread.h"
#include "trace.h"

// Space for newlib's stdio path and CtrLog's vsnprintf, the deepest calls of a
// pass. The buffers of a pass are statics, not locals.
#define IO_STACK_SIZE (32 * 1024)

static Thread       sThread;
static LightEvent   sWake;
static volatile int sQuit;
static volatile int sRunning;

static void io_main(void *arg)
{
    (void)arg;

    for (;;) {
        LightEvent_Wait(&sWake);

        CtrLogDrain();
        CtrSettingsDrain();
        CtrAchDrain();

        // After the pass, not before it: data that was queued with the stop
        // request still reaches the card.
        if (sQuit)
            break;
    }
}

void CtrIoInit(void)
{
    s32 prio = 0x30;

    if (sRunning)
        return;

    // One step below the caller, so the writer never takes the core from the
    // game. The caller is the main thread, after fix_thread_priority()
    // (3ds/host/audio.c) set its priority. Thus this runs after CtrAudioInit(),
    // not at the start of main().
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    prio += 1;
    if (prio > 0x3F)
        prio = 0x3F;

    LightEvent_Init(&sWake, RESET_ONESHOT);
    sQuit = 0;

    // Core 0, next to the main thread. The other cores are for the rasterizer
    // (3ds/host/video.c). A writer that is mostly blocked in FS calls gains
    // nothing from its own core.
    sThread = threadCreate(io_main, NULL, IO_STACK_SIZE, prio, 0, false);

    // Always log which path started, like the other such lines. Without it,
    // "the writer runs" and "all writes are still direct" give the same log.
    if (sThread == NULL) {
        CtrLog("emerald3ds: io thread not started; SD writes stay synchronous\n");
        return;
    }

    sRunning = 1;
    CtrLog("emerald3ds: io thread started (priority 0x%02lX)\n",
           (unsigned long)prio);
}

void CtrIoExit(void)
{
    if (!sRunning)
        return;

    sQuit = 1;
    LightEvent_Signal(&sWake);
    threadJoin(sThread, U64_MAX);
    threadFree(sThread);
    sThread = NULL;
    sRunning = 0;

    // Anything queued after the writer's last pass. With sRunning clear, any
    // later write takes the direct path, so nothing can come after these.
    CtrLogDrain();
    CtrSettingsDrain();
    CtrAchDrain();
}

int CtrIoRunning(void)
{
    return sRunning;
}

void CtrIoWake(void)
{
    if (sRunning)
        LightEvent_Signal(&sWake);
}
