// Boot tracing for a port that has no console.
//
// Both screens belong to the game (top) and the touch UI (bottom), so there is
// nowhere to consoleInit() to. svcOutputDebugString goes to the emulator's log
// instead (Azahar/Citra print it at Debug level) and is a no-op on hardware.
// SystemCallAccess in 3ds/emerald3ds.rsf already grants OutputDebugString (61).
//
// Set CTR_BOOT_DIAG=0 in 3ds/Makefile to compile all of this away.

#ifndef CTR_TRACE_H
#define CTR_TRACE_H

#ifndef CTR_BOOT_DIAG
#define CTR_BOOT_DIAG 1
#endif

#if CTR_BOOT_DIAG

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static inline void CtrTrace(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(buf) - 1)
        n = (int)sizeof(buf) - 1;
    svcOutputDebugString(buf, n);
}

#else
#define CtrTrace(...) ((void)0)
#endif

// Always compiled, unlike CtrTrace. For the handful of conditions a user needs
// to know about even in a release build -- printf() is not an option here, so
// without this they fail silently and look like bugs in the game.
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>

static inline void CtrLog(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(buf) - 1)
        n = (int)sizeof(buf) - 1;
    svcOutputDebugString(buf, n);
}

#endif // CTR_TRACE_H
