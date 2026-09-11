// The background SD-card writer.
//
// Every write this port makes to the card used to happen on the main thread,
// inside a frame. On a console a single fflush() is a blocking round trip to
// the FS process, and several of them landed exactly where they hurt:
//
//   The profiler. CtrProfile() closes a stage's window after ten seconds and
//   writes a line. A stage that runs only now and then, such as `paint` or
//   `upload.bot.*`, closes its window on its first sample after a quiet
//   spell, and that sample is the repaint the battle's quick-throw strip or
//   shiny notice just caused. So the card write landed in the one frame that
//   was already over budget, and the stutter looked like the overlay's fault.
//
//   The settings file. Throwing a different kind of ball records it as the
//   last one thrown, and a second later settings.bin was rewritten while the
//   ball was still shaking on screen.
//
// So those writes are queued and done here instead. The thread sits one
// priority step below the main thread on the same core, which means it only
// ever runs while the main thread is blocked: in the VBlank wait, or waiting
// on the rasteriser. A card write then costs the frame nothing.
//
// What this does NOT take over: the save file. save.c commits a save the moment
// the game finishes writing one, synchronously, because that is what survives
// the emulator window being closed. A save already has the game's own "SAVING"
// pause, so there is no frame to protect.

#include <3ds.h>

#include "io_thread.h"
#include "trace.h"

// Room for newlib's stdio path and CtrLog's vsnprintf, which is the deepest
// thing a pass calls. The buffers a pass works on are statics, not locals.
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

        // After the pass, not before it: whatever was queued alongside the
        // request to stop still reaches the card.
        if (sQuit)
            break;
    }
}

void CtrIoInit(void)
{
    s32 prio = 0x30;

    if (sRunning)
        return;

    // One step below the caller, so the writer never takes the core away from
    // the game. The caller is the main thread, after fix_thread_priority()
    // (3ds/host/audio.c) has placed it, which is why this runs after
    // CtrAudioInit() rather than first thing in main().
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    prio += 1;
    if (prio > 0x3F)
        prio = 0x3F;

    LightEvent_Init(&sWake, RESET_ONESHOT);
    sQuit = 0;

    // Core 0, beside the main thread. The other cores are the rasteriser's
    // (3ds/host/video.c), and a writer that spends its life blocked in FS calls
    // gains nothing from a core of its own.
    sThread = threadCreate(io_main, NULL, IO_STACK_SIZE, prio, 0, false);

    // Unconditional, like every other "which path did we get" line: without it
    // "the writer is running" and "every write is still synchronous" leave
    // identical logs.
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
    // later write takes the synchronous path, so nothing can slip in behind
    // these two.
    CtrLogDrain();
    CtrSettingsDrain();
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
