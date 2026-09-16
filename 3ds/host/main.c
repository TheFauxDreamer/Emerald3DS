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
void CtrAudioInit(void);
void CtrAudioExit(void);
void CtrAudioFrame(void);
void CtrSaveLoad(void);
void CtrSaveFlush(int force);
void CtrSettingsLoad(void);
void CtrSettingsFlush(int force);
void CtrAchStoreInit(void);
void CtrAchFlush(int force);
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

    // Start the rasterizer on the other core now, before the bottom screen
    // paints, so the two run at the same time. The rasterizer first copies the
    // video state, so the touch handlers below cannot tear it. See
    // CtrVideoRenderBegin in 3ds/host/video.c.
    if (presenting)
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

    if (presenting) {
        sSubFrame = 0;

        // Collect the render, upload and present. C3D_FrameBegin waits for
        // VBlank, which paces the game to 60 Hz. Fast-forward skips this call.
        // That skips the rasterize (the expensive part) and the pacing, so the
        // other frames cost only game logic.
        CtrVideoPresent();

        // Write the save image after the sector writes stop, and give the
        // settings to the I/O thread after the player stops changing them. Both
        // occur here, after CtrVideoPresent() showed the frame.
        CtrSaveFlush(0);
        CtrSettingsFlush(0);
        CtrAchFlush(0);
    } else {
        sSubFrame++;
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
    // Stop the log writer before the other exits, so their lines go to the card
    // directly and do not wait for a thread that stops.
    CtrIoExit();
    CtrAudioExit();
    CtrVideoExit();
    return 0;
}
