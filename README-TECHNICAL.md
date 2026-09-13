# Emerald3DS: how it works

The architecture of the port, the audio path, the debugging tools, and where
everything lives. See [README.md](README.md) for what the port is and how to get
it.

Two documents sit below this one and go deeper on their own areas:
[3ds/SECOND_SCREEN_CHEATSHEET.md](3ds/SECOND_SCREEN_CHEATSHEET.md) is the
working reference for the bottom screen and should be read before editing
`3ds/ui/`, and [3ds/ROADMAP.md](3ds/ROADMAP.md) is outstanding work with the
reasoning behind the decisions already taken.

## Contents

- [Two rules](#two-rules)
- [Why the build defines RP2350=1](#why-the-build-defines-rp23501)
- [Why it cannot be a .3dsx](#why-it-cannot-be-a-3dsx)
- [Audio](#audio)
- [Bring-up and debugging](#bring-up-and-debugging)
- [What a bottom-screen repaint costs](#what-a-bottom-screen-repaint-costs)
- [Repository layout](#repository-layout)

## Two rules

### Two worlds, one bridge

`include/gba/types.h` and `<3ds.h>` both typedef `u8`/`u16`/`u32`, and the
game's `include/` shadows libc's `string.h`, so no translation unit may include
both.

```
game-side : src/**, rp2350/{bios,asm_stubs,m4a_1}.c, 3ds/gba_mem.c,
            3ds/tweaks.c, 3ds/ui/**
host-side : 3ds/host/**, rp2350/ppu.c
```

Everything the two sides say to each other is in [3ds/bridge.h](3ds/bridge.h),
in stdint types only. That header includes neither side's headers and must stay
that way.

This is exactly what makes the bottom screen possible. It is game-side, so the
game's party data, item tables, wild encounter tables, fonts and icons are
ordinary symbols, with no RAM scraping and nothing to keep in sync.

### The GBA memory map becomes one array

The 3DS cannot hand out arbitrary virtual addresses, so EWRAM, IWRAM, VRAM,
palette, OAM and the registers are offsets into a single `gGbaMem` array, and
the game keeps writing them exactly as before.

It is an array rather than a pointer because an array's address is a link-time
constant, so `&REG_WIN0H` still works in the static initialisers that
`src/field_screen_effect.c` and `src/pokenav_menu_handler_gfx.c` build. A `u8 *`
makes those *"initializer element is not a compile-time constant"*.

The consequence is that the regions sit in `.bss` alongside the game's own
`EWRAM_DATA` variables, so their relative addresses are link-order dependent.
Exactly one place in the tree ever compared them: `IsTileMapOutsideWram()` in
`src/bg.c`, which asked `tilemap > IWRAM_END` to mean "is this in VRAM?". The
`PLATFORM_3DS` branch there asks that question directly instead.

The port also has no timers, and I/O is a zeroed buffer nothing writes. That
matters more than it sounds: `SeedRngAndSetTrainerId` takes the lower half of
the trainer ID from `REG_TM1CNT_L`, which read 0 forever here, so every save on
every console got the same trainer ID. It now reads `CtrTimeNowMs()` instead.
Anything else inheriting entropy from GBA hardware needs the same check, because
the register reads fine and answers zero.

## Why the build defines `RP2350=1`

In this tree the `RP2350` macro has come to mean *native CPU build, not GBA
hardware*: no link cable, no LCD to chase VCOUNT, no IWRAM mixer copy, saves
through the `Rp2350Save*` hooks, and a per-frame `Rp2350PresentFrame()`
callback. All of that is what the 3DS wants too, so the port inherits all 47
seam sites instead of duplicating them.

`PLATFORM_3DS=1` then overrides only the places where the 3DS genuinely differs:

| File | Why |
|---|---|
| `include/gba/defines.h` | Region bases, as above. `EWRAM_DATA` and `IWRAM_DATA` become plain `.bss`. |
| `include/gba/flash_internal.h` | `FLASH_BASE` points at a RAM array mirrored to the SD card, not at QSPI flash. |
| `src/siirtc.c` | The GBA carts carried an S-3511A; a 3DS has no cart, so the driver is backed by the console clock. |

`3ds/tweaks.c` follows the same principle for the gameplay tweaks. All the logic
lives in that one file so the hooks inside `src/` stay to one or two lines each,
fenced with `#if PLATFORM_3DS`. The less original source a port rewrites, the
easier it stays to rebase.

## Why it cannot be a `.3dsx`

Emerald's script bytecode packs a 4-byte pointer directly after a 1-byte opcode
(`.byte SCR_OP_GOTO` then `.4byte \destination`), so thousands of relocations
are unaligned. The 3DSX relocation table is indexed in whole 4-byte words, and
`3dsxtool` aborts outright with `Unaligned relocation!`.

Realigning the bytecode would mean changing 332 macros across 9 files plus every
interpreter that reads them. A CXI is loaded at a fixed address and never
relocated, so the same pointers are simply correct as linked.

`make -C 3ds 3dsx` still exists, purely so the failure stays reproducible.

## Audio

The path is m4a to NDSP, PCM16 the whole way. `rp2350/m4a_mix.c` is the mixer
seam and is compiled game-side: it drains the engine's `pcmBuffer`, synthesises
and adds the four PSG voices that a GBA would have summed in hardware, and hands
`3ds/host/audio.c` interleaved stereo `int16` through `Rp2350MixFrameStereo16()`,
already panned (m4a pans DirectSound per note, the PSG pans through NR51). NDSP
plays PCM16 natively, so the samples reach the DSP exactly as the mixer produced
them, with no conversion and no requantisation.

Rate: Emerald initialises m4a to `SOUND_MODE_FREQ_13379`, which is
`gPcmSamplesPerVBlankTable[3]` = 224 samples per frame. The game is paced by the
3DS panel rather than by the GBA's 59.7275 Hz, so playback runs at
`224 x 59.8261 = 13401 Hz`. Four wave buffers of 224 samples each put worst-case
latency at 67 ms.

### Which mixer ships

`rp2350/m4a_engine.c` is a C reimplementation of `src/m4a_1.s`, written because
the RP2350's Cortex-M33 is Thumb-2 only and physically cannot execute ARMv4T
ARM-mode code. **The ARM11 is ARMv6K and can**, so this port runs the original
assembly: the code Game Freak shipped, which cannot be wrong about the engine
the way a reimplementation can.

That was a build switch and a two-job CI matrix for as long as it was unproven.
It has since been heard on a console, so there is one build and no switch.
`3ds/build_objs.sh` always assembles `src/m4a_1.s` plus `3ds/asm/m4a_arm11.s`,
and never compiles `m4a_engine.c`. The two define the same 31 symbols, so
building both is a link error rather than a choice. The C engine stays in the
tree for the RP2350 port, which has no alternative.

### The five faults, and what fixed them

All five are fixed.

**1. Thread priority: silence with everything else correct.** libctru hardcodes
the NDSP service thread at priority `0x18` (`ndspInit`,
`libctru/source/ndsp/ndsp.c`). A main thread that outranks it starves it, the
DSP never drains a wave buffer, and every counter on our side still reads
healthy. `fix_thread_priority()` lowers the main thread to `0x30`, the priority
every `.3dsx` gets under hbmenu, which is the ordering NDSP is actually tested
in. It logs the resulting priority unconditionally: an absent line cannot tell
"the inversion is not happening" from "the code never ran".

**2. A ring buffer sized in the wrong unit.** The ring was a "samples" count
with the array declared to match, while the indexing was already
`[frame * 2 + channel]`. Every frame wrote 0x1C00 bytes past the end into the
neighbouring `.bss`. Azahar tolerated it and the sound was fine; a real ARM11
found a zeroed `sBlock[]` pointer and took a data abort writing to address 0 a
few seconds after boot. `RING_FRAMES` is now named for what it counts, and
everything downstream sizes from `AUDIO_CHANNELS` rather than a literal 2.

**3. Playback rate off by a refresh rate.** The DSP was asked for `224 x 60.0`
Hz while the game produces 224 samples per *panel* refresh, which is 59.8261 Hz.
That is 39 samples a second more than exist, so the ring drained at a fixed rate
and clicked roughly every six seconds even when nothing else was wrong. The
corrected 13401 Hz is also closer to the GBA's true 13379 than the old value,
dropping pitch error from 0.46% to 0.16%.

**4. Eight-bit output under a sixteen-bit mixer.** DirectSound is 8-bit at
source and survives the wider type exactly, but the PSG channels are generated
at 16-bit precision and a console sums the two in the analog domain rather than
on an 8-bit grid. The old 8-bit entry point quantised the PSG away and mixed one
bit from clipping. `Rp2350MixFrame16()` and `Rp2350MixFrameStereo16()` replaced
it; the 8-bit form remains only because the RP2350 port's I2S ring is built on
it.

**5. DPCM and reverse-playback instruments were silent stubs.** A fault in the C
reimplementation only, since `src/m4a_1.s` always decoded compressed samples, so
this port stopped being affected the moment it switched to the assembly. The fix
still matters to the RP2350, which has no such option: `MixChannelSpecial()` in
`rp2350/m4a_engine.c` decodes the BDPCM block format (64 samples per 33-byte
block: one verbatim sample, then 4-bit deltas indexing `gDeltaEncodingTable`)
and honours `TONEDATA_TYPE_REV`. The decoded block is cached on the wave and
block index rather than on the channel, so two channels reading different
compressed samples cannot see each other's buffer. Cries and every compressed
instrument were affected.

Related, though it was never a fault: fast-forward used to play the soundtrack
at the multiplier and drop the surplus, because the engine advances one tick per
`m4aSoundMain()` call. `CTR_FFAUDIO_NORMAL` (the default) ticks it once per
*displayed* frame instead, so the music keeps its tempo and quality while the
game runs fast. `CTR_FFAUDIO_FAST` keeps the old behaviour, because a rising
pitch is a useful cue that fast-forward is engaged.

### Diagnostics

Audio faults are about how something *sounds*, and no counter can measure that,
so the port carries two instruments.

`3ds/host/audio.c` writes a health report to the log at frame 600, about ten
seconds in. It reports the host-side counters, the engine's own state, the first
live channel's contents, per-subsystem output peaks (DirectSound, PSG and the
compressed path each separately, because "the PSG is silent under a healthy
DirectSound" and "both are silent" are different faults), a discontinuity count
that says whether clicks land on frame boundaries or inside the audio, and a
one-line verdict. When the mix is genuinely all zero the verdict walks the
engine chain and names the first link that was never made, rather than just
reporting silence.

The debug page of the bottom screen's EXTRA tab silences one half of the mixer
at a time (PSG, reverb, DirectSound) and toggles the stereo downmix. Neither
half can be judged by ear while the other is playing, so this is the only
instrument there is for a fault about quality rather than plumbing. The stereo
switch downmixes into both sides rather than reconfiguring the NDSP channel: a
switch that exists to diagnose a fault must not be able to introduce one.

### Why the DSP firmware dump is unavoidable

Not a quirk of this port; it applies to every 3DS homebrew that uses NDSP.
libctru loads the DSP component from `sdmc:/3ds/dspfirm.cdc`, and `ndspInit()`
fails outright if that file is missing, so the game runs silent. `CtrAudioInit()`
says so via `CtrLog()` (always compiled, unlike the `CTR_BOOT_DIAG` traces).
Before that it used `printf`, which goes nowhere on a console-less platform and
made a missing file look like a broken audio path.

Under an emulator using HLE audio (Azahar's default) the contents are never
used, so any file at that path satisfies libctru: `DSP_DSP::LoadComponent`
returns success before it even reads the buffer, and `DspHle::LoadComponent`
states outright that "HLE doesn't need DSP program" and only hashes it for the
log.

Why not sidestep it with CSND, which needs no firmware: Azahar stubs
`CSND_SND::ExecuteCommands`, so a CSND backend would be silent in exactly the
place most testing happens. Not worth a second audio backend.

## Bring-up and debugging

The boot crash worth remembering, because nothing about it is obvious:

`REG_KEYINPUT` is **active-low**, so a clear bit means *pressed*. `gGbaMem`
starts zeroed, so the register powered on reading "all ten buttons held", and
`CtrSetKeyInput()` could not correct it in time because it runs from
`Rp2350PresentFrame()` at the *end* of a frame while `ReadKeys()` samples at the
*start*. The first frame therefore saw A+B+START+SELECT, the soft-reset combo,
and called into `rfu_REQ_stopMode()`, whose `gSTWIStatus` is NULL here because
`InitRFU()` is GBA-only. The result was a null dereference at `+0xA`
(`STWIStatus::timerSelect`) before a single frame was drawn.
`Ctr3dsInitGbaMemory()` now seeds `REG_KEYINPUT = KEYS_MASK`.

The RP2350 port never hit this: its buttons come from GPIO pull-ups, which read
high, meaning released.

For the next such problem, build with diagnostics on:

```sh
CTR_BOOT_DIAG=1 3ds/build_objs.sh && make -C 3ds CTR_BOOT_DIAG=1
```

That enables `svcOutputDebugString` tracing through `main()` and `AgbMain()`
(an emulator logs it; needs `Debug.Emulated:Debug` in Azahar's log filter), a
blue/green splash before `AgbMain` proving the video path, and a cycling
pillarbox showing the frame loop is alive. A crash log gives a raw address; the
ELF and `.map` published by CI turn it back into a function name.

Two logging facilities are always compiled, because the build that actually
stalls is the one the measurement has to come from. `CtrLogSlow()` times a stage
and writes a line only when it overran, so the log names the call rather than
the symptom. The stage profiler measures the frame loop itself; see below.

`CTR_DEBUG_MENU` in `3ds/bridge.h` gates all of it at the file level. A release
build keeps the timing but never creates `sdmc:/3ds/emerald3ds/log.txt`, because
a build handed to someone else should not write to their SD card, and everything
in that file is written for whoever is developing the port.
`svcOutputDebugString` survives either way, so an emulator still shows the same
lines.

No log line and no settings write touches the card from inside a frame any more.
Both are queued for an I/O thread (`3ds/host/io_thread.c`) that sits one priority
below the main thread, so it only runs while the main thread waits. That matters
more than it sounds: the profiler closes a rarely-run stage's window on its first
sample after a quiet spell, which is exactly the repaint a battle overlay just
caused, so its card write used to land on the one frame already over budget.
Boot, before the thread starts, is still written synchronously, and a crash can
lose the last frame or so of lines.

## What a bottom-screen repaint costs

Worth knowing before touching `3ds/ui/`, because every plausible answer to this
was wrong at least once, and two of the wrong ones shipped.

The frame budget, measured:

| Stage | Mean | |
|---|---|---|
| `ppu` | ~9000 µs | every frame, the GBA rasteriser |
| `framebegin` | ~5700 µs | every frame, and this is the frame's slack |
| `paint` | ~4900 µs | per bottom-screen repaint |
| `upload.bot.copy` | 500 µs | per repaint |

A frame has 5.7 ms spare and a full repaint costs 5.6 ms of it, 87% of that
being the paint itself rather than the upload. It fits or misses depending on
where the rasteriser's 7 to 10 ms lands that frame, which is why the symptom was
a wobbly 55fps rather than a clean halving.

That table was measured with everything on core 0. **The rasteriser now runs on
a second core** (core 2 on a New 3DS, core 1 otherwise; `3ds/host/video.c`), and
the frame is ordered so the two overlap:

```
game frame + VBlankIntr                   core 0
snapshot video state, kick render         core 0   ~99 KB copy
  bottom update + paint, audio            core 0   | rasteriser on core 2 or 1
  bottom upload, if it repainted          core 0   |
collect render, top upload, VBlank wait   core 0
```

A repaint shorter than the rasteriser now costs the frame nothing, which is what
fixed the battle stutter: the quick-throw strip repaints the screen at least
twice a turn and the shiny notice on open, dismiss and expiry. On this path the
bottom screen is uploaded whole, before the render is collected, so its upload
is overlapped as well and a repaint reaches the panel on the frame it was
painted; that is what let the bottom screen's animations go back to the pace
they were first written at. The rasteriser
reads a private copy of the video state rather than `gGbaMem`, so nothing the
paint or a touch handler does can tear it. The profiler's `ppu.wait` says how
long core 0 sat waiting for it, and `frame` plus the "missed VBlank" line say
whether a frame was dropped at all. `make -C 3ds CTR_PPU_THREAD=0` builds the old
single-core path for comparison, and the port falls back to it by itself if no
second core is available. Everything below still holds for that path, and the
upload cost holds for both.

Measured on a New 3DS XL, the rasteriser took about 4.5 ms a frame and 8.6 ms at
worst, core 0 had about 10 ms to spare, and the longest frame in five minutes of
play, repaints included, was 16.76 ms, which is one VBlank. That still held once
the bottom screen's animations were restored (`59a0ba6`): about two and a half
minutes of battles with the shiny panel up, at up to 33 bottom-screen uploads a
second, had a longest frame of 16.77 ms and about 10 ms to spare. Azahar's
numbers are off in either direction depending on its CPU clock setting;
[the cheatsheet](3ds/SECOND_SCREEN_CHEATSHEET.md#measured-after-the-move) puts
the three side by side.

Two things that cost a lot of time to learn:

- **`C3D_FrameEnd` is not the VBlank wait.** `C3D_FRAME_SYNCDRAW` waits at
  `C3D_FrameBegin`. That call went uninstrumented for the whole investigation
  while a comment above the wrong one asserted otherwise.
- **The upload was never the cost.** The entire bottom upload path is about 1 ms
  per repaint. Slicing it across five frames fixed nothing, because at five
  repaints a second it was 5 ms in every 1000.

So the screen does not repaint to animate. `UiSnapshot` and `UiRestoreRect` keep
a copy of the last full paint, and an animation step puts back only the rects it
is about to redraw: six 32x32 icons, or four 16x14 sparkles. That is ~270 µs
against 4900, and it is why there is one shared animation clock rather than one
per animation. Two animations on private periods ask for repaints on different
frames and cost close to double.

The full version of this, including the rule that makes the snapshot correct,
is in [3ds/SECOND_SCREEN_CHEATSHEET.md](3ds/SECOND_SCREEN_CHEATSHEET.md).

## Repository layout

Almost everything here is upstream decompilation. The port is confined to one
directory.

| Path | What it is |
|---|---|
| `3ds/bridge.h` | **The seam.** The only thing both worlds include. |
| `3ds/gba_mem.c` | The `gGbaMem` block and the save-flash backing (game side). |
| `3ds/tweaks.c` | EXP All, level cap, randomiser, bag sort, phone calls (game side). |
| `3ds/build_objs.sh` | Game sources to `libpokeemerald.a` (ARM11). |
| `3ds/Makefile`, `3ds/emerald3ds.rsf` | Host sources, link, and makerom packaging. |
| `3ds/meta/` | Icon, banner art and banner audio for the CIA. |
| `3ds/host/` | libctru side: `main.c` (entry point, per-frame hook, input, every port setting), `video.c` (including the rasteriser's worker thread), `audio.c`, `save.c`, `settings.c`, `log.c`, `io_thread.c` (the background SD writer). |
| `3ds/ui/` | **The second screen**, game side: `bottom_screen.c` is the shell, one `tab_*.c` per tab, `view_encounters.c` and `matchup.c` are overlays, `ui_draw.c` and `ui_text.c` are the primitives. |
| `rp2350/` | The RP2350 port this is built on. `ppu.c` and the `m4a_*.c` pair are shared. |
| `src/`, `data/`, `graphics/`, `sound/` | Upstream pokeemerald sources and assets. |
| `web/`, `tools/wasm_*` | The WASM build, retained as the rasteriser reference. |
| `docs/` | RP2350 hardware, build and porting documentation. |

Working documents live beside the code they describe, all under `3ds/`:

- [SECOND_SCREEN_CHEATSHEET.md](3ds/SECOND_SCREEN_CHEATSHEET.md), how the bottom
  screen works. Read it before editing `3ds/ui/`.
- [ROADMAP.md](3ds/ROADMAP.md), what is planned next and why.
- [SECOND_SCREEN_PLAN.md](3ds/SECOND_SCREEN_PLAN.md),
  [UI_SKIN_PLAN.md](3ds/UI_SKIN_PLAN.md) and
  [ICON_AND_BANNER_PLAN.md](3ds/ICON_AND_BANNER_PLAN.md), design records.
- [SECOND_SCREEN_ANIMATION_PLAN.md](3ds/SECOND_SCREEN_ANIMATION_PLAN.md), giving
  the bottom screen back the animation it gave up for frame rate, now that the
  rasteriser has its own core. Proposed, not started.
- [ACHIEVEMENTS_PLAN.md](3ds/ACHIEVEMENTS_PLAN.md), built-in achievements on a
  sixth bottom-screen tab with an unlock toast over any tab, plus why real
  RetroAchievements is not the first step. Proposed, not started.

The RP2350 and WASM targets still build (`make wasm`, and see
[docs/BUILD.md](docs/BUILD.md)). The WASM build is deliberately kept:
`rp2350/ppu_validate.sh` pixel-diffs `rp2350/ppu.c` against the JavaScript
rasteriser, and that harness is the only reason the rasteriser can be called
byte-exact.
