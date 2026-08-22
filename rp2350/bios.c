// GBA BIOS syscalls reimplemented in C for the RP2350 port.
//
// The browser host (web/app.js, importsFor()) provides these as imports under
// WASM; on bare-metal we provide real C definitions. Semantics are matched to
// app.js so behaviour is identical to the (verified) WASM reference and the
// Phase-2 software PPU that will be ported from the same file. Interrupt/reset
// syscalls are no-ops, matching app.js's default (undefined-import -> 0) — the
// main loop drives frames directly via VBlankIntr() (see src/main.c).
#include "global.h"
#include "gba/syscall.h"
#include <math.h>

// Frame heartbeat: bumped each WasmRunFrame() (src/main.c) so liveness can be
// confirmed over SWD during bring-up before any display exists.
volatile u32 gRp2350FrameCount = 0;

// Per-frame present hook, called at the end of each WasmRunFrame() (src/main.c).
// Weak no-op default so the headless/SWD targets (e.g. emerald_hwtest) link with
// no display; the `emerald` target overrides it (game_main.c) to render the PPU
// frame into the SRAM framebuffer and scan it out over HSTX.
__attribute__((weak)) void Rp2350PresentFrame(void) {}

// Save-flash write hooks (include/gba/flash_internal.h). Weak no-ops so
// headless/SWD targets link without the QSPI write path; the `emerald`
// target overrides them in rp2350/hw/flash_save.c with real flash
// programming. The no-ops report success and persist nothing.
__attribute__((weak)) u16 Rp2350SaveEraseChip(void) { return 0; }
__attribute__((weak)) u16 Rp2350SaveEraseSector(u16 sectorNum) { return 0; }
__attribute__((weak)) u16 Rp2350SaveProgramSector(u16 sectorNum, u8 *src) { return 0; }
__attribute__((weak)) u16 Rp2350SaveProgramByte(u16 sectorNum, u32 offset, u8 data) { return 0; }
__attribute__((weak)) void Rp2350SaveSync(void) {}

// --- Memory fill/copy (CpuSet / CpuFastSet) ---------------------------------
// control: bits 0..20 = unit count, bit 24 = fill (src is one element),
//          bit 26 = 32-bit transfer (CpuSet only; CpuFastSet is always 32-bit).
#undef CpuSet
#undef CpuFastSet

void CpuSet(const void *src, void *dest, u32 control)
{
    u32 count = control & 0x1FFFFF;
    u32 fill = (control >> 24) & 1;

    if (control & CPU_SET_32BIT)
    {
        const u32 *s = src;
        u32 *d = dest;
        if (fill) { u32 v = *s; while (count--) *d++ = v; }
        else      { while (count--) *d++ = *s++; }
    }
    else
    {
        const u16 *s = src;
        u16 *d = dest;
        if (fill) { u16 v = *s; while (count--) *d++ = v; }
        else      { while (count--) *d++ = *s++; }
    }
}

void CpuFastSet(const void *src, void *dest, u32 control)
{
    u32 count = control & 0x1FFFFF;
    u32 fill = (control >> 24) & 1;
    const u32 *s = src;
    u32 *d = dest;
    if (fill) { u32 v = *s; while (count--) *d++ = v; }
    else      { while (count--) *d++ = *s++; }
}

// --- LZ77 decompression -----------------------------------------------------
static void Lz77UnComp(const u32 *srcWord, void *destVoid)
{
    const u8 *src = (const u8 *)srcWord;
    u8 *dest = destVoid;
    u32 size = src[1] | (src[2] << 8) | (src[3] << 16);
    const u8 *s = src + 4;
    u8 *d = dest;
    u8 *end = dest + size;

    while (d < end)
    {
        u8 flags = *s++;
        for (int bit = 7; bit >= 0 && d < end; bit--)
        {
            if (flags & (1 << bit))
            {
                u32 pair = (s[0] << 8) | s[1];
                s += 2;
                u32 length = (pair >> 12) + 3;
                u32 disp = (pair & 0xFFF) + 1;
                while (length-- && d < end) { *d = *(d - disp); d++; }
            }
            else
            {
                *d++ = *s++;
            }
        }
    }
}

void LZ77UnCompWram(const u32 *src, void *dest) { Lz77UnComp(src, dest); }
void LZ77UnCompVram(const u32 *src, void *dest) { Lz77UnComp(src, dest); }

// --- Run-length decompression ----------------------------------------------
static void RlUnComp(const u32 *srcWord, void *destVoid)
{
    const u8 *src = (const u8 *)srcWord;
    u8 *dest = destVoid;
    u32 size = src[1] | (src[2] << 8) | (src[3] << 16);
    const u8 *s = src + 4;
    u8 *d = dest;
    u8 *end = dest + size;

    while (d < end)
    {
        u8 flag = *s++;
        if (flag & 0x80)
        {
            u32 count = (flag & 0x7F) + 3;
            u8 value = *s++;
            while (count-- && d < end) *d++ = value;
        }
        else
        {
            u32 count = (flag & 0x7F) + 1;
            while (count-- && d < end) *d++ = *s++;
        }
    }
}

void RLUnCompWram(const u32 *src, void *dest) { RlUnComp(src, dest); }
void RLUnCompVram(const u32 *src, void *dest) { RlUnComp(src, dest); }

// --- Affine matrix helpers (matched to app.js affineTerms) ------------------
// app.js: angle = rotation * 2*PI / 256; sin/cos scaled by 256 (8.8 fixed).
static void AffineTerms(s16 xScale, s16 yScale, u16 rotation,
                        s32 *pa, s32 *pb, s32 *pc, s32 *pd)
{
    float angle = rotation * (float)(2.0 * M_PI) / 256.0f;
    float s = sinf(angle) * 256.0f;
    float c = cosf(angle) * 256.0f;
    *pa = (s32)(c * xScale / 256.0f);
    *pb = (s32)(-s * yScale / 256.0f);
    *pc = (s32)(s * xScale / 256.0f);
    *pd = (s32)(c * yScale / 256.0f);
}

void BgAffineSet(struct BgAffineSrcData *src, struct BgAffineDstData *dest, s32 count)
{
    for (s32 i = 0; i < count; i++)
    {
        s32 pa, pb, pc, pd;
        AffineTerms(src[i].sx, src[i].sy, src[i].alpha, &pa, &pb, &pc, &pd);
        dest[i].pa = (s16)pa;
        dest[i].pb = (s16)pb;
        dest[i].pc = (s16)pc;
        dest[i].pd = (s16)pd;
        dest[i].dx = src[i].texX - src[i].scrX * pa - src[i].scrY * pb;
        dest[i].dy = src[i].texY - src[i].scrX * pc - src[i].scrY * pd;
    }
}

void ObjAffineSet(struct ObjAffineSrcData *src, void *dest, s32 count, s32 offset)
{
    u8 *d = dest;
    for (s32 i = 0; i < count; i++)
    {
        s32 pa, pb, pc, pd;
        AffineTerms(src[i].xScale, src[i].yScale, src[i].rotation, &pa, &pb, &pc, &pd);
        // 'offset' is the BYTE distance between consecutive matrix terms (GBA
        // BIOS pBoundary; matches app.js objAffineSet): 2 for the packed
        // 4-halfword matrix structs the game passes, 8 when writing OAM
        // directly. (Misreading it as a halfword stride wrote pc/pd past the
        // caller's 8-byte struct: garbage matrices -- affine sprites like the
        // Game Freak letters collapsed to a line -- plus stack corruption.)
        *(s16 *)(void *)(d + 0) = (s16)pa;
        *(s16 *)(void *)(d + offset) = (s16)pb;
        *(s16 *)(void *)(d + offset * 2) = (s16)pc;
        *(s16 *)(void *)(d + offset * 3) = (s16)pd;
        d += offset * 4;
    }
}

// --- Math syscalls ----------------------------------------------------------
s32 Div(s32 num, s32 denom) { return denom ? num / denom : 0; }
u16 Sqrt(u32 num) { return (u16)sqrt((double)num); }

u16 ArcTan2(s16 x, s16 y)
{
    // GBA returns a u16 where 0x10000 == 360 degrees.
    float a = atan2f((float)y, (float)x);          // (-PI, PI]
    s32 v = (s32)(a * (float)(0x10000) / (float)(2.0 * M_PI));
    return (u16)(v & 0xFFFF);
}

// --- Interrupt / reset / link (no-ops, matching app.js defaults) ------------
// Frames are driven directly: src/main.c calls VBlankIntr() each WasmRunFrame.
void VBlankIntrWait(void) {}
void RegisterRamReset(u32 resetFlags) { (void)resetFlags; }
void SoftReset(u32 resetFlags) { (void)resetFlags; }
int MultiBoot(struct MultiBootParam *mp) { (void)mp; return 1; }
