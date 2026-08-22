// PSRAM bring-up test for the WeAct Core2350B. Inits the APS6404 on QSPI CS1
// (GPIO 0), reports the detected ID/size over USB-CDC, then runs a full
// write/read pattern check (32-bit, plus a 16-bit framebuffer-style pass).

#include <stdio.h>

#include "pico/stdlib.h"
#include "../psram.h"

// NB: PSRAM CS is on GPIO47 on this board, NOT GPIO0 as the SDK board header
// (WEACT_STUDIO_RP2350B_PSRAM_CS_PIN) claims. Confirmed by JEDEC ID probe:
// GPIO0 returns zeros, GPIO47 returns MFID 0x0D / KGD 0x5D (APS6404, 8 MiB).
#define PSRAM_CS_PIN 47

int main(void) {
    stdio_init_all();
    sleep_ms(2500); // let USB enumerate

    size_t size = psram_init(PSRAM_CS_PIN);
    printf("psram_init: detected %u bytes (%u MiB)\n",
           (unsigned)size, (unsigned)(size / (1024 * 1024)));
    if (!size) {
        while (1) { printf("PSRAM NOT DETECTED (KGD != 0x5D)\n"); sleep_ms(1000); }
    }

    volatile uint32_t *p32 = (volatile uint32_t *)PSRAM_BASE;
    size_t words = size / 4;

    // Pass 1: 32-bit address-derived pattern, write whole chip then verify.
    for (size_t i = 0; i < words; i++)
        p32[i] = (uint32_t)(i * 2654435761u) ^ 0xA5A5A5A5u;
    size_t errors32 = 0, firstBad = 0;
    for (size_t i = 0; i < words; i++) {
        uint32_t want = (uint32_t)(i * 2654435761u) ^ 0xA5A5A5A5u;
        if (p32[i] != want) {
            if (!errors32) firstBad = i;
            errors32++;
        }
    }
    printf("pass1 (32-bit, %u MiB): %u errors%s\n",
           (unsigned)(size / (1024 * 1024)), (unsigned)errors32,
           errors32 ? "" : "  OK");
    if (errors32)
        printf("  first bad word %u: got 0x%08x want 0x%08x\n",
               (unsigned)firstBad, p32[firstBad],
               (uint32_t)(firstBad * 2654435761u) ^ 0xA5A5A5A5u);

    // Pass 2: 16-bit access over a framebuffer-sized region (240*160 RGB565).
    volatile uint16_t *p16 = (volatile uint16_t *)PSRAM_BASE;
    const size_t fb = 240 * 160;
    for (size_t i = 0; i < fb; i++) p16[i] = (uint16_t)(i * 40503u);
    size_t errors16 = 0;
    for (size_t i = 0; i < fb; i++)
        if (p16[i] != (uint16_t)(i * 40503u)) errors16++;
    printf("pass2 (16-bit, 240x160 fb): %u errors%s\n",
           (unsigned)errors16, errors16 ? "" : "  OK");

    uint n = 0;
    while (1) {
        printf("PSRAM test done: %u MiB, pass1=%s pass2=%s (%u)\n",
               (unsigned)(size / (1024 * 1024)),
               errors32 ? "FAIL" : "ok", errors16 ? "FAIL" : "ok", n++);
        sleep_ms(2000);
    }
}
