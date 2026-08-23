# Pokémon Emerald: 3DS dual-screen port

A native 3DS build of the pokeemerald decompilation: the game on the top screen,
a touch dashboard (party/HP, bag, Pokédex) on the bottom. No emulator, and no
ROM file, because the decomp builds its own assets.

## Where the work came from

- [pret/pokeemerald](https://github.com/pret/pokeemerald): the decompilation.
- [tripplyons/pokeemerald-wasm](https://github.com/tripplyons/pokeemerald-wasm)
  fenced every dependency on real GBA hardware behind `#if WASM`, and its
  software PPU is the byte-exact reference.
- [mattdeeds/pokeemerald-rp2350](https://github.com/mattdeeds/pokeemerald-rp2350)
  is this repo's base. Recompiled the game for a different physical CPU behind
  `#if WASM || RP2350`, with `rp2350/ppu.c` (modes 0-4, affine, sprites,
  windows, blending), the m4a mixer in C, and flash-save hooks.

## Why the 3DS build defines `RP2350=1`

In this tree the `RP2350` macro has come to mean *native CPU build, not GBA
hardware*: no link cable, no LCD to chase VCOUNT, no IWRAM mixer copy, saves
through the `Rp2350Save*` hooks, and a per-frame `Rp2350PresentFrame()` callback.
All of that is what the 3DS wants too, so the port inherits all 47 seam sites
instead of duplicating them. `PLATFORM_3DS=1` then overrides only the two places
where the 3DS genuinely differs:

| File | Why |
|---|---|
| `include/gba/defines.h` | Region bases. The 3DS cannot hand out arbitrary virtual addresses, so EWRAM/IWRAM/VRAM/PLTT/OAM/REG are offsets into one `gGbaMem` array. `EWRAM_DATA`/`IWRAM_DATA` become plain `.bss`. |
| `include/gba/flash_internal.h` | `FLASH_BASE` points at a RAM array mirrored to the SD card, not at QSPI flash. |

Plus one behavioural fix in `src/bg.c`; see below.

## The two rules that keep this building

**1. Two worlds, one bridge.** `include/gba/types.h` and `<3ds.h>` both typedef
`u8`/`u16`/`u32`, and the game's `include/` shadows libc's `string.h`. A
translation unit that includes both will not compile.

```
game-side : src/**, rp2350/{bios,asm_stubs,m4a_1}.c, 3ds/gba_mem.c, 3ds/ui/**
host-side : 3ds/host/**, rp2350/ppu.c
```

Everything they say to each other is in `3ds/bridge.h`, in stdint types only.
This is also what makes the bottom screen easy: it is game-side, so Emerald's
party data, item tables, fonts and icons are ordinary symbols, with no RAM scraping.

**2. `gGbaMem` is an array, not a pointer.** An array's address is a link-time
constant, so `&REG_WIN0H` still works in the static initialisers that
`src/field_screen_effect.c` and `src/pokenav_menu_handler_gfx.c` build. A `u8 *`
makes those *"initializer element is not a compile-time constant"*.

The consequence is that the regions sit in `.bss` alongside the game's own
`EWRAM_DATA` variables, so their relative addresses are link-order dependent.
Exactly one place in the tree ever compared them: `IsTileMapOutsideWram()` in
`src/bg.c`, which asked `tilemap > IWRAM_END` to mean "is this in VRAM?". The
`PLATFORM_3DS` branch there asks that question directly instead.

## Build

CI does this on every push (`.github/workflows/build-3ds.yml`) using the
official `devkitpro/devkitarm` container, so a local toolchain is optional.
It publishes `emerald3ds.cia`, `emerald3ds.3ds` and the raw `emerald3ds.elf`.

Locally you need devkitPro (devkitARM, libctru, citro2d/citro3d) and libpng for
the decomp's `gbagfx`:

```sh
make tools && make generated             # decomp tools + generated headers
python3 tools/generate_wasm_assets.py    # -> build/assets
rp2350/gen_sound_assets.sh               # one-time: wav->bin, mid->song .s
3ds/build_objs.sh                        # game sources -> 3ds/build/libpokeemerald.a
make -C 3ds                              # -> 3ds/emerald3ds.{cia,3ds}
```

Packaging also needs [makerom](https://github.com/3DSGuy/Project_CTR/releases)
on `PATH` (or `make -C 3ds MAKEROM=/path/to/makerom`).

## Why this is not a .3dsx

A `.3dsx` is loaded at a variable address, so every absolute pointer needs a
load-time relocation, and the 3DSX relocation table is indexed in whole 4-byte
words. Emerald's script bytecode packs a 4-byte pointer directly after a 1-byte
opcode:

```
	.macro goto destination:req
	.byte SCR_OP_GOTO       @ 1 byte
	.4byte \destination     @ pointer at an odd offset
	.endm
```

Thousands of those relocations are therefore unaligned and cannot be encoded at
all -- `3dsxtool` aborts with `Unaligned relocation!`. Realigning the bytecode
would mean changing 332 macros across 9 files plus every interpreter that reads
them.

A CXI (`.cia`/`.3ds`) is loaded at a fixed address and never relocated, so the
same pointers are simply correct as linked. That is why the port ships this way.
`emerald3ds.3ds` boots directly in an emulator; `emerald3ds.cia` installs to a
console. `make -C 3ds 3dsx` still exists, purely so the failure stays
reproducible.

## Layout

```
3ds/
  bridge.h          the only thing both worlds include
  gba_mem.c         GBA regions + save-flash backing        (game side)
  build_objs.sh     game sources -> libpokeemerald.a (ARM11)
  emerald3ds.rsf    makerom ROM spec (CIA/CCI packaging)
  Makefile          host sources + link -> emerald3ds.{cia,3ds}
  host/
    main.c          entry point, per-frame hook, input
    video.c         PPU -> PICA200 texture -> both screens
    audio.c         m4a -> NDSP
    save.c          Rp2350Save* -> SD card
  ui/
    bottom_screen.c bottom-screen UI                        (game side)
```

## Status

**Boots and runs**, verified in Azahar, not yet on real hardware. The bottom
screen is still the Phase 1 stub: it draws one plain-rectangle slot per party
member (species / HP / level) straight out of `gPlayerParty`, so it is correctly
blank until you receive your first Pokémon. The real tabbed PARTY / BAG / DEX
interface in Emerald's own style comes next.

## Audio requires a DSP firmware dump

Not a quirk of this port; it applies to every 3DS homebrew that uses NDSP.
libctru loads the DSP component from `sdmc:/3ds/dspfirm.cdc`, and `ndspInit()`
fails outright if that file is missing, so the game runs silent. `CtrAudioInit()`
now says so via `CtrLog()` (always compiled, unlike the `CTR_BOOT_DIAG` traces);
before that it used `printf`, which goes nowhere on a console-less platform and
made a missing file look like a broken audio path.

**On hardware**, dump it from your own console with
[DSP1](https://github.com/zoogie/DSP1), a one-time step already included in the
standard 3DS CFW setup guides.

**Under an emulator using HLE audio** (Azahar/Citra's default) the contents are
never used, so any file at that path satisfies libctru:
`DSP_DSP::LoadComponent` returns success before it even reads the buffer, and
`DspHle::LoadComponent` states outright that "HLE doesn't need DSP program" and
only hashes it for the log. A placeholder is enough to test with.

Why not sidestep it with CSND, which needs no firmware: Azahar stubs
`CSND_SND::ExecuteCommands`, so a CSND backend would be silent in exactly the
place most testing happens. Not worth a second audio backend.

Rate: m4a is initialised to `SOUND_MODE_FREQ_13379`, which is
`gPcmSamplesPerVBlankTable[3]` = 224 samples per frame; the port plays them at
224 x 60 = 13440 Hz (0.5% sharp of the GBA's 59.73 Hz, which keeps the ring from
drifting).

### Bring-up notes

The boot crash worth remembering, because nothing about it is obvious:

`REG_KEYINPUT` is **active-low**: a clear bit means *pressed*. `gGbaMem` starts
zeroed, so the register powered on reading "all ten buttons held", and
`CtrSetKeyInput()` could not correct it in time because it runs from
`Rp2350PresentFrame()` at the *end* of a frame while `ReadKeys()` samples at the
*start*. The first frame therefore saw A+B+START+SELECT, the soft-reset combo,
and called into `rfu_REQ_stopMode()`, whose `gSTWIStatus` is NULL here because
`InitRFU()` is GBA-only. The result was a null dereference at `+0xA`
(`STWIStatus::timerSelect`) before a single frame was drawn.
`Ctr3dsInitGbaMemory()` now seeds `REG_KEYINPUT = KEYS_MASK`.

The RP2350 port never hit this: its buttons come from GPIO pull-ups, which read
high, i.e. released.

For the next such problem, build with diagnostics on:

```sh
CTR_BOOT_DIAG=1 3ds/build_objs.sh && make -C 3ds CTR_BOOT_DIAG=1
```

That enables `svcOutputDebugString` tracing through `main()` and `AgbMain()`
(an emulator logs it; needs `Debug.Emulated:Debug` in Azahar's log filter), a
blue/green splash before `AgbMain` proving the video path, and a cycling
pillarbox showing the frame loop is alive. A crash log gives a raw PC; the ELF
and `.map` published by CI turn it back into a function name.
