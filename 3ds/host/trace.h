// Boot tracing for a port that has no console.
//
// Both screens belong to the game (top) and the touch UI (bottom), so there is
// nowhere to consoleInit() to. Everything here goes through CtrLog()
// (3ds/host/log.c), which writes to svcOutputDebugString -- the emulator's log,
// discarded on a console -- and, in a debug build only, to
// sdmc:/3ds/emerald3ds/log.txt, which is the only one of the two a real 3DS can
// show you afterwards.
// SystemCallAccess in 3ds/emerald3ds.rsf already grants OutputDebugString (61).
//
// Two switches, and they are not the same one:
//
//   CTR_BOOT_DIAG (3ds/Makefile) compiles the per-step tracing away. CtrLog
//   itself stays, so the handful of real failures still report.
//
//   CTR_DEBUG_MENU (3ds/bridge.h) decides whether ANY of it reaches the SD
//   card. At 0 the log file is not created at all, because a build you hand to
//   someone else should not write to their card.

#ifndef CTR_TRACE_H
#define CTR_TRACE_H

#include <3ds.h>

// Implemented in 3ds/host/log.c rather than inline, because it owns the log
// file handle and the one-attempt-per-boot state behind it.
void CtrLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifndef CTR_BOOT_DIAG
#define CTR_BOOT_DIAG 1
#endif

#if CTR_BOOT_DIAG
// Same destination as CtrLog, including the SD file: a bring-up build's whole
// point is that the trace survives to be read after the console has crashed or
// been powered off.
#define CtrTrace(...) CtrLog(__VA_ARGS__)
#else
#define CtrTrace(...) ((void)0)
#endif

#endif // CTR_TRACE_H
