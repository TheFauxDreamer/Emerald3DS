// APS6404 PSRAM bring-up for the WeAct Core2350B (CS on GPIO 0 = QSPI CS1).
//
// This SDK has no built-in PSRAM support, so we configure the QMI for memory
// window 1 (XIP CS1) ourselves. Once initialised, PSRAM is XIP-mapped at
// PSRAM_BASE and behaves like normal memory (cached reads, write-through).

#ifndef RP2350_PSRAM_H
#define RP2350_PSRAM_H

#include <stddef.h>
#include <stdint.h>

// XIP window for the second chip select (CS1). CS0 (flash) is at 0x10000000.
#define PSRAM_BASE 0x11000000u

// Detect and initialise the PSRAM on the given chip-select GPIO (0 on this
// board). Returns the size in bytes, or 0 if no APS6404 PSRAM responded.
// Must run before any code relies on PSRAM; safe to call once at boot.
size_t psram_init(uint32_t cs_pin);

#endif // RP2350_PSRAM_H
