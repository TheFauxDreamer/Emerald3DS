#ifndef GUARD_GBA_FLASH_INTERNAL_H
#define GUARD_GBA_FLASH_INTERNAL_H

#if PLATFORM_3DS
// The 3DS backs the cart's 128 KB save flash with a plain RAM array
// (3ds/gba_mem.c). Reads go straight through it; the write hooks below mirror
// the region out to the SD card (3ds/host/save.c). Same hook names as RP2350
// so agb_flash.c needs no further seams.
extern u8 gCtrSaveFlash[];
#define FLASH_BASE (gCtrSaveFlash)
u16 Rp2350SaveEraseChip(void);
u16 Rp2350SaveEraseSector(u16 sectorNum);
u16 Rp2350SaveProgramSector(u16 sectorNum, u8 *src);
u16 Rp2350SaveProgramByte(u16 sectorNum, u32 offset, u8 data);
void Rp2350SaveSync(void);
#elif RP2350
// The GBA cart's 128 KB save flash is emulated in the LAST 128 KB of the
// 16 MB QSPI flash (offset 0xFE0000) -- far above the ~13.4 MB game image,
// so saves survive firmware reflashes. The mapping is exact: 4 KB sectors,
// erased state 0xFF, programming only clears bits. Reads go straight through
// this XIP-cached pointer; WRITES must reprogram the QSPI chip while the
// game executes from it, so they go through the hooks below, implemented
// RAM-resident with core-1 lockout in rp2350/hw/flash_save.c (weak no-op
// stubs in rp2350/bios.c keep non-game targets linking).
#define FLASH_BASE ((u8 *)0x10FE0000)
u16 Rp2350SaveEraseChip(void);
u16 Rp2350SaveEraseSector(u16 sectorNum);
u16 Rp2350SaveProgramSector(u16 sectorNum, u8 *src);
u16 Rp2350SaveProgramByte(u16 sectorNum, u32 offset, u8 data);
// The save code writes sectors as ~4080 single-byte programs; those coalesce
// into a 256-byte page buffer and flush on page boundaries. Reads must call
// this first so the XIP view includes the pending page.
void Rp2350SaveSync(void);
#else
#define FLASH_BASE ((u8 *)0xE000000)
#endif

#define FLASH_WRITE(addr, data) ((*(vu8 *)(FLASH_BASE + (addr))) = (data))

#define FLASH_ROM_SIZE_1M 131072 // 1 megabit ROM

#define SECTORS_PER_BANK 16

struct FlashSector
{
    u32 size;
    u8 shift;
    u16 count;
    u16 top;
};

struct FlashType {
    u32 romSize;
    struct FlashSector sector;
    u16 wait[2]; // game pak bus read/write wait

    // TODO: add support for anonymous unions/structs if possible
    union {
        struct {
        u8 makerId;
        u8 deviceId;
        } separate;
        u16 joined;
    } ids;
};

struct FlashSetupInfo
{
    u16 (*programFlashByte)(u16, u32, u8);
    u16 (*programFlashSector)(u16, u8 *);
    u16 (*eraseFlashChip)(void);
    u16 (*eraseFlashSector)(u16);
    u16 (*WaitForFlashWrite)(u8, u8 *, u8);
    const u16 *maxTime;
    struct FlashType type;
};

extern u16 gFlashNumRemainingBytes;

extern u16 (*ProgramFlashByte)(u16, u32, u8);
extern u16 (*ProgramFlashSector)(u16, u8 *);
extern u16 (*EraseFlashChip)(void);
extern u16 (*EraseFlashSector)(u16);
extern u16 (*WaitForFlashWrite)(u8, u8 *, u8);
extern const u16 *gFlashMaxTime;
extern const struct FlashType *gFlash;

extern u8 (*PollFlashStatus)(u8 *);
extern u8 gFlashTimeoutFlag;

extern const struct FlashSetupInfo MX29L010;
extern const struct FlashSetupInfo LE26FV10N1TS;
extern const struct FlashSetupInfo DefaultFlash;

void SwitchFlashBank(u8 bankNum);
u16 ReadFlashId(void);
void StartFlashTimer(u8 phase);
void SetReadFlash1(u16 *dest);
void StopFlashTimer(void);
void ReadFlash(u16 sectorNum, u32 offset, u8 *dest, u32 size);

u16 WaitForFlashWrite_Common(u8 phase, u8 *addr, u8 lastData);

u16 EraseFlashChip_MX(void);
u16 EraseFlashSector_MX(u16 sectorNum);
u16 ProgramFlashByte_MX(u16 sectorNum, u32 offset, u8 data);
u16 ProgramFlashSector_MX(u16 sectorNum, u8 *src);

#endif // GUARD_GBA_FLASH_INTERNAL_H
