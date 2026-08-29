# 3DS port roadmap

Outstanding work on the port, and the reasoning behind the decisions already
taken. Companion to `3ds/README.md`, which describes how the port is built and
why it is structured the way it is.

---

# Part A: Top-screen sharpness

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

# Part B: Bottom-screen UI (Stages 3-5 outstanding)

Stages 0-2 are implemented: `ui_draw.c` (4bpp blitter, BGR555→RGB565, Emerald
nine-slice window frames, mon icons), `ui_text.c` (Emerald's own font via
`DecompressGlyphTile`), the tabbed shell in `bottom_screen.c`, and the 2x3 party
grid with detail view in `tab_party.c`. `tab_bag.c` and `tab_map.c` are
placeholders.

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

## Stage 3: BAG and item use (WORKING)

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

## Stage 4: MAP (WORKING)

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

Not built: flying (it would have to drive the field warp flow from the per-frame
hook, which is the thing "The limit" above warns about), the indoor icon blink
(one cosmetic effect for a new per-frame tick entry point), fly-destination icons
and the city zoom.

## Stage 5: DEX

Seen/caught counts from `GetHoennPokedexCount()` / `GetNationalPokedexCount()`,
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

# Part D: Gameplay tweaks (WORKING)

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

## Facts worth recording

Four things that cost real time and are not recoverable by reading the code:

- **A level cap must gate exp, never the level field.** `CalculateMonStats`
  (`src/pokemon.c:2840`) recomputes `MON_DATA_LEVEL` from `MON_DATA_EXP` every
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
  option off and back on.
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

# Part C: Local wireless (Cable Club over UDS)

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

- `LinkMain1()` (line 1896) calls `EnqueueSendCmd()` / `DequeueRecvCmds()`,
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
  multiplayer room can test this without hardware, which matters because nothing
  in this port has ever run on a real console.

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
  `VBlankIntr()` (`src/main.c:427`) whenever `gWirelessCommType == 0` and
  `gLinkVSyncDisabled` is clear, which is exactly the cable case.
- Drive `gLink.state` from UDS connection status instead of the SIO handshake:
  `LINK_STATE_HANDSHAKE` completes when `total_nodes` matches and holds steady
  for a few frames, then goes straight to `LINK_STATE_CONN_ESTABLISHED`.
  `LINK_STATE_INIT_TIMER` exists only to start timer 3, which has no meaning
  here.
- Reproduce `DoSend`/`DoRecv` at whole-command granularity rather than one u16 at
  a time: pop one entry from `gLink.sendQueue`, push each peer's command into
  `gLink.recvQueue`. Keep the `receivedNothing` and `queueFull` bookkeeping,
  since the layers above read both.
- `EnableSerial`, `DisableSerial`, `StartTransfer`, `InitTimer`, `StopTimer` and
  `CheckMasterOrSlave` become UDS equivalents or no-ops. `SerialCB` and
  `Timer3Intr` keep their signatures, because `gIntrTable` still references them,
  but do nothing.
- **Failure path**: on disconnect or a missed deadline, set `gLink.lag` to
  `LAG_MASTER` / `LAG_SLAVE`. That is the existing route to
  `LINK_STAT_ERROR_LAG_*` (`include/link.h:34-39`), so a dropped connection
  surfaces as Emerald's own link error screen and recovers, rather than hanging
  mid-trade.

The checksum logic in `DoRecv` is a cable artefact: it validates the shared bus.
UDS frames are already checked, so leave `gLink.badChecksum` clear rather than
inventing a checksum to satisfy it.

## C.3: LINK tab on the bottom screen

`UI_TAB_LINK` added to `enum UiTab` and to `sTabs[]` (`3ds/ui/bottom_screen.c:56`)
with flag `0`, always available, like BAG. The tab bar already divides by the
visible count, so a fifth tab needs no layout change.

New `3ds/ui/tab_link.c` following the established shape (`UiLinkDraw`,
`UiLinkTouch`, entry points in `ui_shell.h`), on `UiWindowFrame` like every other
tab:

- HOST button, SCAN button, and a list of nearby games showing the host's trainer
  name and node count from the beacon appdata.
- Once connected, the connected players and a DISCONNECT button.
- Errors shown as text rather than silently swallowed.

Doing the pairing explicitly here is also what avoids the "who hosts" race that
an automatic scan-then-host would suffer from.

## C: Files

- `3ds/host/link.c` (new), plus `HOST_SRCS` in `3ds/Makefile:42`.
- `3ds/bridge.h`: the transport seam, stdint only, next to the existing
  `Ctr3dsGetClock` block.
- `src/link.c`: one `#if PLATFORM_3DS` block over the transport functions.
- `include/link.h`: bridge declarations in game types.
- `3ds/ui/tab_link.c` (new), `3ds/ui/ui_shell.h`, `3ds/ui/bottom_screen.c`.
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

- HOST on one, SCAN on the other; the host's trainer name should appear.
- Connect, walk both into the Cable Club, confirm the player count and that
  `IsLinkMaster()` is true on exactly one side.
- Trade a mon and confirm both saves reflect it after a reload. This is the real
  test: trading is the longest block transfer and the least forgiving of drift.
- A link battle, which tests latency rather than throughput.
- Record mixing, a four-way transfer if enough instances are available.
- Kill one instance mid-trade and confirm the other shows Emerald's own link
  error and returns to the overworld rather than hanging.

## C: Risks

- **Lockstep drift is the thing most likely to bite.** Blocking on a peer stalls
  the whole frame, including audio, so a bad connection will crackle before it
  desyncs. The bounded wait keeps that finite; the jitter buffer keeps it rare.
- Nothing in this port has run on real hardware, and UDS on hardware behaves in
  ways an emulator will not reproduce, particularly around wireless being
  disabled and around the sleep switch.
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
| `party_menu.c:553` | `IsDma3ManagerBusyWithBgCopy()` directly | was the bug, fixed at source |
| `berry_tag_screen.c:201` | via `FreeTempTileDataBuffersIfPossible()` | already safe |
| `pokeblock.c:506` | via `FreeTempTileDataBuffersIfPossible()` | already safe |
| `credits.c:429` | nothing, self-advancing | safe |
| `battle_tower.c:2318/2528` | bounded RNG retry | safe |
| `VBlankIntrWait()` x2 | no-op stub, `rp2350/bios.c:198` | `ereader_helpers.c` only, unreachable |

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
