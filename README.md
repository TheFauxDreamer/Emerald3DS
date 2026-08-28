# Emerald3DS

**Pokémon Emerald as a native, dual-screen 3DS game.** The game plays on the top
screen. The bottom screen is a real touch interface for your team, bag, Pokédex
and the port's own settings, drawn with Emerald's own fonts, icons and window
borders so it looks like part of the game rather than an overlay.

No emulator and no ROM. The [pret decompilation](https://github.com/pret/pokeemerald)
is recompiled from ARMv4T to the 3DS's ARM11, and builds its own assets from
source.

> [!Note]
> **Written with AI.** Bro I ain't gonna lie
> I used AI out the wazoo cause I don't know the 
> first thing about GBA decamps or 3DS home-brew
> I just wanted to see what was possible

## The second screen

Every other way to play a GBA game on a 3DS, whether GBA Virtual Console or
[open_agb_firm](https://github.com/profi200/open_agb_firm), hands the cartridge
to the 3DS's built-in GBA hardware. That runs as a FIRM with the operating
system shut down and only one screen available. **Native recompilation is the
only route to a GBA game that can use the touch screen**, which is the reason
this project exists.

Because the port is a recompile rather than an emulator, the bottom screen is
not scraping RAM and guessing. It is compiled *with the game*, so `gPlayerParty`,
`gBagPockets`, `gPokedexEntries` and the game's own sprite and font data are
ordinary symbols it reads directly. Everything it draws comes from Emerald's own
data, which is why it cannot drift out of sync with what the game believes.

```
  ┌────────────────────────────────────┐   top: software PPU (rp2350/ppu.c)
  │           GBA output               │        240x160 -> RGB565 -> PICA200
  │      1x / 1.5x / fill screen       │        via citro2d, nearest-neighbour
  └────────────────────────────────────┘
  ┌──────────────────────────────┐         bottom: game-side C, reading the
  │ [icon] SPARKY  /\  Lv12      │         game's own structures directly
  │ [PSN ] ####------  24/38     │
  │  PARTY  BAG  MAP  DEX  EXTRA │
  └──────────────────────────────┘
```

## What the bottom screen does

Tabs appear only once the game has given you the thing they show, mirroring the
start menu: no Pokédex tab before you own a Pokédex.

**PARTY.** A 2×3 grid of your team: mon icon, nickname, level, and an HP bar
that slides the way the battle bar does instead of jumping. Status is the party
menu's own PSN/SLP/BRN badge. In battle each card gains **type matchup arrows**:
a green up arrow when that mon's moves hit hard, amber or red down when they do
not, plus a second arrow for how dangerous the opponent is to it. Tap a mon for
stats, ability, nature, held item and moves.

**BAG.** Pockets, quantities, and a details pane with the item's sprite and
description. USE opens a picker of your team, so a Potion asks who it is for
instead of guessing. Items work in the field, **and in battle**, where using one
goes through the engine's action queue so it costs your turn exactly as the
in-game bag does, and an item that would do nothing costs you nothing. The
original menus are untouched; this is an alternative route to the same action,
never a replacement.

**DEX.** Emerald's Pokédex: the selected mon's sprite and the seen/owned counts
on the left, the scrolling list on the right, and an entry screen with the
sprite, footprint, category, and height and weight in the game's own imperial
format. Unseen entries stay anonymous, as they should.

**MAP.** Not built yet.

**EXTRA.** Things the original game has no concept of. Fast-forward at 1x, 2x,
4x or 8x; top-screen size at 1x (pixel-perfect), 1.5x (fills the height) or
FILL (fills the panel, stretching 11%); and the four buttons the GBA has no use
for (X, Y, ZL and ZR) bindable to a hold-for-speed or to the touch UI's
modifier key. Screen size and bindings persist; fast-forward deliberately
resets each launch.

The whole thing follows your **Options → Frame** border choice, live, as you
cycle through it.

## Status

**Boots and plays, verified in the Azahar emulator. Never run on real
hardware.** Read every row as "works where it has been tested", which so far
means an emulator only.

| | |
|---|---|
| Boot, overworld, battles, saving | ✅ |
| Top screen | ✅ Software PPU, selectable scale |
| Real-time clock | ✅ Backed by the console clock, so berries and time-of-day work |
| Bottom: party, bag, Pokédex, extras | ✅ |
| Bottom: map | ❌ Placeholder |
| Battle items from the touch screen | ✅ |
| Audio | ⚠️ Needs a DSP firmware dump; some sample types silent |
| Link cable / wireless | 🚧 On the `local-wireless` branch, unbuilt and untested |
| Frame rate, battery, timing | ❓ Never measured on a 3DS |

## Getting it

Every push builds it. Grab **`emerald3ds`** from the latest
[Actions run](../../actions): it contains `emerald3ds.cia` (install to a
console) and `emerald3ds.3ds` (boots directly in an emulator, no install step).
The **`emerald3ds-elf`** artifact holds the ELF and linker map; keep the one
matching your build, because a crash log only ever gives a raw PC and those are
what turn it back into a function name.

To build locally you need devkitPro (devkitARM, libctru, citro2d/citro3d),
libpng for the decomp's `gbagfx`, and
[makerom](https://github.com/3DSGuy/Project_CTR/releases):

```sh
make tools && make generated             # decomp tools + generated headers
python3 tools/generate_wasm_assets.py    # -> build/assets
rp2350/gen_sound_assets.sh               # one-time: wav->bin, mid->song .s
3ds/build_objs.sh                        # game sources -> libpokeemerald.a (ARM11)
make -C 3ds                              # -> 3ds/emerald3ds.{cia,3ds}
```

**It is not a `.3dsx`, and cannot be.** Emerald's script bytecode packs a 4-byte
pointer directly after a 1-byte opcode, so thousands of relocations are
unaligned, and the 3DSX relocation table is indexed in whole 4-byte words, so
they cannot be encoded at all. A CXI is loaded at a fixed address and never
relocated, so the same pointers are simply correct.

## How it works

[3ds/README.md](3ds/README.md) is the real explanation. Two rules shape
everything:

**Two worlds, one bridge.** `include/gba/types.h` and `<3ds.h>` both typedef
`u8`/`u16`/`u32`, and the game's `include/` shadows libc's `string.h`, so no
translation unit may include both. Game-side code (`src/**`, `3ds/gba_mem.c`,
`3ds/ui/**`) sees only game headers; host-side code (`3ds/host/**`,
`rp2350/ppu.c`) sees only libctru. Everything they say to each other is in
[3ds/bridge.h](3ds/bridge.h), in stdint types. This is exactly what makes the
bottom screen possible: it is game-side, so the game's data is just there.

**The GBA memory map becomes one array.** The 3DS cannot hand out arbitrary
virtual addresses, so EWRAM/IWRAM/VRAM/palette/OAM/registers are offsets into a
single `gGbaMem` array, and the game keeps writing them exactly as before.

The port defines `RP2350=1`, which in this tree means *native CPU build, not GBA
hardware*, inheriting all 47 of that port's hardware seams rather than
duplicating them. `PLATFORM_3DS=1` then overrides only where the 3DS genuinely
differs: the memory map, the save backing, and the cartridge clock.

[3ds/ROADMAP.md](3ds/ROADMAP.md) has what is planned next, plus the bring-up
notes worth reading before debugging anything.

## Limitations

- **Audio needs a DSP firmware dump.** libctru loads the DSP component from
  `sdmc:/3ds/dspfirm.cdc` and `ndspInit()` fails outright without it, so the
  game runs silent and says so in the log. Dump it from your own console with
  [DSP1](https://github.com/zoogie/DSP1); it is a standard one-time CFW step
  every NDSP homebrew needs. Separately, DPCM and reverse-playback instruments
  are still silent stubs in `rp2350/m4a_1.c`, so some instruments and cries do
  not play at all.
- **Never run on hardware.** No fill-rate, battery or timing data exists, and
  ZL/ZR bindings are untestable on an Old 3DS by definition.
- **No trading or link battles yet.** The Cable Club over 3DS local wireless is
  written but unbuilt on the `local-wireless` branch. The Union Room and Mystery
  Gift use a separate wireless stack that is still stubbed.
- **No true 2× top screen, ever.** 240×160 × 2 = 480×320 exceeds a 400×240
  panel in both dimensions, so 1.5× is the largest fit and its rows alternate
  1px/2px. The 1× mode is the only pixel-perfect one.
- **No save states.** Unlike an emulator, this is a native build: the game's
  state lives scattered through the binary's own BSS rather than in one
  snapshottable region.

## Repository layout

Almost everything here is upstream decomp. The port is confined to one
directory.

| Path | What it is |
|---|---|
| `3ds/` | **The port.** Bridge, GBA memory block, build scripts, ROM spec. |
| `3ds/host/` | libctru side: entry point, video, audio, saves, settings, tracing. |
| `3ds/ui/` | **The second screen**, game-side: drawing, Emerald-font text, tabs. |
| `rp2350/` | The RP2350 port this is built on. `ppu.c` and `m4a_1.c` are shared. |
| `src/`, `data/`, `graphics/`, `sound/` | Upstream pokeemerald sources and assets. |
| `web/`, `tools/wasm_*` | The WASM build, retained as the PPU reference. |
| `docs/` | RP2350 hardware, build and porting documentation. |

The RP2350 and WASM targets still build (`make wasm`, and see
[docs/BUILD.md](docs/BUILD.md)). The WASM build is deliberately kept:
`rp2350/ppu_validate.sh` pixel-diffs `rp2350/ppu.c` against the JavaScript
rasteriser, and that harness is the only reason the PPU can be called
byte-exact.

## Licensing

`3ds/` and `rp2350/` are original work under the **MIT License**
([rp2350/LICENSE](rp2350/LICENSE)).

The rest is the pret decompilation and carries no license from this project.
Following pret convention, no ROM is required or included; the decompilation
builds the game from its own committed sources. Pokémon and Pokémon character
names are trademarks of Nintendo, Creatures Inc., and GAME FREAK Inc. This
project is not affiliated with or endorsed by any of them.

## Credits

- **[pret/pokeemerald](https://github.com/pret/pokeemerald)**: the
  decompilation everything is built on.
- **[tripplyons/pokeemerald-wasm](https://github.com/tripplyons/pokeemerald-wasm)**
  fenced every dependency on real GBA hardware behind `#if WASM`. That work,
  reused as `#if WASM || RP2350`, is why this port did not have to rediscover
  where a 1M-line decomp touches hardware.
- **[mattdeeds/pokeemerald-rp2350](https://github.com/mattdeeds/pokeemerald-rp2350)**
  is this repo's direct base: the software PPU, the m4a mixer in C, and the
  flash-save hooks the 3DS port inherits.
- The original pokeemerald README is preserved at
  [docs/original-pokeemerald-readme.md](docs/original-pokeemerald-readme.md).
