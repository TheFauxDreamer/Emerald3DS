# Porting Pokémon Emerald to the RP2350

An engineering log. What the plan was, what the measurements said, what broke,
and what the fixes actually were.

---

## The strategy: piggyback on the WASM port

The GBA and the RP2350 have almost nothing in common at the hardware level, but
they share one thing that matters: an ARM C compiler. The pokeemerald
decompilation's `modern` build path is ordinary C targeting ARMv4T. Cortex-M33
is ARMv8-M. That is a compiler flag away — *if* you can sever every dependency
on real GBA hardware.

Someone had already done that severing. [pokeemerald-wasm](https://github.com/tripplyons/pokeemerald-wasm)
fenced every hardware dependency behind `#if WASM` in order to run the game in a
browser. Auditing those 16 sites and re-sharing the de-hardwaring ones as
`#if WASM || RP2350` — superloop instead of interrupt-driven main loop, software
DMA, stubbed audio, plain-memory flash, no link cable — meant the hardest
conceptual work was inherited rather than redone.

That decision shaped everything else. The browser host (`web/app.js`, 869 lines)
became the specification for what had to be rewritten in bare-metal C, and later
the reference implementation the C rasteriser was validated against.

---

## Phase 0 — Measure before building

The go/no-go was entirely a question of whether the game fits. It does, with
room to spare:

| | Measured | Available | |
|---|---|---|---|
| ROM image | 13.3 MB | 16 MB QSPI flash | ✅ 2.7 MB headroom |
| Live RAM | ~378 KB | 520 KB SRAM | ✅ ~142 KB headroom |

The ROM breaks down as 9.58 MB `.rodata` (graphics, maps, tables), 1.97 MB
`.text`, 1.06 MB script data, 0.68 MB song data — executed in place from flash,
never copied.

The RAM figure is the sum of the GBA's memory regions, which the port keeps
intact: EWRAM 250 KB + IWRAM 30 KB + VRAM 96 KB + palette/OAM 2 KB. Notably the
GBA build is at **95% of its 256 KB EWRAM cap** — the original game is nearly
out of memory. The RP2350 is not bound by that limit, so what is a hard ceiling
on real hardware is merely a number here.

That ~142 KB of headroom is what paid for the framebuffer, the PPU's lookup
tables, the SDK, and two stacks. The original plan was to render scanline-by-
scanline into ~1 KB line buffers precisely to avoid spending 77 KB on a full
framebuffer — but the measurement showed a full framebuffer fits, and it made
the display driver dramatically simpler.

---

## Phase 1 — It compiles, links, and boots

**All 311 game C files compiled clean for Cortex-M33 on the first serious
attempt.** This retired the single biggest risk in the project. The MODERN path
is ARMv8-M-clean out of the box; only assembly needed replacing.

The memory map is a straight relocation. Under `#if RP2350`, the GBA's regions
are redefined to point into SRAM — EWRAM at `0x20000000`, VRAM at `0x20048000`,
registers at `0x20060800`, and so on. The game keeps writing `DISPCNT` and VRAM
at "their" addresses, entirely unaware that those are now ordinary memory that
no hardware is watching. The ROM base `0x08000000` remaps to XIP flash at
`0x10000000`.

What had to be written from scratch: the BIOS syscalls (`rp2350/bios.c` —
`CpuSet`, `LZ77UnComp`, `BgAffineSet`, `Div`, `Sqrt`, and friends, ported from
`app.js`), and stubs for the assembly files that aren't built (`rp2350/asm_stubs.c`).
Verification was mechanical: aggregate the undefined symbols across all 319
objects and confirm nothing remained but libc, libgcc, and a few deliberately
zero-stubbed data tables. **No code gaps.**

Then it booted. Three hardware-only bugs surfaced immediately:

**Buttons that were never wired read as "all pressed."** GBA keys are
active-low, so a zeroed register region means every button held — which is
Emerald's A+B+Start+Select soft-reset combo, which jumps into RFU code, which
hangs. One line: initialise `REG_KEYINPUT` to `0x03FF`.

**Mystery Gift relocates downloaded data and executes it as Thumb code.** That
cannot work on an M33 as written. Stubbed.

**Save-load read garbage** because the flash base pointed nowhere useful.
Pointed at a zeroed read-only region so a fresh game starts cleanly. (Properly
fixed in Phase 3b.)

---

## Phase 2 — The PPU, and 45× of optimisation

The GBA's picture processing unit — background modes 0–4, affine
transformation, sprites, windowing, alpha blending — is a chunk of silicon that
now has to be a C function running inside a 16.7 ms budget.

Correctness came first, and cheaply, because the reference implementation
already existed. `rp2350/ppu.c` is a port of the `app.js` rasteriser, validated
**byte-exact** against it: dump GBA memory and the reference frame at 18 points
along an intro→Mudkip route, render the same state with the C code, pixel-diff.

Then came the problem. The first correct version rendered real scenes at **909
milliseconds per frame** — roughly one frame per second, against a budget of
16.7 ms. Getting to 60 fps took a 45× speedup, from four sources:

1. **Per-frame lookup tables.** Palette conversion and the screen-fade
   multiply, computed once per frame instead of per pixel — including folding
   the fade directly into the palette LUT.
2. **Per-scanline window masks** instead of per-pixel window tests.
3. **8-pixel tile spans.** Amortise the per-pixel dispatch across a whole tile,
   with incremental stepping for affine backgrounds.
4. **Core 1**, plus the 252 MHz overclock.

That got the game running. Locking it at 60 took a second pass, driven by
per-pass profiling that found something specific: **the overworld leaves the
window and blending machinery switched on while configured as a functional
no-op** — `WIN0` covering the full screen, a blend effect with no source layers.
The hardware doesn't care. The software rasteriser was paying full price per
pixel for nothing.

So: detect uniform window rows, skip passes whose layer the mask blocks
entirely, and drop into specialised inner loops with no masking, no per-pixel
flip branch, and direct stores. Bedroom render 21.8 → 11.5 ms. Later passes
added a 565 alpha-blend fast path and a translation-only affine path (the way
games actually use affine backgrounds — for plain images — with `pa==256 &&
pc==0`, which can then render in tile spans like a normal background). Title
screen affine pass: 5.87 → 1.38 ms.

Every one of those changes was gated on a byte-exact A/B run over 37 snapshots.

> **Two lessons worth stealing.** First, **host benchmarks lied in both
> directions** — clang on arm64 showed flat results on real wins and phantom
> 27–43% regressions from pure codegen perturbation. Only on-device numbers
> decided anything. Second, one `noinline` is load-bearing: inlining the blend
> helper into its seven call sites wrecked register allocation in the
> *blend-free* hot loops and cost 27% on scenes that never blend.

---

## Phase 2b — Making the display stop blinking

The first display driver worked but dropped the HDMI signal roughly once a
minute. The root cause was a chain: a late scanout interrupt → the DMA chain
re-runs a stale configuration → the HSTX expander interprets pixel data as
commands → signal lost.

Mitigations helped (NOP-guard tails on every command list, a watchdog that
resets the DMA and HSTX blocks). But the real fix was architectural: **take the
CPU out of the scanout path entirely.**

The driver now runs a control-block DMA ring. One channel streams data to the
HSTX FIFO; a second writes the next block's transfer count; a third writes its
read address and retriggers the first. Two 256-entry rings tile exactly one
525-line frame — 16 vertical-blanking blocks plus 240 active lines, each block
self-contained and 1024-byte aligned so that hardware read-ring wrapping streams
it *twice*, which is where the free 2× vertical upscale comes from.

The interrupt became advisory: it fills the next row and counts vsyncs, deriving
its position from the DMA read address each time so it self-resynchronises. A
late interrupt now costs one duplicated scanline instead of the whole signal.

Result: **zero blinks** over a 5.5-minute soak, against ~0.8/minute before. And
because scanout no longer needs the CPU, it survives the multi-millisecond
stalls that flash writes cause — which is precisely what made persistent saves
possible in the next phase.

One subtlety, learned by violating it: each line block is exactly 256 words with
*no* padding. The HSTX expander pops at most one FIFO word per cycle, so a run
of NOPs starves the serialiser.

---

## Phase 3 — Input and saves

**Input** was the easy phase. Ten GPIOs, active-low with internal pull-ups,
mapped so that GPIO *n* is key bit *n* — no translation, just a masked read of
`gpio_get_all()` once per frame.

**Saves** were not. The GBA cartridge's 128 KB save flash is mapped to the last
128 KB of the 16 MB QSPI chip, above the firmware image so that reflashing the
game preserves saves. Reads go through XIP; writes go through
`flash_safe_execute` with core 0's interrupts off and core 1 held in lockout.
The display keeps running throughout — the DMA ring, its control blocks, and its
interrupt handler all live in SRAM, so nothing touches flash.

The game's save code calls a byte-programming routine ~4080 times per sector,
which would be intolerable against real flash; those coalesce into a 256-byte
page buffer flushed on page crossings. About 55 ms per sector, 14 sectors across
a save.

Three bugs stood between "implemented" and "works," each presenting as a freeze
or bootloop:

**The RTC bit-bangs cartridge GPIO at `0x080000C4`.** On the GBA that is
cartridge space. On the RP2350 it is unmapped — a bus fault that kills core 0
and freezes the display on its last frame. It reproduced 100% of the time
immediately after naming your character, because that is when `NewGameInitData`
first touches the clock. The WASM build survives it because `0x08xxxxxx` happens
to be valid memory in a browser. Fixed by redirecting the GPIO ports to statics:
the RTC is now a dead cartridge clock, exactly like a Pokémon cartridge with a
flat battery.

**`common_data` is `NOLOAD`**, so the flash driver's function-pointer
initialisers were being discarded. Fixed by calling `IdentifyFlash()` explicitly
at boot.

**`save.c` indexes an array with a raw sector ID read from flash.** On a fresh
chip that value is `0xFFFF`. On the GBA, an out-of-bounds read there is harmless
open-bus. On an M33 it is a bus fault. Bounded.

All three were found with a hard-fault recorder that stashes PC, LR, and the
fault-status registers into watchdog scratch and reports them after the reboot —
which is the single highest-leverage piece of debug infrastructure in the
project. Silent freezes became labelled crashes.

---

## Phase 3c — Tear-free, and locked to the beam

With rendering fast enough, two artefacts remained: tearing (one framebuffer,
unsynchronised) and judder (the game's native 59.73 Hz against a 60.00 Hz
display).

Both dissolved with one change. Core 1 now starts each render at a
*beam-derived* alignment point, computed from the DMA ring's current read
pointer, with the lead predicted from the previous render's duration — so a fast
render stays ahead of the beam and a slow one stays behind it. No seam up to
~26 ms of render time. If a frame is predicted too slow to align, it free-runs,
because free-running beats being quantised to 30 fps.

The side effect is the good part: **the game is now locked to the 60.00 Hz
scanout.** Measured frame period 16667 µs with a 24 µs standard deviation, game
frames 1:1 with vsyncs. Timer-based pacing is reduced to a floor — which *must*
stay under the beam period, or the two clocks beat against each other and
produce a ~28 ms hiccup every second.

---

## Phase 4 — Audio

The GBA's m4a sound engine lives in hand-written ARM assembly (`m4a_1.s`) that
cannot be assembled for Cortex-M33. It is reimplemented in C in `rp2350/m4a_1.c`
— mixer, sequence interpreter, and note handling — feeding a PCM5102A over
PIO-driven I²S at the game's native 13440 Hz, so no resampling is needed.

Music plays. It is not finished: DPCM-compressed and reverse-playback
instruments are silent stubs, and mixing is coupled to the frame rate, so scenes
below 60 fps underrun. It has not had a careful by-ear pass.

One non-obvious bug cost a while: the port was silent not because the mixer was
wrong but because **`m4aSoundInit` was never called** on the RP2350 boot path.

---

## Reference: things that will bite you

Collected gotchas, most of which cost hours.

- **`arm-none-eabi-ar`, never the system `ar`.** macOS BSD `ar` silently
  truncates the GNU archive to 96 bytes.
- **`bash`, not `zsh`,** for the build scripts — zsh mishandles `$CFLAGS`
  word-splitting.
- **Close the serial console before flashing.** A process holding the USB CDC
  port blocks `picotool`'s reboot, and it fails silently.
- **`printf` costs ~0.28 ms per character**, blocking. A 5-second status line
  was an 80–110 ms frame spike. All frame-path printing now goes through a
  queue in EWRAM slack, drained 32 bytes per frame. stdio-over-UART is disabled
  outright — UART0's default pins are GP12/13, which belong to HSTX.
- **`clk_hstx`'s divider is only 2 bits** (max ÷3). The overclock cannot be
  raised freely without breaking the 25.2 MHz pixel clock. Options if more speed
  is ever needed: 378 MHz still divides to a valid HSTX clock, or clock USB from
  `pll_sys` to free `pll_usb` for HSTX.
- **The PPU validation harness cannot catch BIOS bugs.** Snapshots embed results
  the JavaScript BIOS already computed, so `rp2350/bios.c` is never exercised.
  Diff it against `web/app.js` by hand when device-only weirdness appears. This
  is how an `ObjAffineSet` stride bug survived — byte boundaries treated as
  halfwords, producing garbage matrices and stack corruption for every animated
  affine sprite.
- **Some intro frames legitimately retain the previous frame's pixels** via
  `winout` with no backdrop bit. A single-snapshot diff renders those black and
  reports a false failure. Use old-vs-new A/B there.
- **Host benchmarks are not evidence.** Measure on the device.
- **EWRAM and the SDK RAM region are both full.** New buffers go in
  `.ewram_top` or `.iwram_top`; see `memmap_emerald.ld`.

---

## What's left

- Finish audio: DPCM and reverse instruments, decouple mixing from the frame
  rate, and a real by-ear correctness pass.
- The OBJ-window intro cinematic is the last scene without a fast path
  (20–25 ms/frame).
- Link cable and wireless are unimplemented, and would be a substantial project.
