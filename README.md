# pokeemerald-rp2350

**Pokémon Emerald running natively on a $6 microcontroller** — full game, 60 fps,
HDMI out, real buttons, saves that survive a power cycle. No emulator: the
[pret decompilation](https://github.com/pret/pokeemerald) is recompiled from
ARMv4T to Cortex-M33 and the Game Boy Advance's video hardware is reimplemented
in software on the second core.

Target board is a **WeAct Studio Core2350B** (RP2350B, 16 MB QSPI flash). The
whole game — code, graphics, maps, music — is an 11.7 MB image executed in place
from flash.

```
        RP2350B @ 252 MHz
  ┌──────────────┬──────────────┐
  │    core 0    │    core 1    │
  │  game logic  │  PPU render  │──▶ 240×160 RGB565 framebuffer
  │   ~0.3 ms/f  │  ~12 ms/f    │         │
  └──────────────┴──────────────┘         ▼
         ▲                          HSTX ──▶ DVI/HDMI 640×480p60
    GP0–GP9 buttons                        (2× upscale, DMA control-block ring)
```

## Status

Playable start to finish as far as it has been tested — boots, plays the intro,
starts a new game, walks the overworld, battles, and saves. Every scene holds a
locked 60 fps except the OBJ-window intro cinematic. It is **not** a finished
product; see [Limitations](#limitations).

| | |
|---|---|
| Video | ✅ Full PPU: modes 0–4, affine, sprites, windows, blending. Byte-exact vs. reference. |
| Frame rate | ✅ Locked 60 fps, tear-free, vsync-locked to the scanout beam |
| Input | ✅ 10 GPIO buttons |
| Saves | ✅ Persist to QSPI flash, survive reboot and reflash |
| Audio | ⚠️ Music plays; some instrument classes silent (see below) |
| Link cable / RFU | ❌ Not implemented |

## Hardware

See **[docs/HARDWARE.md](docs/HARDWARE.md)** for the full pinout, bill of
materials, and wiring notes. In brief: an RP2350B board, an HSTX-to-HDMI
breakout wired in the Pico DVI Sock pin order, ten buttons to ground, and
optionally a PCM5102A I²S DAC for sound.

## Build

See **[docs/BUILD.md](docs/BUILD.md)** for prerequisites and the full procedure.
The short version, from the repo root:

```sh
make tools && make modern           # first: decomp tools + generated assets
rp2350/gen_sound_assets.sh          # one-time: wav→bin, mid→song .s
rp2350/build_objs.sh                # game sources → rp2350/build/libpokeemerald.a
cmake -B rp2350/hw/build -S rp2350/hw -G Ninja   # PICO_SDK_PATH must be set
ninja -C rp2350/hw/build emerald
picotool load -f rp2350/hw/build/emerald.uf2
```

`rp2350/hw/CMakeLists.txt` also builds standalone bring-up targets that were
used to validate each subsystem on silicon before integration, and which are the
fastest way to debug a new board: `hstx_test` (colour bars), `psram_test`,
`i2s_test` (440 Hz sine), `display_test`, `ppu_display_test`, `emerald_hwtest`.

## How it works

The full engineering log — every phase, the measurements that made the go/no-go
call, and the bugs that cost the most time — is in
**[docs/PORTING.md](docs/PORTING.md)**. The short story:

**It piggybacks on a WASM port.** [tripplyons/pokeemerald-wasm](https://github.com/tripplyons/pokeemerald-wasm)
had already fenced every dependency on real GBA hardware behind `#if WASM`.
Reusing those seams as `#if WASM || RP2350` meant the de-hardwaring work was
already done; this port only had to add the MCU-specific half.

**The GBA memory map survives.** The game still writes DISPCNT, VRAM, OAM and
palette RAM at their original addresses — those are just plain SRAM now. Live
RAM is ~378 KB of the RP2350's 520 KB. ROM lives in QSPI flash via XIP, with the
game's `0x08000000` base remapped to `0x10000000`.

**The PPU is a software rasteriser on core 1.** Ported from the WASM host's
`web/app.js` and validated byte-exact against it. The first working version ran
at 909 ms/frame; per-frame palette LUTs, per-scanline window masks, 8-pixel tile
spans, and incremental affine stepping brought that to 60 fps — about 45×
faster. Specialised inner loops then cut typical scenes to ~12 ms.

**Scanout needs no CPU at all.** A DMA control-block ring feeds the HSTX
serialiser a full 525-line frame with the CPU never touching the re-arm path.
The interrupt is advisory. This is what makes the display immune to the
multi-millisecond stalls that flash writes cause during saves — and it took
killing an ~0.8-blink-per-minute HDMI dropout to get there.

## Limitations

Honest list of what is stubbed, missing, or wrong:

- **Audio is incomplete.** The m4a engine is ported and music plays, but DPCM
  (compressed) and reverse-playback instruments are silent stubs. Mixing is
  coupled to the frame rate, so any scene below 60 fps underruns. It has not had
  a careful by-ear correctness pass.
- **No link cable or wireless.** The RFU and multiboot paths are stubbed. Trading,
  battling, and the Mystery Gift download-code path do not work. (Mystery Gift's
  run-downloaded-code path relocates data as Thumb code, which cannot work on an
  M33 as written.)
- **The RTC is a dead cartridge clock.** Real Emerald bit-bangs an RTC over
  cartridge GPIO, which does not exist here. Time reads as zero and the in-game
  clock-set screen reports "clock is stopped." Berry growth and other
  time-of-day events are affected.
- **The OBJ-window intro cinematic runs at 20–25 ms/frame**, below 60 fps. It is
  the one scene with no fast path.
- **Audio hardware is optional but unpolished.** Without the I²S DAC wired, the
  game runs silently and correctly.

## Repository layout

This is a fork of pokeemerald-wasm, which is a fork of pret/pokeemerald. Almost
everything here is upstream; the port is confined to one directory.

| Path | What it is |
|---|---|
| `rp2350/` | **The port.** PPU, HSTX display driver, BIOS calls, m4a engine, build scripts. |
| `rp2350/hw/` | Pico SDK project: `game_main.c`, linker script, flash saves, I²S, test targets. |
| `docs/` | Hardware, build, and porting documentation. |
| `src/`, `data/`, `graphics/`, `sound/` | Upstream pokeemerald game sources and assets. |
| `web/`, `tools/wasm_*` | The WASM build, kept as the PPU reference (see below). |

**The WASM build is deliberately retained.** `rp2350/ppu_validate.sh` runs the
game in a browser, dumps GBA memory plus reference frames from the JavaScript
rasteriser, renders the same state with `rp2350/ppu.c`, and pixel-diffs the two.
That harness is the only reason the PPU can be called byte-exact, and it caught
real bugs during every optimisation pass. The Cloudflare deployment config from
upstream has been removed, but `make wasm` and `make serve-wasm` still work.

## Licensing

`rp2350/` is original work, released under the **MIT License**
([rp2350/LICENSE](rp2350/LICENSE)).

The rest of the repository is the pret decompilation and carries no license from
this project. Following pret convention, no ROM is required or included — the
decompilation builds the game from its own committed sources. Pokémon and
Pokémon character names are trademarks of Nintendo, Creatures Inc., and GAME
FREAK Inc. This project is not affiliated with or endorsed by any of them.

## Credits

- **[pret/pokeemerald](https://github.com/pret/pokeemerald)** — the decompilation
  this is built on. Years of work by many people; none of this is possible
  without it.
- **[tripplyons/pokeemerald-wasm](https://github.com/tripplyons/pokeemerald-wasm)**
  — the WebAssembly port, whose `#if WASM` seams and reference rasteriser this
  port depends on directly.
- The original pokeemerald README is preserved at
  [docs/original-pokeemerald-readme.md](docs/original-pokeemerald-readme.md).
