# Second-screen feature expansion

Design record. Sits alongside `3ds/UI_SKIN_PLAN.md` (which reskins and
re-lays-out what already exists) and `3ds/ROADMAP.md`.

**Status.** The structural work in Parts 2 and 3 is not started; assume steps 0
to 9 are all outstanding. One catalogue item has shipped ahead of it, out of
order and without the view stack: **#6, the EV/IV and stat spread**, as a panel
in the existing party detail view. A shiny-encounter notice shipped with it,
which was not on this list at all and became the first example of the overlay
pattern now documented in `SECOND_SCREEN_CHEATSHEET.md`. Neither needed the
refactor, which is the point worth carrying forward: the Tier 1 items that fit
inside a view that already exists do not have to wait for step 0.

## Context

The bottom screen currently has five tabs (PARTY, BAG, MAP, DEX, EXTRA). Between them they already deliver a handful of things the DS and 3DS games taught players to expect: an always-on region map with live player tracking and fly-from-map (HGSS/BW), pocket tabs with item icons and descriptions in one view (Gen 4+ bag), animated HP bars and real status badges, a summary-equivalent stats and moves split, and type-effectiveness arrows that Emerald itself never had (the Gen 7 Rotom Dex battle indicator).

What is missing is everything the second screen was invented for in the first place: the Pokétch apps, the PokéNav Plus panes, and the Gen 5 through 8 quality-of-life surfaces. Emerald has the data for almost all of them sitting in plain globals, and this port can read them safely from `CtrBottomUpdate`, which runs once per displayed frame at the end of a completed game frame.

The obstacle is structural, not informational. The UI has no view stack (five independent `static bool sXOpen` flags with three differently-placed BACK buttons), no shared widget layer (scrolling lists implemented three times, buttons three times), and no text clipping at all (every panel width is hand-measured against the longest known string, which cannot survive player-authored names). Adding a dozen views to that would multiply all three problems.

Intended outcome: a structure that absorbs new views cheaply, then a prioritised set of DS/3DS-derived features landing on it.

## Decisions taken

- **Full structure first.** Steps 0 to 3 are refactor with no new feature. Accepted because retrofitting clipping and a view stack into a dozen views is the expensive path, and step 0 fixes a live bug on its own.
- **Write paths in scope: touch battle controls and the quick-use item ring.** Both reuse gating that already exists and is already proven on hardware.
- **Write paths out of scope for now: PC deposit/withdraw, and save-anywhere plus backups.** They stay in the catalogue as future work. The read-only box viewer stays in scope.
- **Modern QOL welcome, balance untouched.** Surface information Emerald hides (IVs, EVs, encounter tables, catch marks). Do not change encounter rates, drops, catch rates or any other outcome. Anything that would shift balance goes behind the existing cheats page or does not ship.
- **First features: area encounters, trainer card and records, berry and daycare trackers.** PC boxes moves after them.

---

## Part 1: The feature catalogue (research)

Origin column names where the feature comes from. Everything in Tier 1 is a pure read of data that is already reachable.

### Tier 1: read-only, high value, cheap

| # | Feature | Origin | Data source |
|---|---|---|---|
| 1 | **Area encounters.** What appears on this map by method (grass / surf / rock smash / old-good-super rod), level range, and a seen/caught mark per species. Plus mass outbreak and roamer. | ORAS **DexNav**, BW **Habitat List** | `gWildMonHeaders` (`include/wild_encounter.h:29`), `gSaveBlock1Ptr->location`, `GetSetPokedexFlag`. Slot lookup is `GetCurrentMapWildMonHeaderId` (static, `src/wild_encounter.c:310`) so copy the 10-line loop; precedent at `src/pokedex_area_screen.c:288`. **Must** route species through `Ctr3dsMapWildSpecies()` or the list lies when the randomiser is on. |
| 2 | **PC boxes.** Browse all 14 boxes, 30 slots each, read-only. Box names, per-box count, tap a mon for its summary. | Gen 8 **Pokémon Box Link**, Gen 7 in-box judge | `gPokemonStoragePtr` (`include/pokemon_storage_system.h:27`), `GetBoxMonDataAt`, `GetBoxNamePtr`, `CountMonsInBox`, `CheckBoxMonSanityAt`. |
| 3 | **Trainer card / records.** Badges, play time, money, dex counts, and the game stats Emerald tracks but never shows (steps taken, battles won, Pokémon caught, times saved). | Every gen; **B2W2 Medals** is the extended version | `gSaveBlock2Ptr` fields (`include/global.h:537-551`), `GetMoney` (`include/money.h:4`), `GetGameStat` (`include/overworld.h:63`), `FlagGet(FLAG_BADGE01_GET + n)`. Money, coins and game stats are XOR-encrypted: the accessors are not optional. |
| 4 | **Berry tracker.** Every planted tree: berry, growth stage, minutes to next stage, watered flags, expected yield. | DPPt **Pokétch Berry Searcher** | `gSaveBlock1Ptr->berryTrees` (`include/global.h:1054`), `GetBerryInfo`, `GetStageByBerryTreeId`. Timer only advances on `DoTimeBasedEvents`, so display it as "as of last update", and never call `BerryTreeTimeUpdate` (it mutates save state). |
| 5 | **Day-care checker.** The two deposited mons, level gained, and whether an egg is waiting. | DPPt **Pokétch app #7** | `gSaveBlock1Ptr->daycare` (`include/global.h:1090`, `include/daycare.h`). |
| 6 | ✅ **SHIPPED.** **EV / IV / stat spread.** Six bars per mon, EV total against the 510 cap, IVs, and the nature's plus/minus stats marked. | XY **Super Training** chart, Gen 7 **judge** | `MON_DATA_*_EV` / `*_IV` (`include/pokemon.h:34-52`), `GetMonEVCount` (`:495`), `gNatureStatTable` (`:392`). Built as a left-column tenant of the party detail view (`3ds/ui/tab_party.c:516`) rather than as a pushed view: four columns (computed stat, base, IV, EV) over six rows, EV total against the 510 cap, perfect IVs and 252+ EVs marked, nature signed on the row label. Bars were dropped for numbers -- an EV trainer reads exact values, and 176px of column does not carry six bars and their numbers both. |
| 7 | **Learnset and evolution preview.** Next moves by level, full level-up list, what it evolves into and how, TM/HM compatibility. | Gen 8 summary screen, Gen 6 **move reminder** | `gLevelUpLearnsets` (`include/pokemon.h:385`), `gEvolutionTable` (declare locally, as `src/daycare.c:31` does), `GetEvolutionTargetSpecies` (`:478`), `CanMonLearnTMHM` (`:502`). |
| 8 | **Type matchup chart.** Attacking type against all 17 defending types, paged by attacking type. | DPPt **Pokétch Move Tester** | `gTypeEffectiveness` (`include/battle_main.h:85`). Reuse `TypeMultiplier()` from `3ds/ui/matchup.c:36`, which already handles the `TYPE_FORESIGHT` mid-table marker correctly. |
| 9 | **Clock and counters.** Wall clock, play time, steps, Repel steps remaining, egg-hatch step estimate. | DPPt **Pokétch** digital watch + pedometer | `Ctr3dsGetClock()` (`3ds/bridge.h:240`, side-effect free; prefer it over `RtcCalcLocalTime`, which is part of the game's time-event chain), `VAR_REPEL_STEP_COUNT`, `GetGameStat(GAME_STAT_STEPS)`. |

### Tier 2: writes game state, needs the port's existing gating discipline

| # | Feature | Origin | Notes |
|---|---|---|---|
| 10 | **Touch battle controls.** Tap a move to select it; per-move effectiveness badge against the current opponent. | Gen 5+ battle bottom screen; Gen 7 **Rotom Dex** effectiveness indicator | The highest-value item on the whole list and the most DS-like. Follow `Ctr3dsQueueBattleItem` (`src/battle_controller_player.c:272`) exactly: require `gBattlerControllerFuncs[player] == HandleInputChooseAction`, reject link/frontier/recorded battles, save and restore `gActiveBattler`. The effectiveness half is Tier 1 and can ship alone. |
| 11 | **Quick-use item ring.** Four to six registered items usable from any tab (Bicycle, Repel, Escape Rope). | Gen 4-7 **Y-button registration** | Bag already has `CanUseItemNow()` + `ItemTargeting()` (`3ds/ui/tab_bag.c:153`, `:188`). This is a second entry point to an existing, well-gated commit path. |
| 12 | **PC deposit / withdraw.** *Out of scope for now.* | Gen 8 boxes-anywhere | Recorded for later. Only after #2 ships read-only, and it needs the four-gate overworld check plus a party-count guard. |
| 13 | **Save anywhere + rolling backups.** *Out of scope for now.* | Emulator convention, not a DS feature | Recorded for later. `TrySavingData` (`include/save.h:100`) is synchronous and `CtrSaveCommit()` is already hooked at `src/save.c:787`, so the cost is small if it is ever wanted; the caveat is that the failure path calls `DoSaveFailedScreen`, which seizes `gMain.callback2`. |
| 14 | **Dig / Escape Rope from the map tab.** | Gen 4+ convenience | `CanUseDigOrEscapeRopeOnCurMap()` (`include/item_use.h:33`). Same shape as the fly button already in `3ds/ui/tab_map.c`. |

### Tier 3: larger or lower value

Dex search and filter (Gen 6); dex area map, since `src/pokedex_area_screen.c` already exists; achievement medals off `GetGameStat` (B2W2); Battle Frontier streak records (`gSaveBlock2Ptr->frontier`); secret base viewer; rematch tracker off `gSaveBlock1Ptr->trainerRematches` (PokéNav Match Call); screenshot to SD (no libpng in the link line, so BMP or raw RGB565).

---

## Part 2: The structure that has to land first

### Navigation: a launcher, not more tabs

`DrawTabBar` (`3ds/ui/bottom_screen.c:228`) is `tabW = 320 / visibleCount`. Five tabs is 64px; twelve would be 26px, narrower than a fingertip and narrower than the labels. The tab bar also reflows as progress flags unlock, so widths already move under the player.

Instead: **cap the tab bar at six**, convert EXTRA into **HOME**, and make HOME a 4x3 launcher of 80x64 tiles. That divides 320x192 exactly on whole 8px tiles, which `UiWindowFrame` requires. Each tile is 5120 square pixels against a tab cell's 3072. A pager gives a second page of twelve. Locked tiles keep their cell and draw dimmed rather than compacting, so tile positions stay learnable.

### The tab bar becomes a title bar at depth > 0

```
depth 0:  [ PARTY ][ BAG ][ MAP ][ DEX ][ HOME ][ LINK ]
depth 1+: [ < BACK ]  PC BOX 3                  [ action ]
```

One 80x48 BACK target replaces the three hand-placed rects at 38x22 (`tab_party.c:101`), 42x22 (`tab_dex.c:96`) and 56x20 (`tab_bag.c:105`). Every pushed view gets a title, which matters once there are a dozen of them. It also makes the current bug structurally impossible: today a modal flag survives a tab switch, so leaving PARTY's detail view and coming back re-enters it.

### New files

| File | Contents |
|---|---|
| `3ds/ui/ui_view.c/.h` | View stack (depth 4, `.bss`, no heap) and a registry of `struct UiView { title, enter, draw, touch, key, tick, flag, flags }`. Adding a view is one enum line, one extern, one table row, one file. A pushed view derives everything from its `arg` and re-seeds in `enter()`, so no state can survive a pop. |
| `3ds/ui/ui_widgets.c/.h` | `UiButton` (seed `tab_extra.c:150`), `UiChoiceRow` (seed the `WIDE_X`/`SPD_X` families, ~10 callers in `tab_extra.c` alone), `UiList` (seed `tab_dex.c:215 MoveCursor`, which is the better of the two existing scroll models), `UiField`. Later: `UiGrid`, `UiStatBar`, `UiPager`. **Rule: this file may not include game headers.** Anything needing `GetMonData` is view code, not a widget. |
| `3ds/ui/ui_skin.h` | The `UI_COL_*` block moved out of `ui_shell.h`, so `UI_SKIN_PLAN.md` later changes one file. |
| `3ds/ui/ui_layout.h` | Content rect, title-bar geometry, shared margins. Per-view hand-fitted constants stay per-view. |
| `3ds/ui/ui_regionmap.c/.h` | The affine tilemap decoder and asset caches currently making up most of `tab_map.c`'s 699 lines. |

### Text safety

There is no clipping, wrapping, ellipsis or truncation today. Every width is hand-measured against the longest known game string, which was never going to work for nicknames, OT names or box names.

- `UiClipPush/Pop/Get` in `ui_draw.c`. Nearly free: `UiFillRect` (`:50`), `UiBlit4bppTile` (`:79`), `UiBlit8bppTile` (`:113`) and `BlitGlyph` (`ui_text.c:48`) already do per-pixel bounds tests against `0..UI_W/UI_H`. They just test against the clip instead. Four one-line edits, zero extra per-pixel work.
- `UiTextClipped(x, y, maxW, ...)` and `UiTextWrapped(x, y, maxW, maxLines, ...)`. Use `CHAR_ELLIPSIS` (`include/constants/characters.h:101`), which the game font has and `UiAscii` cannot produce.
- Enforced, not merely offered: `UiListDraw`/`UiGridDraw` push a clip around each row before calling the painter, so any future row painter is overrun-proof even using raw `UiText`. Widgets taking a width clip internally.

### Repaint scaling

`UiStateHash` (`bottom_screen.c:153`) folds only the active tab's key into one `top[4]` slot via a four-branch `if/else`. Replace with `top[4] = UiViewTop() ^ (UiViewKey() * 2654435761u)`.

Most new views need **no key at all**: encounter tables, learnsets, base stats and the type chart are static data that changes only under their own touch handler, which already marks dirty. For the ones that do, the rule is that a key must be O(what is on screen), not O(what is in the data).

The PC box case is the one that looks expensive and is not. `GetMonData` decrypts in place (`src/pokemon.c:3745`), so hashing 420 species reads would be 420 decrypt round trips. But `MON_DATA_PERSONALITY`, `MON_DATA_OT_ID` and `MON_DATA_SANITY_HAS_SPECIES` sit **before** `MON_DATA_ENCRYPT_SEPARATOR` (`include/pokemon.h:8-19`), so they answer from the plaintext header. Hashing 30 visible `personality ^ otId` pairs is 60 plain loads and zero decryptions. Write that rule into `ui_view.h`.

One win falls out of the same refactor: the 6-mon party loop in the hash does six decrypt round trips per frame on all five tabs, and moves into the party views' own `key`. (`UiPartyTick()` no longer needs this: it takes the tab's visibility as an argument and returns immediately off screen, which it had to once the mon icons started animating on it.)

### Build-script hazard, worth fixing regardless

`3ds/build_objs.sh:113-114` globs `3ds/ui/*.c` non-recursively and writes `$OBJ/$(basename).o` into the same object directory as all of `src/*.c`, with `3ds/ui` globbed last. A file named `3ds/ui/pokedex.c` would silently overwrite `src/pokedex.o` and delete the game's Pokédex from the archive. This becomes likely the moment view files get named after game features.

Mitigation: mandatory prefixes, `ui_*` for shared, `tab_*` for the six tab roots, `view_*` for pushed views, and a comment in `build_objs.sh` saying why so nobody tidies the names later. Keep the directory flat; subdirectories are silently not compiled.

---

## Part 3: Build order

| Step | Work | Verification |
|---|---|---|
| 0 | `ui_view.c/.h`, registry, title bar. Pure refactor: register the five tabs as roots, convert the three modals to pushed views, delete the three BACK rects, replace `Redraw()`'s switch. | Open PARTY detail, tap BAG, tap PARTY: you get the grid, not the detail. Same for DEX entry and the BAG picker. |
| 1 | `UiClipPush/Pop`, `UiTextClipped`, `UiTextWrapped`. ~100 lines. Before any new view, because retrofitting clipping into twelve views is the retrofit to avoid. | Nothing on the existing five tabs changes by a pixel. |
| 2 | `ui_widgets.c` with exactly four widgets. Prove each on two existing callers on day one: `UiButton` on `tab_extra.c` and `tab_map.c:448`; `UiList` on `tab_dex.c` and `tab_bag.c`, retiring bag's inferior scroll model. | Existing tabs behave identically; bag's list now keeps the selection when scrolling. |
| 3 | HOME. `tab_extra.c` becomes `tab_home.c` (4x3 tiles) plus `view_settings.c` / `view_tweaks.c` / `view_audio.c`. First non-modal use of the stack; `UiGrid` gets its first caller. | All EXTRA controls still reachable and still persist. |
| 4 | **Trainer card and records** (#3). Cheapest of the three chosen features and almost entirely `UiField` rows. Ships with `key == NULL`, confirming the "most views need no key" claim. Adds `UiStatBar` if the badge row wants it. | Every figure matches the in-game trainer card; money and game stats read through `GetMoney`/`GetGameStat`, never raw. |
| 5 | **Berry and daycare trackers** (#4, #5). Two small views, both on the cheap key tier (a handful of scalar reads). First real repaint keys under the new scheme. | Berry stages and timers match the in-game trees; the day-care view agrees with the attendant's dialogue about egg readiness. |
| 6 | **Area encounters** (#1). First genuine `UiList` caller among the new views, plus a method selector on `UiChoiceRow`. | List matches the game's own tables for several maps, with the randomiser both off and on. A map with no water and no fishing shows empty methods rather than stale rows. |
| 7 | **Quick-use item ring** (#11). First write path. A second entry point into `CanUseItemNow()` + `ItemTargeting()` (`3ds/ui/tab_bag.c:153`, `:188`), which are already proven. | Registered items work from every tab; all four gates still refuse during a battle, a script, locked field controls, or a non-overworld callback. |
| 8 | **Battle effectiveness badges** (the read-only half of #10). Per-move super-effective / resisted / immune marks in the party move list, reusing `TypeMultiplier()` from `3ds/ui/matchup.c:36`. Ships alone and is worth having on its own. | Badges agree with actual damage in a real battle, including Normal against Ghost (the `TYPE_FORESIGHT` case `matchup.c` already handles). |
| 9 | **Touch move selection** (the write half of #10). Highest-risk item in the plan; deliberately last. Follows `Ctr3dsQueueBattleItem` (`src/battle_controller_player.c:272`) exactly: require `gBattlerControllerFuncs[player] == HandleInputChooseAction`, reject link/frontier/recorded battle types, save and restore `gActiveBattler` and `gBattlerInMenuId`. | A tapped move resolves identically to the same move chosen with the D-pad, in singles and doubles, and is refused outright in link and frontier battles. |

**Gap worth naming.** None of the three chosen first features stresses `UiGrid` or a long scrolling list hard. HOME's 4x3 launcher (step 3) is therefore the only `UiGrid` proving ground before several views pile on it, and the read-only PC box viewer (#2) is the real structural stress test: 14-row list, 30-cell grid, pushed detail, windowed plaintext key, and long player-authored box names through `UiTextClipped`. Recommend scheduling it immediately after step 6 rather than leaving it until the widget layer has calcified around easier callers.

**Later, in rough value order:** PC boxes read-only (#2), learnset and evolution preview (#7), type matchup chart (#8), clock and counters (#9), Dig/Escape Rope on the map tab (#14), then Tier 3.

### Deliberately not built yet

Two-level tab bar (costs 48px of 192; every tab, the encounters view and both overlays are fitted to `UI_CONTENT_H`). Dirty-rectangle or partial redraw (the hash gate already makes repaints rare; invalidation bookkeeping would infect every widget; per `AGENTS.md`, measure on hardware before optimising). Heap-allocated views, a scene graph, or transition animations (the whole UI layer is deliberately heap-free). A generic pixel-scrolling viewport (row-granular is enough; the type chart pages by attacking type rather than scrolling). Press feedback and long-press, which belong to `UI_SKIN_PLAN.md` step 5 and would confound measurement of steps 0-2. Persisted per-view state, which `tab_extra.c:146` already argues against for its own page number.

---

## Part 4: Interaction with UI_SKIN_PLAN.md

Recommendation is structure first, skin second. The skin is a deliberately small diff ("two functions change, ~98 call sites do not") and doing the structure first makes it smaller, not larger.

1. `DrawButtonH` moves out of `tab_extra.c` into `UiButton`. The skin plan's step 5 defines `UiButton` and folds in both local helpers, `DrawButtonH` (`tab_extra.c:188`) and `DrawBtn` (`tab_map.c:476`), plus about a dozen inline outlines; whichever plan lands first owns it, and if `ui_widgets.c` exists by then it lives there, so it covers every future view's buttons for the same edit.
2. `tabbar.png` is specced at 64x96 (cell idle, cell active, bar ground). A title bar also needs a back chevron and a title ground. **Decide before the art is drawn** or it gets authored twice. Suggest 64x160.
3. **Highest-risk item:** the skin plan says `ui_gfx.c`'s entry points clamp against `0..UI_W/UI_H` "the way `UiFillRect` already does". After step 1 `UiFillRect` clamps against the *clip*. `UiBlitPart`, `UiTileFill` and `UiNineSlice` must do the same, or a nine-slice panel inside a clipped list row paints over its neighbours and looks like a layout bug rather than a blitter bug.
4. Both plans edit `UiStateHash`: the skin drops `top[0] = UiFrameId()`, this drops the party loop and rewrites `top[4]`. Different lines, mergeable, but do them in one sitting.
5. Create `ui_skin.h` in step 2 even as a pure re-export, so the skin's colour move is one `#include` change.

One ordering constraint. This plan leaves `UI_TABBAR_H 48` and `UI_CONTENT_H 192` alone, but the skin plan no longer freezes them: its `shell.png` wireframe may move both. The title bar reuses whatever band the tab bar ends up with, and the launcher's 4x3 grid of 80x64 fills 320x192 exactly on whole tiles only at today's values. So settle `shell.png` before sizing the launcher and the title bar, and derive both from `UI_CONTENT_H` / `UI_TABBAR_H` rather than from 192 and 48.

---

## Verification

- Build: `make tools && make generated`, `python3 tools/generate_wasm_assets.py`, `3ds/build_objs.sh`, `make -C 3ds`. Both mixer configurations still link.
- Regression on the existing five tabs after steps 0-2: no pixel changes, all controls still reachable, settings still persist across a restart.
- Modal bug fix: the tab-switch sequence above.
- Text safety: nickname a party member to eight wide characters (`WWWWWWWWWW`) and confirm it ellipsises rather than overrunning its cell. Same for a renamed PC box.
- New views checked against the game's own screens, each against its authoritative source: trainer card against the in-game trainer card and the Pokédex counts; berry stages against the trees themselves; day-care against the attendant's dialogue; encounter list against the real tables on several maps, with the randomiser both off and on.
- Write paths (steps 7 and 9) verified against their gates, not only their happy paths: confirm each is refused during a battle, a script, locked field controls, and a non-overworld callback, and that a tapped move resolves identically to the same move chosen with the D-pad.
- On hardware, not only in an emulator: the audio health report at frame 600 and the boot log stay clean, and repaint frequency has not visibly risen (`AGENTS.md` on measuring on the device).

## Open question for later

The catalogue is deliberately larger than the first slice. Once steps 0 to 3 are in, adding a Tier 1 view should cost roughly 150 lines and one afternoon, so the remaining items become a menu to pick from rather than a commitment. Revisit PC deposit/withdraw and save-anywhere once the read-only box viewer has shipped and its gating has been exercised.
