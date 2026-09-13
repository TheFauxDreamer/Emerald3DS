// The background SD-card writer. See io_thread.c.
//
// Host-side only: nothing here crosses the seam, so it lives beside the files
// that use it rather than in 3ds/bridge.h.

#ifndef CTR_IO_THREAD_H
#define CTR_IO_THREAD_H

// Start the writer. Call once, after CtrAudioInit() has settled the main
// thread's priority, because the writer is created one step below it. Until
// this has succeeded every write stays synchronous, exactly as before.
void CtrIoInit(void);

// Run one last pass and stop the writer. Anything written afterwards goes back
// to being synchronous, so this is safe to call before the other exits.
void CtrIoExit(void);

// Non-zero while the writer is running and accepting work.
int  CtrIoRunning(void);

// Ask the writer for a pass. Cheap, never blocks, callable from any thread.
void CtrIoWake(void);

// The jobs a pass runs, each owned by the file whose state it writes. All are
// also called directly on the exit path, after the writer has stopped.
void CtrLogDrain(void);        // 3ds/host/log.c
void CtrSettingsDrain(void);   // 3ds/host/settings.c
void CtrAchDrain(void);        // 3ds/host/achievements.c

#endif // CTR_IO_THREAD_H
