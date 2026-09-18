# Features to port later from aarant's fork and pokeemerald-expansion

**Status: item 1 done.** Written 2026-09-17 against `b717279`. This is a
catalogue of features from other pokeemerald projects that this port can take
in later, with what research found about each one. Pick an item, check it
against the tree again, and write a plan for that item.

Already ported from the same sources (do not port these again):

| Feature | Source | Commits here |
|---|---|---|
| FOLLOWER: HGSS-style following Pokémon, messages, emotes, Poké Ball sprites, field-move animation, battle slide-in, dynamic overworld palettes | aarant `followers` | `edf4589`, `acbbe64`, `a7cd5f5`, `f973516` |
| Day and night follower lines | aarant `lighting` | `7a6a751` |
| DAY CARE: the Day Care Pokémon walk in the Route 117 yard | aarant `followers-expanded-id` | `b717279` |
| FOLLOWER options: WHO (lead or starter), BOBBING and BALL on EXTRA page 4 (item 1) | aarant `followers` | not committed yet |

## Contents

- [Sources](#sources)
- [How to port an item](#how-to-port-an-item)
- [Candidates](#candidates)
  1. [Follower options](#1-follower-options) (done)
  2. [Overworld Pokémon walk in place and bob](#2-overworld-pokémon-walk-in-place-and-bob)
  3. [Gen 6 icons with shiny palettes](#3-gen-6-icons-with-shiny-palettes)
  4. [Day and night lighting](#4-day-and-night-lighting)
  5. [Key item wheel](#5-key-item-wheel)
  6. [Nature change that keeps the PID](#6-nature-change-that-keeps-the-pid)
  7. [Modern text case](#7-modern-text-case)
  8. [Follower extras from pokeemerald-expansion](#8-follower-extras-from-pokeemerald-expansion)
- [Not applicable](#not-applicable)

## Sources

[aarant/pokeemerald](https://github.com/aarant/pokeemerald), by Ariel
Antonitis. It is a fork of pret pokeemerald that keeps the retail save layout.
Each branch has one main feature. Heads on 2026-09-17:

| Branch | Head | Date | Base |
|---|---|---|---|
| `followers` | `e914652eb0` | 2025-10-19 | pret `fa5ca10` |
| `followers-expanded-id` | `48ba7614fe` | 2025-10-19 | `followers` |
| `lighting` | `9eb835d441` | 2025-10-19 | `followers` |
| `icons` | `7d10d6b4f9` | 2025-04-09 | `followers` |
| `key-item-wheel` | `71404bc46b` | 2025-09-29 | pret master (old) |
| `guillotine` | `66c0d7aaef` | 2026-02-14 | pret master |
| `map-previews` | `ae0d1ed7e3` | 2023-09-17 | pret master (old) |

[rh-hideout/pokeemerald-expansion](https://github.com/rh-hideout/pokeemerald-expansion)
took the follower system in as `OW_FOLLOWERS_ENABLED`. Its engine is far from
vanilla, so use it for ideas and small fixes, not as the source of a port.

## How to port an item

The follower port set these rules. Follow them, or say why not.

- **Pin the source commit** and diff it against its merge base with pret. Do
  not diff against pret master.
- **Fence every change to an original file** with `#if PLATFORM_3DS`, and keep
  the vanilla code in `#else`. A hunk that splits an `#if` group must be joined
  with the next hunk, or the fence pairs with the wrong directive.
- **Prove the other builds do not change.** Preprocess each changed file in
  `src`, `include`, `data` and `asm` from HEAD and from the working tree,
  without `PLATFORM_3DS`, with the GBA, RP2350 and WASM defines. The text must
  be the same. Include the `.inc` files.
- **Graphics:** this tree uses `INCGFX_U32("x.png", ".4bpp", "-mwidth W
  -mheight H")`. Take W and H from the source branch's spritesheet rules. A
  vanilla asset that the source changes goes to `graphics/followers/` (or a
  similar folder) with the same relative path, so the vanilla file stays.
- **Field effects:** the 3DS build rebuilds `gFieldEffectScriptPointers` in
  `src/wasm_field_effect_scripts.c`. Add each new `FLDEFF_*` there too.
- **Map data:** `map.json` makes the GBA map data and cannot be fenced. Add
  objects at runtime instead, as `AddDayCareYardTemplates` does. Map
  `scripts.inc` files can be fenced.
- **GBA-tolerant faults:** on a GBA, a read of address 0 gives BIOS bytes and a
  write does nothing. On the 3DS both crash. Scan the new code for NULL, FALSE
  or 0 given to a pointer parameter, and for NULL returns used without a test
  (`f973516`).
- **Credits:** add the source and its contributors to the README Credits, with
  what each gave.

Space that is left:
- **settings.bin:** one free byte in each per-save record (`pad` in
  `struct CtrSaveSettings`), and bits 4 to 7 of the record's `follower` byte for
  more follower options. Also two free per-console bytes (`pad[2]` in
  `struct CtrSettings`), and three in the table header (`pad2`). A larger
  record needs a new version and a migration.
- **EXTRA tab:** pages 1 and 2 are full. Page 3 has four check rows and room for
  one more. Page 4 (FOLLOWER) has four rows at a 34px pitch that end at y 158,
  so a fifth row needs a 30px pitch. A sixth page does not fit in a debug
  build: the pager would reach the "test build" caption of the debug page.
- **Save data:** keep the retail layout (`SAVE_STRUCT_ALIGNED` in
  `include/global.h`). Unused space: `SaveBlock2.filler_90[8]`, and
  `SaveBlock1.unused_9C2[6]`, `unused_3598[0x180]` and `unused_3D5A[10]`. There
  are also 376 `FLAG_UNUSED_*` flags.

## Candidates

### 1. Follower options

**Done.** EXTRA page 4 has FOLLOWER, WHO (LEAD or STARTER), BOBBING and BALL.
The notes below are the research from before the work.

**What it gives.** Settings for what the `followers` branch fixes at build
time:
- **Bobbing** on or off (`OW_MON_BOBBING`).
- **Ball:** the Pokémon's own ball, or a plain Poké Ball (`OW_MON_POKEBALLS`).
- **Starter only:** only one Pokémon may follow, like Pikachu in Yellow. The
  branch has examples: species, met level and met location
  (`OW_MON_ALLOWED_SPECIES`, `OW_MON_ALLOWED_MET_LVL`,
  `OW_MON_ALLOWED_MET_LOC`). The Hoenn starter is met level 5 on Route 101.

**Source.** Already in the tree, in `include/constants/event_objects.h` and
`src/event_object_movement.c` (`GetFirstLiveMon`).

**Size.** Small.

**Fit.**
- Make each macro read a setting. `GetFirstLiveMon` reads `VarGet` on the
  macros now, so the restriction becomes a runtime test.
- Storage: per save, like FOLLOWER. Use bits of the `followerOn` byte, or the
  last free record byte. Either way bump the settings version.
- UI: a new EXTRA page. The user preferred a FOLLOWER page with FOLLOWER, WHO
  (LEAD or STARTER), BOBBING and BALL, and FOLLOWER moved from page 3.

### 2. Overworld Pokémon walk in place and bob

**What it gives.** Wandering overworld Pokémon walk in place between steps
(`OW_MON_WANDER_WALK`), and static ones bob while they walk in place.

**Source.** `followers-expanded-id`, commits "feat: added `OW_MON_WANDER_WALK`
config option" and "feat: static OW pokemon now bob while walking in place".
About 100 lines in `src/event_object_movement.c` and the movement tables.

**Size.** Tiny.

**Fit.** Low value today. Only objects with the follower graphics id use it:
Regice, and the Day Care yard. Worth it only together with item 8's NPC
Pokémon.

### 3. Gen 6 icons with shiny palettes

**What it gives.** New icons for all 386 Pokémon. They use the palette of the
front sprite, so a shiny Pokémon has a shiny icon. The branch updates the party
menu, the PC, trades, contests, mail, the Battle Dome and the Colosseum/XD
link. It also adds `FadeScreenHardware` for the PC fades.

**Source.** `icons` (31 own commits). The icons are from
[msikma/pokesprite](https://github.com/msikma/pokesprite), which must be
credited.

**Size.** Large: about 300 images, palette tables, and the party, storage,
trade and contest code.

**Fit.**
- The bottom screen draws icons with `UiMonIcon` (`3ds/ui/ui_draw.c`) from the
  game's icon tables. It gets the new art, but it must learn the front-sprite
  palettes to show shiny icons.
- The icon palette slots change. Check every bottom-screen user of
  `gMonIconPaletteIndices`.
- It changes many vanilla screens, so it needs its own switch or must be
  accepted as always on for the 3DS.

### 4. Day and night lighting

**What it gives.**
- The overworld tints by time of day, blended with the weather.
- Windows light up at night (GSC style).
- Lamps glow at night (marked WIP upstream).
- Object shadows are alpha-blended (HGSS style).
- The day and night follower lines, which are already ported.

**Source.** `lighting` (65 own commits, about +2500 and -660 lines):
`src/palette.c` (+430), `src/overworld.c` (+186), `src/field_weather.c` (+156),
`src/event_object_movement.c` (+221), 79 tileset files, and `.pla` files that
mark which palette colors are light. It changes `tools/gbagfx` to read `.pla`.

**Size.** Large.

**Fit.**
- The clock works: the game clock runs on the 3DS clock (`src/siirtc.c`).
- **Alpha shadows need the rasterizer.** `rp2350/ppu.c` does not implement OBJ
  semi-transparency (objMode 1), so blended sprites draw opaque. A change to
  `ppu.c` must stay byte-exact against the JavaScript reference in `web/`, so
  both must change.
- **Lamp glow alternates frames.** With fast-forward, or a dropped frame, it can
  flicker. Test on a console.
- The `gbagfx` change affects every build's assets. The `.pla` files are new
  inputs, and `tools/generate_wasm_assets.py` must pass them through.
- It rewrites the palette fade code that the whole game uses. Guard all of it,
  and test fades in battle, menus and cutscenes.
- The weather blend coefficients (`Weather_SetBlendCoeffs(8, 12)` in `None_Init`
  and `Sunny_InitVars`) were left out of the follower port because they belong
  to this feature.

### 5. Key item wheel

**What it gives.** The player registers up to four key items. SELECT opens a
wheel to pick one, and closes it again.

**Source.** `key-item-wheel` (8 own commits). Most is in `src/item_menu.c`
(+431), with new bag graphics. Credits in the history: Pokabbie (fixes) and
Jaizu (window clear, SELECT to close).

**Size.** Medium.

**Fit.**
- **Save data:** it cuts `MAX_REMATCH_ENTRIES` from 100 to 92 and puts
  `registeredItems[4]` in the freed 8 bytes of `SaveBlock1`. Vanilla uses 78
  rematch entries, so the bytes are free, but check the achievements code and
  any port code that reads the rematch table.
- The old `registeredItem` field stays as `registeredItemCompat`, so an older
  build still finds one registered item.
- The branch is on an old pret master, so expect conflicts in the bag code.
- The bottom screen's BAG tab could show and edit the four registrations.

### 6. Nature change that keeps the PID

**What it gives.** `SetMonData(mon, MON_DATA_NATURE, ...)` changes a Pokémon's
nature. It keeps the gender, the ability, the substructure order and, where it
can, the shiny state and the Unown letter.

**Source.** `followers`, commit `e9d01b480` ("Added a non-RNG, PID-preserving
way to modify pokemon nature"), in `src/pokemon.c` and `include/pokemon.h`. It
was left out of the follower port.

**Size.** Small code. It needs a UI.

**Fit.**
- A cheat, so it belongs on EXTRA's cheat page or in the PARTY detail view (a
  NATURE button that cycles).
- The personality is half of the encryption key (the bad-egg pitfall in the
  cheatsheet). Upstream's `MON_DATA_NATURE` is after
  `MON_DATA_ENCRYPT_SEPARATOR`, so `SetBoxMonData` decrypts with the old key
  and encrypts again with the new one. Keep it after the separator, and test
  that the Pokémon is not a Bad Egg after a save and a reload.

### 7. Modern text case

**What it gives.** Text is no longer all capitals: "POKéMON" becomes
"Pokémon", "BAG" becomes "Bag". Chosen words keep their case (TM, PC, EV, OK,
BP).

**Source.** `guillotine`. Since 2025-03 the conversion runs at build time in
`tools/preproc`, with changes to the string data files (species, items, moves,
easy chat).

**Size.** Medium.

**Fit.**
- Build time means no switch. The runtime version (commits from 2024-01 to
  2025-02) could be a switch, but it is the older design.
- It changes every string in every build unless the preproc change is fenced by
  a build flag.
- The bottom screen prints game strings and its own `UiAscii` labels. Decide
  which of them follow.

### 8. Follower extras from pokeemerald-expansion

Ideas, not a direct port. Expansion's code is far from this tree.

- **Copy a wild Pokémon by move or ability.** Expansion lets a follower that
  knows Transform, or has Illusion or Imposter, copy wild Pokémon
  (`OW_FOLLOWERS_COPY_WILD_PKMN`). This port checks for Mew and Ditto by
  species. Gen 3 has no Illusion or Imposter, so this means Transform only.
- **NPC Pokémon by species.** `OBJ_EVENT_GFX_SPECIES(name)` gives any NPC a
  species sprite (`OW_POKEMON_OBJECT_EVENTS`). `followers-expanded-id` has a
  backport. Vanilla NPC Pokémon (Zigzagoon, Kecleon, Azumarill and the others)
  could use the follower sprites. Map objects are in `map.json`, so change the
  graphics at runtime by map and local id, as the Day Care yard does.
- **Follower NPCs.** A trainer partner follows the player, from ghoulslash's
  "Follow Me". It needs story scripts to be useful.

## Not applicable

- **`map-previews`:** ghoulslash's dungeon preview screens. The data covers
  FireRed and LeafGreen maps only.
- **`followers-expanded-id` graphics ids and compression:** 16-bit graphics ids
  and compressed overworld graphics. No visible gain on the 3DS, and the id
  change reaches the map data and the save handling.
- **`just-lighting`, `lighting-expanded-id`, `guillotine-expansion`:** variants
  of the branches above.
- **`battle_engine`, `romhack_pokemon_expansion`:** old experiments that are far
  from pret.
