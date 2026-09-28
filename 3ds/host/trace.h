// Boot tracing for a port that has no console.
//
// The game uses the top screen and the touch UI uses the bottom screen, so
// there is no console. Everything here goes through CtrLog() (3ds/host/log.c).
// It writes to svcOutputDebugString (the emulator's log; a console discards
// it). In a debug build, it also writes to sdmc:/3ds/emerald3ds/log.txt, which
// a real 3DS keeps. SystemCallAccess in 3ds/emerald3ds.rsf grants
// OutputDebugString (61).
//
// Two separate switches:
// - CTR_BOOT_DIAG (3ds/Makefile) removes the per-step tracing. CtrLog stays, so
//   the real failures still report.
// - CTR_DEBUG_MENU (3ds/bridge.h) decides what reaches the SD card. At 0, only
//   CtrLogWarn lines do, because a shared build must not write to the player's
//   card in a normal session.

#ifndef CTR_TRACE_H
#define CTR_TRACE_H

#include <3ds.h>

// Defined in 3ds/host/log.c, not inline, because that file owns the log file
// handle and its one-attempt-per-boot state.
void CtrLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// A fault that a player must be able to report. Every build writes it to the
// file, a release build too. Use it only for a failure, never for a line that
// a healthy session makes.
void CtrLogWarn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// The first call of main(). It logs the boot line, and a release build removes
// the file of an earlier session.
void CtrLogBoot(const char *stamp);

#ifndef CTR_BOOT_DIAG
#define CTR_BOOT_DIAG 1
#endif

#if CTR_BOOT_DIAG
// The same destination as CtrLog, the SD file too. A bring-up trace must
// survive a crash or a power-off, so that you can read it later.
#define CtrTrace(...) CtrLog(__VA_ARGS__)
#else
#define CtrTrace(...) ((void)0)
#endif

#endif // CTR_TRACE_H
