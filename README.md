# Emerald3DS

**Pokémon Emerald as a native, dual-screen 3DS game.** The game plays on the top
screen. The bottom screen is a real touch interface for your team, bag, Pokédex,
the Hoenn map and the port's own settings, drawn with Emerald's own fonts, icons
and window borders so it looks like part of the game rather than an overlay.

No emulator and no ROM. The [pret decompilation](https://github.com/pret/pokeemerald)
is recompiled from ARMv4T to the 3DS's ARM11, and builds its own assets from
source.

> [!Note]
> **Written with AI.** I don't know the first thing about GBA decompilations or
> 3DS homebrew. I wanted to see how far this could get, and almost all of the
> code here came out of that.

## Why a native port

Every other way to play a GBA game on a 3DS, whether GBA Virtual Console or
[open_agb_firm](https://github.com/profi200/open_agb_firm), hands the cartridge
to the 3DS's built-in GBA hardware. That runs as a FIRM, with the operating
system shut down and only one screen available. **A native recompile is the only
route to a GBA game that can use the touch screen**, which is the reason this
project exists.

Because it is a recompile rather than an emulator, the bottom screen is not
scraping RAM and guessing. It is compiled *with the game*, so `gPlayerParty`,
`gBagPockets`, `gPokedexEntries` and the game's own sprite and font data are
ordinary symbols it reads directly. Nothing it draws can drift out of sync with
what the game believes.

```
  ┌────────────────────────────────────┐   top: the GBA picture, software
  │           GBA output               │        rasterised and drawn by the
  │      1x / 1.5x / fill screen       │        3DS GPU
  └────────────────────────────────────┘
  ┌──────────────────────────────┐         bottom: game-side code, reading the
  │ [icon] SPARKY  /\  Lv12      │         game's own structures directly
  │ [PSN ] ####------  24/38     │
  │  PARTY  BAG  MAP  DEX  EXTRA │
  └──────────────────────────────┘
```

## The bottom screen

Tabs appear only once the game has given you the thing they show, the same way
the start menu works: no Pokédex tab before you own a Pokédex.

**PARTY.** Your team as a 2x3 grid of cards: animated mon icon, nickname, level,
and an HP bar that slides the way the battle bar does instead of jumping. Status
is the party menu's own PSN/SLP/BRN badge. In battle each card also gets **type
matchup arrows**, one for how hard that mon hits the opponent and one for how
dangerous the opponent is to it. Tap a mon for stats, ability, nature, held item
and moves, and an **IV/EV** button there swaps the stat block for the full
spread: base stat, IV and EV beside the computed total for each of the six, the
EV total against the 510 cap, perfect IVs and maxed EVs marked, and the nature's
boosted and hindered stats signed. Emerald works all of that out and shows you
none of it.

**BAG.** Pockets, quantities, and a details pane with the item's sprite and
description. USE opens a picker of your team, so a Potion asks who it is for
instead of guessing. Items work in the field **and in battle**, where using one
goes through the engine's action queue and costs your turn exactly as the
in-game bag does. An item that would do nothing costs you nothing. The original
menus are untouched; this is another route to the same action, never a
replacement.

**DEX.** The selected mon's sprite and the seen/owned counts on the left, the
scrolling list on the right, and an entry screen with the sprite, footprint,
category, and height and weight in the game's own imperial format. Unseen
entries stay anonymous.

**MAP.** Hoenn's region map, the game's own art at 1:1, with your marker where
you actually are: on the right stretch of a long route, and on the cave mouth or
the town door when you are inside one. Tap anywhere to read that place's name,
and tap the sea to go back to following yourself. Two buttons appear on the
caption when they apply:

- **FLY** flies you there, using the same checks the party menu does. It is
  offered only from a town you have already reached on foot, with a badge and a
  Pokémon that knows Fly, and only when you are outdoors and not mid-anything.
- **WILD PKMN** lists what lives there: every wild species for that place with
  its types, marked with a ball if you have caught it. Anything you have not
  seen yet is a silhouette, so the list reads as what is left to find rather
  than a spoiler for it. It follows the randomiser when that is on, and it
  follows you from room to room inside a cave.

**TROPHY.** 67 achievements, earned by playing, from choosing your starter and
getting the Bike through the eight badges, the HMs, the Master Ball and the
Champion, plus the legends, Pokédex milestones, contests and a few odder ones
(use Splash, hit a jackpot, catch a shiny). Each kind has its own colour, on
the list and on the toast: story gold, legendary green, Pokémon red, battle
purple, extras blue and contests pink. Two buttons split them into MAIN and
POST-GAME. The post-game ones stay "Hidden Achievement" until you enter the Hall
of Fame, then show what they are. Every one is read from the game's own data,
and counters show how far along the shorter goals are. Progress is kept per
playthrough, keyed on your trainer ID, so a new game starts a fresh list, and
loading a save that already has badges unlocks them straight away. Nothing
syncs to RetroAchievements; `3ds/ACHIEVEMENTS_PLAN.md` says why.

**EXTRA.** Three pages of port settings.

*Page 1* changes nothing about how the game plays: fast-forward at 1x, 2x, 4x or
8x; top-screen size at 1x (pixel-perfect), 1.5x (fills the height) or FILL
(fills the panel, stretching 11%); and the four buttons a GBA has no use for (X,
Y, ZL, ZR) bindable to hold-for-speed or to the touch UI's modifier key. Screen
size and bindings persist. Fast-forward deliberately resets each launch.

*Page 2* is behind a page turn because every option on it is a cheat, and all
four are off by default:

- **EXP ALL.** Every living party member gains experience, as though it were
  holding an Exp. Share. The game's own participant split does the work, so the
  totals stay balanced rather than multiplied.
- **LEVEL CAP: OFF / SOFT / HARD.** The cap follows your badges (15, 19, 23, 29,
  31, 33, 42, 46, then 58 for the Elite Four), which are Emerald's real gym
  leader ace levels. HARD stops experience dead at the cap, Rare Candies and the
  day care included. SOFT sharply reduces it instead. The row shows the cap you
  are currently under.
- **RANDOMISER.** Wild encounters, trainer parties, gift and static Pokémon, the
  legendaries, the roaming Lati and your starter are all remapped. The mapping
  comes from your save's trainer ID, so it is the same every launch and toggling
  it off and back on does not reshuffle anything. It cannot softlock the game:
  no item is ever randomised, so every key item, HM and badge is untouched, and
  a species that could learn a field HM in the original is always replaced by
  one that can learn it too. Pokémon you have already caught do not change.
- **BAG SORT: OFF / TYPE / NAME.** TYPE is category order, NAME is alphabetical.
  It reorders the real bag, so the in-game bag and the BAG tab agree, and the
  order sticks in your save.

*Page 3* is quality of life. **PHONE CALLS** can be turned off, which silences
only the trainers who ring unprompted mid-route. Every scripted call still comes
through, and so does the PokéNav's own Match Call screen. **QUICK BALL** turns off
the quick-throw strip described below, for anyone who would rather keep those
forty rows of their party grid during a wild battle.

**A fourth page exists in debug builds only.** It holds the switches that test
the port rather than play the game: force the next wild encounter shiny, show
every tab regardless of what the save has unlocked, silence one half of the
mixer at a time, and fire a test achievement toast or re-derive the save's
achievements. One value in `3ds/bridge.h` turns the page off, and turning it
off also pins those settings to their harmless values, so a `settings.bin`
written by a debug build cannot leave a player with a muted channel and no
control to unmute it.

**Quick throw.** The NDS games remembered which ball you last threw and kept it
in reach on the touch screen. Emerald has no such idea, so this adds one: when a
wild battle asks what you want to do, a strip appears along the bottom of
whichever tab you are on with that ball, how many you have left, and a THROW
button that spends your turn on it. Tap the name to cycle through the other
balls you own; only an actual throw changes what it remembers, so browsing costs
you nothing. It remembers across launches, and it learns from the in-game bag
too, not just from itself.

It appears only while the game is waiting for your decision, so it is gone
again the moment you make one, and only against something you could really
throw a ball at. Until you have thrown one it offers the first ball in your bag
that is not the Master Ball, which it will never suggest on its own. It can be
switched off on page 3 of EXTRA.

**Shiny alerts, over every tab.** When a wild Pokémon is shiny, a gold panel
takes the middle of the bottom screen and names it at double size, whichever tab
you are on. It stays until you press DISMISS, and clears itself the moment the
encounter is decided one way or the other, so it can never be left up over a
battle that is already over. The tab bar keeps working underneath, so you can
still check your team before deciding what to throw.

It appears only for a Pokémon you could actually throw a ball at, so a trainer's
shiny, a link battle and the whole Battle Frontier stay quiet. Emerald does
already tell you twice, with a recoloured sprite and a sparkle, but both land in
the first second of a battle whose transition you may not have been watching,
and neither survives being missed.

**Achievement toasts, over every tab.** When one unlocks, a gold strip appears
across the top of the bottom screen with its name and a VIEW button that opens
the list on it. It goes by itself after four seconds, and anything you did not
look at leaves a gold dot on the TROPHY tab until you do. The strip ends exactly
where the shiny panel begins, which ends exactly where the quick-throw strip
begins, so all three can be up at once without covering each other.

The whole interface follows your **Options → Frame** border choice, live, as you
cycle through it.

## Status

Boots and plays, on a console and in the [Azahar](https://azahar-emu.org/)
emulator.

| | |
|---|---|
| Boot, overworld, battles, saving | ✅ |
| Top screen | ✅ Software rasteriser, selectable scale |
| Real-time clock | ✅ Backed by the console clock, so berries and time of day work |
| Bottom: party, bag, Pokédex, map, extras | ✅ |
| Bottom: shiny alerts, IV/EV viewer | ✅ |
| Bottom: encounter lists and Fly from the map | ✅ |
| Tweaks: EXP All, level cap, randomiser, bag sort | ✅ |
| Battle items from the touch screen | ✅ |
| Bottom: quick throw of your last used ball | ✅ |
| Bottom: achievements and unlock toasts | 🚧 On the `achievements` branch, not yet tested on a console |
| Audio | ✅ Stereo PCM16: music, cries and PSG. Needs a DSP firmware dump |
| Link cable and wireless | 🚧 On the `local-wireless` branch, unbuilt and untested |
| Battery life, sustained timing | ❓ Never measured over a long session |

## Getting it

Every push builds it. Grab **`emerald3ds`** from the latest
[Actions run](../../actions): it contains `emerald3ds.cia` (install to a
console) and `emerald3ds.3ds` (boots directly in an emulator, no install step).
The **`emerald3ds-elf`** artifact holds the ELF and linker map. Keep the one
matching your build, because a crash log only ever gives a raw address and those
are what turn it back into a function name.

**Sound needs one extra file.** libctru loads the DSP component from
`sdmc:/3ds/dspfirm.cdc` and the game runs silent without it. Dump it from your
own console with [DSP1](https://github.com/zoogie/DSP1), which is a one-time
step already in the standard CFW setup guides. Under an emulator with HLE audio
the contents are never read, so any file at that path will do. **This is the
first thing to check if a build has no sound.**

To build it yourself you need devkitPro (devkitARM, libctru, citro2d/citro3d),
libpng for the decompilation's `gbagfx`, and
[makerom](https://github.com/3DSGuy/Project_CTR/releases):

```sh
make tools && make generated             # decomp tools + generated headers
python3 tools/generate_wasm_assets.py    # -> build/assets
rp2350/gen_sound_assets.sh               # one-time: wav->bin, mid->song .s
3ds/build_objs.sh                        # game sources -> libpokeemerald.a (ARM11)
make -C 3ds                              # -> 3ds/emerald3ds.{cia,3ds}
```

## Limitations

- **Audio needs a DSP firmware dump**, as above. It is also the one failure the
  port cannot report: a shipping build writes no log file, and no build can ship
  a DSP dump.
- **No trading or link battles yet.** The Cable Club over 3DS local wireless is
  written but unbuilt on the `local-wireless` branch. The Union Room and Mystery
  Gift use a separate wireless stack that is still stubbed.
- **No true 2x top screen, ever.** 240x160 doubled is 480x320, which overruns a
  400x240 panel in both directions, so 1.5x is the largest fit and its rows
  alternate 1px and 2px. The 1x mode is the only pixel-perfect one.
- **No save states.** This is a native build, not an emulator: the game's state
  lives scattered through the binary's own memory rather than in one
  snapshottable region.
- **Thin hardware data.** It runs on a console, but there are no fill-rate,
  battery or long-session timing measurements, and ZL/ZR bindings are
  untestable on an Old 3DS by definition.

## How it works

See **[README-TECHNICAL.md](README-TECHNICAL.md)** for the architecture: the
two-worlds bridge between game and host code, the GBA memory map, the audio
path and the faults fixed along the way, the debugging tools, and the repository
layout.

Before editing the bottom screen, read
[3ds/SECOND_SCREEN_CHEATSHEET.md](3ds/SECOND_SCREEN_CHEATSHEET.md), which is the
working reference for how that layer is wired.
[3ds/ROADMAP.md](3ds/ROADMAP.md) is what is planned next.

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
  where a million-line decompilation touches hardware.
- **[mattdeeds/pokeemerald-rp2350](https://github.com/mattdeeds/pokeemerald-rp2350)**
  is this repo's direct base: the software rasteriser, the m4a mixer in C, and
  the flash-save hooks the 3DS port inherits.
- The original pokeemerald README is preserved at
  [docs/original-pokeemerald-readme.md](docs/original-pokeemerald-readme.md).
