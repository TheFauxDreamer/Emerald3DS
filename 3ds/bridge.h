// The seam between the two halves of this port.
//
// The headers include/gba/types.h (game) and <3ds.h> (libctru) both define
// u8/u16/u32 differently, and the game's include/ hides libc headers (string.h,
// strings.h). A translation unit that includes both does not build. Thus:
//
//   game-side TUs : src/**, rp2350/{bios,asm_stubs,m4a_1}.c, 3ds/gba_mem.c,
//                   3ds/ui/**            (game headers only, never <3ds.h>)
//   host-side TUs : 3ds/host/**, rp2350/ppu.c
//                                        (libctru only, never game headers)
//
// This header declares everything that the two halves share, in stdint types
// only. It includes neither side's headers, and it must stay so.

#ifndef CTR_BRIDGE_H
#define CTR_BRIDGE_H

#include <stdint.h>

#define CTR_GBA_WIDTH   240
#define CTR_GBA_HEIGHT  160

#define CTR_BOTTOM_WIDTH   320
#define CTR_BOTTOM_HEIGHT  240

// ---------------------------------------------------------------- game side --

// Allocate and clear the GBA memory regions. Call this before any other game
// code: every VRAM, palette, OAM and register access uses gGbaMem.
void Ctr3dsInitGbaMemory(void);

// The region bases for ppu_set_memory(). Valid only after
// Ctr3dsInitGbaMemory().
void CtrGetGbaRegions(const void **reg, const void **pal,
                      const void **vram, const void **oam);

// Write the frame's button state into REG_KEYINPUT. GBA keys are active low: a
// 0 bit means pressed. The bit order is A, B, Select, Start, Right, Left, Up,
// Down, R, L.
void CtrSetKeyInput(uint16_t keysActiveLow);

// The game's main loop (src/main.c). It never returns.
void AgbMain(void);

// ---- bottom screen (game side: it draws with the game's fonts and gfx) ------

typedef struct {
    int16_t x, y;          // touch position, 0..319 / 0..239, when touching
    uint8_t touching;
    uint8_t justPressed;
    uint8_t justReleased;
} CtrTouchState;

void CtrBottomInit(void);
void CtrBottomUpdate(const CtrTouchState *touch);

// Not zero when the framebuffer changed after the host's last upload. The
// bottom screen is mostly static, so this gates the texture upload.
int  CtrBottomIsDirty(void);
void CtrBottomClearDirty(void);

// The framebuffer is 320x240 RGB565, in row order. The pointer does not change.
// Valid after CtrBottomInit().
const uint16_t *CtrBottomFramebuffer(void);

// ---------------------------------------------------------------- host side --

// The per-frame hook. AgbMain's loop calls it at the end of every game frame
// (src/main.c, under #if RP2350). It renders, presents, reads input, feeds
// audio, and paces the game to the frame rate.
void Rp2350PresentFrame(void);

// TRUE when the GBA rasterizer runs on its own core (core 2 on a New 3DS, core
// 1 otherwise). Set at startup, before CtrBottomInit(). When FALSE, the bottom
// screen keeps its single-core tuning: each repaint costs a VBlank on that path
// (see section 7 of 3ds/SECOND_SCREEN_CHEATSHEET.md).
int Ctr3dsRasteriserOnOwnCore(void);

// The build: the output of `git describe --always --dirty` at build time, and
// the branch name before it when the build is not off main. For example
// "bc688ed" from main, "local-wireless/bc688ed" from a branch,
// "bc688ed-dirty" for a tree with uncommitted changes, or "nogit" outside a
// repository. ASCII, never NULL. The title screen shows it in the bottom
// screen's corner, so a player can check the installed build, and two consoles
// that must run the same build can be compared at a glance. The boot log also
// has it, with the build time.
const char *Ctr3dsBuildId(void);

// The save flash write hooks, called from src/agb_flash*.c. Reads go directly
// through FLASH_BASE (gCtrSaveFlash). Only writes come here. They return 0 on
// success and 0x80FF on failure.
uint16_t Rp2350SaveEraseChip(void);
uint16_t Rp2350SaveEraseSector(uint16_t sectorNum);
uint16_t Rp2350SaveProgramSector(uint16_t sectorNum, uint8_t *src);
uint16_t Rp2350SaveProgramByte(uint16_t sectorNum, uint32_t offset, uint8_t data);
void     Rp2350SaveSync(void);

// Write the save image to the SD card now, with no debounce. The file
// src/save.c calls this when a save completes, so the file is current before
// the player continues. Without it, the save depends on the process living long
// enough for the debounce. A close of the emulator window does not wait for
// that.
void     CtrSaveCommit(void);

// Fast-forward. The game's main loop runs one game frame for each
// Rp2350PresentFrame() call. This sets how many of those occur for each
// displayed frame, so 2 makes the game advance twice as fast.
//
// The software rasterize and the VBlank wait both occur only on the presented
// frame. Thus a skipped frame removes that cost and the 60 Hz pacing together.
// The value is clamped to CTR_SPEED_MIN..CTR_SPEED_MAX.
//
// This is a request, not a guarantee. If the console cannot keep up, the game
// runs slower than asked. That is a slowdown, not a fault.
//
// Any whole multiplier in this range works. The UI selects which values to
// offer (3ds/ui/tab_extra.c).
#define CTR_SPEED_MIN 1
#define CTR_SPEED_MAX 8

void Ctr3dsSetSpeed(int multiplier);
int  Ctr3dsGetSpeed(void);

// What fast-forward does to the music (EXTRA tab, page 1).
//
// The sound engine advances the song one tick for each m4aSoundMain() call, and
// fast-forward runs 2x to 8x game frames for each displayed frame. A tick on
// each game frame plays the music at that multiplier. The DSP uses only one
// frame of samples, so the rest drops, and the music skips.
//
// NORMAL ticks the engine once for each displayed frame, so the music keeps its
// tempo and quality. FAST is the older behavior. The higher pitch also shows
// that fast-forward is on.
#define CTR_FFAUDIO_NORMAL 0
#define CTR_FFAUDIO_FAST   1

void Ctr3dsSetFfAudio(int mode);
int  Ctr3dsGetFfAudio(void);

// TRUE on the game frame that carries audio. Always TRUE at 1x. Under
// fast-forward, it follows the setting above. NORMAL gives once for each
// displayed frame, and FAST gives every game frame. The file src/main.c gates
// m4aSoundMain() and MapMusicMain() on this, and the host gates the mixer drain
// on it.
int Ctr3dsIsAudioFrame(void);

// Turbo binds for the four 3DS buttons that the GBA does not have. A held bound
// button overrides the speed above while it is held. The GAME SPEED selection
// stays the resting speed.
//
// ZL and ZR exist only on a New 3DS. On an Old 3DS they never register, so a
// bind there does nothing. The UI tells the player.
#define CTR_TURBO_X      0
#define CTR_TURBO_Y      1
#define CTR_TURBO_ZL     2
#define CTR_TURBO_ZR     3
#define CTR_TURBO_COUNT  4

// A button binds to exactly one thing: nothing, a turbo speed, or the touch UI
// modifier. One value, so the two uses cannot share a button.
#define CTR_BIND_OFF  0
#define CTR_BIND_MOD  0xFF   // outside the speed range on purpose

// `value` is CTR_BIND_OFF, a multiplier in CTR_SPEED_MIN..CTR_SPEED_MAX, or
// CTR_BIND_MOD. Other values are refused.
void Ctr3dsSetTurboBind(int button, int value);
int  Ctr3dsGetTurboBind(int button);

// TRUE while a button bound to CTR_BIND_MOD is held.
//
// The modifier is always one of these four buttons, never a GBA button. The
// game keeps running on the top screen, so a held GBA button also goes to the
// game. With the L=A option, L is an A press (src/main.c), so a list jump would
// also talk to the character in front of the player.
int  Ctr3dsUiModifierHeld(void);

// The top-screen scale. The GBA frame is 240x160 and the top screen is 400x240.
// Only 1.5x fills the height exactly. The other two trade borders for fidelity.
// This setting persists (3ds/host/settings.c), because it is a display
// preference.
//
// FILL stretches the image 11% horizontally, because the GBA is 3:2 and the
// panel is 5:3. The only other way to fill the panel is to crop the picture.
// The UI tells the player.
#define CTR_TOP_SCALE_1X     0   // 240x160, pixel-perfect, wide borders
#define CTR_TOP_SCALE_1_5X   1   // 360x240, fills the height, 20px bars
#define CTR_TOP_SCALE_FILL   2   // 400x240, no borders, 11% wider
#define CTR_TOP_SCALE_COUNT  3
#define CTR_TOP_SCALE_DEFAULT CTR_TOP_SCALE_1_5X

void Ctr3dsSetTopScale(int mode);
int  Ctr3dsGetTopScale(void);

// ---- the debug menu -------------------------------------------------------
//
// Whether EXTRA has its debug page: the shiny test switch, the tab-unlock
// override and the audio A/B switches. They test the port, not the game, so
// they are together behind one value.
//
// Set this to 0 for a release build. It then does three things:
// - The page and its pager button disappear. Nothing else on the EXTRA tab
//   moves.
// - The three settings behind it are held at their neutral values
//   (3ds/host/main.c). Two of them persist in settings.bin. Without this, a
//   release build could get "show every tab" or a muted PSG channel from a
//   debug session, with no control to undo it.
// - There is no log file (3ds/host/log.c). A shared build must not write to the
//   player's SD card. svcOutputDebugString stays, so an emulator still shows
//   the same lines.
#define CTR_DEBUG_MENU 1

// Show every bottom-screen tab, the ones that the save has not unlocked too.
//
// A test aid, not a cheat. The tabs use the same flags as the start menu, so
// before the PokeNav the MAP tab does not show. Nothing that it shows writes
// game state (PARTY, MAP and DEX only read). The worst case is a Pokedex that
// the player did not get yet. It persists, so a test session survives a
// relaunch.
void Ctr3dsSetShowAllTabs(int on);
int  Ctr3dsGetShowAllTabs(void);

// Gameplay tweaks, on page 2 of the EXTRA tab.
//
// These are cheats. Fast-forward, the top scale and the tab override leave the
// game as Game Freak wrote it. Every option below changes it. They are here,
// not in the save block. A tapped setting must persist even if the player does
// not save, and settings.bin is written after the tap.
//
// Each save has its own values (see CtrSettingsAdopt below).
//
// The randomizer has no seed field. It comes from the save's trainer ID. Thus a
// mapping is stable for one playthrough and different between playthroughs. A
// toggle off and on does not change it.
#define CTR_CAP_OFF      0
#define CTR_CAP_SOFT     1   // less exp above the cap
#define CTR_CAP_HARD     2   // no exp above the cap

#define CTR_BAGSORT_OFF  0   // keep the player's own order
#define CTR_BAGSORT_TYPE 1   // item id order, which groups by category
#define CTR_BAGSORT_NAME 2   // alphabetical order

void Ctr3dsSetExpAll(int on);
int  Ctr3dsGetExpAll(void);

void Ctr3dsSetLevelCap(int mode);   // CTR_CAP_*
int  Ctr3dsGetLevelCap(void);

void Ctr3dsSetRandomizer(int on);
int  Ctr3dsGetRandomizer(void);

void Ctr3dsSetBagSort(int mode);    // CTR_BAGSORT_*
int  Ctr3dsGetBagSort(void);

// Stops the game's unrequested Match Call: the trainer who calls during a
// route, stops the player and talks. Only that call. The scripted story calls
// and the PokeNav's Match Call screen use other routes and stay. Thus nothing
// that the story needs can be turned off here.
//
// Stored as "off", so a zero settings byte means that calls occur. Files from
// before this option have zero there. The audioDbgMuted field in settings.c
// uses the same idea.
void Ctr3dsSetPhoneCallsOff(int on);
int  Ctr3dsGetPhoneCallsOff(void);

// The first Pokemon of the party walks behind the player (src/follower_helper.c
// and UpdateFollowingPokemon). Zero means off, which is the original game. Each
// save has its own value.
void Ctr3dsSetFollowerOn(int on);
int  Ctr3dsGetFollowerOn(void);

// The FOLLOWER page of the EXTRA tab (page 4). Each save has its own values, in
// the same record byte as the FOLLOWER switch. Zero is the default of each.
//
// WHO: LEAD is the first party Pokemon that can fight. STARTER is only the
// Pokemon from Professor Birch's bag, as Pikachu in Yellow. If the starter
// cannot fight or is not in the party, nothing follows.
#define CTR_FOLLOWER_LEAD     0
#define CTR_FOLLOWER_STARTER  1

void Ctr3dsSetFollowerWho(int mode);   // CTR_FOLLOWER_*
int  Ctr3dsGetFollowerWho(void);

// Stored as "off": zero means that the follower bobs as it walks.
void Ctr3dsSetFollowerBobOff(int on);
int  Ctr3dsGetFollowerBobOff(void);

// Zero means that the follower comes out of the ball that caught it. On means a
// plain Poke Ball for every Pokemon.
void Ctr3dsSetFollowerPokeBall(int on);
int  Ctr3dsGetFollowerPokeBall(void);

// The Day Care Pokemon walk in the yard on Route 117. Zero means off, which is
// the original game. The value is for the console, not for each save. A change
// shows at the next load of Route 117.
void Ctr3dsSetDayCareYard(int on);
int  Ctr3dsGetDayCareYard(void);

// The last Poke Ball that the player threw. The bottom screen then offers it
// again, and the player does not need to search the bag in each encounter.
//
// Stored as a raw item id, with no range check on this side. The ball ids are
// FIRST_BALL..LAST_BALL in include/constants/items.h, a game header that
// bridge.h cannot include. A copy of the numbers here could drift from the
// originals. UiQuickBallItem() (3ds/ui/ui_quickball.c) checks the value where
// the real constants are. Zero means that nothing was thrown yet. Each save has
// its own value.
void Ctr3dsSetLastBall(int item);
int  Ctr3dsGetLastBall(void);

// The quick-throw strip (EXTRA page 3). It shows over the current tab while a
// catchable wild battle asks the player for an action. A player who wants those
// 40 rows of the party grid can turn it off.
//
// Stored as "off", like phoneCallsOff above: a zero settings byte means that
// the strip shows. That is the default, and older files have zero there.
void Ctr3dsSetQuickBallOff(int on);
int  Ctr3dsGetQuickBallOff(void);

// The bottom-screen animation in battle (EXTRA page 3).
//
// The PARTY grid's sliding HP bars and cycling icons are the only things on
// this screen that move by themselves. They move most in battle. Off means:
// show the true values, animate nothing, and ask for no repaint beyond real
// changes. The top screen then keeps all of its frame time. The display stays
// correct.
//
// Stored as "off", so a zero settings byte means that the animation runs, as in
// older files.
void Ctr3dsSetBattleAnimOff(int on);
int  Ctr3dsGetBattleAnimOff(void);

// The shiny test switch (EXTRA debug page).
//
// It arms the next wild encounter to be shiny, then disarms itself. Without it,
// nobody can test the bottom screen's shiny notice: the real odds are
// SHINY_ODDS/65536, one encounter in 8192.
//
// This tweak does not persist. It is the only one that expires by itself, so a
// stored "armed" state would fire in a later session. Also, the player keeps
// the mon if they catch it. Fast-forward resets on each launch for the same
// reason.
//
// On the game side, 3ds/tweaks.c clears it when it fires. Thus the EXTRA tab
// must poll it, because its button is not the only thing that changes it.
void Ctr3dsSetShinyTest(int on);
int  Ctr3dsGetShinyTest(void);

// ---- per-save settings -----------------------------------------------------
//
// EXP All, the level cap, the randomizer, the bag sort, the phone-call switch
// and the last ball belong to a save, not to the console. The file
// 3ds/host/settings.c keeps one record for each save, keyed on the save's full
// 32-bit trainer ID. A save with no record gets the defaults.
//
// The game side calls this on the first overworld frame of a save (Adopt in
// 3ds/achievements.c). The host then applies the values of that save. Main
// thread only.
void CtrSettingsAdopt(uint32_t playerId);

// ---- achievements store ----------------------------------------------------
//
// The achievements that a playthrough has unlocked, and which of those the
// player has not seen yet. The file 3ds/host/achievements.c keeps them in
// sdmc:/3ds/emerald3ds/achievements.bin. The game side (3ds/achievements.c)
// decides what an achievement is and when it unlocks, because every condition
// uses a game accessor. This side only keeps the bits.
//
// The key is the save's full 32-bit trainer ID, so each playthrough has its own
// list and a new game starts empty. The file holds CTR_ACH_RECORDS records. A
// new playthrough replaces the least recently used one.
//
// Both bitsets are CTR_ACH_BYTES long. The achievement with id i is at
// bit (i % 8) of byte i / 8. The id is the permanent id in the game-side
// tables, not the row position, so rows can move without a change to saved
// unlocks.
//
// Next to them, CTR_ACH_PLACE_BYTES hold the map sections where the player
// stood: section s at bit (s % 8) of byte s / 8. The save records the towns
// (FLAG_VISITED_*) but only some routes, so this is the only complete record of
// where the player went.
//
// Load fills all three arrays and returns 1 if the playthrough has a record.
// Otherwise it clears them and returns 0. Save copies them in and queues a
// write, like a settings change, so nothing writes the card during a frame (see
// the header of 3ds/host/settings.c). Main thread only.
#define CTR_ACH_BYTES        16   // space for 128 achievements
#define CTR_ACH_PLACE_BYTES  16   // every map section below 128: all of Hoenn
#define CTR_ACH_RECORDS      8

int  CtrAchStoreLoad(uint32_t playerId, uint8_t *unlocked, uint8_t *unseen,
                     uint8_t *places);
void CtrAchStoreSave(uint32_t playerId, const uint8_t *unlocked,
                     const uint8_t *unseen, const uint8_t *places);

// The console's real-time clock, in place of the cartridge RTC. The GBA carts
// had an S-3511A. A 3DS has no cart, so src/siirtc.c uses this instead (under
// PLATFORM_3DS). That file is on the game side and includes this header
// directly, like 3ds/ui/*.c.
//
// The fields are plain binary. The driver applies the BCD encoding of the real
// chip, because src/rtc.c reads BCD.
typedef struct {
    uint8_t year;       // years after 2000, 0..99: the chip stores two digits
    uint8_t month;      // 1..12
    uint8_t day;        // 1..31
    uint8_t dayOfWeek;  // 0..6, Sunday = 0
    uint8_t hour;       // 0..23, always 24-hour
    uint8_t minute;     // 0..59
    uint8_t second;     // 0..59
} CtrClock;

void Ctr3dsGetClock(CtrClock *out);

// Mix one frame of PCM. Defined on the game side in rp2350/m4a_mix.c.
//
// Use the 16-bit form. DirectSound is 8-bit at the source and survives the
// wider type exactly. The PSG channels are made at 16 bits, and a console mixes
// the two in analog, not on an 8-bit grid. The 8-bit form removes the PSG
// detail and mixes one bit from clipping. It stays only for the RP2350 port's
// I2S ring.
int Rp2350MixFrame(int8_t *out, int n);
int Rp2350MixFrame16(int16_t *out, int n);

// Interleaved stereo PCM16: `out` holds 2*n samples, left then right. This is
// the preferred form. The m4a engine renders DirectSound into two buffers and
// pans every note. The PSG pans its four channels through NR51. The two mono
// forms above lose that placement.
int Rp2350MixFrameStereo16(int16_t *out, int n);

// The m4a engine data for the audio health report in 3ds/host/audio.c. Also
// defined on the game side in rp2350/m4a_mix.c, which records these on every
// mix.
//
// It answers what the host side cannot: does the sound engine run? `ident` must
// be ID_NUMBER after m4aSoundInit, and `samplesPerVBlank` must be 224.
// `bgmStatus` is zero when no song plays, which is a normal reason for silence
// but looks the same as a broken DSP. Any pointer can be NULL.
void Rp2350AudioDebug(uint32_t *ident, int32_t *samplesPerVBlank,
                      uint32_t *bgmStatus, uint32_t *zeroReturns);

// The mixer's own state, for the same report. It is a separate call because
// Rp2350AudioDebug's signature is shared with the RP2350 port's game_main.c. It
// explains a full frame of samples on every frame, all zero. Any pointer can be
// NULL. `engineFlags` follows the chain that the mix needs, the lowest bit
// first, so the lowest clear bit names the missing link. The M4A_DBG_* defines
// next to the implementation give the bit values.
void Rp2350MixerDebug(uint8_t *masterVolume, uint8_t *maxChans,
                      uint32_t *activeChans, uint32_t *engineFlags);

// The first live channel's contents, for the same report. When the engine is
// healthy, it tells apart three causes of a zero mix:
// - The volume chain is at zero.
// - The sample data is silence.
// - The channel took the compressed or reverse path, which is still a stub.
// `envVol` packs envelopeVolume<<16 | envelopeVolumeRight<<8 |
// envelopeVolumeLeft.
void Rp2350ChannelDebug(uint32_t *type, uint32_t *statusFlags, uint32_t *envVol,
                        uint32_t *frequency, uint32_t *sampleNonZero);

// The output peaks of each subsystem. Each half of the mixer is measured
// separately, because "PSG is silent, DirectSound is healthy" and "both are
// silent" are different faults. The summed peak cannot tell them apart.
//
// `dsPeak` and `cryPeak` are in the DirectSound sample domain (0..128).
// `psgPeak` is in the s16 domain of the sum. Any pointer can be NULL. They are
// running maxima and never reset.
//
// `clipped` counts the samples that the final clamp had to catch. It shows if
// the mix has headroom. It must stay 0.
void Rp2350AudioPeaks(uint32_t *dsPeak, uint32_t *psgPeak, uint32_t *cryPeak,
                      uint32_t *clipped);

// Mute one half of the mixer at a time, from the EXTRA tab.
//
// A tester cannot judge one half by ear while the other plays, and the log
// cannot measure "sounds wrong". Thus this is the only tool for a sound quality
// fault. Both halves are on by default.
void Rp2350SetAudioDebug(int psgOn, int reverbOn, int dsOn);

// The audio A/B state. STEREO is host side (a downmix, not a mixer setting).
// Rp2350SetAudioDebug pushes the other two into the mixer.
#define CTR_AUDIO_DBG_PSG    0
#define CTR_AUDIO_DBG_REVERB 1
#define CTR_AUDIO_DBG_STEREO 2
#define CTR_AUDIO_DBG_DS     3   // the sampled half
#define CTR_AUDIO_DBG_COUNT  4

int  Ctr3dsGetAudioDbg(int which);
void Ctr3dsSetAudioDbg(int which, int on);
void Ctr3dsApplyAudioDbg(int which, int on);

// ---- stall diagnostics ----------------------------------------------------
//
// A stage that takes a millisecond in an emulator and a second on a console
// cannot be seen from outside. The game stops, then continues, and nothing says
// which call caused it. These functions time a stage and write a line only when
// it is too slow. Thus sdmc:/3ds/emerald3ds/log.txt names the call. A release
// build keeps the timing and drops the file.
//
// Declared here, not in 3ds/host/trace.h, because the bottom screen is on the
// game side and must never see <3ds.h>. `const char *` and `unsigned int` cross
// the seam safely, like CtrTraceHex.
//
// Always compiled, like CtrLog: the measurement must come from the build that
// stalls.
unsigned int CtrTimeNowMs(void);
void CtrLogSlow(const char *stage, unsigned int startMs);

// ---- stage profiling ------------------------------------------------------
//
// CtrLogSlow finds stalls: it writes nothing under 50 ms. A cost below that on
// each frame can still set the frame rate, and only this profiler shows it.
//
// This adds up a stage over many frames and reports the mean and the worst. The
// average also gives resolution, because one sample of a very short stage tells
// nothing.
//
// It uses ticks, not milliseconds. CtrTimeNowMs is osGetTime, and its 1 ms
// granularity cannot resolve a paint. With SYSCLOCK_ARM11, a tick is about 3.7
// ns.
//
// `unsigned long long` crosses the seam safely, like `unsigned int` above.
//
// CtrProfile and CtrLogSlow are for the main thread only: both use static
// tables with no lock. The rasterizer's worker thread (3ds/host/video.c) and
// the I/O thread (3ds/host/io_thread.c) measure themselves, and the main thread
// reports. CtrTicksNow is safe from any thread.
unsigned long long CtrTicksNow(void);
void CtrProfile(const char *stage, unsigned long long startTicks);

#endif // CTR_BRIDGE_H
