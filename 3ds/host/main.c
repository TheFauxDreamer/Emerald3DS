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
void CtrSettingsLoad(void);
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

// Fast-forward state. Deliberately not persisted: booting straight into 4x
// because of a setting left on days ago would be a nasty surprise.
static int sSpeed     = 1;  // game frames per displayed frame, in effect now
static int sBaseSpeed = 1;  // what GAME SPEED chose; turbo overrides it while held
static int sSubFrame;       // 0 .. sSpeed-1, wraps on the displayed frame

// What each button is bound to: CTR_BIND_OFF, a speed, or CTR_BIND_MOD.
// Indexed by CTR_TURBO_*.
//
// Y defaults to the modifier so the touch UI's jump-by-5 works out of the box.
// One value per button is what makes turbo and the modifier mutually exclusive:
// binding a speed to a button necessarily stops it being the modifier, and
// there is no combination that quietly does both.
static uint8_t sTurbo[CTR_TURBO_COUNT] = {
    [CTR_TURBO_Y] = CTR_BIND_MOD,
};

// The 3DS keys the GBA has no use for. Order must match CTR_TURBO_*.
static const uint32_t kTurboKeys[CTR_TURBO_COUNT] = {
    KEY_X, KEY_Y, KEY_ZL, KEY_ZR
};

void CtrSettingsSave(void);   // 3ds/host/settings.c

// Lowering the speed must restart the group, or a counter left above the new
// limit stalls presentation for a frame. Shared by every path that changes it.
static void set_speed(int multiplier)
{
    if (multiplier < CTR_SPEED_MIN) multiplier = CTR_SPEED_MIN;
    if (multiplier > CTR_SPEED_MAX) multiplier = CTR_SPEED_MAX;

    if (multiplier != sSpeed) {
        sSpeed = multiplier;
        sSubFrame = 0;
    }
}

void Ctr3dsSetSpeed(int multiplier)
{
    set_speed(multiplier);
    sBaseSpeed = sSpeed;   // the menu sets the RESTING speed
}

int Ctr3dsGetSpeed(void)
{
    // Reports the baseline, not the momentary override, so the EXTRA tab keeps
    // showing what the player chose while a turbo button is held.
    return sBaseSpeed;
}

// Set without persisting, for CtrSettingsLoad(). Writing the file back during
// the load that produced it would be pointless churn, and would turn a
// read-only SD card into a write attempt on every boot. Mirrors
// Ctr3dsApplyTopScale in video.c.
static int bind_is_valid(int value)
{
    return value == CTR_BIND_OFF || value == CTR_BIND_MOD
        || (value >= CTR_SPEED_MIN && value <= CTR_SPEED_MAX);
}

void Ctr3dsApplyTurboBind(int button, int value)
{
    if (button < 0 || button >= CTR_TURBO_COUNT || !bind_is_valid(value))
        return;

    sTurbo[button] = (uint8_t)value;
}

void Ctr3dsSetTurboBind(int button, int value)
{
    int before = Ctr3dsGetTurboBind(button);

    Ctr3dsApplyTurboBind(button, value);

    if (Ctr3dsGetTurboBind(button) != before)
        CtrSettingsSave();
}

int Ctr3dsGetTurboBind(int button)
{
    if (button < 0 || button >= CTR_TURBO_COUNT)
        return 0;

    return sTurbo[button];
}

// Whether the bottom screen shows tabs the save has not unlocked. Lives here
// rather than game-side because it has to survive a relaunch, and the settings
// file is host-side; bottom_screen.c reads it through the bridge.
static uint8_t sShowAllTabs;

// Set without persisting, for CtrSettingsLoad(), the same split as
// Ctr3dsApplyTurboBind above.
void Ctr3dsApplyShowAllTabs(int on)
{
    sShowAllTabs = on ? 1 : 0;
}

void Ctr3dsSetShowAllTabs(int on)
{
    int before = sShowAllTabs;

    Ctr3dsApplyShowAllTabs(on);

    if (sShowAllTabs != before)
        CtrSettingsSave();
}

int Ctr3dsGetShowAllTabs(void)
{
    return sShowAllTabs;
}

// Gameplay tweaks (EXTRA page 2). Same Apply/Set/Get split as everything above:
// Apply mutates without persisting so CtrSettingsLoad() can use it, Set
// persists, Get reads. The two enums are range-checked in Apply rather than
// trusted, exactly as Ctr3dsApplyTurboBind rejects an invalid bind: a corrupt
// settings byte must leave the default standing, not select a mode that does
// not exist.
//
// Game-side code reads these through 3ds/tweaks.c, which is the only
// translation unit that turns them into behaviour.
static uint8_t sExpAll;
static uint8_t sLevelCap;    // CTR_CAP_*
static uint8_t sRandomizer;
static uint8_t sBagSort;     // CTR_BAGSORT_*

void Ctr3dsApplyExpAll(int on)
{
    sExpAll = on ? 1 : 0;
}

void Ctr3dsSetExpAll(int on)
{
    int before = sExpAll;

    Ctr3dsApplyExpAll(on);

    if (sExpAll != before)
        CtrSettingsSave();
}

int Ctr3dsGetExpAll(void)
{
    return sExpAll;
}

void Ctr3dsApplyLevelCap(int mode)
{
    if (mode != CTR_CAP_OFF && mode != CTR_CAP_SOFT && mode != CTR_CAP_HARD)
        return;

    sLevelCap = (uint8_t)mode;
}

void Ctr3dsSetLevelCap(int mode)
{
    int before = sLevelCap;

    Ctr3dsApplyLevelCap(mode);

    if (sLevelCap != before)
        CtrSettingsSave();
}

int Ctr3dsGetLevelCap(void)
{
    return sLevelCap;
}

void Ctr3dsApplyRandomizer(int on)
{
    sRandomizer = on ? 1 : 0;
}

void Ctr3dsSetRandomizer(int on)
{
    int before = sRandomizer;

    Ctr3dsApplyRandomizer(on);

    if (sRandomizer != before)
        CtrSettingsSave();
}

int Ctr3dsGetRandomizer(void)
{
    return sRandomizer;
}

void Ctr3dsApplyBagSort(int mode)
{
    if (mode != CTR_BAGSORT_OFF && mode != CTR_BAGSORT_TYPE && mode != CTR_BAGSORT_NAME)
        return;

    sBagSort = (uint8_t)mode;
}

void Ctr3dsSetBagSort(int mode)
{
    int before = sBagSort;

    Ctr3dsApplyBagSort(mode);

    if (sBagSort != before)
        CtrSettingsSave();
}

int Ctr3dsGetBagSort(void)
{
    return sBagSort;
}

// Modifier for the touch UI. Read at the same point as everything else, so it
// is the same fresh hidScanInput() the touch state came from.
int Ctr3dsUiModifierHeld(void)
{
    uint32_t held = hidKeysHeld();

    for (int i = 0; i < CTR_TURBO_COUNT; i++)
        if (sTurbo[i] == CTR_BIND_MOD && (held & kTurboKeys[i]))
            return 1;

    return 0;
}

// Fastest bound button currently held, else the resting speed. Fastest rather
// than first so holding two never gives the slower of the two, which would feel
// like the binding had been ignored.
static int effective_speed(uint32_t held)
{
    int best = 0;

    // CTR_BIND_MOD is deliberately outside the speed range, so a modifier
    // button can never be mistaken for a very fast turbo.
    for (int i = 0; i < CTR_TURBO_COUNT; i++)
        if (sTurbo[i] != CTR_BIND_OFF && sTurbo[i] != CTR_BIND_MOD
            && (held & kTurboKeys[i]) && sTurbo[i] > best)
            best = sTurbo[i];

    return best ? best : sBaseSpeed;
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

    // HOME menu / power. Every game frame, not just displayed ones, so a close
    // request is still honoured promptly while fast-forwarding.
    if (!aptMainLoop() && !sQuitting) {
        sQuitting = 1;
        // Before the longjmp, not only after it. This is the last moment the
        // app is certainly still alive, and the flush costs nothing when the
        // image is clean. main() flushes again after the jump; the second call
        // returns immediately because this one cleared the dirty flag.
        CtrSaveFlush(1);
        longjmp(sQuitJmp, 1);
    }

    // Input is physical: it can only change once per DISPLAYED frame, because
    // that is the rate the hardware updates at. Sampling it once per game frame
    // instead would be worse than pointless -- hidScanInput() computes press
    // edges by diffing against the previous scan, so the extra scans would
    // consume the edge and sample_touch()/CtrBottomUpdate() would miss taps.
    //
    // Holding the value across the group is also what makes fast-forward feel
    // right: one real press becomes one JOY_NEW followed by held frames.
    static uint16_t keys;
    if (sSubFrame == 0) {
        hidScanInput();
        keys = sample_keys();

        // Turbo is resolved here, with the rest of the input, because this is
        // the only point in the group where the button state is fresh.
        set_speed(effective_speed(hidKeysHeld()));

        CtrTouchState touch;
        sample_touch(&touch);
        CtrBottomUpdate(&touch);
    }

    // Buttons for the NEXT frame's ReadKeys() (src/main.c), matching how the
    // GBA's key register is sampled between frames.
    CtrSetKeyInput(keys);

    // Every game frame. The mixer produces a frame's worth of samples and drops
    // them when the ring is full, which is exactly the fast-forward case: audio
    // stays at pitch and thins out rather than blocking and undoing the speedup.
    CtrAudioFrame();

    if (++sSubFrame >= sSpeed) {
        sSubFrame = 0;

        // Rasterise + present. C3D_FrameEnd blocks on VBlank, which is what
        // paces the game to 60 Hz. Skipping this call is the whole mechanism:
        // it drops the software rasterise (the expensive part) and the pacing
        // together, so the intermediate frames cost only game logic.
        CtrVideoPresent();

        // Writes the save image out once the burst of sector writes has stopped.
        CtrSaveFlush(0);
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    // First line of the session, and the reason the log file exists at all: on
    // a console svcOutputDebugString goes nowhere, so without a line on the SD
    // card there is no way to tell "the port never started" from "the port
    // started and something later went wrong". See 3ds/host/log.c.
    CtrLog("emerald3ds: boot (built " __DATE__ " " __TIME__ ")\n");

    // Must precede everything: every VRAM/palette/OAM/register access in the
    // game derives from this block.
    CtrTrace("emerald3ds: main() entered\n");

    Ctr3dsInitGbaMemory();
    CtrTrace("emerald3ds: gba memory ready\n");
    CtrSaveLoad();

    // Display preferences. Before CtrVideoInit() so the very first frame is
    // already at the scale the player chose, with no visible snap.
    CtrSettingsLoad();
    CtrTrace("emerald3ds: save loaded\n");

    if (!CtrVideoInit()) {
        // CtrLog, not CtrTrace: this is the one failure that ends the run, so
        // it has to survive into a release build's log.
        CtrLog("emerald3ds: FATAL CtrVideoInit failed\n");
        CtrVideoExit();
        return 1;
    }
    CtrTrace("emerald3ds: video ready\n");
    // New 3DS: 804 MHz + L2 cache; no-op on Old 3DS. Done after the graphics
    // services are up, since it goes through PTM.
    osSetSpeedupEnable(true);

    // Says for itself whether audio came up, and why not when it did not: a
    // missing sdmc:/3ds/dspfirm.cdc is the usual answer and is not fatal.
    CtrAudioInit();
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

    // Unconditional flush: the deferred writeback may still be pending. Usually
    // a no-op by now, because saves commit as they happen (CtrSaveCommit, from
    // src/save.c) and the close path above flushes too. It stays as the last
    // line of defence for writes that arrived outside a save.
    CtrSaveFlush(1);
    CtrAudioExit();
    CtrVideoExit();
    return 0;
}
