# Second-screen feature expansion

A plan, mostly not started. Sits alongside `3ds/UI_SKIN_PLAN.md` (which reskins and
re-lays-out what already exists) and `3ds/ROADMAP.md`.

**Status.** The structural work in Parts 2 and 3 is not started; assume steps 0
to 9 are all outstanding. Re-checked against `a036990` on 2026-09-26. The
first re-check was at `67090b3` on 2026-09-16.

Three catalogue items have shipped ahead of the structure, out of order and
without the view stack:
- **#6, the EV/IV and stat spread**, as a panel in the existing party detail
  view.
- **#1, area encounters**, as a view that MAP pushes
  (`3ds/ui/view_encounters.c`). It lists the species of any place tapped on the
  map by method (land, surf, Rock Smash, each rod), with level ranges, chances,
  seen and caught marks, and the randomizer applied. It does not show a mass
  outbreak or the roamer.
- **Achievement medals** (Tier 3), as the TROPHY tab and the unlock toast.
  [ACHIEVEMENTS.md](ACHIEVEMENTS.md) lists them, and `ROADMAP.md` Part E has
  what is left.

Other things not on this list shipped too. A shiny-encounter notice became the
first example of the overlay pattern in `SECOND_SCREEN_CHEATSHEET.md`. The
quick-throw strip (`3ds/ui/ui_quickball.c`) became the second, and the
achievement toast the third. None of them needed the refactor, which is the
point worth carrying forward: the items that fit inside a view that already
exists do not have to wait for step 0.

Since the first re-check:
- **The LINK page** (EXTRA page 5, `3ds/ui/ui_link.c`) pairs two consoles for
  the Cable Club. `ROADMAP.md` Part C has the full account.
- **Trainer cards on the touch screen** while a link is up (`3ds/ui/ui_card.c`,
  `874b69d` and after). A TRAINER CARDS button on the LINK page opens a view
  that takes the whole content area: the partner's card at 1:1 in the game's
  own art, thumbnails to switch, and a tap to turn it over. This is **not**
  catalogue #3, because it shows only the cards a link exchanged. But it makes
  #3 much cheaper: the card renderer exists, and
  `TrainerCard_GenerateCardForLinkPlayer` (`include/trainer_card.h:73`) fills a
  card from the player's own save.
- **`3ds/ui/ui_team.c`**: every view that lists the party now reads it through
  `UiPartyMon`, which undoes the battle order of the game's party menu and
  marks a partner trainer's slots. A PC box viewer or any other party view must
  use it too.
- **Check rows on EXTRA** (`UiCheckBox`, `b4a3dac`). Every on/off setting is a
  checkbox row now, not an OFF and an ON button. The `UiChoiceRow` widget below
  must cover both shapes.
- **A cheaper paint** (`040609a`, `af5780b`, `4051578`). The UI paints straight
  into the buffer the GPU reads, and the host uploads only the dirty rows. See
  "Repaint scaling" below.

**The tab bar is full.** TROPHY is the sixth tab, and six is the practical floor
(cheatsheet section 5, "Adding a tab"). So Part 2's launcher is now the only way
to add a view, not one option of two.

## Context

The bottom screen currently has six tabs (PARTY, BAG, MAP, DEX, TROPHY, EXTRA). Between them they already deliver a handful of things the DS and 3DS games taught players to expect: an always-on region map with live player tracking and fly-from-map (HGSS/BW), pocket tabs with item icons and descriptions in one view (Gen 4+ bag), animated HP bars and real status badges, a summary-equivalent stats and moves split, and type-effectiveness arrows that Emerald itself never had (the Gen 7 Rotom Dex battle indicator).

What is missing is everything the second screen was invented for in the first place: the Pokétch apps, the PokéNav Plus panes, and the Gen 5 through 8 quality-of-life surfaces. Emerald has the data for almost all of them sitting in plain globals, and this port can read them safely from `CtrBottomUpdate`, which runs once per displayed frame at the end of a completed game frame.

The obstacle is structural, not informational. The UI has no view stack (six independent modal flags, with BACK buttons in five places and four sizes), no shared widget layer (scrolling lists implemented four times, in DEX, BAG, TROPHY and the encounters view; button helpers four times), and, until step 1 shipped, no text clipping at all (every panel width is hand-measured against the longest known string, which cannot survive player-authored names). Adding a dozen views to that would multiply all three problems.

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
| 1 | ✅ **SHIPPED** (`3ds/ui/view_encounters.c`): one list per method with chips to choose it, level range and chance per species (most common first), seen and caught marks, randomizer applied. The chance hides for a tapped place whose maps would add up. **Still open:** mass outbreak and roamer. **Area encounters.** What appears on this map by method (grass / surf / rock smash / old-good-super rod), level range, and a seen/caught mark per species. Plus mass outbreak and roamer. | ORAS **DexNav**, BW **Habitat List** | `gWildMonHeaders` (`include/wild_encounter.h:29`), `gSaveBlock1Ptr->location`, `GetSetPokedexFlag`. Slot lookup is `GetCurrentMapWildMonHeaderId` (static, `src/wild_encounter.c:311`) so copy the 10-line loop; precedent at `src/pokedex_area_screen.c:288`. **Must** route species through `Ctr3dsMapWildSpecies()` or the list lies when the randomiser is on. |
| 2 | **PC boxes.** Browse all 14 boxes, 30 slots each, read-only. Box names, per-box count, tap a mon for its summary. | Gen 8 **Pokémon Box Link**, Gen 7 in-box judge | `gPokemonStoragePtr` (`include/pokemon_storage_system.h:27`), `GetBoxMonDataAt`, `GetBoxNamePtr`, `CountMonsInBox`, `CheckBoxMonSanityAt`. |
| 3 | **Half the parts exist:** `UiCardDraw` (`3ds/ui/ui_card.c`) draws a card from `gTrainerCards`, and `TrainerCard_GenerateCardForLinkPlayer` fills one from the player's save. What is left is a view that fills a local card and draws it, and the records panel. `UiCardDraw` takes a card id today, so it needs a variant that takes a `struct TrainerCard *`. **Trainer card / records.** Badges, play time, money, dex counts, and the game stats Emerald tracks but never shows (steps taken, battles won, Pokémon caught, times saved). | Every gen; **B2W2 Medals** is the extended version | `gSaveBlock2Ptr` fields (`include/global.h:539-553`), `GetMoney` (`include/money.h:4`), `GetGameStat` (`include/overworld.h:71`), `FlagGet(FLAG_BADGE01_GET + n)`. Money, coins and game stats are XOR-encrypted: the accessors are not optional. |
| 4 | **Berry tracker.** Every planted tree: berry, growth stage, minutes to next stage, watered flags, expected yield. | DPPt **Pokétch Berry Searcher** | `gSaveBlock1Ptr->berryTrees` (`include/global.h:1058`), `GetBerryInfo`, `GetStageByBerryTreeId`. Timer only advances on `DoTimeBasedEvents`, so display it as "as of last update", and never call `BerryTreeTimeUpdate` (it mutates save state). |
| 5 | **Day-care checker.** The two deposited mons, level gained, and whether an egg is waiting. | DPPt **Pokétch app #7** | `gSaveBlock1Ptr->daycare` (`include/global.h:1094`, `include/daycare.h`). |
| 6 | ✅ **SHIPPED.** **EV / IV / stat spread.** Six bars per mon, EV total against the 510 cap, IVs, and the nature's plus/minus stats marked. | XY **Super Training** chart, Gen 7 **judge** | `MON_DATA_*_EV` / `*_IV` (`include/pokemon.h:34-52`), `GetMonEVCount` (`:495`), `gNatureStatTable` (`:392`). Built as a left-column tenant of the party detail view (`3ds/ui/tab_party.c:864`) rather than as a pushed view: four columns (computed stat, base, IV, EV) over six rows, EV total against the 510 cap, perfect IVs and 252+ EVs marked, nature signed on the row label. Bars were dropped for numbers -- an EV trainer reads exact values, and 176px of column does not carry six bars and their numbers both. |
| 7 | **Learnset and evolution preview.** Next moves by level, full level-up list, what it evolves into and how, TM/HM compatibility. | Gen 8 summary screen, Gen 6 **move reminder** | `gLevelUpLearnsets` (`include/pokemon.h:385`), `gEvolutionTable` (declare locally, as `src/daycare.c:32` does), `GetEvolutionTargetSpecies` (`:478`), `CanMonLearnTMHM` (`:502`). |
| 8 | **Type matchup chart.** Attacking type against all 17 defending types, paged by attacking type. | DPPt **Pokétch Move Tester** | `gTypeEffectiveness` (`include/battle_main.h:88`). Reuse `TypeMultiplier()` from `3ds/ui/matchup.c:34`, which already handles the `TYPE_FORESIGHT` mid-table marker correctly. |
| 9 | **Clock and counters.** Wall clock, play time, steps, Repel steps remaining, egg-hatch step estimate. | DPPt **Pokétch** digital watch + pedometer | `Ctr3dsGetClock()` (`3ds/bridge.h:435`, side-effect free; prefer it over `RtcCalcLocalTime`, which is part of the game's time-event chain), `VAR_REPEL_STEP_COUNT`, `GetGameStat(GAME_STAT_STEPS)`. |

### Tier 2: writes game state, needs the port's existing gating discipline

| # | Feature | Origin | Notes |
|---|---|---|---|
| 10 | **Touch battle controls.** Tap a move to select it; per-move effectiveness badge against the current opponent. | Gen 5+ battle bottom screen; Gen 7 **Rotom Dex** effectiveness indicator | The highest-value item on the whole list and the most DS-like. Follow `Ctr3dsQueueBattleItem` (`src/battle_controller_player.c:279`) exactly: require `gBattlerControllerFuncs[player] == HandleInputChooseAction`, reject link/frontier/recorded battles, save and restore `gActiveBattler`. The effectiveness half is Tier 1 and can ship alone. |
| 11 | **Quick-use item ring.** Four to six registered items usable from any tab (Bicycle, Repel, Escape Rope). | Gen 4-7 **Y-button registration** | Bag already has `CanUseItemNow()` + `ItemTargeting()` (`3ds/ui/tab_bag.c:152`, `:181`). This is a second entry point to an existing, well-gated commit path. |
| 12 | **PC deposit / withdraw.** *Out of scope for now.* | Gen 8 boxes-anywhere | Recorded for later. Only after #2 ships read-only, and it needs the four-gate overworld check plus a party-count guard. |
| 13 | **Save anywhere + rolling backups.** *Out of scope for now.* | Emulator convention, not a DS feature | Recorded for later. `TrySavingData` (`include/save.h:100`) is synchronous and `CtrSaveCommit()` is already hooked at `src/save.c:787`, so the cost is small if it is ever wanted; the caveat is that the failure path calls `DoSaveFailedScreen`, which seizes `gMain.callback2`. |
| 14 | ✅ **SHIPPED** as ESCAPE on MAP's caption row (`EscState`, `DoEscape` in `3ds/ui/tab_map.c`), on the player's own location, with a YES/NO confirm. DIG first, then the rope. **Dig / Escape Rope from the map tab.** | Gen 4+ convenience | `CanUseDigOrEscapeRopeOnCurMap()` (`include/item_use.h:33`). Same shape as the fly button already in `3ds/ui/tab_map.c`. |

### Tier 3: larger or lower value

Dex search and filter (Gen 6); dex area map, since `src/pokedex_area_screen.c` already exists; ✅ achievement medals off `GetGameStat` (B2W2), **shipped** as the TROPHY tab ([ACHIEVEMENTS.md](ACHIEVEMENTS.md)); Battle Frontier streak records (`gSaveBlock2Ptr->frontier`); secret base viewer; rematch tracker off `gSaveBlock1Ptr->trainerRematches` (PokéNav Match Call); screenshot to SD (no libpng in the link line, so BMP or raw RGB565).

---

## Part 2: The structure that has to land first

### Navigation: a launcher, not more tabs

`DrawTabBar` (`3ds/ui/bottom_screen.c:688`) is `tabW = 320 / visibleCount`. Six tabs, which the bar has now with TROPHY, is 53px each; twelve would be 26px, narrower than a fingertip and narrower than the labels. The tab bar also reflows as progress flags unlock, so widths already move under the player.

Instead: **cap the tab bar at six**, which it has already reached, convert EXTRA into **HOME**, and make HOME a 4x3 launcher of 80x64 tiles. That divides 320x192 exactly on whole 8px tiles, which `UiWindowFrame` requires. Each tile is 5120 square pixels against a tab cell's 3072. A pager gives a second page of twelve. Locked tiles keep their cell and draw dimmed rather than compacting, so tile positions stay learnable.

### The tab bar becomes a title bar at depth > 0

```
depth 0:  [ PARTY ][ BAG ][ MAP ][ DEX ][ TROPHY ][ HOME ]
depth 1+: [ < BACK ]  PC BOX 3                  [ action ]
```

**Settled: LINK is a page of EXTRA.** This sketch first had LINK as the sixth tab, for the Cable Club in `ROADMAP.md` Part C. TROPHY took that slot. LINK went to EXTRA page 5 instead (`3ds/ui/ui_link.c`), which is the one option that does not make Part C wait for this plan. When HOME lands it becomes a tile, and only `UiLinkPageDraw`, `UiLinkPageTouch` and `UiLinkPageStateKey` move.

It is also the precedent for the next such view: an EXTRA page costs one `PAGE_COUNT` bump and three dispatch lines, so a view that does not earn a tab can land there while the launcher is still being built. Put it before the debug page, which is the final `else` of each dispatch chain.

LINK also set a second precedent: a sub-view inside a page. Its trainer card view takes the whole content area, skips EXTRA's frame and pager, and draws its own way back. It is `UI_VIEW_LINK_CARDS` on the view stack, over the LINK page.

`ROADMAP.md` Part C.3 records the same decision.

**Settled 2026-09-26: the tab bar stays.** The sketch above turns the bar into a title bar at depth > 0. The user chose to keep the tab bar instead: tapping another tab leaves the view, tapping the active tab returns it to its top, and each view keeps its own BACK (38x22 `tab_party.c:98`, 42x22 `tab_dex.c:92` and the encounters view, 56x20 BAG's CANCEL `tab_bag.c:99`, 60x22 LINK's cards `ui_link.c:63`). Quick tab switching is the point of the second screen, and a title bar would take it away while a view is open.

The bug this section set out to make impossible is fixed. The modal flags (`sDetailOpen`, `sEntryOpen`, BAG's `sView`, the encounters view's `sOpen`, LINK's `sCardOpen`) are gone: each screen is an entry on the view stack, and the shell's `LeaveTab` empties the stack on every tab change. MAP's FLY and ESCAPE confirms are not views, and `UiMapLeave` clears them on the same path.

### New files

| File | Contents |
|---|---|
| `3ds/ui/ui_view.c/.h` | ✅ **Shipped, lighter than drawn here.** A stack of `{id, arg}` (depth 4, `.bss`, no heap). There is no registry of draw and touch callbacks: each tab still draws and dispatches its own views and asks `UiViewIsOpen(id)` where it tested its flag. That was the smallest change that makes the tab-switch bug impossible, and it touched each view's file in a few lines rather than restructuring it. A registry can still come with the widget layer if views multiply. A view takes everything from its `arg` or from the game when it draws, so no state can survive a pop. |
| `3ds/ui/ui_widgets.c/.h` | `UiButton` (seed `DrawButtonH`, `tab_extra.c:182`), `UiChoiceRow` (seed the `WIDE_X`/`SPD_X` button families and the `DrawCheckRow` check rows, both in `tab_extra.c`), `UiList` (seed `tab_dex.c:215 MoveCursor`, which is the better of the two existing scroll models), `UiField`. Later: `UiGrid`, `UiStatBar`, `UiPager`. **Rule: this file may not include game headers.** Anything needing `GetMonData` is view code, not a widget. |
| `3ds/ui/ui_skin.h` | The `UI_COL_*` block moved out of `ui_shell.h`, so `UI_SKIN_PLAN.md` later changes one file. |
| `3ds/ui/ui_layout.h` | Content rect, title-bar geometry, shared margins. Per-view hand-fitted constants stay per-view. |
| `3ds/ui/ui_regionmap.c/.h` | The affine tilemap decoder and asset caches currently making up most of `tab_map.c`'s 778 lines. |

### Text safety

✅ **Step 1 is shipped.** Every width was hand-measured against the longest known game string, which was never going to work for nicknames, OT names or box names.

- ✅ `UiClipPush` / `UiClipPop` / `UiClipReset` and the read-only rect `gUiClip`, in `ui_draw.c`. Every writer clamps against the clip instead of the screen: `UiFillRect`, `UiPixel`, `UiBlit4bppTile` (both loops), `UiBlit4bppTileFlip`, `UiBlit8bppTile`, `UiBlitRow`, and `BlitGlyph` (`ui_text.c`, both the scale-1 span and the scaled per-pixel test). No extra per-pixel work: the same compares, against the rect instead of constants. `UiClear` and `UiRestoreRect` ignore it on purpose. A push intersects with the current rect; the stack is 4 deep; the shell resets it before every paint, so a missing pop spoils one paint. The existing tabs never push, so they draw as before.
- ✅ `UiTextClipped(x, y, maxW, ...)` and `UiTextWrapped(x, y, maxW, maxLines, ...)` in `ui_text.c`. They cut between whole units (a glyph, a two-byte symbol or a control code), end a cut with `CHAR_ELLIPSIS`, drop spaces before it, and wrap at the last space that fits or inside a word wider than the line. A string that fits draws exactly as `UiText` draws it. `DrawNameClipped` in `ui_card.c` is left alone, so the LINK card's names do not change.
- Enforced, not merely offered: `UiListDraw`/`UiGridDraw` push a clip around each row before calling the painter, so any future row painter is overrun-proof even using raw `UiText`. Widgets taking a width clip internally.

### Repaint scaling

`UiStateHash` (`bottom_screen.c:563`) folds only the active tab's key into one `top[4]` slot via a five-branch `if/else`, one branch for each tab with a key (PARTY, DEX, MAP, TROPHY, EXTRA). The other slots, up to `top[9]`, belong to the overlays, the title prompt and the achievements provider, and stay as they are. Replace only `top[4]`, with `top[4] = UiViewTop() ^ (UiViewKey() * 2654435761u)`.

Most new views need **no key at all**: encounter tables, learnsets, base stats and the type chart are static data that changes only under their own touch handler, which already marks dirty. For the ones that do, the rule is that a key must be O(what is on screen), not O(what is in the data).

The PC box case is the one that looks expensive and is not. `GetMonData` decrypts in place (`src/pokemon.c:3746`), so hashing 420 species reads would be 420 decrypt round trips. But `MON_DATA_PERSONALITY`, `MON_DATA_OT_ID` and `MON_DATA_SANITY_HAS_SPECIES` sit **before** `MON_DATA_ENCRYPT_SEPARATOR` (`include/pokemon.h:8-19`), so they answer from the plaintext header. Hashing 30 visible `personality ^ otId` pairs is 60 plain loads and zero decryptions. Write that rule into `ui_view.h`.

The 6-mon party loop in the hash, which this section once counted as a win for the refactor, already runs only where the party shows: the PARTY tab and BAG's target picker (`bottom_screen.c:648`). With views, it can move into those views' own `key`, but that is tidiness, not a saving. (`UiPartyTick()` takes the tab's visibility as an argument and returns immediately off screen, which it had to once the mon icons started animating on it.)

**A repaint is cheaper than this section first assumed.** Since `040609a` every drawing primitive widens a dirty row band (`UiTouchRows`), and the host uploads only that band, in 8-row strips. A view that repaints a small area uploads a small area. But `Redraw()` still clears and paints the whole screen, so a key that changes too often still costs a full paint. The rule above stands.

### Build-script hazard, worth fixing regardless

`3ds/build_objs.sh:120` globs `3ds/ui/*.c` non-recursively and writes `$OBJ/$(basename).o` into the same object directory as all of `src/*.c`, with `3ds/ui` globbed last. A file named `3ds/ui/pokedex.c` would silently overwrite `src/pokedex.o` and delete the game's Pokédex from the archive. This becomes likely the moment view files get named after game features.

Mitigation: mandatory prefixes, `ui_*` for shared, `tab_*` for the six tab roots, `view_*` for pushed views, and a comment in `build_objs.sh` saying why so nobody tidies the names later. Keep the directory flat; subdirectories are silently not compiled. The cheatsheet documents the hazard (section 12, "Object-name collision hazard"), and `build_objs.sh` now says it too, next to the glob. The files added since (`ui_card.c`, `ui_link.c`, `ui_team.c`) follow the `ui_*` prefix. Three files still do not: `bottom_screen.c`, `matchup.c` and `status_tags.c`. None of them collides with a `src/` name today.

---

## Part 3: Build order

| Step | Work | Verification |
|---|---|---|
| 0 | ✅ **Shipped, without the registry or the title bar** (see Part 2). `ui_view.c/.h`. Convert the modals (PARTY detail, DEX entry, BAG picker, MAP's encounters view, and LINK's card view) to pushed views. | Open PARTY detail, tap BAG, tap PARTY: you get the grid, not the detail. Same for DEX entry, the BAG picker, the encounters view and the card view. |
| 1 | ✅ **Shipped.** `UiClipPush/Pop`, `UiTextClipped`, `UiTextWrapped`. ~100 lines. Before any new view, because retrofitting clipping into twelve views is the retrofit to avoid. | Nothing on the existing six tabs changes by a pixel. |
| 2 | `ui_widgets.c` with exactly four widgets. Prove each on two existing callers on day one: `UiButton` on `tab_extra.c` and `tab_map.c:554`; `UiChoiceRow` on a button row and a check row; `UiList` on `tab_dex.c` and `tab_bag.c`, retiring bag's inferior scroll model. | Existing tabs behave identically; bag's list now keeps the selection when scrolling. |
| 3 | HOME. `tab_extra.c` becomes `tab_home.c` (4x3 tiles) plus `view_settings.c` / `view_tweaks.c` / `view_audio.c`. First non-modal use of the stack; `UiGrid` gets its first caller. | All EXTRA controls still reachable and still persist. |
| 4 | **Trainer card and records** (#3). Cheapest of the three chosen features: the card is `UiCardDraw` from a locally filled card, and the records are almost entirely `UiField` rows. It can also ship before step 0, as a sub-view the way LINK's cards did. Ships with `key == NULL`, confirming the "most views need no key" claim. Adds `UiStatBar` if the badge row wants it. | Every figure matches the in-game trainer card; money and game stats read through `GetMoney`/`GetGameStat`, never raw. |
| 5 | **Berry and daycare trackers** (#4, #5). Two small views, both on the cheap key tier (a handful of scalar reads). First real repaint keys under the new scheme. | Berry stages and timers match the in-game trees; the day-care view agrees with the attendant's dialogue about egg readiness. |
| 6 | ✅ **Shipped, except the outbreak and roamer.** **Area encounters, the rest** (#1). Method chips, level ranges and chances landed in `view_encounters.c` without the widget layer; the chips use the local button idiom, and step 2 can move them onto `UiChoiceRow`. What is left: the mass outbreak (`gSaveBlock1Ptr->outbreak*`) and the roamer, whose location is private to `src/roamer.c`. | List matches the game's own tables for several maps, with the randomiser both off and on. A map with no water and no fishing shows empty methods rather than stale rows. |
| 7 | **Quick-use item ring** (#11). First write path. A second entry point into `CanUseItemNow()` + `ItemTargeting()` (`3ds/ui/tab_bag.c:152`, `:181`), which are already proven. The quick-throw strip (`ui_quickball.c`) is not this item: it offers balls, only in battle, and is not registered by the player. It is the proof that a second entry point into the bag's gates works. | Registered items work from every tab; all four gates still refuse during a battle, a script, locked field controls, or a non-overworld callback. |
| 8 | ✅ **SHIPPED.** **Battle effectiveness badges** (the read-only half of #10). An offence arrow per move per opponent in the corner of each move row, and the exact multiplier in the tapped move's panel (`DrawMoveMarks`, `DrawMoveInfoMultiplier` in `3ds/ui/tab_party.c`). The multiplier is `UiMatchupMove` (`3ds/ui/matchup.c`), which follows `Cmd_typecalc`: Hidden Power's IV type, Weather Ball's weather type, Levitate, Foresight, and fixed-damage moves as immune-or-neutral only. The grid's per-mon arrow now uses it too. | Badges agree with actual damage in a real battle, including Normal against Ghost (the `TYPE_FORESIGHT` case `matchup.c` already handles). |
| 9 | **Touch move selection** (the write half of #10). Highest-risk item in the plan; deliberately last. Follows `Ctr3dsQueueBattleItem` (`src/battle_controller_player.c:279`) exactly: require `gBattlerControllerFuncs[player] == HandleInputChooseAction`, reject link/frontier/recorded battle types, save and restore `gActiveBattler` and `gBattlerInMenuId`. | A tapped move resolves identically to the same move chosen with the D-pad, in singles and doubles, and is refused outright in link and frontier battles. |

**Gap worth naming.** None of the three chosen first features stresses `UiGrid` or a long scrolling list hard. HOME's 4x3 launcher (step 3) is therefore the only `UiGrid` proving ground before several views pile on it, and the read-only PC box viewer (#2) is the real structural stress test: 14-row list, 30-cell grid, pushed detail, windowed plaintext key, and long player-authored box names through `UiTextClipped`. Recommend scheduling it immediately after step 6 rather than leaving it until the widget layer has calcified around easier callers.

**Later, in rough value order:** PC boxes read-only (#2), learnset and evolution preview (#7), type matchup chart (#8), clock and counters (#9), then Tier 3.

### Deliberately not built yet

Two-level tab bar (costs 48px of 192; every tab, the encounters view, the card view and all three overlays are fitted to `UI_CONTENT_H`). Dirty-rectangle invalidation in the painters (the hash gate already makes repaints rare; invalidation bookkeeping would infect every widget; per `AGENTS.md`, measure on hardware before optimising). The host side of this exists since `040609a`: it uploads only the rows that were drawn into. What is not built is a paint that redraws only part of a view. Heap-allocated views, a scene graph, or transition animations (the whole UI layer is deliberately heap-free). A generic pixel-scrolling viewport (row-granular is enough; the type chart pages by attacking type rather than scrolling). Press feedback and long-press, which belong to `UI_SKIN_PLAN.md` step 5 and would confound measurement of steps 0-2. Persisted per-view state, which `tab_extra.c:176` already argues against for its own page number.

---

## Part 4: Interaction with UI_SKIN_PLAN.md

Recommendation is structure first, skin second. The skin is a deliberately small diff ("two functions change, ~98 call sites do not") and doing the structure first makes it smaller, not larger.

1. `DrawButtonH` moves out of `tab_extra.c` into `UiButton`. The skin plan's step 5 defines `UiButton` and folds in both local helpers, `DrawButtonH` (`tab_extra.c:182`) and `DrawBtn` (`tab_map.c:554`), plus about a dozen inline outlines; whichever plan lands first owns it, and if `ui_widgets.c` exists by then it lives there, so it covers every future view's buttons for the same edit.
2. `tabbar.png` is specced at 64x96 (cell idle, cell active, bar ground). A title bar also needs a back chevron and a title ground. **Decide before the art is drawn** or it gets authored twice. Suggest 64x160. `icons.png` holds one icon for each root tab (six today), so where LINK goes (Part 2) changes that sheet too.
3. **Highest-risk item:** the skin plan says `ui_gfx.c`'s entry points clamp against `0..UI_W/UI_H` "the way `UiFillRect` already does". After step 1 `UiFillRect` clamps against the *clip*. `UiBlitPart`, `UiTileFill` and `UiNineSlice` must do the same, or a nine-slice panel inside a clipped list row paints over its neighbours and looks like a layout bug rather than a blitter bug.
4. Both plans edit `UiStateHash`: the skin drops `top[0] = UiFrameId()`, this drops the party loop and rewrites `top[4]`. Different lines, mergeable, but do them in one sitting.
5. Create `ui_skin.h` in step 2 even as a pure re-export, so the skin's colour move is one `#include` change.

One ordering constraint. This plan leaves `UI_TABBAR_H 48` and `UI_CONTENT_H 192` alone, but the skin plan no longer freezes them: its `shell.png` wireframe may move both. The title bar reuses whatever band the tab bar ends up with, and the launcher's 4x3 grid of 80x64 fills 320x192 exactly on whole tiles only at today's values. So settle `shell.png` before sizing the launcher and the title bar, and derive both from `UI_CONTENT_H` / `UI_TABBAR_H` rather than from 192 and 48.

---

## Verification

- Build: `make tools && make generated`, `python3 tools/generate_wasm_assets.py`, `3ds/build_objs.sh`, `make -C 3ds`. Both mixer configurations still link.
- Regression on the existing six tabs after steps 0-2: no pixel changes, all controls still reachable, settings still persist across a restart.
- Modal bug fix: the tab-switch sequence above.
- Text safety: nickname a party member to eight wide characters (`WWWWWWWWWW`) and confirm it ellipsises rather than overrunning its cell. Same for a renamed PC box.
- New views checked against the game's own screens, each against its authoritative source: trainer card against the in-game trainer card and the Pokédex counts; berry stages against the trees themselves; day-care against the attendant's dialogue; encounter list against the real tables on several maps, with the randomiser both off and on.
- Write paths (steps 7 and 9) verified against their gates, not only their happy paths: confirm each is refused during a battle, a script, locked field controls, and a non-overworld callback, and that a tapped move resolves identically to the same move chosen with the D-pad.
- On hardware, not only in an emulator: the audio health report at frame 600 and the boot log stay clean, and repaint frequency has not visibly risen (`AGENTS.md` on measuring on the device).

## Open question for later

The catalogue is deliberately larger than the first slice. Once steps 0 to 3 are in, adding a Tier 1 view should cost roughly 150 lines and one afternoon, so the remaining items become a menu to pick from rather than a commitment. Revisit PC deposit/withdraw and save-anywhere once the read-only box viewer has shipped and its gating has been exercised.
