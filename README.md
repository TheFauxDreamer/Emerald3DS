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
stats, ability, nature, held item and moves. An **IV/EV** button there swaps the
stat block for the full spread: base stat, IV and EV beside the computed total
for each of the six, the EV total against the 510 cap, perfect IVs and maxed
EVs marked, and the nature's boosted and hindered stats signed. Emerald
calculates every one of those numbers and shows you none of them.

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

**MAP.** Hoenn's region map, the game's own art at 1:1, with your marker where
you actually are: on the right stretch of a long route, and on the cave mouth or
the town door when you are inside one. Tap anywhere to read that place's name;
tap the sea to go back to following yourself. The caption names the landmark you
are standing on when there is one.

**EXTRA.** Two pages. **Page 1** is things the original game has no concept of
but which leave it playing exactly as it shipped. Fast-forward at 1x, 2x,
4x or 8x; top-screen size at 1x (pixel-perfect), 1.5x (fills the height) or
FILL (fills the panel, stretching 11%); and the four buttons the GBA has no use
for (X, Y, ZL and ZR) bindable to a hold-for-speed or to the touch UI's
modifier key. Screen size and bindings persist; fast-forward deliberately
resets each launch.

**Page 2** is different in kind, which is why it is behind a page turn. Every
option on it is a cheat, and all four are off by default:

- **EXP ALL.** Every living party member gains experience, exactly as though it
  were holding an Exp. Share. The game's own participant/share split does the
  work, so the totals stay balanced rather than multiplied.
- **LEVEL CAP: OFF / SOFT / HARD.** The cap follows your badges (15, 19, 23, 29,
  31, 33, 42, 46, then 58 for the Elite Four), which are Emerald's real gym
  leader ace levels. HARD stops experience dead at the cap, including Rare
  Candies and the day care. SOFT sharply reduces it instead. The row shows the
  cap you are currently under.
- **RANDOMISER.** Wild encounters, trainer parties, gift and static Pokemon,
  the legendaries, the roaming Lati, and your starter are all remapped. The
  mapping is derived from your save's trainer ID, so it is the same every launch
  and toggling it off and back on does not reshuffle anything. It cannot
  softlock the game: no item is ever randomised, so every key item, HM and badge
  is untouched, and a species that could learn a field HM in the original is
  always replaced by one that can learn it too. Already-caught Pokemon do not
  change.
- **BAG SORT: OFF / TYPE / NAME.** TYPE is category order, NAME is alphabetical.
  It reorders the real bag, so the in-game bag and the BAG tab agree, and the
  order sticks in your save.

**A third page exists in debug builds only.** It gathers the three switches
that test the port rather than play the game: **SHINY**, which makes the next
wild encounter shiny so the alert below can be exercised without waiting out
odds of one in 8192; **TABS**, which shows every tab regardless of what the save
has unlocked; and the four **audio A/B** switches that silence one half of the
mixer at a time, which is the only way to tell "the PSG channels are dead" from
"everything is dead". One value in `3ds/bridge.h` turns the page off, and
turning it off also pins those three settings to their harmless values, so a
`settings.bin` written by a debug build cannot leave a player with a muted
channel and no control to unmute it.

**Shiny alerts, over every tab.** Not a sixth tab and not a setting. When a wild
Pokémon is shiny, a panel takes the middle of the bottom screen and names it at
double size, whichever tab you happen to be on. It stays until you press
DISMISS, and clears itself the moment the encounter is decided one way or the
other — caught, knocked out, fled or run from — so it can never be left up over
a battle that is already over. The tab bar keeps working underneath, so you can
still check your team before deciding what to throw.

It appears only for a Pokémon you could actually throw a ball at, so a trainer's
shiny, a link battle and the whole Battle Frontier stay quiet. Emerald does
already tell you twice, with a recoloured sprite and a sparkle, but both land in
the first second of a battle whose transition you may not have been watching,
and neither of them survives being missed.

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
| Bottom: map | ✅ Region map, player marker, tap for names |
| Bottom: tweaks | ✅ EXP All, badge level cap, randomiser, bag sort |
| Shiny alert, IV/EV viewer, debug page | 🚧 Written, not yet compiled or run |
| Battle items from the touch screen | ✅ |
| Audio | ✅ Stereo PCM16, music, cries and PSG; needs a DSP firmware dump |
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
pointer directly after a 1-byte opcode (`.byte SCR_OP_GOTO` then
`.4byte \destination`), so thousands of relocations are unaligned, and the 3DSX
relocation table is indexed in whole 4-byte words: `3dsxtool` aborts outright
with `Unaligned relocation!`. Realigning the bytecode would mean changing 332
macros across 9 files plus every interpreter that reads them. A CXI is loaded at
a fixed address and never relocated, so the same pointers are simply correct as
linked. `make -C 3ds 3dsx` still exists, purely so the failure stays
reproducible.

## How it works

Two rules shape everything.

**Two worlds, one bridge.** `include/gba/types.h` and `<3ds.h>` both typedef
`u8`/`u16`/`u32`, and the game's `include/` shadows libc's `string.h`, so no
translation unit may include both.

```
game-side : src/**, rp2350/{bios,asm_stubs,m4a_1}.c, 3ds/gba_mem.c, 3ds/ui/**
host-side : 3ds/host/**, rp2350/ppu.c
```

Everything they say to each other is in [3ds/bridge.h](3ds/bridge.h), in stdint
types only; that header includes neither side's headers and must stay that way.
This is exactly what makes the bottom screen possible: it is game-side, so the
game's party data, item tables, fonts and icons are ordinary symbols, with no
RAM scraping.

**The GBA memory map becomes one array.** The 3DS cannot hand out arbitrary
virtual addresses, so EWRAM/IWRAM/VRAM/palette/OAM/registers are offsets into a
single `gGbaMem` array and the game keeps writing them exactly as before. It is
an array rather than a pointer because an array's address is a link-time
constant, so `&REG_WIN0H` still works in the static initialisers that
`src/field_screen_effect.c` and `src/pokenav_menu_handler_gfx.c` build; a `u8 *`
makes those *"initializer element is not a compile-time constant"*.

The consequence is that the regions sit in `.bss` alongside the game's own
`EWRAM_DATA` variables, so their relative addresses are link-order dependent.
Exactly one place in the tree ever compared them: `IsTileMapOutsideWram()` in
`src/bg.c`, which asked `tilemap > IWRAM_END` to mean "is this in VRAM?". The
`PLATFORM_3DS` branch there asks that question directly instead.

Before editing the bottom screen, read
[3ds/SECOND_SCREEN_CHEATSHEET.md](3ds/SECOND_SCREEN_CHEATSHEET.md): it is the
working reference for how that layer is wired, from the per-frame hook to the
repaint policy to the gates a write path has to pass.

### Why the build defines `RP2350=1`

In this tree the `RP2350` macro has come to mean *native CPU build, not GBA
hardware*: no link cable, no LCD to chase VCOUNT, no IWRAM mixer copy, saves
through the `Rp2350Save*` hooks, and a per-frame `Rp2350PresentFrame()` callback.
All of that is what the 3DS wants too, so the port inherits all 47 seam sites
instead of duplicating them. `PLATFORM_3DS=1` then overrides only the two places
where the 3DS genuinely differs, plus the cartridge clock:

| File | Why |
|---|---|
| `include/gba/defines.h` | Region bases, as above. `EWRAM_DATA`/`IWRAM_DATA` become plain `.bss`. |
| `include/gba/flash_internal.h` | `FLASH_BASE` points at a RAM array mirrored to the SD card, not at QSPI flash. |
| `src/siirtc.c` | The GBA carts carried an S-3511A; a 3DS has no cart, so the driver is backed by the console clock. |

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

### The five faults, and what fixed them

All five are fixed in the tree. The mixer question this list used to end on is
settled: the original `src/m4a_1.s` runs on the ARM11, sounds right on a
console, and is now the only mixer this port builds.

**1. Thread priority: silence with everything else correct.** libctru hardcodes
the NDSP service thread at priority `0x18` (`ndspInit`, `libctru/source/ndsp/ndsp.c`).
A main thread that outranks it starves it, the DSP never drains a wave buffer,
and every counter on our side still reads healthy. `fix_thread_priority()`
lowers the main thread to `0x30`, the priority every `.3dsx` gets under hbmenu,
which is the ordering NDSP is actually tested in. It logs the resulting priority
unconditionally: an absent line cannot tell "the inversion is not happening"
from "the code never ran".

**2. A ring buffer sized in the wrong unit.** The ring was a "samples" count
with the array declared to match, while the indexing was already
`[frame * 2 + channel]`. Every frame wrote 0x1C00 bytes past the end into the
neighbouring `.bss`. Azahar tolerated it and the sound was fine; a real ARM11
found a zeroed `sBlock[]` pointer and took a data abort writing to address 0 a
few seconds after boot. `RING_FRAMES` is now named for what it counts, and
everything downstream sizes from `AUDIO_CHANNELS` rather than a literal 2.

**3. Playback rate off by a refresh rate.** The DSP was asked for
`224 x 60.0` Hz while the game produces 224 samples per *panel* refresh, which
is 59.8261 Hz. That is 39 samples a second more than exist, so the ring drained
at a fixed rate and clicked roughly every six seconds even when nothing else was
wrong. The corrected 13401 Hz is also closer to the GBA's true 13379 than the
old value, dropping pitch error from 0.46% to 0.16%.

**4. Eight-bit output under a sixteen-bit mixer.** DirectSound is 8-bit at
source and survives the wider type exactly, but the PSG channels are generated
at 16-bit precision and a console sums the two in the analog domain rather than
on an 8-bit grid. The old 8-bit entry point quantised the PSG away and mixed one
bit from clipping. `Rp2350MixFrame16()` and `Rp2350MixFrameStereo16()` replaced
it; the 8-bit form remains only because the RP2350 port's I2S ring is built on
it.

**5. DPCM and reverse-playback instruments were silent stubs.** A fault in the
C reimplementation only -- `src/m4a_1.s` always decoded compressed samples, so
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

### Which mixer ships

`rp2350/m4a_engine.c` is a C reimplementation of `src/m4a_1.s`, written because
the RP2350's Cortex-M33 is Thumb-2 only and physically cannot execute ARMv4T
ARM-mode code. **The ARM11 is ARMv6K and can**, so this port runs the original
assembly: the code Game Freak shipped, which cannot be wrong about the engine
the way a reimplementation can.

That was a `CTR_M4A_ASM` build switch and a two-job CI matrix for as long as it
was unproven. It has since been heard on a console, so there is one build and no
switch. `3ds/build_objs.sh` always assembles `src/m4a_1.s` plus
`3ds/asm/m4a_arm11.s`, and never compiles `m4a_engine.c` -- the two define the
same 31 symbols, so building both is a link error rather than a choice. The C
engine stays in the tree for the RP2350 port that has no alternative.

### Diagnostics

Audio faults are about how something *sounds*, and no counter can measure that,
so the port carries two instruments.

`3ds/host/audio.c` writes a health report to the log at frame 600 (about ten
seconds in). It reports the host-side counters, the engine's own state, the
first live channel's contents, per-subsystem output peaks (DirectSound, PSG and
the compressed path each separately, because "the PSG is silent under a healthy
DirectSound" and "both are silent" are different faults), a discontinuity count
that says whether clicks land on frame boundaries or inside the audio, and a
one-line verdict. When the mix is genuinely all zero the verdict walks the
engine chain and names the first link that was never made, rather than just
reporting silence.

The debug page of the bottom screen's EXTRA tab silences one half of the mixer
at a time (PSG, reverb, DirectSound) and toggles the stereo downmix. Neither half can
be judged by ear while the other is playing, so this is the only instrument
there is for a fault about quality rather than plumbing. The stereo switch
downmixes into both sides rather than reconfiguring the NDSP channel: a switch
that exists to diagnose a fault must not be able to introduce one.

### Audio still requires a DSP firmware dump

Not a quirk of this port; it applies to every 3DS homebrew that uses NDSP.
libctru loads the DSP component from `sdmc:/3ds/dspfirm.cdc`, and `ndspInit()`
fails outright if that file is missing, so the game runs silent. `CtrAudioInit()`
says so via `CtrLog()` (always compiled, unlike the `CTR_BOOT_DIAG` traces);
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

## Bring-up and debugging

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

## Limitations

- **Audio needs a DSP firmware dump.** libctru loads the DSP component from
  `sdmc:/3ds/dspfirm.cdc` and `ndspInit()` fails outright without it, so the
  game runs silent and says so in the log. Dump it from your own console with
  [DSP1](https://github.com/zoogie/DSP1); it is a standard one-time CFW step
  every NDSP homebrew needs.
- **Audio has never been heard on hardware.** DPCM and reverse instruments,
  stereo, the sample rate and the NDSP thread ordering are all fixed and correct
  in an emulator, but the port ships two interchangeable mixers (the C
  reimplementation of `m4a_1.s`, and the original assembly, which the ARM11 can
  run) and which of them sounds right can only be settled by ear on a console.
  See [Audio](#audio).
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
| `3ds/bridge.h` | **The seam.** The only thing both worlds include. |
| `3ds/gba_mem.c` | The `gGbaMem` block and the save-flash backing (game side). |
| `3ds/build_objs.sh` | Game sources to `libpokeemerald.a` (ARM11). |
| `3ds/Makefile`, `3ds/emerald3ds.rsf` | Host sources, link, and makerom packaging. |
| `3ds/host/` | libctru side: `main.c` (entry point, per-frame hook, input, every port setting), `video.c`, `audio.c`, `save.c`, `settings.c`, `log.c`. |
| `3ds/ui/` | **The second screen**, game side: `bottom_screen.c` is the shell, one `tab_*.c` per tab, `ui_draw.c` and `ui_text.c` are the primitives. |
| `rp2350/` | The RP2350 port this is built on. `ppu.c` and the `m4a_*.c` pair are shared. |
| `src/`, `data/`, `graphics/`, `sound/` | Upstream pokeemerald sources and assets. |
| `web/`, `tools/wasm_*` | The WASM build, retained as the PPU reference. |
| `docs/` | RP2350 hardware, build and porting documentation. |

Working documents live beside the code they describe, all under `3ds/`:
[SECOND_SCREEN_CHEATSHEET.md](3ds/SECOND_SCREEN_CHEATSHEET.md) is how the bottom
screen works and should be read before editing `3ds/ui/`;
[ROADMAP.md](3ds/ROADMAP.md) is what is planned next;
[SECOND_SCREEN_PLAN.md](3ds/SECOND_SCREEN_PLAN.md) and
[UI_SKIN_PLAN.md](3ds/UI_SKIN_PLAN.md) are design records for work not yet done.

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
