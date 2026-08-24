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

// ---- local wireless: the Cable Club over 3DS UDS ----
//
// The GBA cable link is a fixed shared bus that moves exactly ONE 16-byte
// command per player per frame, and src/link.c isolates that behind
// gLink.sendQueue / gLink.recvQueue. This is that pipe and nothing more: 64
// bytes a frame for four players, about 3.8 KB/s.
//
// Cross-play with a real GBA is impossible. This is Emerald3DS talking to
// Emerald3DS, and the UI must not imply otherwise.

#define CTR_LINK_CMD_BYTES    16   // CMD_LENGTH (8) * sizeof(u16)
#define CTR_LINK_MAX_PLAYERS  4    // MAX_LINK_PLAYERS; UDS itself allows 16
#define CTR_LINK_NAME_LEN     24   // host name shown in the join list

// What the pairing UI and the transport both poll.
#define CTR_LINK_IDLE       0
#define CTR_LINK_SCANNING   1
#define CTR_LINK_HOSTING    2   // network up, may be alone
#define CTR_LINK_JOINING    3
#define CTR_LINK_CONNECTED  4   // two or more nodes present
#define CTR_LINK_FAILED     5

typedef struct {
    uint8_t state;        // CTR_LINK_*
    uint8_t playerCount;  // nodes present, 1..CTR_LINK_MAX_PLAYERS
    uint8_t localId;      // 0-based; 0 is the host, i.e. the GBA's master
    uint8_t isHost;
} CtrLinkStatus;

// Pairing, driven from the LINK tab. All are non-blocking; poll the status.
void Ctr3dsLinkHost(void);
void Ctr3dsLinkScan(void);
void Ctr3dsLinkJoin(int index);
void Ctr3dsLinkStop(void);
void Ctr3dsLinkGetStatus(CtrLinkStatus *out);

// Results of the last Ctr3dsLinkScan(). `name` is plain ASCII, not the game's
// encoding, because it comes from the host console rather than from Emerald.
int  Ctr3dsLinkScanCount(void);
void Ctr3dsLinkScanName(int index, char *out, int outSize);
int  Ctr3dsLinkScanPlayers(int index);

// Scalar view of the same status, for src/link.c. The game side declares these
// in include/link.h in game types rather than including this header, the way
// include/gba/flash_internal.h declares Rp2350Save*; keeping CtrLinkStatus out
// of src/ means there is no struct definition to drift between the two worlds.
int  Ctr3dsLinkIsConnected(void);
int  Ctr3dsLinkPlayerCount(void);
int  Ctr3dsLinkLocalId(void);

// One frame of link traffic. `sendCmd` is CTR_LINK_CMD_BYTES; `recvCmds` is
// CTR_LINK_MAX_PLAYERS * CTR_LINK_CMD_BYTES, indexed by 0-based player id.
//
// Returns 1 when a complete set for this frame arrived, 0 when it did not, in
// which case the caller reports lag rather than treating it as fatal. A cable
// is synchronous and UDS is not, so this blocks briefly waiting for peers: that
// stall IS the lockstep, and is what stops the two consoles drifting apart.
int  Ctr3dsLinkExchange(const void *sendCmd, void *recvCmds);

// Mix one frame of PCM. Implemented game-side in rp2350/m4a_1.c.
int Rp2350MixFrame(int8_t *out, int n);

#endif // CTR_BRIDGE_H
