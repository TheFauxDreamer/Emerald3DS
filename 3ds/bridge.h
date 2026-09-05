// The seam between the two worlds of this port.
//
// include/gba/types.h (game) and <3ds.h> (libctru) both typedef u8/u16/u32 to
// different-but-compatible things, and the game's include/ shadows libc headers
// (string.h, strings.h). A translation unit that includes both will not build.
// So:
//
//   game-side TUs : src/**, rp2350/{bios,asm_stubs,m4a_1}.c, 3ds/gba_mem.c,
//                   3ds/ui/**            -- game headers only, never <3ds.h>
//   host-side TUs : 3ds/host/**, rp2350/ppu.c
//                                        -- libctru only, never game headers
//
// Everything they say to each other is declared here, in stdint types only.
// This header includes neither side's headers and must stay that way.

#ifndef CTR_BRIDGE_H
#define CTR_BRIDGE_H

#include <stdint.h>

#define CTR_GBA_WIDTH   240
#define CTR_GBA_HEIGHT  160

#define CTR_BOTTOM_WIDTH   320
#define CTR_BOTTOM_HEIGHT  240

// ---------------------------------------------------------------- game side --

// Allocate and clear the GBA memory regions. MUST run before any other game
// code: every VRAM/palette/OAM/register access derives from gGbaMem.
void Ctr3dsInitGbaMemory(void);

// Region bases for ppu_set_memory(). Valid only after Ctr3dsInitGbaMemory().
void CtrGetGbaRegions(const void **reg, const void **pal,
                      const void **vram, const void **oam);

// Write the frame's button state into REG_KEYINPUT. GBA keys are ACTIVE-LOW:
// a 0 bit means pressed. Bit order is A,B,Select,Start,Right,Left,Up,Down,R,L.
void CtrSetKeyInput(uint16_t keysActiveLow);

// The game's superloop (src/main.c). Never returns.
void AgbMain(void);

// ---- bottom screen (game side: it draws with Emerald's own fonts and gfx) ----

typedef struct {
    int16_t x, y;          // touch position, 0..319 / 0..239; valid if touching
    uint8_t touching;
    uint8_t justPressed;
    uint8_t justReleased;
} CtrTouchState;

void CtrBottomInit(void);
void CtrBottomUpdate(const CtrTouchState *touch);

// Non-zero when the framebuffer changed since the host last uploaded it.
// The bottom screen is mostly static, so this gates the texture upload.
int  CtrBottomIsDirty(void);
void CtrBottomClearDirty(void);

// 320x240 RGB565, row-major. Stable pointer, valid after CtrBottomInit().
const uint16_t *CtrBottomFramebuffer(void);

// ---------------------------------------------------------------- host side --

// Per-frame hook called at the end of every game frame from AgbMain's loop
// (src/main.c, under #if RP2350). Renders, presents, samples input, feeds
// audio, and paces the game to the GBA frame rate.
void Rp2350PresentFrame(void);

// Save flash write hooks, called from src/agb_flash*.c. Reads go straight
// through FLASH_BASE (gCtrSaveFlash); only writes come through here.
// Return 0 on success, 0x80FF on failure.
uint16_t Rp2350SaveEraseChip(void);
uint16_t Rp2350SaveEraseSector(uint16_t sectorNum);
uint16_t Rp2350SaveProgramSector(uint16_t sectorNum, uint8_t *src);
uint16_t Rp2350SaveProgramByte(uint16_t sectorNum, uint32_t offset, uint8_t data);
void     Rp2350SaveSync(void);

// Write the save image to the SD card NOW, ignoring the write-coalescing
// debounce. Called from src/save.c the instant a save completes, so the file is
// current before the game hands control back to the player. Without it the save
// depends on the process surviving long enough for the debounce to fire, which
// closing the emulator window does not guarantee.
void     CtrSaveCommit(void);

// Fast-forward. The game's superloop always runs one logical frame per
// Rp2350PresentFrame() call; this sets how many of those happen per DISPLAYED
// frame, so 2 means the game advances twice as fast.
//
// It works because CtrVideoPresent() is both the software rasteriser and the
// VBlank wait: skipping it on the intermediate frames drops the cost and the
// 60 Hz pacing together. Clamped to CTR_SPEED_MIN..CTR_SPEED_MAX.
//
// This is a request, not a guarantee. If the console cannot keep up the game
// simply runs slower than asked, which is a slowdown, not a fault.
//
// Any whole multiplier in this range works; the bounds are only a clamp. Which
// values are actually offered is the UI's business (3ds/ui/tab_extra.c), so a
// button shortcut could pick a different set without touching the host.
#define CTR_SPEED_MIN 1
#define CTR_SPEED_MAX 8

void Ctr3dsSetSpeed(int multiplier);
int  Ctr3dsGetSpeed(void);

// What fast-forward does to the soundtrack (EXTRA tab, page 1).
//
// The sound engine advances the song one tick per m4aSoundMain() call, and the
// superloop runs 2x to 8x logical frames per displayed frame while
// fast-forwarding. Ticking it every game frame therefore plays the music at the
// multiplier, and because the DSP still consumes only one frame's worth of
// samples the surplus is dropped: music racing past in chunks.
//
// NORMAL ticks the engine once per DISPLAYED frame instead, so the soundtrack
// keeps its tempo and full quality while the game runs fast. FAST is the older
// behaviour, kept because a rising pitch is also a useful cue that
// fast-forward is engaged.
#define CTR_FFAUDIO_NORMAL 0
#define CTR_FFAUDIO_FAST   1

void Ctr3dsSetFfAudio(int mode);
int  Ctr3dsGetFfAudio(void);

// Whether this game frame is the one that carries audio. Always true at 1x, and
// under fast-forward it follows the setting above: once per displayed frame for
// NORMAL, every game frame for FAST. src/main.c gates m4aSoundMain() and
// MapMusicMain() on this, and the host gates the mixer drain on it.
int Ctr3dsIsAudioFrame(void);

// Turbo bindings for the four 3DS buttons the GBA has no equivalent of, so
// nothing else wants them. Holding a bound button overrides the speed above for
// as long as it is held; the GAME SPEED selection stays the resting speed.
//
// ZL and ZR exist only on a New 3DS. On an Old 3DS they never register, so a
// binding there does nothing at all, which the UI says rather than leaving it a
// mystery.
#define CTR_TURBO_X      0
#define CTR_TURBO_Y      1
#define CTR_TURBO_ZL     2
#define CTR_TURBO_ZR     3
#define CTR_TURBO_COUNT  4

// A button is bound to exactly one thing: nothing, a turbo speed, or the touch
// UI modifier. One value, so the two uses cannot both claim a button.
#define CTR_BIND_OFF  0
#define CTR_BIND_MOD  0xFF   // outside the speed range on purpose

// `value` is CTR_BIND_OFF, a multiplier in CTR_SPEED_MIN..CTR_SPEED_MAX, or
// CTR_BIND_MOD. Anything else is rejected.
void Ctr3dsSetTurboBind(int button, int value);
int  Ctr3dsGetTurboBind(int button);

// Is a button bound to CTR_BIND_MOD currently held?
//
// The modifier is always one of these four, never a GBA button: the game keeps
// running on the top screen while the touch screen is in use, so a GBA button
// held as a modifier is delivered to it as well. L would be worst of all, since
// the L=A option turns it into an A press (src/main.c:363) and jumping a list
// would talk to whatever is standing in front of you.
int  Ctr3dsUiModifierHeld(void);

// Top-screen scale. The GBA frame is 240x160 and the top screen is 400x240, so
// only 1.5x fills the height exactly; the other two trade borders against
// fidelity. Unlike the speed above, this one is remembered across launches
// (3ds/host/settings.c): it is a display preference, not a mode you can be
// surprised by.
//
// FILL stretches horizontally by 11%, because the GBA is 3:2 and the panel is
// 5:3. There is no aspect-correct way to fill it without cropping the picture,
// so the trade is deliberate and the UI says so.
#define CTR_TOP_SCALE_1X     0   // 240x160, pixel-perfect, wide borders
#define CTR_TOP_SCALE_1_5X   1   // 360x240, fills the height, 20px bars
#define CTR_TOP_SCALE_FILL   2   // 400x240, no borders, 11% wider
#define CTR_TOP_SCALE_COUNT  3
#define CTR_TOP_SCALE_DEFAULT CTR_TOP_SCALE_1_5X

void Ctr3dsSetTopScale(int mode);
int  Ctr3dsGetTopScale(void);

// Show every bottom-screen tab, including the ones the save has not unlocked.
//
// A testing aid, not a cheat: the tabs are gated on the same flags the start
// menu uses, so before the PokeNav there is normally no way to look at the MAP
// tab at all. Nothing it reveals writes game state -- PARTY, MAP and DEX are
// read-only -- so the worst it can do is show you a Pokedex you have not been
// given yet. Persisted, so a test session survives a relaunch.
void Ctr3dsSetShowAllTabs(int on);
int  Ctr3dsGetShowAllTabs(void);

// Gameplay tweaks, shown on page 2 of the EXTRA tab.
//
// These ARE cheats, unlike everything above, and the distinction is worth
// keeping sharp. Fast-forward, top scale and the tab override all leave the
// game playing exactly as Game Freak wrote it; every option below deliberately
// does not. They live here rather than in the save block because a setting the
// player taps should stick even if the game is never saved afterwards, and
// settings.bin is written on the tap.
//
// The randomiser has no seed field on purpose: it is derived from the save's
// own trainer ID, so a mapping is stable for one playthrough, differs between
// playthroughs, and survives being toggled off and back on.
#define CTR_CAP_OFF      0
#define CTR_CAP_SOFT     1   // reduced exp past the cap
#define CTR_CAP_HARD     2   // no exp at all past the cap

#define CTR_BAGSORT_OFF  0   // leave the player's own order alone
#define CTR_BAGSORT_TYPE 1   // ascending item id, which groups by category
#define CTR_BAGSORT_NAME 2   // alphabetical

void Ctr3dsSetExpAll(int on);
int  Ctr3dsGetExpAll(void);

void Ctr3dsSetLevelCap(int mode);   // CTR_CAP_*
int  Ctr3dsGetLevelCap(void);

void Ctr3dsSetRandomizer(int on);
int  Ctr3dsGetRandomizer(void);

void Ctr3dsSetBagSort(int mode);    // CTR_BAGSORT_*
int  Ctr3dsGetBagSort(void);

// The console's real-time clock, standing in for the cartridge RTC. The GBA
// carts carried an S-3511A; a 3DS has no cart, so src/siirtc.c is backed by
// this instead (under PLATFORM_3DS). That file is game-side and includes this
// header directly, the way 3ds/ui/*.c already do.
//
// Fields are PLAIN BINARY. The driver applies the BCD encoding the real chip
// would have used, because that is what src/rtc.c reads back.
typedef struct {
    uint8_t year;       // years since 2000, 0..99: the chip stores two digits
    uint8_t month;      // 1..12
    uint8_t day;        // 1..31
    uint8_t dayOfWeek;  // 0..6, Sunday = 0
    uint8_t hour;       // 0..23, always 24-hour
    uint8_t minute;     // 0..59
    uint8_t second;     // 0..59
} CtrClock;

void Ctr3dsGetClock(CtrClock *out);

// Mix one frame of PCM. Implemented game-side in rp2350/m4a_mix.c.
//
// Prefer the 16-bit form. DirectSound is 8-bit at source and survives the wider
// type exactly, but the PSG channels are generated at 16-bit precision and a
// console mixes the two in the analog domain rather than on an 8-bit grid, so
// the narrow form quantises the PSG away and mixes one bit from clipping. The
// 8-bit entry point remains only because the RP2350 port's I2S ring is built
// on it.
int Rp2350MixFrame(int8_t *out, int n);
int Rp2350MixFrame16(int16_t *out, int n);

// Interleaved stereo PCM16: `out` holds 2*n samples, left then right. The
// preferred form. m4a renders DirectSound into two separate buffers and pans
// every note across them, and the PSG pans its four channels through NR51, so
// the two mono forms above are both discarding placement the music was written
// with.
int Rp2350MixFrameStereo16(int16_t *out, int n);

// m4a engine telemetry, for the audio health report in 3ds/host/audio.c. Also
// game-side in rp2350/m4a_mix.c, which already snapshots these every mix.
//
// It answers the question the host side cannot: whether the SOUND ENGINE is
// running at all. `ident` should read ID_NUMBER once m4aSoundInit has run,
// `samplesPerVBlank` should be 224, and `bgmStatus` is zero when no song is
// playing, which is a perfectly good reason for silence and looks identical to
// a broken DSP from outside. Any pointer may be NULL.
void Rp2350AudioDebug(uint32_t *ident, int32_t *samplesPerVBlank,
                      uint32_t *bgmStatus, uint32_t *zeroReturns);

// The mixer's own state, for the same report. Separate call because
// Rp2350AudioDebug's signature is shared with the RP2350 port's game_main.c.
// Answers the one case the host side cannot see into: a full frame of samples
// arriving every frame with every sample zero. Any pointer may be NULL.
// `engineFlags` walks the chain the mix depends on, lowest bit first, so the
// lowest CLEAR bit names the link that was never made. Bit values and their
// meanings are the M4A_DBG_* defines beside the implementation.
void Rp2350MixerDebug(uint8_t *masterVolume, uint8_t *maxChans,
                      uint32_t *activeChans, uint32_t *engineFlags);

// The first live channel's contents, for the same report. Separates "the
// volume chain collapsed" from "the sample data is silence" from "this channel
// took the still-stubbed compressed/reverse path", which is everything that can
// still produce an all-zero mix once the engine itself measures healthy.
// `envVol` packs envelopeVolume<<16 | envelopeVolumeRight<<8 | envelopeVolumeLeft.
void Rp2350ChannelDebug(uint32_t *type, uint32_t *statusFlags, uint32_t *envVol,
                        uint32_t *frequency, uint32_t *sampleNonZero);

// Per-subsystem output peaks. Each half of the mixer is measured on its own,
// because "PSG is silent under a healthy DirectSound" and "both are silent" are
// different faults with different fixes, and the summed peak cannot tell them
// apart. `dsPeak` and `cryPeak` are in the DirectSound sample domain (0..128);
// `psgPeak` is in the s16 domain the two are summed in. Any pointer may be
// NULL. Free-running maxima, never reset.
//
// `clipped` counts samples the final clamp had to catch, which is the one
// figure that says whether the mix still has headroom. It should stay 0.
void Rp2350AudioPeaks(uint32_t *dsPeak, uint32_t *psgPeak, uint32_t *cryPeak,
                      uint32_t *clipped);

// Silence one half of the mixer at a time, from the EXTRA tab.
//
// Neither half can be judged by ear while the other is playing, and nothing in
// the log can measure "sounds wrong", so this is the only instrument there is
// for a fault about quality rather than plumbing. Both default on.
void Rp2350SetAudioDebug(int psgOn, int reverbOn, int dsOn);

// Audio A/B state. STEREO is host-side (it is a downmix, not a mixer setting),
// the other two are pushed into the mixer by Rp2350SetAudioDebug.
#define CTR_AUDIO_DBG_PSG    0
#define CTR_AUDIO_DBG_REVERB 1
#define CTR_AUDIO_DBG_STEREO 2
#define CTR_AUDIO_DBG_DS     3   // the sampled half
#define CTR_AUDIO_DBG_COUNT  4

int  Ctr3dsGetAudioDbg(int which);
void Ctr3dsSetAudioDbg(int which, int on);
void Ctr3dsApplyAudioDbg(int which, int on);

#endif // CTR_BRIDGE_H
