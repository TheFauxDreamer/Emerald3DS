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
  │  PARTY  BAG  MAP  DEX  HOME  │
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
and moves. In battle each move there gets its own arrow against the opponent
(two in a double battle), and tapping a move shows the exact multiplier, x2 or
x0.5. The marks follow the battle's own rules: Hidden Power's real type, Weather
Ball in weather, Levitate, Foresight, and the fixed-damage moves that only care
whether they can hit at all. An **IV/EV** button there swaps the stat block for the full
spread: base stat, IV and EV beside the computed total for each of the six, the
EV total against the 510 cap, perfect IVs and maxed EVs marked, and the nature's
boosted and hindered stats signed. Emerald works all of that out and shows you
none of it.

**Fight from the touch screen.** While you are choosing what to do in a
battle, the PARTY tab becomes a battle panel, as on the DS: your four moves as
big buttons, each with its type, PP and how well it hits the opponent (x2,
x0.5, x0, or a dim x1 when neutral, with a coloured arrow), and your team in a
row below. A Pokémon's card says why it cannot switch in, if it cannot. Tap a move to use it. Tap a Pokémon to see its card, then SWITCH
IN to send it out, or INFO for its full details. Both go through the game's
own battle menus, so the game's rules and messages still apply: a move with no
PP, or one that is Disabled or Taunted, gets the game's own refusal, and a
trapped Pokémon gets the party menu's "can't be switched out". A fainted
Pokémon, an egg, one already out or a partner's Pokémon shows why on the panel.
In a double battle, a move that needs a target uses the game's own target
choice on the top screen. The panel stays for the whole battle: while the turn
plays out the moves stay on screen, and the next choice lights them up again.
The grid comes back when the battle ends.

Above the moves, each opponent has a line with its name, level and HP bar. Tap
it for its types, status, **stat stages** ("ATK -1  SPE +2") and the conditions
the game keeps but never shows: confused, seeded, trapped, cursed, behind a
substitute. The card of your own Pokémon that is out shows its stages too.
**BAG** jumps to the BAG tab, and **RUN** runs, with the game's own "No!
There's no running from a trainer battle!" when you cannot. When a Pokémon
faints, the panel asks which one goes next: tap it, then SEND OUT. Press A
instead and the game's own party menu opens as it always did.

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
and tap the sea to go back to following yourself. Three buttons appear on the
caption when they apply:

- **FLY** flies you there, using the same checks the party menu does. It is
  offered only from a town you have already reached on foot, with a badge and a
  Pokémon that knows Fly, and only when you are outdoors and not mid-anything.
- **ESCAPE** takes you back out of a cave or a building to where you went in,
  as Dig and the Escape Rope do. It shows on your own location, wherever the
  game would let you use them. A Pokémon that knows Dig goes first, because it
  is free; otherwise it uses one Escape Rope from your bag. It asks before it
  acts, and names which one.
- **WILD PKMN** lists what lives there, like the DexNav: one list for each way
  to meet them (LAND, SURF, Rock SMASH, and the OLD, GOOD and SUPER rods), with
  each Pokémon's types, level range and chance, most common first. A ball marks
  the ones you have caught. Anything you have not seen yet is a silhouette, with
  its level and chance but no name, so the list reads as what is left to find
  rather than a spoiler for it. It follows the randomiser when that is on, and
  it follows you from room to room inside a cave. The roaming Latias or Latios
  (once you have seen it) and the mass outbreak from the TV news go at the top
  of the list of the place they are, in a coloured frame.

Like the PokéNav Plus, the map also marks places with news: a red dot where a
trainer wants a rematch, a purple one where the roamer is now, and a green one
where the outbreak is. A small key in the corner says which is which, and
the caption of a marked place says what is there ("2 want a rematch", "Many
SEEDOT"). The marks follow the game: a trainer calls, the roamer moves when
you change maps, and the outbreak ends when the game ends it.

**TROPHY.** 86 achievements, earned by playing, from choosing your starter and
getting the Bike through the eight badges, the HMs, the Master Ball and the
Champion, plus the legends, the Regi puzzles, Pokédex milestones, contests and
a few odder ones (use Splash, hit a jackpot, catch a shiny, find a hidden item,
play the White or Black Flute, walk into Shoal Cave at low tide, trade with
another player, beat one in a link battle, meet the woman who built the PC
system). One, An Impossible
Task, asks for the complete National Pokédex, which needs species that only
FireRed, LeafGreen or Colosseum can give, so it cannot be earned.
`3ds/ACHIEVEMENTS.md` lists them all.
Seasoned Traveller asks for every route and town in Hoenn, and once three or
fewer are left it names them. A save from an earlier build already counts the
routes where it found an item or beat a trainer. The event islands count too: after the Hall of
Fame, Dad hands over the Eon, Aurora and Mystic Tickets and the Old Sea Map,
opening Southern Island, Faraway Island, Birth Island and Navel Rock. On a save
already past that scene, any S.S. Tidal ferry attendant hands them over
instead. Each kind has its own colour, on
the list and on the toast: story gold, legendary green, Pokémon red, battle
purple, extras blue and contests pink. Two buttons split them into MAIN and
POST-GAME. The post-game ones stay "Hidden Achievement" until you enter the Hall
of Fame, then show what they are. Every one is read from the game's own data,
and counters show how far along the shorter goals are. Progress is kept per
playthrough, keyed on your trainer ID, so a new game starts a fresh list, and
loading a save that already has badges unlocks them straight away. Nothing
syncs to RetroAchievements; Part E of `3ds/ROADMAP.md` says why.

**HOME.** A grid of tiles, like the DS games' Pokétch. The first five read your
game: **TRAINER** is your own trainer card, the game's own art, tap to flip,
with RECORDS for the counters the game keeps and never shows (steps, battles,
catches, eggs hatched, trades, shopping trips). **CLOCK** has the game's time,
your play time and steps, the Repel steps left, and about how many steps each
egg in your party needs to hatch. **DOWSING** is the Itemfinder as a radar, once
you have one: the same range it searches, with a dot for every hidden item
still there. **BERRIES** lists every tree you planted, where it is, how far it
has grown, when it grows next and how often you watered it. **DAY CARE** shows
who is there, the level each one had when you left it and has now, and the old
man's words for how they get along.

Then one tile for each page of port settings: SETTINGS, GAMEPLAY, EXTRAS and
FOLLOWER, plus LINK for the Cable Club. Tap a tile to open its page and BACK to
return; switching tab closes it, so HOME always opens on the grid. A setting
that is only on or off is a checkbox: tap the box or its name.

*SETTINGS* changes nothing about how the game plays: fast-forward at 1x, 2x, 4x or
8x; top-screen size at 1x (pixel-perfect), 1.5x (fills the height) or FILL
(fills the panel, stretching 11%); and the four buttons a GBA has no use for (X,
Y, ZL, ZR) bindable to hold-for-speed or to the touch UI's modifier key. Screen
size and bindings persist. Fast-forward deliberately resets each launch.

*GAMEPLAY* is a tile of its own because every option on it is a cheat, and all
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

*EXTRAS* is quality of life. **PHONE CALLS** can be turned off, which silences
only the trainers who ring unprompted mid-route. Every scripted call still comes
through, and so does the PokéNav's own Match Call screen. **QUICK BALL** turns off
the quick-throw strip described below, for anyone who would rather keep those
forty rows of their party grid during a wild battle.

**DAY CARE** lets the Pokémon you leave at the Route 117 Day Care walk in its
yard. With two, they face away from each other, turn toward each other or walk
toward each other, by how well they get along. It is separate from FOLLOWER, it
is kept once for the console, and a change shows the next time you enter
Route 117.

*FOLLOWER* is the follower. **FOLLOWER** makes the first Pokémon in your party
walk behind you, as in HeartGold and SoulSilver. It comes out of its own type of
Poké Ball. It goes back into the ball for Surf, the bike, doors and cutscenes,
and comes out again after. Talk to it and it shows an emote and says how it
feels. Outdoors, it can also look up at the sky in the day or gaze at the stars
at night, by the 3DS clock. It takes part when you use Cut or Rock Smash. If
your first Pokémon has fainted, the next one follows. The switch is off by
default. The feature is aarant's `followers` branch of pokeemerald, with its
sprites for all 386 Pokémon.

Three options below the switch change how the follower behaves. That branch
sets them when the game is built. Here each save keeps its own values, and its
own FOLLOWER switch.

- **WHO: LEAD / STARTER.** LEAD is the first Pokémon that can fight. With
  STARTER, only your starter follows, as Pikachu does in Yellow. The starter is
  the Pokémon with your trainer ID that you met at level 5 on Route 101. If it
  has fainted or is not in your party, nothing follows.
- **BOBBING.** The follower moves up and down as it walks. Turn it off for a
  steady sprite.
- **BALL: OWN / POKE BALL.** OWN is the ball that caught the Pokémon. POKE BALL
  uses a plain Poké Ball for every Pokémon.

**A DEBUG tile exists in debug builds only.** Its page holds the switches that test
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
switched off on HOME's EXTRAS page.

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
| Bottom: encounter lists, Fly and Dig / Escape Rope from the map | ✅ |
| Tweaks: EXP All, level cap, randomiser, bag sort | ✅ |
| Battle items from the touch screen | ✅ |
| Moves and switching from the touch screen | ✅ |
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
builds the game from its own committed sources.

The follower code and its sprites come from aarant's fork of pokeemerald, which
states no license. They are used here with credit, and the rights to the art
stay with its creators. The tools under `tools/` that pret includes keep their
own licenses.

Pokémon and Pokémon character names are trademarks of Nintendo, Creatures Inc.,
and GAME FREAK Inc. This project is not affiliated with or endorsed by any of
them.

## Credits

**Code this port is built on**

- **[pret/pokeemerald](https://github.com/pret/pokeemerald)**: the
  decompilation everything is built on. Its tools include third-party code:
  [wav2agb](https://github.com/ipatix/wav2agb) by ipatix,
  [inja](https://github.com/pantor/inja) by pantor and
  [JSON for Modern C++](https://github.com/nlohmann/json) by nlohmann.
- **[tripplyons/pokeemerald-wasm](https://github.com/tripplyons/pokeemerald-wasm)**
  fenced every dependency on real GBA hardware behind `#if WASM`. That work,
  reused as `#if WASM || RP2350`, is why this port did not have to rediscover
  where a million-line decompilation touches hardware.
- **[mattdeeds/pokeemerald-rp2350](https://github.com/mattdeeds/pokeemerald-rp2350)**
  is this repo's direct base: the software rasteriser, the m4a mixer in C, and
  the flash-save hooks the 3DS port inherits. Its hardware build uses the
  [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk).

**Features taken from other projects**

- **[aarant/pokeemerald](https://github.com/aarant/pokeemerald)** by Ariel
  Antonitis. Its `followers` branch is the FOLLOWER option: the following
  Pokémon, their messages, emotes and field-move animations, the Poké Ball
  sprites and the dynamic overworld palettes. The day and night lines come from
  its `lighting` branch, and the DAY CARE yard from its `followers-expanded-id`
  branch. The branch history credits these contributors:
  - **Jaizu**: the Combusken, Chansey, Espeon, Gyarados, Magikarp, Skiploom and
    Shuppet sprites, the Lombre palette, and fixes.
  - **SonikkuA-DatH**: the Donphan, Taillow, Swellow, Silcoon and Cascoon
    sprites, palette edits, the extra footprint types, and the movements in
    the follower messages.
  - **[LarryTurbo](https://www.deviantart.com/larryturbo)**: the resized 32x32
    sprites for large Pokémon, and the Castform forms.
  - **ShinyDragonHunter**: the Regirock and Registeel sprites.
  - **shikashipx**: the Poké Ball sprites, the Substitute sprite and
    asymmetrical followers.
  - **andrian_timeswift**: the Squirtle, Wartortle and Blastoise sprites.
  - **rayrobdod**: the right-walking Krabby and Kingler sprites, from
    [pokeemerald-expansion PR #7881](https://github.com/rh-hideout/pokeemerald-expansion/pull/7881).
  - **Bassoonian** and **Eduardo Quezada**: text fixes and code.

  The follower sprites are based on the overworld sprites of Pokémon HeartGold
  and SoulSilver.
- **[rh-hideout/pokeemerald-expansion](https://github.com/rh-hideout/pokeemerald-expansion)**:
  the LEVEL CAP values come from its `src/caps.c`, and it carried the
  Krabby and Kingler sprites above.

**Tools and libraries for the 3DS build**

- **[devkitPro](https://devkitpro.org)**: the devkitARM toolchain and the
  libctru, citro2d and citro3d libraries the port links against.
- **[makerom](https://github.com/3DSGuy/Project_CTR)** from 3DSGuy's Project_CTR
  builds the CIA and the .3ds file.
- **bannertool** by Steveice10 builds the Home Menu banner. CI uses the
  [Linux build by Epicpkmn11](https://github.com/Epicpkmn11/bannertool).
- **[TricksterGuy/3ds-template](https://github.com/TricksterGuy/3ds-template)**:
  `3ds/emerald3ds.rsf` starts from its RSF.

- The original pokeemerald README is preserved at
  [docs/original-pokeemerald-readme.md](docs/original-pokeemerald-readme.md).
