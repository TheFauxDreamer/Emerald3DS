// The 3DS entry point.
//
// AgbMain() is the game's main loop and never returns, so the game owns the
// frame loop. Each frame, src/main.c calls Rp2350PresentFrame() (under #if
// RP2350) when the frame's VRAM, palette, OAM and registers are final. All 3DS
// work occurs in that hook: rasterize, present, read input, feed audio and
// flush saves.
//
// The HOME menu can ask the application to close at any time. AgbMain() never
// returns by itself, so the exit path uses a longjmp back to here.

#include <3ds.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../bridge.h"
#include "io_thread.h"
#include "trace.h"

// The file 3ds/Makefile gives these values. The defaults let this file build
// alone. A missing value shows as "unknown" in the log, not as an old value.
#ifndef CTR_BUILD_STAMP
#define CTR_BUILD_STAMP "build stamp unknown"
#endif
#ifndef CTR_BUILD_ID
#define CTR_BUILD_ID "unknown"
#endif

// For the title screen's corner (3ds/ui/ui_title.c). It is here, not on the
// game side, because make rebuilds this file each time, so the value is never
// stale.
const char *Ctr3dsBuildId(void)
{
    return CTR_BUILD_ID;
}

int  CtrVideoInit(void);
void CtrVideoExit(void);
void CtrVideoRenderBegin(void);
void CtrVideoPresent(void);
unsigned long long CtrVideoLastWaitTicks(void);
void CtrVideoSetFrameVBlanks(int n);
void CtrAudioInit(void);
void CtrAudioExit(void);
void CtrAudioFrame(void);
void CtrSaveLoad(void);
void CtrSaveFlush(int force);
void CtrSettingsLoad(void);
void CtrSettingsFlush(int force);
void CtrAchStoreInit(void);
void CtrAchFlush(int force);
void CtrLinkExit(void);
#if CTR_BOOT_DIAG
void CtrDiagSplash(void);
#endif

static jmp_buf sQuitJmp;
static int     sQuitting;

// The bit order of the GBA's REG_KEYINPUT. Active low: a clear bit means
// pressed.
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

    // The circle pad also works as the d-pad. KEY_C* are the digital edges that
    // libctru makes, so the sticks and the pad set the same GBA bits.
    if (k & (KEY_DRIGHT | KEY_CPAD_RIGHT)) gba |= GBA_RIGHT;
    if (k & (KEY_DLEFT  | KEY_CPAD_LEFT))  gba |= GBA_LEFT;
    if (k & (KEY_DUP    | KEY_CPAD_UP))    gba |= GBA_UP;
    if (k & (KEY_DDOWN  | KEY_CPAD_DOWN))  gba |= GBA_DOWN;

    // The GBA hardware reports 1 for released.
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

    // Each hidScanInput() call clears the touch position, and fills it only
    // while the panel is pressed. Thus, on the release frame,
    // hidTouchRead returns (0,0) and not the last contact point. Keep the last
    // point here. Without this, every tap acts at the top-left corner.
    if (touching)
        lastPos = pos;

    t->x = (int16_t)lastPos.px;
    t->y = (int16_t)lastPos.py;
    t->touching     = (uint8_t)touching;
    t->justPressed  = (uint8_t)(touching && !wasTouching);
    // With the latch above, the release frame has the last contact point, which
    // is where a tap acts.
    t->justReleased = (uint8_t)(!touching && wasTouching);

    wasTouching = touching;
}

// Game-side code can call this, and it must never include <3ds.h> (the
// two-worlds rule in 3ds/bridge.h). `const char *` and `unsigned int` cross the
// seam safely: there is no u8/u16/u32 and no string.h in the signature.
//
// This is not behind CTR_BOOT_DIAG, unlike CtrTraceMsg below. The only way to
// find which function pointer is null is to print them, and that must work in
// the build that crashes.
void CtrTraceHex(const char *label, unsigned int value)
{
    CtrLog("emerald3ds: %s = %08X\n", label, value);
}

#if CTR_BOOT_DIAG
void CtrTraceMsg(const char *msg)
{
    CtrTrace("%s", msg);
}
#endif

// The cartridge RTC, from the console clock. The code in src/siirtc.c calls
// this in place of the S-3511A chip, which a 3DS does not have.
//
// Cached to the second. RtcCalcLocalTime() runs from DoTimeBasedEvents() on
// each frame in the overworld, and the chip has one-second resolution.
//
// Use localtime(), not gmtime(). A 3DS stores the local time that the user set,
// with no timezone data. Thus the two agree when TZ is not set. Only
// localtime() stays correct if that changes.
void Ctr3dsGetClock(CtrClock *out)
{
    static time_t   cachedAt = (time_t)-1;
    static CtrClock cached;

    time_t now = time(NULL);

    if (now != cachedAt)
    {
        struct tm *lt = localtime(&now);

        if (lt == NULL) {
            // This must not occur. A zero clock gives an invalid month and day
            // to RtcCheckInfo, so return the date that the cartridge RTC resets
            // to.
            cached.year = 0; cached.month = 1; cached.day = 1;
            cached.dayOfWeek = 0;
            cached.hour = 0; cached.minute = 0; cached.second = 0;
        } else {
            // The chip holds a two-digit year. The wrap keeps the value in the
            // 0..99 range of the BCD encoding and ConvertBcdToBinary. Outside
            // that range, the game reports an invalid year.
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

// Fast-forward state. It does not persist: a boot at 4x from an old setting is
// a bad surprise.
static int sSpeed     = 1;  // game frames for each displayed frame, now
static int sBaseSpeed = 1;  // the GAME SPEED choice; turbo overrides it
static int sSubFrame;       // 0 .. sSpeed-1, wraps on the displayed frame

// ---- the display divider ---------------------------------------------------
//
// An Old 3DS cannot always fit a frame into 16.6 ms, and the way it fails looks
// worse than it measures: the work crosses the line and comes back, so the
// period alternates between one VBlank and two and the picture judders. A
// steady half rate reads far better than an irregular full one.
//
// Engaged, the game still runs one frame for each iteration and keeps its 60 Hz
// rate, but only every second iteration renders and presents. The pair needs
// one sync point, not two, and the one it has does the whole job: the render
// frame's C3D_FrameBegin lands on the first VBlank boundary after the PAIR's
// work is done, so two game frames take exactly two VBlanks. The skipped frame
// must therefore NOT wait for a VBlank of its own. Adding one there was the
// first attempt and it gives three VBlanks for each pair, 20 Hz, whenever the
// render overruns, which is exactly when the divider is on.
//
// The one thing this relies on is that a pair's work always exceeds one VBlank.
// Below that the single sync point would pace the pair to one VBlank and the
// game would run at double speed. The thresholds keep a margin of nearly two
// against it, and DIV_PAIR_FLOOR_TICKS catches a scene that collapses between
// decisions, a fade to black for one.
//
// The signal is what a PAIR would cost: two game frames and one render. That
// matters: the obvious signal, frames that missed VBlank, reads zero as soon as
// the divider engages, so the divider could never let go again. This one does
// not change when it engages, because the mean game frame is measured over
// every frame and the mean render over presenting frames only, and the divider
// changes neither.
//
// It must be the PAIR, not one frame, because that is the quantity every other
// test here is about, and measuring two different things is what made this
// oscillate. The engage threshold used to be 16 ms of single-frame work, which
// is BELOW one VBlank: a scene at 16.1 ms fits a VBlank and needs no divider at
// all, yet it engaged, then measured its pair at 18.1 ms against a floor of
// 18.2, let go at once, and engaged again a second later. A console log caught
// eleven of those in one session, several back to back, and a display flipping
// between 30 and 60 Hz once a second looks far worse than either rate held.
//
// So all three thresholds below are pair spans, and they nest:
//
//   PANIC 16.9 < OFF/FLOOR 17.5 < ON 18.5 ms,  one VBlank being 16.71 ms
//
// Engage above ON, let go below OFF, and the gap between them is the
// hysteresis. A pair under one VBlank would be paced to one VBlank by the
// single sync point and run the game at double speed, which PANIC sits just
// above. Retune against a log, not against a guess.
#define DIV_WINDOW 60         // presenting frames between decisions

#define DIV_PAIR_ON_TICKS    ((unsigned long long)SYSCLOCK_ARM11 / 54)
#define DIV_PAIR_OFF_TICKS   ((unsigned long long)SYSCLOCK_ARM11 / 57)

// The same level as OFF, for the collapse that happens between decisions: a
// fade to black must not wait out the rest of the second.
#define DIV_PAIR_FLOOR_TICKS DIV_PAIR_OFF_TICKS

// Nearly one VBlank. Here the single sync point really would pace the pair to
// one VBlank and run the game at double speed, so this cannot be waited out.
#define DIV_PAIR_PANIC_TICKS ((unsigned long long)SYSCLOCK_ARM11 / 59)

// Pairs below the floor before letting go, unless the panic level says now.
//
// Without this the escape had no hysteresis at all: engaging took 60 rendered
// frames and letting go took one skipped frame. An Old 3DS sits exactly where
// that matters, because the floor was only a mean game frame above it: it
// engaged at an estimate of 16.1 ms, measured a pair at 18.1 against a floor of
// 18.2, let go at once, and engaged again a second later. A console log caught
// eleven of those in one session, several back to back, and a display flipping
// between 30 and 60 Hz once a second looks far worse than either rate.
//
// Six pairs is a fifth of a second at half rate. A scene that has genuinely
// collapsed stays collapsed for far longer than that, and one that is merely
// sitting on the floor never gets six in a row.
#define DIV_ESCAPE_PAIRS 6

static int sDivide;    // 1 when every second frame skips the render
static int sDivPhase;  // 0 renders, 1 skips
static int sEscapeRun; // consecutive pairs measured below the floor

// How long the pair actually took, wall clock. NOT the work estimate: what
// decides whether a pair still needs two VBlanks is the time it occupies, and a
// frame that slept waiting on a link peer occupies that time even though the
// sleep is not work.
static unsigned long long sPairSpan;

static unsigned long long sHookEnd;   // ticks when the last hook returned
static unsigned long long sGameSum, sHookSum;
static unsigned sGameN, sHookN;

static void divider_sample(unsigned long long gameWork,
                           unsigned long long hookWork, int rendered,
                           unsigned long long frameSpan)
{
    unsigned long long pair;
    unsigned est;
    int want;

    sGameSum += gameWork;
    sGameN++;

    if (rendered) {
        sHookSum += hookWork;
        sHookN++;
        sPairSpan = frameSpan;
    } else {
        sPairSpan += frameSpan;

        // Has the pair stopped needing two VBlanks?
        if (sDivide && sPairSpan < DIV_PAIR_FLOOR_TICKS) {
            // Near enough to one VBlank to risk double speed, or below the
            // floor for long enough to believe it.
            if (sPairSpan < DIV_PAIR_PANIC_TICKS)
                sEscapeRun = DIV_ESCAPE_PAIRS;
            else
                sEscapeRun++;
        } else {
            sEscapeRun = 0;
        }

        if (sDivide && sEscapeRun >= DIV_ESCAPE_PAIRS) {
            sDivide = 0;
            sDivPhase = 0;
            sEscapeRun = 0;
            sGameSum = sHookSum = 0;
            sGameN = sHookN = 0;
            CtrLog("emerald3ds: display full rate, 60 Hz (scene got cheap)\n");
            return;
        }
    }

    if (sHookN < DIV_WINDOW || sGameN == 0)
        return;

    // Two game frames and one render: what a pair costs, engaged or not.
    pair = 2 * (sGameSum / sGameN) + (sHookSum / sHookN);
    est = (unsigned)(pair / (SYSCLOCK_ARM11 / 1000000));

    want = sDivide ? (pair > DIV_PAIR_OFF_TICKS) : (pair > DIV_PAIR_ON_TICKS);
    if (want != sDivide) {
        sDivide = want;
        sDivPhase = 0;
        sEscapeRun = 0;
        CtrLog("emerald3ds: display %s (pair work %u us)\n",
               want ? "halved, 30 Hz" : "full rate, 60 Hz", est);
    }

    sGameSum = sHookSum = 0;
    sGameN = sHookN = 0;
}

// The bind of each button: CTR_BIND_OFF, a speed, or CTR_BIND_MOD. The index is
// CTR_TURBO_*.
//
// Y is the modifier by default, so the touch UI's jump-by-5 works at once. Each
// button has one value, so a button cannot be turbo and the modifier at the
// same time.
static uint8_t sTurbo[CTR_TURBO_COUNT] = {
    [CTR_TURBO_Y] = CTR_BIND_MOD,
};

// The 3DS keys that the GBA does not use. The order must match CTR_TURBO_*.
static const uint32_t kTurboKeys[CTR_TURBO_COUNT] = {
    KEY_X, KEY_Y, KEY_ZL, KEY_ZR
};

void CtrSettingsMarkDirty(void);   // 3ds/host/settings.c

// A lower speed must restart the group. Otherwise a counter above the new limit
// stops presentation for a frame. Every path that changes the speed uses this.
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
    sBaseSpeed = sSpeed;   // the menu sets the resting speed
}

int Ctr3dsGetSpeed(void)
{
    // Returns the baseline, not the temporary override. Thus the EXTRA tab
    // shows the player's choice while a turbo button is held.
    return sBaseSpeed;
}

// Sets the value with no write, for CtrSettingsLoad(). A write during the load
// that gave the value is unnecessary, and a read-only SD card would get a write
// attempt at each boot. Ctr3dsApplyTopScale in video.c does the same.
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
        CtrSettingsMarkDirty();
}

int Ctr3dsGetTurboBind(int button)
{
    if (button < 0 || button >= CTR_TURBO_COUNT)
        return 0;

    return sTurbo[button];
}

// TRUE when the bottom screen shows tabs that the save has not unlocked. It is
// here, not on the game side, because it must persist, and the settings file is
// host side. The bottom screen (bottom_screen.c) reads it through the bridge.
static uint8_t sShowAllTabs;

// Sets the value with no write, for CtrSettingsLoad(), like
// Ctr3dsApplyTurboBind above.
void Ctr3dsApplyShowAllTabs(int on)
{
    // Refuse it when the debug menu is not in the build. The guard is here, not
    // in the getter, because CtrSettingsLoad() calls this directly. Without it,
    // a release build would get "show every tab" from a debug session's
    // settings.bin, with no control to undo it. Hold the value at 0, so every
    // reader agrees.
    sShowAllTabs = (CTR_DEBUG_MENU && on) ? 1 : 0;
}

void Ctr3dsSetShowAllTabs(int on)
{
    int before = sShowAllTabs;

    Ctr3dsApplyShowAllTabs(on);

    if (sShowAllTabs != before)
        CtrSettingsMarkDirty();
}

int Ctr3dsGetShowAllTabs(void)
{
    return sShowAllTabs;   // held at 0 by Apply when there is no debug menu
}

// Gameplay tweaks (EXTRA page 2), with the same Apply/Set/Get split as above.
// Apply changes the value with no write, for 3ds/host/settings.c. Set also
// writes. Get reads. Apply range-checks the two enums, like
// Ctr3dsApplyTurboBind: a bad settings byte must keep the default.
//
// The game side reads these through 3ds/tweaks.c, the only file that turns them
// into behavior.
static uint8_t sExpAll;
static uint8_t sLevelCap;    // CTR_CAP_*
static uint8_t sRandomizer;
static uint8_t sBagSort;     // CTR_BAGSORT_*
static uint8_t sPhoneCallsOff;
static uint8_t sQuickBallOff;
static uint8_t sBattleAnimOff;
static uint8_t sFollowerOn;
static uint8_t sFollowerWho;        // CTR_FOLLOWER_*
static uint8_t sFollowerBobOff;
static uint8_t sFollowerPokeBall;
static uint8_t sDayCareYard;

// The last ball thrown, as a raw item id. There is no range check here: the
// valid range is a game constant that this side cannot include.
// UiQuickBallItem() checks it. See the note in bridge.h.
static uint8_t sLastBall;

// The shiny test switch. There is no Apply/Set split and no
// CtrSettingsMarkDirty() call, because this tweak does not persist (see
// bridge.h). The game side clears it with Ctr3dsSetShinyTest(0) when the armed
// encounter starts, so both worlds call this setter.
static uint8_t sShinyTest;

void Ctr3dsSetShinyTest(int on)
{
    sShinyTest = (CTR_DEBUG_MENU && on) ? 1 : 0;
}

int Ctr3dsGetShinyTest(void)
{
    return sShinyTest;   // held at 0 by the setter when there is no debug menu
}

void Ctr3dsApplyExpAll(int on)
{
    sExpAll = on ? 1 : 0;
}

void Ctr3dsSetExpAll(int on)
{
    int before = sExpAll;

    Ctr3dsApplyExpAll(on);

    if (sExpAll != before)
        CtrSettingsMarkDirty();
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
        CtrSettingsMarkDirty();
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
        CtrSettingsMarkDirty();
}

int Ctr3dsGetRandomizer(void)
{
    return sRandomizer;
}

void Ctr3dsApplyPhoneCallsOff(int on)
{
    sPhoneCallsOff = on ? 1 : 0;
}

void Ctr3dsSetPhoneCallsOff(int on)
{
    int before = sPhoneCallsOff;

    Ctr3dsApplyPhoneCallsOff(on);

    if (sPhoneCallsOff != before)
        CtrSettingsMarkDirty();
}

int Ctr3dsGetPhoneCallsOff(void)
{
    return sPhoneCallsOff;
}

void Ctr3dsApplyFollowerOn(int on)
{
    sFollowerOn = on ? 1 : 0;
}

void Ctr3dsSetFollowerOn(int on)
{
    int before = sFollowerOn;

    Ctr3dsApplyFollowerOn(on);

    if (sFollowerOn != before)
        CtrSettingsMarkDirty();
}

int Ctr3dsGetFollowerOn(void)
{
    return sFollowerOn;
}

void Ctr3dsApplyFollowerWho(int mode)
{
    if (mode != CTR_FOLLOWER_LEAD && mode != CTR_FOLLOWER_STARTER)
        return;

    sFollowerWho = (uint8_t)mode;
}

void Ctr3dsSetFollowerWho(int mode)
{
    int before = sFollowerWho;

    Ctr3dsApplyFollowerWho(mode);

    if (sFollowerWho != before)
        CtrSettingsMarkDirty();
}

int Ctr3dsGetFollowerWho(void)
{
    return sFollowerWho;
}

void Ctr3dsApplyFollowerBobOff(int on)
{
    sFollowerBobOff = on ? 1 : 0;
}

void Ctr3dsSetFollowerBobOff(int on)
{
    int before = sFollowerBobOff;

    Ctr3dsApplyFollowerBobOff(on);

    if (sFollowerBobOff != before)
        CtrSettingsMarkDirty();
}

int Ctr3dsGetFollowerBobOff(void)
{
    return sFollowerBobOff;
}

void Ctr3dsApplyFollowerPokeBall(int on)
{
    sFollowerPokeBall = on ? 1 : 0;
}

void Ctr3dsSetFollowerPokeBall(int on)
{
    int before = sFollowerPokeBall;

    Ctr3dsApplyFollowerPokeBall(on);

    if (sFollowerPokeBall != before)
        CtrSettingsMarkDirty();
}

int Ctr3dsGetFollowerPokeBall(void)
{
    return sFollowerPokeBall;
}

void Ctr3dsApplyDayCareYard(int on)
{
    sDayCareYard = on ? 1 : 0;
}

void Ctr3dsSetDayCareYard(int on)
{
    int before = sDayCareYard;

    Ctr3dsApplyDayCareYard(on);

    if (sDayCareYard != before)
        CtrSettingsMarkDirty();
}

int Ctr3dsGetDayCareYard(void)
{
    return sDayCareYard;
}

void Ctr3dsApplyQuickBallOff(int on)
{
    sQuickBallOff = on ? 1 : 0;
}

void Ctr3dsSetQuickBallOff(int on)
{
    int before = sQuickBallOff;

    Ctr3dsApplyQuickBallOff(on);

    if (sQuickBallOff != before)
        CtrSettingsMarkDirty();
}

int Ctr3dsGetQuickBallOff(void)
{
    return sQuickBallOff;
}

void Ctr3dsApplyBattleAnimOff(int on)
{
    sBattleAnimOff = on ? 1 : 0;
}

void Ctr3dsSetBattleAnimOff(int on)
{
    int before = sBattleAnimOff;

    Ctr3dsApplyBattleAnimOff(on);

    if (sBattleAnimOff != before)
        CtrSettingsMarkDirty();
}

int Ctr3dsGetBattleAnimOff(void)
{
    return sBattleAnimOff;
}

// Battle logic calls this setter, not an EXTRA button: HandleAction_UseItem()
// in src/battle_util.c, once for each ball thrown. That is safe, as the header
// of settings.c explains. CtrSettingsMarkDirty() only queues, and the write
// occurs from the frame loop one second after the last change. Many throws cost
// one write.
void Ctr3dsApplyLastBall(int item)
{
    sLastBall = (uint8_t)(item & 0xFF);
}

void Ctr3dsSetLastBall(int item)
{
    int before = sLastBall;

    Ctr3dsApplyLastBall(item);

    if (sLastBall != before)
        CtrSettingsMarkDirty();
}

int Ctr3dsGetLastBall(void)
{
    return sLastBall;
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
        CtrSettingsMarkDirty();
}

int Ctr3dsGetBagSort(void)
{
    return sBagSort;
}

// The modifier for the touch UI. It uses the same hidScanInput() as the touch
// state.
int Ctr3dsUiModifierHeld(void)
{
    uint32_t held = hidKeysHeld();

    for (int i = 0; i < CTR_TURBO_COUNT; i++)
        if (sTurbo[i] == CTR_BIND_MOD && (held & kTurboKeys[i]))
            return 1;

    return 0;
}

// What fast-forward does to the music. It persists, like the other display
// preferences.
static uint8_t sFfAudio = CTR_FFAUDIO_NORMAL;

// Sets the value with no write, for CtrSettingsLoad(), like
// Ctr3dsApplyTurboBind above.
void Ctr3dsApplyFfAudio(int mode)
{
    if (mode != CTR_FFAUDIO_NORMAL && mode != CTR_FFAUDIO_FAST)
        return;

    sFfAudio = (uint8_t)mode;
}

void Ctr3dsSetFfAudio(int mode)
{
    int before = sFfAudio;

    Ctr3dsApplyFfAudio(mode);

    if (sFfAudio != before)
        CtrSettingsMarkDirty();
}

int Ctr3dsGetFfAudio(void)
{
    return sFfAudio;
}

// TRUE on the game frame that carries audio.
//
// Fast-forward runs several game frames for each displayed frame. The sound
// engine advances the song one tick for each m4aSoundMain() call. A call on
// every game frame plays the music at the fast-forward speed. The DSP uses only
// one frame of samples, so the rest drops and the music skips.
//
// NORMAL thus ticks the engine once for each displayed frame. FAST keeps a tick
// on every game frame, for a player who wants the pitch as a cue.
//
// Use subframe 0, where the input is also read. Any frame of the group works,
// because the ring buffer separates this from presentation. At 1x every frame
// is subframe 0, so both settings are the same.
int Ctr3dsIsAudioFrame(void)
{
    return sFfAudio == CTR_FFAUDIO_FAST || sSubFrame == 0;
}

// The audio A/B switches. All three are ON by default, so a normal boot uses
// the real mixer. They matter only to a tester who looks for a sound fault.
//
// One small array, not three flags, because the EXTRA row and the settings file
// read and write them generically.
static uint8_t sAudioDbg[CTR_AUDIO_DBG_COUNT] = { 1, 1, 1, 1 };

// Sets the value with no write, for CtrSettingsLoad(), like Ctr3dsApplyFfAudio
// above.
void Ctr3dsApplyAudioDbg(int which, int on)
{
    if (which < 0 || which >= CTR_AUDIO_DBG_COUNT)
        return;

    // The same reason as Ctr3dsApplyShowAllTabs: CtrSettingsLoad() calls this.
    // ON is the neutral value, because ON is the real mixer. Set the stored
    // byte, because CtrAudioFrame reads the STEREO entry from this array
    // directly.
    if (!CTR_DEBUG_MENU)
        on = 1;

    sAudioDbg[which] = (uint8_t)(on ? 1 : 0);

    // STEREO is a host-side downmix, and CtrAudioFrame reads it from this
    // array. The other two are in the mixer and must be pushed.
    Rp2350SetAudioDebug(sAudioDbg[CTR_AUDIO_DBG_PSG],
                        sAudioDbg[CTR_AUDIO_DBG_REVERB],
                        sAudioDbg[CTR_AUDIO_DBG_DS]);
}

void Ctr3dsSetAudioDbg(int which, int on)
{
    int before;

    if (which < 0 || which >= CTR_AUDIO_DBG_COUNT)
        return;

    before = sAudioDbg[which];
    Ctr3dsApplyAudioDbg(which, on);

    if (sAudioDbg[which] != before)
        CtrSettingsMarkDirty();
}

int Ctr3dsGetAudioDbg(int which)
{
    if (which < 0 || which >= CTR_AUDIO_DBG_COUNT)
        return 1;

    return sAudioDbg[which];   // held at 1 by Apply when there is no debug menu
}

// The fastest bound button that is held, or the resting speed. The fastest, not
// the first, so two held buttons never give the slower speed.
static int effective_speed(uint32_t held)
{
    int best = 0;

    // CTR_BIND_MOD is outside the speed range, so a modifier button is never a
    // very fast turbo.
    for (int i = 0; i < CTR_TURBO_COUNT; i++)
        if (sTurbo[i] != CTR_BIND_OFF && sTurbo[i] != CTR_BIND_MOD
            && (held & kTurboKeys[i]) && sTurbo[i] > best)
            best = sTurbo[i];

    return best ? best : sBaseSpeed;
}

void Rp2350PresentFrame(void)
{
    // For the display divider, which wants two different things from this span.
    //
    // The span since the last return is the game's own frame. Almost all of it
    // is work, with one exception: LinkVSync() runs in there, and a link
    // exchange sleeps up to LINK_WAIT_US waiting on a peer. That sleep is not
    // work, and counting it as work halved the display on scenes that were
    // never expensive, which cost the pair two sleeps instead of one and made
    // the peer later still.
    //
    // So the estimate takes the work, and the pair accounting takes the whole
    // span. Whether a pair still needs two VBlanks is a question about the time
    // it occupies, and a frame that slept occupies that time.
    unsigned long long tHook = CtrTicksNow();
    unsigned long long gameSpan = (sHookEnd != 0) ? tHook - sHookEnd : 0;
    unsigned long long linkTicks = Ctr3dsLinkTakeBlockedTicks();
    unsigned long long gameWork =
        (gameSpan > linkTicks) ? gameSpan - linkTicks : 0;
    unsigned long long blockTicks = 0;

    // Only the first frames matter. Frame 1 shows that the game's init did not
    // hang, and a rising count shows that the frame loop did not hang. More
    // lines add nothing.
    {
        static unsigned frame;
        frame++;
        if (frame <= 3 || frame == 60 || frame == 600)
            CtrTrace("emerald3ds: present frame %u\n", frame);
    }

    // HOME menu or power. Check on every game frame, not only displayed ones,
    // so a close request works quickly during fast-forward.
    if (!aptMainLoop() && !sQuitting) {
        sQuitting = 1;
        // Flush before the longjmp too. This is the last point where the app is
        // surely alive. The flush costs nothing when the image is clean. The
        // function main() flushes again after the jump, and that call returns
        // at once.
        //
        // Force the settings too. A preference changed just before the close is
        // still in the debounce. Without this, the setting seems not to
        // persist.
        CtrSaveFlush(1);
        CtrSettingsFlush(1);
        CtrAchFlush(1);
        longjmp(sQuitJmp, 1);
    }

    // Input changes only once for each displayed frame, which is the hardware's
    // rate. The hidScanInput() function finds press edges by a comparison with
    // the previous scan. A scan on each game frame uses up the edge, and
    // sample_touch() and CtrBottomUpdate() miss taps.
    //
    // One value for the full group also makes fast-forward correct: one press
    // gives one JOY_NEW and then held frames.
    static uint16_t keys;
    int presenting;
    int rendering;

    if (sSubFrame == 0) {
        hidScanInput();
        keys = sample_keys();

        // Resolve turbo here, with the input, because the button state is fresh
        // only here.
        set_speed(effective_speed(hidKeysHeld()));
    }

    // TRUE on the game frame that is displayed. Decide this once, after
    // set_speed() (which can restart the group) and before anything else. The
    // render start and the present both use it. Two decisions would let a tap
    // on EXTRA's speed buttons start a render that nothing waits for.
    presenting = (sSubFrame + 1 >= sSpeed);
    rendering = presenting;

    // The divider rides on top of the group, and leaves sSubFrame alone: that
    // counter drives the input scan and the bottom screen, which must keep
    // running at the full rate so that controls stay as responsive as ever.
    //
    // Fast-forward is exempt. It already drops renders by a factor the player
    // asked for, and halving that again would be a speed nobody chose.
    if (sDivide && sSpeed == 1) {
        if (sDivPhase)
            rendering = 0;
        sDivPhase ^= 1;
    }

    // What a presented frame is supposed to cost, for the late-frame counter in
    // video.c. Both terms: the divider is exempt during fast-forward, so
    // sDivide on its own would call every turbo frame late.
    CtrVideoSetFrameVBlanks((sDivide && sSpeed == 1) ? 2 : 1);

    // Start the rasterizer on the other core now, before the bottom screen
    // paints, so the two run at the same time. The rasterizer first copies the
    // video state, so the touch handlers below cannot tear it. See
    // CtrVideoRenderBegin in 3ds/host/video.c.
    if (rendering)
        CtrVideoRenderBegin();

    if (sSubFrame == 0) {
        CtrTouchState touch;
        sample_touch(&touch);
        CtrBottomUpdate(&touch);
    }

    // The buttons for the next frame's ReadKeys() (src/main.c), as the GBA
    // reads its key register between frames.
    CtrSetKeyInput(keys);

    // Take what m4aSoundMain() just made. Thus this must run on the same frames
    // as the engine ticks. One test decides both.
    if (Ctr3dsIsAudioFrame())
        CtrAudioFrame();

    if (presenting)
        sSubFrame = 0;
    else
        sSubFrame++;

    if (rendering) {
        // Collect the render, upload and present. C3D_FrameBegin waits for
        // VBlank, which paces the game to 60 Hz. Fast-forward skips this call.
        // That skips the rasterize (the expensive part) and the pacing, so the
        // other frames cost only game logic.
        CtrVideoPresent();
        blockTicks += CtrVideoLastWaitTicks();

        // Write the save image after the sector writes stop, and give the
        // settings to the I/O thread after the player stops changing them. Both
        // occur here, after CtrVideoPresent() showed the frame.
        CtrSaveFlush(0);
        CtrSettingsFlush(0);
        CtrAchFlush(0);
    }

    sHookEnd = CtrTicksNow();
    if (sSpeed == 1) {
        unsigned long long span = sHookEnd - tHook;
        // blockTicks is the wait inside C3D_FrameBegin, which is the slack the
        // divider is deciding how to spend. It comes out of both figures. The
        // link's sleep does not: it comes out of the work only.
        unsigned long long hookWork =
            (span > blockTicks) ? span - blockTicks : 0;

        divider_sample(gameWork, hookWork, rendering, gameSpan + hookWork);
    }
}

// The HOME menu, either side of it.
//
// libctru runs these from inside aptMainLoop() on the main thread, so the link
// is not being pumped underneath them and a send here is safe. ONSUSPEND is the
// last moment this console runs before it freezes: it is the only chance to
// tell the peer, which otherwise cannot tell a suspended console from a slow
// one and waits out the whole lag tolerance.
static aptHookCookie sAptCookie;

static void apt_hook(APT_HookType hook, void *param)
{
    (void)param;

    switch (hook) {
    case APTHOOK_ONSUSPEND:
    case APTHOOK_ONSLEEP:
        Ctr3dsLinkSuspending();
        break;
    case APTHOOK_ONRESTORE:
    case APTHOOK_ONWAKEUP:
        Ctr3dsLinkResumed();
        break;
    default:
        break;
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    // The first line of the session. On a console, svcOutputDebugString goes
    // nowhere. Without a line on the SD card, "the port did not start" and "the
    // port failed later" look the same. See 3ds/host/log.c. Do not use __DATE__
    // and __TIME__: they come from the last compile of this file, so they can
    // be stale. CTR_BUILD_STAMP is new on each build.
    CtrLog("emerald3ds: boot (%s)\n", CTR_BUILD_STAMP);

    // This must come first: every VRAM, palette, OAM and register access in the
    // game uses this block.
    CtrTrace("emerald3ds: main() entered\n");

    Ctr3dsInitGbaMemory();
    CtrTrace("emerald3ds: gba memory ready\n");
    CtrSaveLoad();

    // The display preferences. Load them before CtrVideoInit(), so the first
    // frame has the player's scale.
    CtrSettingsLoad();

    // Load the achievements of each playthrough once. The game side's first
    // lookup then comes from memory, not from the card in the middle of a
    // frame.
    CtrAchStoreInit();
    CtrTrace("emerald3ds: save loaded\n");

    if (!CtrVideoInit()) {
        // Use CtrLog, not CtrTrace. This failure ends the run, so it must be in
        // a release build's log.
        CtrLog("emerald3ds: FATAL CtrVideoInit failed\n");
        CtrVideoExit();
        return 1;
    }
    CtrTrace("emerald3ds: video ready\n");

    // New 3DS: 804 MHz and L2 cache. No effect on an Old 3DS. Do this after the
    // graphics services start, because it goes through PTM.
    //
    // It uses ptm:sysm, which 3ds/emerald3ds.rsf must grant. The libctru
    // library uses the same service to apply the boost again after each return
    // from the HOME menu. The function osSetSpeedupEnable() reports nothing, so
    // open the service once and log the result. If this fails, the game drops
    // to 268 MHz after the first suspend. The exheader's CpuSpeed still sets
    // 804 MHz at launch.
    {
        bool isNew3ds = false;
        Result rc = ptmSysmInit();

        APT_CheckNew3DS(&isNew3ds);
        CtrLog("emerald3ds: %s, ptm:sysm %s (rc=0x%08lX)\n",
               isNew3ds ? "New 3DS" : "Old 3DS",
               R_SUCCEEDED(rc) ? "available" : "UNAVAILABLE",
               (unsigned long)rc);

        if (R_SUCCEEDED(rc))
            ptmSysmExit();
    }
    osSetSpeedupEnable(true);

    // After the services, before the game loop. See apt_hook above.
    aptHook(&sAptCookie, apt_hook, NULL);

    // It logs if audio started, and the reason if not. The usual reason is a
    // missing sdmc:/3ds/dspfirm.cdc, which is not fatal.
    CtrAudioInit();

    // After CtrAudioInit(), which sets the main thread's priority. The writer
    // thread runs one step below it. All log lines before this point went to
    // the card directly, which is correct for a boot log.
    CtrIoInit();

    CtrBottomInit();
    CtrTrace("emerald3ds: bottom screen ready\n");

#if CTR_BOOT_DIAG
    // Leave a known image on the screen. If it stays, the game hung. If the
    // screen stays black, nothing in this file ran.
    CtrDiagSplash();
#endif

    if (setjmp(sQuitJmp) == 0) {
        // If nothing after this line appears, the game hung in its own init.
        CtrTrace("emerald3ds: entering AgbMain\n");
        AgbMain();   // never returns; exits through the longjmp above
    }

    // Flush always: the delayed write can still be pending. Usually this does
    // nothing, because saves commit when they occur (CtrSaveCommit, from
    // src/save.c) and the close path above flushes too. This is the last guard
    // for writes outside a save.
    CtrSaveFlush(1);
    CtrSettingsFlush(1);
    CtrAchFlush(1);
    // Close the network before the other exits. A UDS network left up keeps
    // the wireless hardware busy and strands a peer that is still connected.
    CtrLinkExit();
    // Stop the log writer before the other exits, so their lines go to the card
    // directly and do not wait for a thread that stops.
    CtrIoExit();
    CtrAudioExit();
    CtrVideoExit();
    return 0;
}
