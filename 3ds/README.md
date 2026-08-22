# Pokémon Emerald — 3DS dual-screen port

A native 3DS build of the pokeemerald decompilation: the game on the top screen,
a touch dashboard (party/HP, bag, Pokédex) on the bottom. No emulator, and no
ROM file — the decomp builds its own assets.

## Where the work came from

- [pret/pokeemerald](https://github.com/pret/pokeemerald) — the decompilation.
- [tripplyons/pokeemerald-wasm](https://github.com/tripplyons/pokeemerald-wasm) —
  fenced every dependency on real GBA hardware behind `#if WASM`, and its
  software PPU is the byte-exact reference.
- [mattdeeds/pokeemerald-rp2350](https://github.com/mattdeeds/pokeemerald-rp2350) —
  this repo's base. Recompiled the game for a different physical CPU behind
  `#if WASM || RP2350`, with `rp2350/ppu.c` (modes 0–4, affine, sprites,
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

Plus one behavioural fix in `src/bg.c` — see below.

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
party data, item tables, fonts and icons are ordinary symbols — no RAM scraping.

**2. `gGbaMem` is an array, not a pointer.** An array's address is a link-time
constant, so `&REG_WIN0H` still works in the static initialisers that
`src/field_screen_effect.c` and `src/pokenav_menu_handler_gfx.c` build. A `u8 *`
makes those *"initializer element is not a compile-time constant"*.

The consequence is that the regions sit in `.bss` alongside the game's own
`EWRAM_DATA` variables, so their relative addresses are link-order dependent.
Exactly one place in the tree ever compared them — `IsTileMapOutsideWram()` in
`src/bg.c`, which asked `tilemap > IWRAM_END` to mean "is this in VRAM?". The
`PLATFORM_3DS` branch there asks that question directly instead.

## Build

CI does this on every push (`.github/workflows/build-3ds.yml`) using the
official `devkitpro/devkitarm` container, so a local toolchain is optional.

Locally you need devkitPro (devkitARM, libctru, citro2d/citro3d) and libpng for
the decomp's `gbagfx`:

```sh
make tools && make generated             # decomp tools + generated headers
python3 tools/generate_wasm_assets.py    # -> build/assets
rp2350/gen_sound_assets.sh               # one-time: wav->bin, mid->song .s
3ds/build_objs.sh                        # game sources -> 3ds/build/libpokeemerald.a
make -C 3ds                              # -> 3ds/emerald3ds.3dsx
```

## Layout

```
3ds/
  bridge.h          the only thing both worlds include
  gba_mem.c         GBA regions + save-flash backing        (game side)
  build_objs.sh     game sources -> libpokeemerald.a (ARM11)
  Makefile          host sources + link -> emerald3ds.3dsx
  host/
    main.c          entry point, per-frame hook, input
    video.c         PPU -> PICA200 texture -> both screens
    audio.c         m4a -> NDSP
    save.c          Rp2350Save* -> SD card
  ui/
    bottom_screen.c bottom-screen UI                        (game side)
```

## Status

Phase 1 (boot on hardware) is written but not yet run on a device — nothing here
has been executed on a 3DS. The bottom screen is a stub that draws a live
party/HP readout to prove the data path; the real tabbed PARTY / BAG / DEX UI in
Emerald's own style comes next.
