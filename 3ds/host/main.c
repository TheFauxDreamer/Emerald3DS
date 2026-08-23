// 3DS entry point.
//
// AgbMain() is the game's superloop and never returns, so this file does not
// own a frame loop -- the game does. Every frame, src/main.c calls back into
// Rp2350PresentFrame() (under #if RP2350) once the frame's VRAM, palette, OAM
// and registers are final. That hook is where all 3DS work happens: rasterise,
// present, sample input, feed audio, flush saves.
//
// The one thing the 3DS needs that a GBA superloop has no concept of is a way
// out: the HOME menu can ask the application to close at any time. Since
// AgbMain() will never return on its own, the exit path longjmps back here.

#include <3ds.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../bridge.h"
#include "trace.h"

int  CtrVideoInit(void);
void CtrVideoExit(void);
void CtrVideoPresent(void);
void CtrAudioInit(void);
void CtrAudioExit(void);
void CtrAudioFrame(void);
void CtrSaveLoad(void);
void CtrSaveFlush(int force);
#if CTR_BOOT_DIAG
void CtrDiagSplash(void);
#endif

static jmp_buf sQuitJmp;
static int     sQuitting;

// GBA REG_KEYINPUT bit order. Active-low: a CLEAR bit means pressed.
#define GBA_A      (1 << 0)
#define GBA_B      (1 << 1)
#define GBA_SELECT (1 << 2)
#define GBA_START  (1 << 3)
#define GBA_RIGHT  (1 << 4)
#define GBA_LEFT   (1 << 5)
#define GBA_UP     (1 << 6)
#define GBA_DOWN   (1 << 7)
#define GBA_R      (1 << 8)
#define GBA_L      (1 << 9)
#define GBA_KEY_MASK 0x03FF

static uint16_t sample_keys(void)
{
    uint32_t k = hidKeysHeld();
    uint16_t gba = 0;

    if (k & KEY_A)      gba |= GBA_A;
    if (k & KEY_B)      gba |= GBA_B;
    if (k & KEY_SELECT) gba |= GBA_SELECT;
    if (k & KEY_START)  gba |= GBA_START;
    if (k & KEY_R)      gba |= GBA_R;
    if (k & KEY_L)      gba |= GBA_L;

    // The circle pad doubles as the d-pad: KEY_C* are the libctru-synthesised
    // digital edges, so both sticks and the pad drive the same GBA bits.
    if (k & (KEY_DRIGHT | KEY_CPAD_RIGHT)) gba |= GBA_RIGHT;
    if (k & (KEY_DLEFT  | KEY_CPAD_LEFT))  gba |= GBA_LEFT;
    if (k & (KEY_DUP    | KEY_CPAD_UP))    gba |= GBA_UP;
    if (k & (KEY_DDOWN  | KEY_CPAD_DOWN))  gba |= GBA_DOWN;

    // GBA hardware reports 1 = released.
    return (uint16_t)(~gba & GBA_KEY_MASK);
}

static void sample_touch(CtrTouchState *t)
{
    static int wasTouching;
    static touchPosition lastPos;

    touchPosition pos;
    hidTouchRead(&pos);

    uint32_t held = hidKeysHeld();
    int touching = (held & KEY_TOUCH) != 0;

    // hidScanInput() memsets the touch position every scan and only refills it
    // while the panel is actually pressed, so on the RELEASE frame hidTouchRead
    // returns (0,0) rather than the last contact point. Latch it -- otherwise
    // every tap is reported at the top-left corner, which silently made all
    // release-driven hit tests target whatever sits at (0,0).
    if (touching)
        lastPos = pos;

    t->x = (int16_t)lastPos.px;
    t->y = (int16_t)lastPos.py;
    t->touching     = (uint8_t)touching;
    t->justPressed  = (uint8_t)(touching && !wasTouching);
    // Thanks to the latch above, the coordinates on the release frame are the
    // last contact point, which is what a tap wants to act on.
    t->justReleased = (uint8_t)(!touching && wasTouching);

    wasTouching = touching;
}

#if CTR_BOOT_DIAG
// Callable from game-side code, which must never include <3ds.h> (the
// two-worlds rule in 3ds/README.md). A plain `const char *` crosses the seam
// safely -- no u8/u16/u32 and no string.h in the signature.
void CtrTraceMsg(const char *msg)
{
    CtrTrace("%s", msg);
}
#endif

// The cartridge RTC, backed by the console clock. src/siirtc.c calls this in
// place of bit-banging an S-3511A that a 3DS does not have.
//
// Cached to the second. RtcCalcLocalTime() runs from DoTimeBasedEvents() every
// frame in the overworld, and the chip only ever had one-second resolution, so
// there is nothing to gain from breaking the time down sixty times a second.
//
// localtime() rather than gmtime(): a 3DS stores the wall-clock time the user
// set, with no timezone database, so with TZ unset the two agree. localtime()
// is the one that stays correct if that ever stops being true.
void Ctr3dsGetClock(CtrClock *out)
{
    static time_t   cachedAt = (time_t)-1;
    static CtrClock cached;

    time_t now = time(NULL);

    if (now != cachedAt)
    {
        struct tm *lt = localtime(&now);

        if (lt == NULL) {
            // Should not happen, but a zeroed clock here would read as an
            // invalid month and day to RtcCheckInfo. Hand back the epoch the
            // cart RTC itself resets to.
            cached.year = 0; cached.month = 1; cached.day = 1;
            cached.dayOfWeek = 0;
            cached.hour = 0; cached.minute = 0; cached.second = 0;
        } else {
            // The chip holds a two-digit year. Wrapping keeps it inside the
            // 0..99 the BCD encoding and ConvertBcdToBinary can represent;
            // outside that the game would report an invalid-year error.
            int year = lt->tm_year + 1900 - 2000;

            cached.year      = (uint8_t)(((year % 100) + 100) % 100);
            cached.month     = (uint8_t)(lt->tm_mon + 1);
            cached.day       = (uint8_t)lt->tm_mday;
            cached.dayOfWeek = (uint8_t)lt->tm_wday;
            cached.hour      = (uint8_t)lt->tm_hour;
            cached.minute    = (uint8_t)lt->tm_min;
            cached.second    = (uint8_t)lt->tm_sec;
        }

        cachedAt = now;
    }

    *out = cached;
}

void Rp2350PresentFrame(void)
{
    // The first frames are what matter: reaching frame 1 at all rules out a
    // hang in the game's init, and a steadily rising count rules out a hang in
    // the frame loop. After that it would just spam the log.
    {
        static unsigned frame;
        frame++;
        if (frame <= 3 || frame == 60 || frame == 600)
            CtrTrace("emerald3ds: present frame %u\n", frame);
    }

    // HOME menu / power. Do this first so a close request is honoured even if
    // the frame below would misbehave.
    if (!aptMainLoop() && !sQuitting) {
        sQuitting = 1;
        longjmp(sQuitJmp, 1);
    }

    hidScanInput();

    // Buttons for the NEXT frame's ReadKeys() (src/main.c), matching how the
    // GBA's key register is sampled between frames.
    CtrSetKeyInput(sample_keys());

    CtrTouchState touch;
    sample_touch(&touch);
    CtrBottomUpdate(&touch);

    CtrAudioFrame();

    // Rasterise + present. C3D_FrameEnd blocks on VBlank, which is what paces
    // the game to 60 Hz -- no manual frame pacing needed.
    CtrVideoPresent();

    // Writes the save image out once the burst of sector writes has stopped.
    CtrSaveFlush(0);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    // Must precede everything: every VRAM/palette/OAM/register access in the
    // game derives from this block.
    CtrTrace("emerald3ds: main() entered\n");

    Ctr3dsInitGbaMemory();
    CtrTrace("emerald3ds: gba memory ready\n");
    CtrSaveLoad();
    CtrTrace("emerald3ds: save loaded\n");

    if (!CtrVideoInit()) {
        CtrTrace("emerald3ds: FATAL CtrVideoInit failed\n");
        CtrVideoExit();
        return 1;
    }
    CtrTrace("emerald3ds: video ready\n");
    // New 3DS: 804 MHz + L2 cache; no-op on Old 3DS. Done after the graphics
    // services are up, since it goes through PTM.
    osSetSpeedupEnable(true);

    CtrAudioInit();
    CtrTrace("emerald3ds: audio ready\n");
    CtrBottomInit();
    CtrTrace("emerald3ds: bottom screen ready\n");

#if CTR_BOOT_DIAG
    // Leaves a known image on screen. If it survives, the game hung; if the
    // screen stays black, nothing in this file ever ran.
    CtrDiagSplash();
#endif

    if (setjmp(sQuitJmp) == 0) {
        // If nothing after this line ever appears, the game hung inside its own
        // init -- everything above it completed.
        CtrTrace("emerald3ds: entering AgbMain\n");
        AgbMain();   // never returns; exits via longjmp above
    }

    // Unconditional flush: the deferred writeback may still be pending.
    CtrSaveFlush(1);
    CtrAudioExit();
    CtrVideoExit();
    return 0;
}
