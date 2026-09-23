# 3DS port roadmap

Outstanding work on the port, and the reasoning behind the decisions already
taken. Companion to the root `README-TECHNICAL.md`, which describes how the port is
built and why it is structured the way it is.

## Status

Checked against `d482f2a` on 2026-09-18.

| Part | State |
|---|---|
| A: Top-screen sharpness | **Done.** A runtime SCREEN SIZE setting, not a build flag. |
| B: Bottom-screen UI, stages 3-5 | **Done.** BAG, MAP (with fly-from-map) and DEX. |
| D: Gameplay tweaks | **Done.** |
| Save durability | **Done.** |
| The busy-wait audit | **Done.** |
| C: Local wireless | **Pairs and links on hardware; a trade is waiting on a console test.** The transport now delivers each command once, the handshake waits for the host's confirm, and `GetMultiplayerId()` answers correctly. On the `local-wireless` branch. |
| E: Achievements | **Built in.** Two items are left. |

The done parts stay in this file because their facts are recorded nowhere else.

## Open items

The large pieces:
- **[SECOND_SCREEN_PLAN.md](SECOND_SCREEN_PLAN.md):** a view stack, a widget
  layer, text clipping and a catalogue of new views. Not started.
- **[UI_SKIN_PLAN.md](UI_SKIN_PLAN.md):** a bottom screen drawn from image
  assets. Not started.
- **[UPSTREAM_FEATURES_PLAN.md](UPSTREAM_FEATURES_PLAN.md):** features that can
  come from aarant's pokeemerald fork and pokeemerald-expansion: shiny-aware
  icons, day and night lighting, a key item wheel and others, with the port
  rules from the follower work. Item 1, the follower options, is done.
- **Part C, the Cable Club over local wireless.** The `local-wireless` branch
  now carries main. C.3 is decided: LINK is page 5 of EXTRA. Pairing and the
  transport both run on hardware. Three faults that stopped a trade are fixed
  and waiting on a console test: the transport lost and repeated commands, the
  link went live at pairing time instead of at the host's confirm, and
  `GetMultiplayerId()` answered 0 on both consoles. The next run must show
  `lost 0` on every period line, `local` and `sio` agreeing in the `link ids`
  line, and `link handshake done` arriving when the host presses A rather than
  when the consoles pair.
- **Part E:** the trade-dependent achievements, and a RetroAchievements
  provider.
- **[NULL_CRASHES_PLAN.md](NULL_CRASHES_PLAN.md):** the crash class that keeps
  stopping the console, which is a read through a NULL pointer that a GBA
  ignores. 15 are fixed. The detector cannot yet see the callback-tail shape
  that caused 5 of them, the wasm sweep that would enumerate the rest has not
  run, and nothing behind `#if PLATFORM_3DS` is swept at all.

The small ones:
- **A bar that starts to slide under an overlay stalls** on the single-core
  path (cheatsheet section 5, "Single core"). The second-core path repaints for
  it. One full repaint when the slide ends would fix the single-core path, for
  the cost of that repaint.
- **The Old 3DS has no second core, and that is settled.** Two attempts:
  `APT_SetAppCpuTimeLimit` returns `0xD8E05BF4` (PM, permanent, **not
  implemented**), and setting the exheader `AffinityMask` to 3 changed nothing.
  The firmware does not give an application a share of the system core. The
  port is single core there for good, which is why the display divider
  (`3ds/host/main.c`) exists. `ppu_try_core()` now says which of the two
  refusals happened, if it is ever worth another look. ZL and ZR cannot be
  tested there.
- **Save flushes stall a link: fixed.** `CTR_SAVE_QUIET_MS`
  (`3ds/host/save.c`) is a second now, above the gap between a link save's
  sectors, so a trade's writes collapse into one instead of rewriting the whole
  128 KB about eight times at 130 ms each. `CtrSaveCommit` still forces an
  immediate write at the real save points, so nothing waits on the debounce for
  durability. A console log before the change caught the eight stalls on both
  consoles.
- **MAP extras not built:** the city zoom, the indoor icon blink and the
  fly-destination icons (Part B, stage 4).
- **Bottom screen on core 0: done, and it was necessary after all.** An Old 3DS
  was missing 190 to 206 VBlanks in every 600 in game. It is 1 to 9 now. What
  landed: the UI paints straight into the 512-wide linear stage, so the 2.0 ms
  copy is gone; the upload carries only the dirty rows; the title blink and
  five tabs of six stopped taking a full repaint they never read; and the
  window frame, tile blit and glyph loops got cheaper. See the cheatsheet,
  sections 2 and 7.
- **What is left of that list.** Neither is needed at the measured numbers:
  - Start the bottom transfer asynchronously, and wait for it before
    `C3D_FrameBegin`.
  - Trim `ppu.snap`, the one part of the rasterizer's cost still on core 0
    (about 0.7 ms on the console). It does not exist on an Old 3DS, which
    points the rasterizer at live memory instead.
- **The Old 3DS title screen still stutters, and it is the rasteriser.** The
  bottom screen is no longer involved: `paint.blank` is a few hundred
  microseconds and the blink repaints its own rect. The log shows
  `slow scene 16414 us ... affobj=11` and `prof ppu ... worst 23194 us` with
  387 of 600 frames late, against 11.4 ms and 1 to 9 late in game. Eleven
  affine sprites on one scene is what costs it: `ppu.c`'s affine sprite loop is
  the only hot path in that file with no span batching, no transparent-row skip
  and a per-pixel `objTileOffset`. That is the next measurement, and
  `PPU_PROFILE` in `rp2350/ppu.c` already has the per-pass timers written; the
  3DS build just does not define it.

---

# Part A: Top-screen sharpness (done)

**How it landed.** A.2 shipped as a runtime setting, SCREEN SIZE on EXTRA page
1, not as the `CTR_TOP_SCALE` build flag below. It has three modes
(`3ds/bridge.h:182`, `3ds/host/video.c:38`):
- 1x: 240x160, pixel-perfect, wide borders;
- 1.5x: 360x240, the default, 20px bars;
- FILL: 400x240, no borders, 11% wider.

The 800-wide mode 3 was not built, and FILL took its place, so nothing calls
`gfxSetWide`. The reasoning below, and the A.1 emulator settings, still hold.

## Context

The top screen looks soft, and the obvious question is whether the GBA image can
be rendered at 2x for sharpness.

Two findings shape everything below.

**1. 2x is impossible.** `240x160 * 2 = 480x320`; the top screen is `400x240`.
It exceeds *both* dimensions, so it cannot be done at any setting: not by
filling less of the screen, and not in wide mode, which adds horizontal columns
only and leaves the 240px height unchanged. The current 1.5x is already the
largest fit: `240/160 = 1.5` exactly, filling the panel height with a 20px
pillarbox each side.

**2. Most of the blur is added after the port is finished.** Azahar selects its
present sampler with `present_samplers[!filter_mode]`, where index 0 is
`vk::Filter::eLinear`. With `filter_mode=true` (the default) the finished
400x240 frame is bilinear-upscaled to a desktop-sized window;
`resolution_factor=1` and `use_integer_scaling=false` compound it. A
pixel-perfect frame would still arrive soft through that filter, so **Part A.1
must be done first or A.2 cannot be judged.**

Because filling the panel is not a priority, pixel-perfect **1x** is viable,
the only genuinely artifact-free option, since one GBA pixel maps to exactly one
3DS pixel with no resampling. Vertical 1.5x is otherwise a fixed 2:3 ratio and
cannot be improved.

## A.1: Emulator settings (no code; do this first)

Quit Azahar first, because it rewrites its config on exit, then in
`~/Library/Application Support/Azahar/config/qt-config.ini`, setting each key's
matching `\default=false` alongside it:

- `filter_mode=false`: nearest present instead of bilinear. Biggest single win.
- `resolution_factor=4`: rasterises the quad at 4x internally so nearest-sampled
  GBA pixels stay crisp when enlarged. Valid range 0-18.
- `use_integer_scaling=true`: avoids fractional window rescaling.

Judge the result before building anything. This may resolve the complaint
entirely, in which case A.2 is optional polish.

## A.2: Selectable top-screen scale

Only `3ds/host/video.c` and `3ds/Makefile` change. This is host-side, so unlike
`CTR_BOOT_DIAG` it does **not** need mirroring into `3ds/build_objs.sh`: no
game-side translation unit is affected.

`3ds/Makefile`: `CTR_TOP_SCALE ?= 2`, passed as `-DCTR_TOP_SCALE=$(CTR_TOP_SCALE)`
next to the existing `CTR_BOOT_DIAG` define.

`3ds/host/video.c`: replace the single `GBA_SCALE` with a mode block. The axes
now differ, so X and Y scales must be separate.

| `CTR_TOP_SCALE` | Mode | Target | Scale X / Y | Draw at | Result |
|---|---|---|---|---|---|
| 1 | pixel-perfect | 400x240 | 1.0 / 1.0 | 80, 40 | 240x160, no resampling at all |
| 2 (default) | current | 400x240 | 1.5 / 1.5 | 20, 0 | 360x240, fills height |
| 3 | wide | 800x240 | 3.0 / 1.5 | 40, 0 | 720 of 800; same physical size as 1.5x, exact 3x horizontally |

Derive the offsets rather than hard-coding them:
`GBA_DRAW_X = (TOP_SCREEN_W - CTR_GBA_WIDTH * GBA_SCALE_X) / 2.0f`, likewise for
Y against 240.

For mode 3 only, call `gfxSetWide(true)` immediately after `gfxInitDefault()`
and **before** `C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT)`. Ordering is
load-bearing: citro2d picks `GSP_SCREEN_HEIGHT_TOP_2X` (800) over
`GSP_SCREEN_HEIGHT_TOP` (400) by testing `gfxIsWide()` at creation time. No
reallocation concern, since `gfxSetScreenFormat` already sizes the top framebuffer
for 800 unconditionally.

Pass both scales to the existing `C2D_DrawImageAt(...)`, which already accepts
`scaleX` and `scaleY` separately. Report the active mode once via `CtrLog()`
(always compiled) so a build is identifiable from its log.

Nothing else moves: the PPU still renders 240x160, the 256x256 staging texture
and `GX_TRANSFER` path are untouched, the bottom screen is untouched, and touch
is bottom-screen only so its coordinate mapping is unaffected.

**Watch for a half-texel artifact in mode 1.** At exactly 1:1, nearest sampling
is unforgiving: if texel centres do not line up with pixel centres, a column or
row can duplicate or drop. Draw positions are integers, which should suffice,
but if 1x shows a doubled edge the fix is a half-texel inset in `init_subtex`'s
`right`/`bottom` fractions.

## A: Verification

```sh
for s in 1 2 3; do make -C 3ds clean && make -C 3ds CTR_TOP_SCALE=$s; done
```

- **Mode 2 is the regression test**: must be pixel-identical to today: 360x240
  centred, 20px bars. Any difference means the one-scale-to-two refactor is wrong.
- **Mode 1**: 240x160 centred with wide borders, and *perfectly* sharp: no
  uneven row or column widths anywhere. This is the reference for "correct"; if
  it is not crisp, the remaining blur is Azahar's, not the port's.
- **Mode 3**: same physical size as mode 2, but vertical edges on sprites and
  text noticeably cleaner. Check fill cost: wide mode doubles top-screen fill,
  and since `C3D_FrameEnd` blocks on VBlank a regression shows as sluggishness.
- **Unaffected each time**: bottom screen, tab switching, party grid, touch.

## A: Risks

- **Judging A.2 before A.1 will mislead.** With `filter_mode=true` every mode
  looks soft, including pixel-perfect 1x.
- **Azahar's wide-mode support is unverified.** citro2d and libctru clearly
  support it, but it was not possible to statically confirm the emulator renders
  an 800-wide top framebuffer. Mode 3 is opt-in partly for this reason.
- **Wide mode's fill-rate cost** matters far more on real hardware than in an
  emulator; measure before considering it as a default.
- **Vertical stays 2:3 in modes 2 and 3.** If unevenness still bothers you after
  A.1, mode 1 is the only complete fix, and it is small by construction.

---

# Part B: Bottom-screen UI (done)

All stages are implemented. Stages 0-2: `ui_draw.c` (4bpp blitter, BGR555→RGB565, Emerald
nine-slice window frames, mon icons), `ui_text.c` (Emerald's own font via
`DecompressGlyphTile`), the tabbed shell in `bottom_screen.c`, and the 2x3 party
grid with detail view in `tab_party.c`. Stages 3-5, below: `tab_bag.c`,
`tab_map.c` and `tab_dex.c`. The text below is the record of how they were
built. For the code as it is now, read `SECOND_SCREEN_CHEATSHEET.md`.

## Reuse first

The game's *data layer* is reusable and should be called, not reimplemented.
Everything under `3ds/ui/` is game-side, so these are ordinary symbols.

| Need | Reuse |
|---|---|
| Bag contents | `BagGetItemIdByPocketPosition()`, `BagGetQuantityByPocketPosition()`, `IsBagPocketNonEmpty()`, `gBagPockets[]` (`include/item.h`) |
| Item metadata | `GetItemName()`, `GetItemDescription()`, `GetItemPocket()`, `CopyItemNameHandlePlural()` |
| Applying an item | `PokemonUseItemEffects()` (`include/pokemon.h:472`), `RemoveBagItem()` (`include/item.h:48`) |
| Map names | `GetMapNameGeneric()` (`include/region_map.h:107`), `GetLandmarkName()` (`include/landmark.h:4`), `gRegionMapEntries[]` |
| Dex | `GetSetPokedexFlag()`, `GetNationalPokedexCount()`, `GetHoennPokedexCount()`, `GetPokedexHeightWeight()`, `struct PokedexEntry` (`include/pokedex.h`) |

**The limit.** The *presentation* layer is not reusable. `GetItemFieldFunc()`,
`CB2_OpenPokedex()`, `CB2_OpenFlyMap()` and `InitRegionMap()` are coupled to BG
layers, window tilemaps, the task system and callback chains; they render to the
top screen through hardware the bottom screen does not own, and expect to be
entered from their own menu's context. Calling one will appear to work and then
fight the top screen for BG layers and tasks.

## Stage 3: BAG and item use (done)

Verified in Azahar: a Potion used from the touch screen heals the Pokemon the
player picks, in the field and in battle, and the safety gate holds.

The only stage that mutates game state, so the gate is the design.

- List the current pocket with `BagGetItemIdByPocketPosition()` and
  `BagGetQuantityByPocketPosition()`; names via `GetItemName()`, flavour text via
  `GetItemDescription()`. Pocket switcher across the top.
- Flow: BAG → tap item → tap USE → a target picker opens over the tab → tap the
  Pokemon. Items that target nobody (balls, X items, the escape items in battle)
  skip the picker.
- **Which items the picker is offered for** is decided by the game's own tables,
  not a list of ids: `GetItemBattleUsage()` in battle, `GetItemEffectType()`
  in the field. Two field classes are deliberately refused rather than applied,
  and both are correctness rather than tidiness: `ITEM_EFFECT_EVO_STONE`, because
  `PokemonUseItemEffects()` calls `BeginEvolutionScene()` and would seize
  `gMain.callback2` mid-frame, and `ITEM_EFFECT_RAISE_LEVEL`, because the
  new-move check lives in `ItemUseCB_RareCandy()` and not in the table effect.
  Items needing a move chosen as well as a mon (Ether, PP Up, PP Max) are refused
  for the same reason: this UI has no move list.
- **Safety gate**, re-checked immediately before mutation:
  - `gMain.callback2 == CB2_Overworld` (`include/overworld.h:131`)
  - `!ArePlayerFieldControlsLocked()` (`include/script.h:36`)
  - `!ScriptContext_IsEnabled()` (`include/script.h:38`)
  - `!gMain.inBattle` (`include/main.h:39`, a 1-bit field; `VBlankIntr()`
    itself uses it)
- Apply `PokemonUseItemEffects(mon, item, partyIndex, 0, FALSE)`. Note the
  **inverted return**: `FALSE` means the item *did* have an effect. Only then
  `RemoveBagItem(item, 1)`.
- If the gate fails, say so on screen rather than silently ignoring the tap.
- **In battle** the same effect is applied through
  `Ctr3dsQueueBattleItem()` (`src/battle_controller_player.c`), which then emits
  `B_ACTION_USE_ITEM` so the item costs a turn exactly as the d-pad route does.
  Applying it there and not in the battle script is not a shortcut: the engine
  never asks who an item targets, and `BattleScript_PlayerUsesItem` is a no-op
  precisely because the vanilla party menu has already done the healing by the
  time it runs. An item that would have no effect returns
  `CTR3DS_ITEM_NO_EFFECT` and does **not** spend the turn, matching the vanilla
  bag bouncing back to its item list.

Why this point is safe: `CtrBottomUpdate` runs from `Rp2350PresentFrame()`, at
the end of `WasmRunFrame()` *after* `VBlankIntr()`, so the frame's callbacks have
finished and the next has not started.

Item use changes the save; the deferred flush in `3ds/host/save.c` covers it.
Ensure the party hash changes so the grid repaints.

## Stage 4: MAP (done)

Emerald's own region map art, drawn at 1:1, with a marker where the player is and
tap-a-place-for-its-name. Read-only throughout: unlike BAG, nothing here writes
game state.

Three properties of the art are not obvious from the asset files, and each one
produces a plausible-looking wrong picture if you guess it. They cost real time to
establish, so they are recorded here:

- **The background is 8bpp**, not 4bpp (`-num_tiles 233`, 14,912 bytes
  decompressed). `UiBlit4bppTile` cannot draw it; `UiBlit8bppTile` was added
  alongside it for this.
- **Its tilemap is an AFFINE one: ONE BYTE per entry, 64x64.** BG2 is set up with
  `BG_ATTR_SCREENSIZE 2` and `BG_ATTR_PALETTEMODE 1`, and affine screen size 2 is
  64x64 tiles at a byte each -- exactly the 4096 bytes `map.bin` decompresses to.
  So there are no 16-bit screen entries, no flip bits and no palette-bank field:
  the tile id is simply the byte. Reading it as a normal text-mode tilemap yields
  noise.
- **Tile bytes are ABSOLUTE palette indices in 112..143**, because the game loads
  the map's 32 colours at `BG_PLTT_ID(7)`. An 8bpp BG has no palette-bank field to
  add an offset, so the index is baked into the pixel. Hence a 256-entry palette
  with only that slice filled.

The drawn artwork is **31 x 19 tiles = 248 x 152 px** (larger than the 28x15
MAPSEC grid at offset 1,2 -- the northern coast, Dewford's islands and the eastern
edge all sit outside it). That centres in the 320x192 content area with 36px each
side and leaves exactly 40px for the caption. 2x would be 496x304 and fits in
neither dimension, so 1x is the only scale, and it is also the pixel-perfect one.

- Assets and the player's position come from the `PLATFORM_3DS` block in
  `src/region_map.c`; they are file-static there. Same pattern as
  `Ctr3dsLiveWindowFrameType()` in `src/option_menu.c`.
- **The player's position runs the game's own `InitMapBasedOnPlayerLocation()`
  against a scratch `struct RegionMap`**, rather than being reimplemented. That
  function is 150 lines of map-type dispatch (towns, routes, underwater, caves via
  the escape warp, secret bases via the dynamic warp, indoor maps whose mapsec is
  `MAPSEC_DYNAMIC`) plus multi-tile band arithmetic and hardcoded fixups for Routes
  114, 121 and 126, the Marine Cave and the SS Tidal. A copy would be subtly wrong
  on day one and would then drift. It is safe to call because it is *pure
  computation*: it reads `gSaveBlock1Ptr` and `gMapHeader` and writes only
  `mapSecId`, `playerIsInCave` and `cursorPosX/Y`. `sRegionMap` is saved and
  restored around it, because the top screen may have a real map open.
- Names via `GetMapNameGeneric()`, landmarks via `GetLandmarkName()` keyed on the
  `posWithinMapSec` the same accessor returns. Both are already game-encoded.
- Tapping uses the public `GetRegionMapSecIdAt()`, which takes absolute map-tile
  coordinates -- which is what dividing a touch position by 8 gives directly.

**`UiMapStateKey()` is not optional.** Nothing else in `UiStateHash` tracks where
the player is, so without it the map goes stale while they walk. It is naturally
coarse: the cursor position only moves when the player crosses a band boundary
within a mapsec, so walking one stretch of route costs no repaints.

Flying was first left out here, because it would have to drive the field warp
flow from the per-frame hook, which is the thing "The limit" above warns about.
It shipped later (`04fb65b`): `tab_map.c` checks the game's own conditions and
then takes the same steps as `CB_ExitFlyMap`. Still not built: the indoor icon
blink (one cosmetic effect for a new per-frame tick entry point),
fly-destination icons and the city zoom.

## Stage 5: DEX (done)

Shipped in `9026ab3`. Seen/caught counts from `GetHoennPokedexCount()` / `GetNationalPokedexCount()`,
a scrollable list filtered by `GetSetPokedexFlag()`, and per-entry category,
height, weight and description from `struct PokedexEntry`.

## B: Verification

Build with diagnostics on so failures are visible:

```sh
CTR_BOOT_DIAG=1 3ds/build_objs.sh && make -C 3ds CTR_BOOT_DIAG=1
```

- **Stage 3 is the real test.** Damage a Pokémon, use a Potion from the touch
  screen, then open Emerald's own party menu: HP must match and the bag count
  must have decremented. Repeat in a battle: the top-screen healthbox must
  update and the turn must then pass. Verify the gate by tapping USE mid-cutscene
  and while the opponent is acting, and confirm nothing happens.
- **Stage 4**: open MAP and compare it against the PokeNav's own map on the top
  screen -- they must be the same image. Then walk between areas, including
  indoors and caves, and confirm the marker and the name track. Walk the length of
  a tall mapsec (Route 110) and confirm the marker moves down it rather than
  sticking at one end; that is what running the game's own position function
  buys.
- **Regression each stage**: top screen unaffected, and
  `~/Library/Application Support/Azahar/sdmc/3ds/emerald3ds/pokeemerald.sav`
  still updates after an in-game save.

## B: Risks

- **Item use is the only state-mutating path.** Get the gate right before
  anything else in Stage 3, because a wrong gate corrupts saves rather than crashing.
- **Text encoding.** Strings are game-encoded (`charmap.txt`), `EOS`-terminated,
  not ASCII. Passing a C string literal to `UiText` renders garbage; use
  `UiAscii()` for literals.
- **Redraw cost.** A repaint is 76,800 pixels of software fill; keep it
  change-driven via the party hash and `UiMarkDirty()`.
- **Two-worlds rule.** `3ds/ui/` must never include `<3ds.h>`. A violation
  surfaces as a confusing `u8`/`u16` clash. Note `3ds/ui/` headers are prefixed
  `ui_` precisely because `text.h` would otherwise shadow the game's
  `include/text.h`.

---

# Part D: Gameplay tweaks (done)

EXTRA page 2: EXP All, a badge-based level cap, a species randomiser, and a
persistent bag sort. All off by default.

These are the port's first **cheats**, and that is a real departure. `bridge.h`
frames the show-all-tabs override as "a testing aid, not a cheat" precisely
because everything it reveals is read-only; `ui_shell.h` said of EXTRA that
"nothing here touches game state". Page 2 breaks that, so it is kept behind a
page turn rather than mixed into page 1, and both comments were rewritten rather
than left to quietly go stale. Page 1 remains host-side only.

## Shape

All the behaviour is in **`3ds/tweaks.c`** (game-side; added to the file list in
`build_objs.sh`). The hooks inside `src/` are each one extra clause on a
condition the game already evaluates, or one extra statement, always
`#if PLATFORM_3DS` fenced. The toggles themselves are host-side in
`3ds/host/main.c` and persisted by `settings.c`, read through `bridge.h`, with
`src/siirtc.c` as the precedent for game code reading a host setting.

`settings.bin` rather than the save block, because a setting the player taps
should stick even if the game is never saved afterwards. The version went to 4;
a v3 file is **migrated**, not discarded, since the v4 fields are appended and
every v3 offset is unchanged. Discarding would have silently reset the player's
top scale and turbo binds as the price of an unrelated feature.

Version 11 makes the tweaks per save. Before v11, a randomizer that the player
set for one save was also on for every other save. Now each save has a record,
keyed on its trainer ID. The record holds EXP All, the level cap, the
randomizer, the bag sort, the phone-call switch and the last ball. The first
save that a v11 build loads takes the values of the older file. Every other save
starts at the defaults.

## Facts worth recording

Four things that cost real time and are not recoverable by reading the code:

- **A level cap must gate exp, never the level field.** `CalculateMonStats`
  (`src/pokemon.c:2829`) recomputes `MON_DATA_LEVEL` from `MON_DATA_EXP` every
  time it runs, and it runs from `BoxMonToMon`, evolution, PC deposit and
  withdraw, and item use. Anything that clamped the level would be silently
  undone by the next unrelated call. Exp is the source of truth, so the gates
  are at `Cmd_getexp`, Rare Candy, and the day care.
- **`CreateBoxMon` is the deepest randomiser hook and is the wrong one.** The
  Battle Pike and Battle Pyramid do not store a species in the species field of
  their wild tables; they store a 1-based **index** into a second table, create
  the mon with it, then read it back with `GetMonData(...) - 1`
  (`src/battle_pike.c:1113`, `src/battle_pyramid.c:1360`). A random value there
  is an out-of-bounds read. `CreateMonWithGenderNatureLetter` and `CreateMaleMon`
  also loop on the caller's species gender ratio, which never terminates if the
  result is genderless. Targeted hooks avoid both.
- **Valid species are 1..411 with 252..276 excluded.** Those 25 slots are
  `SPECIES_OLD_UNOWN_B..Z`, placeholders with real `gSpeciesInfo` entries but
  named "?". `NUM_SPECIES` is 412, which is `SPECIES_EGG`, a sentinel: both
  `gSpeciesInfo` and `gSpeciesNames` end at 411, so indexing by it overreads.
  That leaves 386 usable, and the index arithmetic in `SpeciesFromIndex` skips
  the hole without a table or a rejection loop.
- **The randomiser needs no stored seed.** It is derived from
  `gSaveBlock2Ptr->playerTrainerId`, which makes the mapping stable for one
  playthrough, different between playthroughs, and unchanged by toggling the
  option off and back on. **That "different between playthroughs" was untrue
  for the first year of this feature**, and nothing about the randomiser was
  wrong: the ID's lower half comes from `REG_TM1CNT_L` in
  `SeedRngAndSetTrainerId` (`src/main.c`), the GBA's Timer 1, started at the
  naming screen so that it measures how long the player took to type a name.
  This port has no timers. I/O is a zeroed buffer (`3ds/gba_mem.c`) nothing
  writes, so that read was always 0, `SeedRng(0)` made the upper half
  deterministic too, and every save file on every console got the same ID and
  the same randomised world. It now reads `CtrTimeNowMs()` instead, which is
  the same kind of source measured at the same moment. Anything inheriting
  entropy from GBA hardware needs this check; the register reads fine and
  answers 0 forever.
- **A rejection loop with a give-up is biased toward whatever it gives up on.**
  The HM-preserving guard drew 16 candidates and returned the ORIGINAL species
  if none covered the mask, which reads as safe and is not: the pool a species
  draws from is the number of mons learning everything it can, and Tentacool's
  is 12 of 386, which 16 draws miss 60% of the time. The species with the
  narrowest pools were therefore the least likely to be randomised at all --
  measured across the 143 species in the wild tables, 13.6% came back
  unchanged. Exactly one species in the game genuinely has no alternative. The
  fix is to stop drawing and select: count the pool, take the nth, which is
  uniform over it and always answers. Scanning forward from the hash instead
  is one pass rather than two but clumps badly, handing one of Tentacool's 12
  a 60% share.
- **Two hooks on one path will map twice, and the starter is that path.** The
  mapping is not idempotent, so `map(map(x))` is a third species. The starter
  reaches `ScriptGiveMon` from `CB2_GiveStarter` carrying a species
  `GetStarterPokemon` has already mapped, so hooking `ScriptGiveMon` as well
  would hand the player a different mon from the one whose sprite and cry they
  just chose. The gift hook therefore sits on `ScrCmd_givemon`, the only other
  caller, and `ScriptGiveMon` itself maps nothing. Any future hook needs the
  same check: enumerate the callers first.

## The softlock guard

Two separate things, and only one of them needed code.

Items are **never randomised**, so key items, HMs and badges are safe by
construction rather than by guard.

Field-move coverage is the real vector: Surf, Waterfall and Dive gate
progression outright. So the mapping is **HM-preserving**. It computes the
original species' mask over the seven progression HMs (Fly is excluded as a
convenience, not a requirement), then rehashes up to 16 times until a candidate
covers it, falling back to the original species if none does. The guarantee is
easy to state: wherever the original gave you a mon that could learn a field HM,
so does the randomiser. `CanSpeciesLearnTMHM` already existed for the lookup,
indexed by `itemId - ITEM_TM01`.

## Deliberately not hooked

Eggs and breeding (species comes from the parents), in-game trades (the trade
requires handing over a specific species), Wally's tutorial Ralts and loaned
Zigzagoon (the tutorial script depends on them), and all Frontier and Tower
parties. Event scripts that buffer a hardcoded species name before `givemon`
will still print the original name; that is a cosmetic data-side mismatch in
`data/scripts/*.inc` and is not worth chasing.

## Notes

- The bag sort rewrites the real pocket arrays, so the in-game bag and the BAG
  tab agree and the order sticks in the save. This is benign precedent-wise:
  the game already re-sorts the TM and berry pockets on every bag open. The
  bottom-screen entry point (`Ctr3dsSortBagNow`) carries the same overworld
  gate the BAG tab's item use does, because reordering a pocket while the
  in-game bag is open would slide an item out from under its cursor.
- **EXTRA now needs a state key.** Page 2's "cap NN" readout moves when a badge
  is earned, which happens nowhere near the tab. Everything else there changes
  only through the touch handler, which marks dirty itself, but the cap alone
  is enough: without `UiExtraStateKey()` in `top[4]` the readout sits stale
  until the tab is re-entered.
- Paging cost no vertical space. Page 1 already filled its 176px interior
  exactly, so the pager sits on the row-1 label line at y=8..24, abutting the
  row-1 buttons at y=25, right-aligned to the interior edge at x=311.

---

# Save durability (done)

Symptom that led here: saving once, closing the game and reopening loaded the
*previous* save, and saving twice worked around it.

That is the two-slot save format behaving correctly, not failing. Emerald keeps
an active slot and a backup; `GetSaveValidStatus` (`src/save.c:519`) takes
whichever has the higher counter and passes its checksums. Loading the backup is
the right answer when the active slot is not on the card. The two-save
workaround follows: saves alternate slots, so a second one gives the file
another chance to be written before the process dies.

**The fact worth keeping: the exit path does not run when the emulator window is
closed.** `CtrSaveFlush(1)` in `main()` is reached only through the `longjmp`
that `aptMainLoop()` returning false triggers. Closing the Azahar window kills
the process outright, so that never happens, and anything still sitting behind
the write-coalescing debounce is lost. A debounce is a guess at when a save
ended; the game knows exactly.

So saves now commit on the event. `CtrSaveCommit()` (`3ds/host/save.c`) forces
the file write, and `src/save.c` calls it the moment a save completes, at three
sites: `TrySavingData` (which covers SAVE_NORMAL, SAVE_LINK, SAVE_HALL_OF_FAME
and SAVE_OVERWRITE_DIFFERENT_FILE), `TryWriteSpecialSaveSector`, and the
signature step of `Task_LinkFullSave`. The last two do not pass through
`TrySavingData`. The debounce stays as a backstop and is now 100 ms.

Two smaller things fixed alongside:

- `Rp2350SaveSync()` stays *deferred* on purpose. `VerifyFlashSector` calls it
  after every sector, so forcing there would be fourteen 128 KB writes per save.
- The file swap moves the old save aside instead of deleting it. FAT will not
  rename onto an existing name, so the old file has to move, but moving rather
  than deleting means a complete save is on the card at every instant: a failed
  rename puts the original back, and dying between the two renames leaves a
  `.bak` that `CtrSaveLoad` recovers at boot. `fclose` is also checked now,
  since on 3DS newlib that is the call that reaches the FS service.

Note the burst size here is small, which is why committing per save is
affordable: on real hardware a sector is ~4080 single-byte programs, but
`ProgramFlashSector_MX` takes its whole-sector branch on this port, so a full
save is 28 hook calls.

---

# Part C: Local wireless (Cable Club over UDS)

**Status: merged with main, not yet built.** The work started as one commit,
`feb3472` ("link: Cable Club over 3DS local wireless (UDS)"), on the
`local-wireless` branch. That branch has since merged main, which resolved the
conflicts in `3ds/Makefile`, `3ds/bridge.h`, `3ds/host/main.c`,
`3ds/ui/bottom_screen.c` and `3ds/ui/ui_shell.h`, and settled C.3.

`src/link.c` and `include/link.h` merged with no conflict at all, so C.2 is
exactly as written below. Nothing has been built or run on hardware yet: the
transport is unproven and the numbers below are still estimates.

## Context

The port has no link at all. `IsWirelessAdapterConnected()` is hardcoded to
`FALSE` (`src/link.c:239`), and the cable path, while still compiled, is dead:
`SerialCB` and `Timer3Intr` are reachable only through `gIntrTable`, and this
port has no interrupts. So `gLink` never advances past its handshake and the
Cable Club counter cannot be used.

That costs trading, link battles, record mixing, the Berry Blender and link
contests, which is most of the reason to play Emerald next to someone else.

Goal: two to four 3DS consoles in the same room can use the Cable Club, over
3DS local wireless (UDS), with a Host/Join panel on the touch screen.

Scope decision: Cable Club only, not the Union Room. The RFU stack
(`link_rfu_2.c`, `link_rfu_3.c`, `librfu_*.c`, roughly 7,500 lines of NI/UNI
packet state machine) stays dead. That is consistent rather than arbitrary,
because the game already reports no wireless adapter and therefore does not
offer the Wireless Club in the first place.

## Why the cable path is the right seam

The GBA cable link is a fixed 4-player shared bus that moves **one 16-byte
command per player per frame**, and `src/link.c` already isolates that:

- `LinkMain1()` (line 1898) calls `EnqueueSendCmd()` / `DequeueRecvCmds()`,
  which only touch `gLink.sendQueue` and `gLink.recvQueue`.
- `SerialCB()` (line 2146) is the only thing that fills those queues, via
  `DoRecv()` / `DoSend()`, eight u16 at a time.

So the queues are the transport boundary. `CMD_LENGTH` is 8 and
`MAX_LINK_PLAYERS` is 4, so a full frame of link traffic is 4 x 16 = 64 bytes,
or 3.8 KB/s. `UDS_DATAFRAME_MAXSIZE` is 0x5C6, so one command fits in a single
frame with room to spare.

Everything above the queues stays untouched: `LinkMain2`, the block-transfer
layer, `cable_club.c`, `trade.c`, `battle_controller_link_*.c`,
`contest_link.c`.

## Feasibility, already checked

- **Service access is granted.** `3ds/emerald3ds.rsf` lists `nwm::UDS` under
  `ServiceAccessControl` and `nwm` under `Dependency`. It arrived with the
  homebrew template rather than by design, but it means no packaging change.
- **The emulator supports it.** Azahar inherits Citra's UDS implementation, and
  every call this needs is fully implemented rather than stubbed:
  `BeginHostingNetwork`, `ConnectToNetwork`, `Bind`, `SendTo`, `PullPacket`,
  `GetConnectionStatus`, `GetNodeInformation`, `RecvBeaconBroadcastData` (which
  backs `udsScanBeacons`) and `SetApplicationData`. Two Azahar instances in one
  multiplayer room can test this without a second console.

## C.1: Transport, host side (`3ds/host/link.c`, new)

libctru UDS, per `<3ds/services/uds.h>`:

- `udsInit(0x3000, username)`, then `udsGenerateDefaultNetworkStruct(&net,
  WLANCOMM_ID, 0, 4)` with a private `wlancommID` (`0x454D3344`, "EM3D") so only
  this port's builds see each other, capped at `MAX_LINK_PLAYERS`.
- Host: `udsCreateNetwork(...)`, plus `udsSetApplicationData` carrying the host's
  trainer name so the join list can show it.
- Client: `udsScanBeacons(...)` into a 0x4000 buffer, then
  `udsConnectNetwork(..., UDSCONTYPE_Client, ...)`.
- Per frame: `udsSendTo(UDS_BROADCAST_NETWORKNODEID, channel,
  UDS_SENDFLAG_Default, cmd, 16)`, drained with `udsPullPacket()`.
- `udsGetConnectionStatus()` supplies `total_nodes` and `cur_NetworkNodeID`.

**Identity mapping.** UDS node IDs are 1-based with the host at 1; the GBA's are
0-based with the master at 0. So `localId = NetworkNodeID - 1` and
`isMaster = (NetworkNodeID == 1) ? LINK_MASTER : LINK_SLAVE`.

**Lockstep is the hard part.** A cable is synchronous: every console's frame is
gated on the master's transfer, so they cannot drift. UDS is not. The transport
therefore runs a **one-frame jitter buffer plus a bounded wait**: frame N sends
the local command and consumes frame N-1's, and if a peer's command has not
arrived, block in `udsWaitDataAvailable()` up to a deadline of roughly half a
frame before giving up. Blocking belongs host-side, where the wait primitives
exist. Missing a deadline is not fatal and must not be treated as such: it maps
onto the lag path the game already has.

## C.2: Game side (one fenced block in `src/link.c`)

Under `#if PLATFORM_3DS`, replace the SIO transport and nothing else. Declare the
bridge functions in game types in `include/link.h`, the way
`include/gba/flash_internal.h` declares `Rp2350Save*`, rather than pulling
`bridge.h` into `src/`.

- `LinkVSync()` (line 2094) becomes the pump. It already runs once per frame from
  `VBlankIntr()` (`src/main.c:452`) whenever `gWirelessCommType == 0` and
  `gLinkVSyncDisabled` is clear, which is exactly the cable case.
- Carry the SIO handshake over UDS; do not replace it with the connection
  status. `LINK_STATE_HANDSHAKE` completes when the master's `MASTER_HANDSHAKE`
  word is on the wire and `total_nodes` has held steady for a frame, which is
  `DoHandshake()`'s own barrier, and then runs through `LINK_STATE_INIT_TIMER`
  as hardware does. `InitTimer()` writes timer registers that only the e-reader
  reads, so it is inert here.

  **Do not shortcut this, and this was got wrong first time.** Declaring the
  link established as soon as two nodes appeared looked harmless, because
  `LINK_STATE_INIT_TIMER` really does exist only to start timer 3. But the
  state also says WHEN the link went live, and the Cable Club reads it: every
  cancel in the link-up chain is guarded by `IsLinkConnectionEstablished() ==
  FALSE`, so B Button: Cancel died the moment two consoles saw each other, and
  `Task_LinkupConfirmWhenReady` has no timeout to escape through. The two
  consoles also went live on different frames, so their link callbacks started
  out of step.
- Reproduce `DoSend`/`DoRecv` at whole-command granularity rather than one u16 at
  a time: pop one entry from `gLink.sendQueue`, push each peer's command into
  `gLink.recvQueue`. Keep the `receivedNothing` and `queueFull` bookkeeping,
  since the layers above read both.
- **A cable delivers each command exactly once, in order, and the layers above
  have no way to repair a transport that does not.** `LINKCMD_CONT_BLOCK`
  carries no sequence number, just 14 bytes appended at `sBlockRecv[i].pos`, so
  a dropped command leaves a block that never completes and a repeated one
  fills it with rubbish that fails its magic check. Keep a ring for each peer
  indexed by the sender's frame, consume a slot as the game takes it, and test
  `peers_ready` for the exact frame owed rather than "has reached".

  Keeping only the newest packet per peer looked sufficient and is not. A peer
  may legitimately lead by a frame, so two of its packets land between two
  pumps: the older was lost and the newer was handed up twice. A client logged
  `rx 621` over 600 frames and a host `ok=576` from `rx 568`, which is the whole
  bug in two numbers.
- **`GetMultiplayerId()` reads `SIO_MULTI_CNT->id`, not `gLink.localId`.** The
  hardware fills that field and `SerialCB` mirrors it into `gLink`; this port
  has no serial interrupt, so the pump must write the register itself or all
  147 call sites answer 0 and both sides of a trade take the player-0 branch.
- `EnableSerial`, `DisableSerial`, `StartTransfer`, `InitTimer`, `StopTimer` and
  `CheckMasterOrSlave` become UDS equivalents or no-ops. `SerialCB` and
  `Timer3Intr` keep their signatures, because `gIntrTable` still references them,
  but do nothing.
- **Failure path**: on disconnect or a missed deadline, count the miss, and set
  `gLink.lag` to `LAG_MASTER` / `LAG_SLAVE` only once a run of them passes
  `CTR_LINK_MISS_LIMIT`. That is the existing route to `LINK_STAT_ERROR_LAG_*`
  (`include/link.h:34-39`), so a genuinely dropped connection surfaces as
  Emerald's own link error screen rather than hanging mid-trade.

  **The tolerance is not optional, and this was got wrong first time.** One
  `gLink.lag` frame is fatal: `LinkMain1` turns it straight into an error bit
  and `CheckLinkErrors` (`src/link.c:1569`) shows the error and closes the link
  on the first one. The cable path never behaves that way, because its slave
  waits for more than 10 VBlanks with no serial interrupt before calling it
  lag. Reporting the first missed wireless frame ended every session within
  seconds of entering the Cable Club.

The checksum logic in `DoRecv` is a cable artefact: it validates the shared bus.
UDS frames are already checked, so leave `gLink.badChecksum` clear rather than
inventing a checksum to satisfy it.

## C.3: LINK page on the bottom screen

**Decided: LINK is page 5 of the EXTRA tab.** The branch first added LINK as a
new tab. TROPHY then became the sixth tab, and six is the practical floor for a
fingertip, so the bar was full (cheatsheet section 5, "Adding a tab"). Of the
three ways out, this is the one that does not make Part C wait for the
`SECOND_SCREEN_PLAN.md` launcher. When HOME lands, the panel becomes a tile of
it and only its three entry points move.

`3ds/ui/ui_link.c`, with `3ds/ui/ui_link.h` declaring `UiLinkPageDraw`,
`UiLinkPageTouch` and `UiLinkPageStateKey`. The name must keep the `ui_` prefix:
`3ds/build_objs.sh` globs `3ds/ui/*.c` into the same object directory as
`src/*.c`, so a `link.c` here would overwrite `src/link.o` and drop the game's
link code from the archive with no error (cheatsheet section 12).

`tab_extra.c` raises `PAGE_COUNT` to 5, or 6 with the debug menu, and dispatches
draw, touch and the state key on `PAGE_LINK`. `PGR_X(i)` is written in terms of
`PAGE_COUNT`, so the pager needed no layout change.

**The state key is not optional.** `UiStateHash` (`3ds/ui/bottom_screen.c:566`)
drives every repaint, and a peer joining or dropping changes the panel with no
touch. `UiLinkPageStateKey()` supplies bits 19-23 and 28-31 of
`UiExtraStateKey()`, and `UiExtraStateKey` calls it only on the LINK page, so no
other page polls the wireless. It runs inside `if (SaveDataLive())`, so the
panel does not repaint on its own before a save loads. The Cable Club needs a
save anyway.

The panel content, on the EXTRA window frame with rows from y 30 like every
other page:

- HOST button, SCAN button, and a list of nearby games showing the host's trainer
  name and node count from the beacon appdata.
- Once connected, the connected players and a DISCONNECT button.
- Errors shown as text rather than silently swallowed.

Doing the pairing explicitly here is also what avoids the "who hosts" race that
an automatic scan-then-host would suffer from.

## C: Files

- `3ds/host/link.c` (new), plus `HOST_SRCS` in `3ds/Makefile:55`.
- `3ds/bridge.h`: the transport seam, stdint only, in its own section at the end
  of the file.
- `src/link.c`: one `#if PLATFORM_3DS` block over the transport functions.
- `include/link.h`: bridge declarations in game types.
- `3ds/ui/ui_link.c` and `3ds/ui/ui_link.h` (new), plus `3ds/ui/tab_extra.c`.
- No change to `3ds/ui/bottom_screen.c` or `3ds/ui/ui_shell.h`: LINK is a page,
  not a tab, so the tab bar and its enum are untouched.
- No change to `emerald3ds.rsf`.

## C: Verification

```sh
CTR_BOOT_DIAG=1 3ds/build_objs.sh && make -C 3ds CTR_BOOT_DIAG=1
```

devkitPro is not installed on the development machine, so the ARM build and the
`nm` duplicate-symbol check run in CI only. Locally: host `-Wall -Wextra` syntax
checks, and confirm the cable path is untouched in the other two configs by
preprocessing `src/link.c` with and without `-DPLATFORM_3DS=1` and diffing the
SIO symbols, the way the RTC change was checked.

**Pass compiler flags literally, never through a shell variable.** zsh does not
word-split unquoted `$var`, and that has produced false results twice in this
repo already.

Two Azahar instances in one multiplayer room, both on the same build:

- EXTRA page 5 on both. HOST on one, SCAN on the other; the host console's
  username should appear. It is the console name, not the trainer name: the
  scan reads `nodes[0].username` from the beacon, so no `udsSetApplicationData`
  is needed.
- Connect, walk both into the Cable Club, confirm the player count and that
  `IsLinkMaster()` is true on exactly one side.
- Trade a mon and confirm both saves reflect it after a reload. This is the real
  test: trading is the longest block transfer and the least forgiving of drift.
- A link battle, which tests latency rather than throughput.
- Record mixing, a four-way transfer if enough instances are available.
- Kill one instance mid-trade and confirm the other shows Emerald's own link
  error and returns to the overworld rather than hanging.

## C: Known, found while merging

Neither is merge damage. Both were in the branch as first written, and both
want a decision after the first run, not before it.

The first hardware run has since happened, New 3DS against Old 3DS. Pairing
worked and the panel reported both consoles. Entering the Cable Club then gave
Emerald's communication error within seconds, from two separate faults: the
frame counter in `Ctr3dsLinkExchange` advanced on every call, so consoles at
different frame rates drifted apart permanently, and the pump reported lag on
the first missed frame, which the game treats as fatal. Both are fixed; neither
fix is confirmed on hardware yet.

Both are now fixed, after a base 3DS log showed how bad each one is. Neither
fix is confirmed on hardware.

- **The bounded wait was a busy spin, not a sleep.** `Ctr3dsLinkExchange`
  called `udsWaitDataAvailable(&sBind, false, false)` in a loop. The third
  argument is `wait`, so `false` made it a zero-timeout poll and the loop spun
  on `svcGetSystemTick()` for up to `LINK_WAIT_US` (8 ms, about half a frame),
  burning core 0 and flooding nwm with IPC. It now waits on the bind event with
  `svcWaitSynchronization` and a real timeout, so the thread sleeps and wakes
  the moment a packet lands. The same pass cut the pump from four
  `udsGetConnectionStatus` round trips a frame to one, behind a cache that no
  caller may refresh more than once every 8 ms.
- **`Ctr3dsLinkScan()` blocked for the whole beacon sweep.** It ran in the
  bottom screen's touch handler, which is inside the frame loop, so a tap on
  SCAN stopped the game and the sound. A base 3DS log showed
  `slow bottom.update 1213 ms` and a matching `prof frame worst 1237275 us`.
  The pairing calls now run on a worker thread one priority step below the main
  thread, and the panel shows SCANNING, JOINING or WORKING while one runs. The
  rule that keeps the two apart, and keeps the per-frame path lock free: while
  `sBusy` is set the worker owns UDS and the main thread makes no UDS call at
  all.

## C: Risks

- **Lockstep drift is the thing most likely to bite.** Blocking on a peer stalls
  the whole frame, including audio, so a bad connection will crackle before it
  desyncs. The bounded wait keeps that finite; the jitter buffer keeps it rare.
- The port runs on a New 3DS XL, but UDS has never run there. On hardware it
  behaves in ways an emulator will not reproduce, particularly around wireless
  being disabled and around the sleep switch.
- Cross-play with a real GBA is impossible and must not be implied anywhere in
  the UI. This is Emerald3DS talking to Emerald3DS.
- Save corruption is conceivable if a trade is interrupted at the wrong moment.
  Test the disconnect case against a copied save first.
- **Internet play is deliberately out of scope.** The transport interface in
  `bridge.h` is shaped so a relay could replace UDS behind it, but the missing
  pieces are a matchmaking server and NAT traversal, not the game-side work.
  That is its own project.

---

## The busy-wait hazard, and the audit

This port has **no interrupts**: `VBlankIntr()` is called explicitly once per
frame from `WasmRunFrame()`. Any of the game's `while (TRUE)` init loops that
waits on state only advanced inside `VBlankIntr()` therefore spins forever,
freezing everything, second screen included, since `Rp2350PresentFrame()` is
downstream of the same loop. It leaves nothing in the log, so it has to be found
by reading.

That is what froze the party menu: `AllocPartyMenuBgGfx()` case 1 polls
`IsDma3ManagerBusyWithBgCopy()`, and the DMA3 queue is only drained by
`ProcessDma3Requests()` inside `VBlankIntr()`. Fixed by making DMA3 synchronous
under `#if WASM || RP2350` in `src/dma3_manager.c`. With no hardware to wait
on there is nothing to defer, so requests transfer immediately and never occupy
a queue slot. That covers all 189 call sites of the busy-checks, not just one.

A sweep for other instances came back clean:

| Loop | Waits on | Status |
|---|---|---|
| `party_menu.c:559` | `IsDma3ManagerBusyWithBgCopy()` directly | was the bug, fixed at source |
| `berry_tag_screen.c:201` | via `FreeTempTileDataBuffersIfPossible()` | already safe |
| `pokeblock.c:506` | via `FreeTempTileDataBuffersIfPossible()` | already safe |
| `credits.c:429` | nothing, self-advancing | safe |
| `battle_tower.c:2318/2528` | bounded RNG retry | safe |
| `VBlankIntrWait()` x2 | no-op stub, `rp2350/bios.c:215` | `ereader_helpers.c` only, unreachable |

`FreeTempTileDataBuffersIfPossible()` (`src/menu.c:1760`) already carried an
upstream `#if WASM || RP2350` inline-drain for this same problem, evidence the
lineage hit it too, but patched only the one helper it noticed. That workaround
is now redundant, but harmless, and it documents the hazard. Leave it.

---

## Debugging notes

A crash log gives only a raw PC. Resolve it against the ELF from that same CI
run (published as the `emerald3ds-elf` artifact alongside `emerald3ds.map`).

`svcOutputDebugString` output needs `Debug.Emulated:Debug` in Azahar's
`log_filter`, or the traces are discarded before reaching the log.

---

# Part E: Achievements (built in; RetroAchievements later, maybe)

The built-in set is done: 83 achievements on MAIN and POST-GAME pages (the
post-game ones hidden until the Hall of Fame), the TROPHY tab and the unlock
toast. [ACHIEVEMENTS.md](ACHIEVEMENTS.md) is the full list. What is left:

- **Trade-dependent goals.** The complete Hoenn Pokedex and anything much past
  200 in the National one need trade evolutions, so they were left out rather
  than shipped unearnable. The one exception is An Impossible Task, the
  complete National Pokedex, which is there on purpose and says so in its
  title. The rest can be added, each with the next unused id (ids are
  permanent, row order is free), once the Cable Club from Part C works.
- **A RetroAchievements provider.** The TROPHY tab and the toast read only
  through `AchActive()` (`3ds/achievements.h`), so a second provider replaces
  the built-in one without touching either. Casual (softcore) unlocks are
  accepted from unrecognised clients, and HayatoG/tmc's Minish Cap Switch port
  ships exactly this. Three things stand in the way here that tmc did not have:
  - **No ROM to hash.** tmc hashes the player's `baserom.gba`. This port needs
    no ROM, so it would have to claim the retail hash, which RA does not
    support for decomp ports.
  - **No retail memory map.** `EWRAM_DATA`/`IWRAM_DATA` are plain `.bss` here,
    and RA's Emerald set follows the save-block pointers, which move on every
    warp. It needs a retail-address translation layer that rewrites pointer
    values, built from a GBA `.map` that CI cannot produce without agbcc.
  - **Plumbing.** curl + mbedtls portlibs, a network thread with responses
    handed back to the main thread, swkbd login storing only the token, and
    text-only badges because there is no libpng.
