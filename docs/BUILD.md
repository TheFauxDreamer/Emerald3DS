# Building for RP2350

No ROM is required. The decompilation builds the game from its own committed
sources.

> Building the **GBA** ROM instead? That is upstream pokeemerald's own build
> (`make` / `make modern`) and is documented in [INSTALL.md](../INSTALL.md).
> This page covers only the RP2350 firmware.

## Prerequisites

| Tool | Notes |
|---|---|
| `arm-none-eabi-gcc` | 14.3 known good. Must include `as` and `ar`. |
| [Pico SDK](https://github.com/raspberrypi/pico-sdk) | 2.x. Export `PICO_SDK_PATH`. |
| CMake ≥ 3.13 + Ninja | |
| `picotool` | For flashing over USB. |
| A host C compiler, `make`, Python 3 | Builds the decomp's own asset tools. |

Optional, for PPU validation only: Node.js and Chrome, plus `clang` and
`wasm-ld` for the WASM reference build.

> **Use `arm-none-eabi-ar`, never the system `ar`.** On macOS, BSD `ar` silently
> truncates the GNU archive to 96 bytes. If a link fails with everything
> undefined, this is why; rebuild with
> `arm-none-eabi-ar rcs rp2350/build/libpokeemerald.a rp2350/build/obj/*.o`.

## Build

All commands run from the repository root.

### 1. Tools and assets

```sh
make tools
make modern
```

`make modern` is not strictly required, but it is the recommended first step: it
generates everything the RP2350 build consumes (`build/assets/`, the mapjson
`.inc` files under `data/maps/`) and proves the tree is healthy before you add
MCU variables to the mix. `make generated` is the lighter alternative.

> If `make modern -j8` fails on an asset, just re-run it — there is a known
> race in the parallel asset rules. It is not a real failure.

### 2. Sound assets (one time)

```sh
rp2350/gen_sound_assets.sh
```

Converts 544 samples with `wav2agb` and 529 songs with `mid2agb`, mirroring
`audio_rules.mk`. Only needs re-running if you change sound sources.

### 3. Game archive

```sh
rp2350/build_objs.sh
```

Compiles all game sources for Cortex-M33 (`-mcpu=cortex-m33 -mthumb
-mfloat-abi=softfp`, `-DMODERN=1 -DRP2350=1`) through the same preproc pipeline
the GBA build uses, and archives them into `rp2350/build/libpokeemerald.a`.

To iterate on a single file, recompile just that translation unit and update the
archive in place with `ar r` rather than re-running the whole script. **Use
`bash`, not `zsh`** — zsh mishandles the word-splitting in `$CFLAGS`.

### 4. Firmware

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -B rp2350/hw/build -S rp2350/hw -G Ninja
ninja -C rp2350/hw/build emerald
```

The `emerald` target links the archive whole (`--whole-archive`, so nothing is
reachability-trimmed) against the Pico SDK runtime using the custom linker
script `rp2350/hw/memmap_emerald.ld`.

### 5. Flash

```sh
picotool load -f rp2350/hw/build/emerald.uf2
```

> **Verify that `picotool` prints its "asked to reboot" line.** It fails
> silently otherwise. If it does, put the board in BOOTSEL manually — hold BOOT,
> tap RST — and retry. Also close any open serial console first; a process
> holding the USB CDC port blocks the reboot.

Then open the console: `cat /dev/cu.usbmodem*` (the port number changes across
re-enumerations).

## Bring-up targets

If you are on new or differently-wired hardware, validate one subsystem at a
time before running the game. Each is a standalone `ninja` target in
`rp2350/hw/build`:

| Target | What it proves |
|---|---|
| `emerald_hwtest` | Board boots, stdio works, the BIOS reimplementations are correct on silicon |
| `hstx_test` | HSTX → HDMI wiring, via 640×480 colour bars |
| `display_test` | The framebuffer scanout driver |
| `ppu_display_test` | PPU + display together, rendering a captured GBA snapshot |
| `i2s_test` | I²S DAC wiring, via a 440 Hz sine |
| `psram_test` | APS6404 PSRAM on QSPI CS1 (not used by the game) |

`ppu_display_test` needs `rp2350/hw/snapshot_data.h`, which is generated (and
git-ignored) — regenerate it from a PPU dump directory.

## Validating the PPU

```sh
make wasm            # build the reference first (see note below)
rp2350/ppu_validate.sh
```

This runs the game in headless Chrome against the WASM build, dumps GBA memory
plus the reference frame from the JavaScript rasteriser at each of 18 points
along an intro → Mudkip-battle route, renders the identical state with
`rp2350/ppu.c` on the host, and pixel-diffs the results. It should report
`FAIL=0`.

Two things to know:

- **`make wasm` needs `make wasm-assets -j1` run first**, single-threaded — the
  Python generators race under `-j8`.
- **It cannot catch bugs in `rp2350/bios.c`.** The snapshots embed results
  already computed by the JavaScript BIOS, so a wrong C implementation is never
  exercised. When you see rendering weirdness that only appears on-device,
  diff `rp2350/bios.c` against `web/app.js` by hand.

When optimising the PPU, the stronger check is an A/B harness — old `ppu.c` vs.
new, over the same snapshots — because some intro frames legitimately retain
prior-frame pixels via `winout`, which a single-snapshot diff renders as black
and reports as a false failure.

## Memory notes

Both EWRAM and the SDK's RAM region are effectively full. The port parks
allocations in deliberate slack:

- PPU palette LUTs and core 1's stack → EWRAM slack (`.ewram_top`)
- I²S DMA buffers and the audio ring → IWRAM slack (`.iwram_top`)

If you add so much as a kilobyte of `.bss` to the SDK region, the link will
overflow. Put new buffers in one of the slack sections, in
`rp2350/hw/memmap_emerald.ld`.
