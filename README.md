# Emerald3DS

**Pokémon Emerald as a native, dual-screen 3DS game.** The game runs on the
top screen, with a touch interface for your party and bag on the bottom. No
emulator and no ROM: the [pret decompilation](https://github.com/pret/pokeemerald)
is recompiled from ARMv4T to the 3DS's ARM11, and it builds its own assets.

The second screen is the point. Every GBA-on-3DS approach that exists, whether
GBA Virtual Console or [open_agb_firm](https://github.com/profi200/open_agb_firm),
hands the game to the 3DS's built-in GBA hardware, which runs as a FIRM with the
OS shut down and only one screen available. Native recompilation is the only
route to a GBA game that can use the touch screen, which is why this exists.

```
  ┌──────────────────────────────────┐   top: software PPU (rp2350/ppu.c)
  │        240x160 -> 360x240        │        -> RGB565 -> PICA200 texture
  │           GBA output             │        via citro2d, nearest, 1.5x
  └──────────────────────────────────┘
  ┌────────────────────────────┐         bottom: drawn game-side, so it reads
  │  PARTY   BAG   MAP         │         gPlayerParty and gBagPockets as
  │  [icon] NAME  Lv12  ####   │         ordinary symbols -- no RAM scraping
  └────────────────────────────┘
```

## Status

**Boots and plays, verified in the Azahar emulator. Not yet run on real
hardware.** Treat every row below as "works where it has been tested", which so
far means an emulator only.

| | |
|---|---|
| Boot / overworld / battles | ✅ Runs |
| Top screen | ✅ Software PPU through citro2d |
| Saves | ✅ 128 KB to `sdmc:/3ds/emerald3ds/pokeemerald.sav` |
| Bottom: party | ✅ 2×3 grid: icon, name, level, HP; tap for a detail view |
| Bottom: bag | ✅ Pockets, quantities, tap-to-use; gated to the overworld |
| Bottom: map | ❌ Placeholder |
| Window borders | ✅ Follows the player's Options → Frame choice |
| Audio | ⚠️ Needs a DSP firmware dump; some sample types silent |
| Frame rate | ❓ Never measured on a 3DS |
| Link cable / RFU | ❌ Not implemented |

## Getting it

Every push builds it. Grab **`emerald3ds`** from the latest
[Actions run](../../actions). It contains `emerald3ds.cia` (install to a
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
relocated, so the same pointers are simply correct. See
[3ds/README.md](3ds/README.md).

## How it works

[3ds/README.md](3ds/README.md) is the real explanation. The two rules that
matter:

**Two worlds, one bridge.** `include/gba/types.h` and `<3ds.h>` both typedef
`u8`/`u16`/`u32`, and the game's `include/` shadows libc's `string.h`, so no
translation unit may include both. Game-side code (`src/**`, `3ds/gba_mem.c`,
`3ds/ui/**`) sees only game headers; host-side code (`3ds/host/**`,
`rp2350/ppu.c`) sees only libctru. Everything they say to each other is in
[3ds/bridge.h](3ds/bridge.h), in stdint types. This is what makes the bottom
screen cheap: it is game-side, so Emerald's party data, item tables, fonts and
mon icons are ordinary symbols.

**The GBA memory map becomes one array.** The 3DS cannot hand out arbitrary
virtual addresses, so EWRAM/IWRAM/VRAM/palette/OAM/registers are offsets into a
single `gGbaMem` array, and the game keeps writing them exactly as before.

The port defines `RP2350=1`, which in this tree means *native CPU build, not GBA
hardware*. It inherits all 47 of that port's hardware seams rather than
duplicating them. `PLATFORM_3DS=1` then overrides only where the 3DS genuinely
differs.

[3ds/ROADMAP.md](3ds/ROADMAP.md) has what is planned next, plus the bring-up
notes worth reading before debugging anything.

## Limitations

- **Audio needs a DSP firmware dump.** libctru loads the DSP component from
  `sdmc:/3ds/dspfirm.cdc` and `ndspInit()` fails outright without it. The game
  then runs silent and says so in the log. Dump it from your own console with
  [DSP1](https://github.com/zoogie/DSP1); it is a standard one-time CFW setup
  step every NDSP homebrew requires. Separately, DPCM (compressed) and
  reverse-playback instruments are still silent stubs in `rp2350/m4a_1.c`.
- **Never run on hardware.** No fill-rate, battery or timing data exists.
- **No link cable or wireless.** RFU and multiboot are stubbed, so trading,
  link battles and Mystery Gift do not work.
- **The RTC is a dead cartridge clock.** Time reads as zero and the clock-set
  screen reports "clock is stopped", which affects berry growth and
  time-of-day events.
- **The top screen is 1.5×, not integer.** 240×160 × 1.5 = 360×240 fills the
  panel height exactly; true 2× is 480×320 and does not fit a 400×240 screen at
  any setting. Rows and columns therefore alternate 1px/2px.

## Repository layout

Almost everything here is upstream decomp. The port is confined to one
directory.

| Path | What it is |
|---|---|
| `3ds/` | **The port.** Bridge, GBA memory block, build scripts, ROM spec. |
| `3ds/host/` | libctru side: entry point, video, audio, saves, tracing. |
| `3ds/ui/` | Bottom screen, game-side: drawing, Emerald-font text, tabs. |
| `rp2350/` | The RP2350 port this is built on. `ppu.c` and `m4a_1.c` are shared. |
| `src/`, `data/`, `graphics/`, `sound/` | Upstream pokeemerald sources and assets. |
| `web/`, `tools/wasm_*` | The WASM build, retained as the PPU reference. |
| `docs/` | RP2350 hardware, build and porting documentation. |

The RP2350 and WASM targets still build (`make wasm`, and see
[docs/BUILD.md](docs/BUILD.md)). The WASM build in particular is deliberately
kept: `rp2350/ppu_validate.sh` pixel-diffs `rp2350/ppu.c` against the JavaScript
rasteriser, and that harness is the only reason the PPU can be called
byte-exact.

## Licensing

`3ds/` and `rp2350/` are original work under the **MIT License**
([rp2350/LICENSE](rp2350/LICENSE)).

The rest is the pret decompilation and carries no license from this project.
Following pret convention, no ROM is required or included. The decompilation
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
