// Phase-1 on-silicon self-test for the RP2350 BIOS reimplementations.
// Links against rp2350/bios.c and exercises each syscall against known vectors,
// printing PASS/FAIL over USB-CDC + UART. Proves toolchain -> SDK -> flash -> run.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"

typedef uint8_t  u8;  typedef uint16_t u16; typedef uint32_t u32;
typedef int16_t  s16; typedef int32_t  s32;

// --- BIOS impls under test (rp2350/bios.c) ---
extern void CpuSet(const void *src, void *dest, u32 control);
extern void CpuFastSet(const void *src, void *dest, u32 control);
extern void LZ77UnCompWram(const u32 *src, void *dest);
extern void RLUnCompWram(const u32 *src, void *dest);
extern s32  Div(s32 num, s32 denom);
extern u16  Sqrt(u32 num);

#define CPU_SET_16BIT 0x00000000u
#define CPU_SET_32BIT 0x04000000u
#define CPU_SET_FILL  0x01000000u  // bit 24: src is one element, repeated

static int g_pass, g_fail;
#define CHECK(cond, name) do { \
    if (cond) { printf("  [PASS] %s\n", name); g_pass++; } \
    else      { printf("  [FAIL] %s\n", name); g_fail++; } } while (0)

static void run_tests(void)
{
    g_pass = g_fail = 0;

    { u16 src[4] = {1,2,3,4}, dst[4] = {0};
      CpuSet(src, dst, 4 | CPU_SET_16BIT);
      CHECK(memcmp(src, dst, sizeof src) == 0, "CpuSet 16-bit copy"); }

    { u32 src[3] = {0xDEADBEEF,0x12345678,0xCAFEBABE}, dst[3] = {0};
      CpuSet(src, dst, 3 | CPU_SET_32BIT);
      CHECK(memcmp(src, dst, sizeof src) == 0, "CpuSet 32-bit copy"); }

    { u16 v = 0xABCD, dst[5] = {0};
      CpuSet(&v, dst, 5 | CPU_SET_16BIT | CPU_SET_FILL);
      int ok = 1; for (int i = 0; i < 5; i++) ok &= (dst[i] == 0xABCD);
      CHECK(ok, "CpuSet 16-bit fill"); }

    { u32 src[8], dst[8] = {0};
      for (int i = 0; i < 8; i++) src[i] = (u32)i * 0x01010101u;
      CpuFastSet(src, dst, 8);
      CHECK(memcmp(src, dst, sizeof src) == 0, "CpuFastSet 32-bit copy"); }

    // LZ77: GBA header 0x10, 24-bit size, then flag byte (0=all literals) + bytes
    { static const u8 __attribute__((aligned(4))) lz[] =
          {0x10, 4,0,0, 0x00, 'A','B','C','D'};
      u8 out[4] = {0};
      LZ77UnCompWram((const u32 *)lz, out);
      CHECK(memcmp(out, "ABCD", 4) == 0, "LZ77 literal decode"); }

    // LZ77 back-reference: literals A,B then match (len 4, disp 2) -> "ABABAB"
    { static const u8 __attribute__((aligned(4))) lz[] =
          {0x10, 6,0,0, 0x20, 'A','B', 0x10,0x01};
      u8 out[6] = {0};
      LZ77UnCompWram((const u32 *)lz, out);
      CHECK(memcmp(out, "ABABAB", 6) == 0, "LZ77 back-reference decode"); }

    // RL: literal run "WXYZ" then repeat 0x42 x5
    { static const u8 __attribute__((aligned(4))) rl[] =
          {0x30, 9,0,0, 0x03,'W','X','Y','Z', 0x82,0x42};
      u8 out[9] = {0};
      RLUnCompWram((const u32 *)rl, out);
      CHECK(memcmp(out, "WXYZ", 4) == 0 && out[4] == 0x42 && out[8] == 0x42,
            "RL decode (literal + run)"); }

    CHECK(Div(100, 7) == 14, "Div 100/7");
    CHECK(Div(5, 0)   == 0,  "Div by zero -> 0");
    CHECK(Sqrt(144)   == 12, "Sqrt 144");
    CHECK(Sqrt(1000)  == 31, "Sqrt 1000");
}

int main(void)
{
    stdio_init_all();
    sleep_ms(2500);  // give USB-CDC time to enumerate

    for (;;)
    {
        printf("\n=== Pokemon Emerald RP2350 - HAL self-test ===\n");
        printf("sys clock: %lu Hz\n", (unsigned long)clock_get_hz(clk_sys));
        run_tests();
        printf("=== %d passed, %d failed ===\n", g_pass, g_fail);
        sleep_ms(3000);
    }
}
