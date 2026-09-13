// The background SD-card writer. See io_thread.c.
//
// Host side only. Nothing here crosses the seam, so it is next to the files
// that use it, not in 3ds/bridge.h.

#ifndef CTR_IO_THREAD_H
#define CTR_IO_THREAD_H

// Start the writer. Call once, after CtrAudioInit() sets the main thread's
// priority, because the writer runs one step below it. Until this succeeds,
// every write is direct.
void CtrIoInit(void);

// Run one last pass and stop the writer. After this, writes are direct again,
// so it is safe to call before the other exits.
void CtrIoExit(void);

// Not zero while the writer runs and accepts work.
int  CtrIoRunning(void);

// Ask the writer for a pass. Cheap, never blocks, and any thread can call it.
void CtrIoWake(void);

// The jobs of a pass, each owned by the file whose state it writes. The exit
// path also calls each one directly, after the writer stops.
void CtrLogDrain(void);        // 3ds/host/log.c
void CtrSettingsDrain(void);   // 3ds/host/settings.c
void CtrAchDrain(void);        // 3ds/host/achievements.c

#endif // CTR_IO_THREAD_H
