# Hardware

Everything the port needs to run, and how to wire it.

## Bill of materials

| Item | Notes |
|---|---|
| **WeAct Studio Core2350B** | RP2350B, 16 MB QSPI flash. The board this was developed and tested on (`PICO_BOARD=weact_studio_rp2350b_core`). |
| **HSTX → HDMI/DVI breakout** | Any board wired in the [Pico DVI Sock](https://github.com/Wren6991/Pico-DVI-Sock) pin order. See below. |
| **10 × momentary buttons** | Wired to ground; the RP2350's internal pull-ups are enabled in firmware, so no external resistors. |
| **PCM5102A I²S DAC** *(optional)* | For audio. The game runs correctly and silently without it. |
| **Debug probe** *(optional)* | A Raspberry Pi Debugprobe was used for SWD. Not required — `picotool` over USB is enough to flash. |

Any RP2350 board with ≥16 MB of flash and GP0–GP9 and GP12–GP19 free (plus
GP20–GP22 if you want audio) should work, but only the Core2350B has been
tested. The image is 11.7 MB and the save region occupies
the last 128 KB of flash, so **16 MB is a hard minimum**.

## Pinout

All of these are fixed in firmware; change them there if your wiring differs.

### Display — HSTX (GP12–GP19)

HSTX output bits map to GP12 + *n*, so the eight display pins are not
relocatable. Defined in `rp2350/hstx_display.c` (`hstx_setup_lanes`).

| TMDS signal | GPIO pair |
|---|---|
| Data 0 (blue) | GP12 (+) / GP13 (−) |
| Clock | GP14 (+) / GP15 (−) |
| Data 2 (red) | GP16 (+) / GP17 (−) |
| Data 1 (green) | GP18 (+) / GP19 (−) |

This is the Pico DVI Sock ordering — note that D1 and D2 are **not** in numeric
GPIO order. If your breakout uses a different lane order, edit
`lane_to_output_bit[]` in `hstx_setup_lanes()` rather than rewiring.

Output is 640×480p60 (2× pixel-doubled from the GBA's 240×160), pixel clock
25.2 MHz.

### Buttons — GP0–GP9

Wire each button between its GPIO and **ground**. Internal pull-ups are enabled,
so an unpressed button reads high. This matches the GBA's active-low
`REG_KEYINPUT`, which is why GPIO *n* maps directly onto key bit *n* with no
translation. Defined in `rp2350/hw/game_main.c` (`buttons_init`).

| GPIO | Button | | GPIO | Button |
|---|---|---|---|---|
| GP0 | A | | GP5 | Left |
| GP1 | B | | GP6 | Up |
| GP2 | Select | | GP7 | Down |
| GP3 | Start | | GP8 | R |
| GP4 | Right | | GP9 | L |

> **Do not leave these pins floating without pull-ups enabled.** A zeroed key
> register reads as *every button held*, which triggers Emerald's
> A+B+Start+Select soft reset into RFU code that hangs. This cost real debugging
> time during bring-up.

### Audio — I²S (GP20–GP22), optional

For a PCM5102A or similar. Defined in `rp2350/hw/i2s_audio.h`.

| GPIO | Signal | PCM5102A pin |
|---|---|---|
| GP20 | DIN (data) | DIN |
| GP21 | BCK (bit clock) | BCK |
| GP22 | LRCK (word select) | LRCK |

Driven by PIO + DMA at the game's native 13440 Hz, so no resampling. On a
PCM5102A breakout, also tie SCK to ground to select its internal PLL.

### PSRAM — QSPI CS1 (GP47), optional and unused

`rp2350/psram.c` brings up an APS6404 on the second XIP chip select. It was the
fallback plan for holding the ROM image if it hadn't fit in flash. **It did fit**
(11.7 MB of 16 MB), so the shipping firmware does not use PSRAM. The driver and
`psram_test` target remain for anyone who needs the headroom.

## Clocking and power

Set in firmware; listed here because they are outside stock defaults.

- **`clk_sys` = 252 MHz** (overclock). `clk_hstx` is `clk_sys / 2` = 126 MHz,
  giving the 25.2 MHz pixel clock 640×480p60 requires.
- **Core voltage 1.15 V** (`vreg`), raised for stability at 252 MHz.

Both values are load-bearing. The HSTX divider is only 2 bits wide (max ÷3), so
`clk_sys` cannot be raised arbitrarily without breaking the pixel clock — see
[PORTING.md](PORTING.md) for the overclock options that preserve it.

## Serial console

USB CDC only — `cat /dev/cu.usbmodem*` on macOS. **stdio over UART is
deliberately disabled**, because UART0's default pins are GP12/GP13, which
belong to HSTX.

Single-key commands, useful for debugging on a board with no buttons attached:

| Key | Action |
|---|---|
| `A` `B` `E` `S` `R` `L` `U` `D` | Inject a 6-frame button press (E = Select, S = Start) — lets you play remotely over USB |
| `p` | Toggle the on-screen FPS overlay |
| `t` | Per-frame timing trace |
| `k` | Toggle key-change printing |
| `d` / `v` / `f` | Hex-dump registers+palette+OAM / VRAM / framebuffer |
| `h` | Scanout hardware registers |
| `w` / `W` | Save-flash smoke test / factory-erase the save region |

A liveness line prints every 5 seconds with frame-rate, vsync, and PPU-pass
statistics.
