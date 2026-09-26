# DS/3DS-style second screen: battle panel, Pokétch tiles, PokéNav Plus

## Context

The user picked three groups of DS/3DS-style features: finish the battle panel,
Pokétch-style apps on HOME, and PokéNav Plus features on MAP. They asked that
everything use the game's own safe calls as much as possible, and that the plan
be saved in the repo.

**Rule for every item: call the game, do not copy it.**
- A public read-only function: call it.
- A useful function that is `static` in its file: add a thin
  `#if PLATFORM_3DS` wrapper *next to it* in that file that calls it, and
  declare the wrapper in the file's header. No copies of game logic in
  `3ds/ui/`.
- Copy only when the game's function writes state or advances the RNG, and say
  so at the copy. Known cases: `ShouldEggHatch` (writes the step counter and egg
  cycles), `CalcBerryYield` (`Random()`), the Itemfinder's search
  (writes `gTasks[taskId]`), `TrySwitchInPokemon` (prints to the party menu).
- Never call: `TryUpdateRandomTrainerRematches`, `UpdateRematchIfDefeated`,
  `RoamerMove*`, `Start/EndMassOutbreak`, `BerryTreeTimeUpdate`,
  `GetDaycareCompatibilityScoreFromSave` (writes `gSpecialVar_Result`).

**Step 0:** save this plan as `3ds/DS_STYLE_PLAN.md` and commit it. Per the
plan lifecycle memory, delete it once all phases ship; anything left moves to
ROADMAP Open items.

Three phases, each its own commits, each compile-checked (clang, every
`3ds/ui/*.c` and changed `src/*.c`, plus `clang -E -P` proof that every changed
`src/` file is unchanged with `PLATFORM_3DS=0`).

---

## Phase A: finish the battle panel (`3ds/ui/view_battle.c`)

**Layout change** (to fit a foe header and the BAG/RUN column):

```
 0..96    frame: foe header (y 8..24, one line per foe), 2x2 moves of 30px
 96..152  frame: party row, 6 cells of 42px, then a BAG / RUN column (50px)
 152..192 message line (under the quick-throw strip in wild battles)
```

1. **RUN.** Emit `B_ACTION_RUN` from action selection with the same gate as
   moves (`Ctr3dsQueueBattleRun` in the controller's 3DS block). No follow-up
   answer is needed. The engine keeps its own checks and messages
   (`src/battle_main.c:4379-4404`: forfeit prompt in the Frontier, "No! There's
   no running from a trainer battle!", `IsRunningFromBattleImpossible`).
2. **BAG.** Switches to the BAG tab, which already does battle items. Needs a
   public `UiSetTab(u8)` in `bottom_screen.c` that goes through `LeaveTab`.
3. **Opponent card.** Tap a foe's header line: the move area shows its name,
   level, types, an HP bar (no numbers, as the game shows a foe), status, stat
   stages and battle conditions. Reads `gBattleMons[foe]` (`statStages`,
   `status2`) and `gStatuses3[foe]`. Only stages that are not
   `DEFAULT_STAT_STAGE` show, as "ATK +1  SPE -2"; conditions as words
   (confused, seeded, substitute, trapped, cursed...).
4. **Your stat stages.** The party card of a Pokémon that is out shows the same
   stage line, from `gBattleMons[its battler]`.
5. **Faint replacement by touch** (user choice: pick first, menu on A). Hook in
   `PlayerHandleChoosePokemon` for `PARTY_ACTION_SEND_OUT` (3DS only): enter a
   new controller state `Ctr3dsWaitSendOutChoice` instead of fading. In it:
   - a pending touch pick answers exactly as the switch path does
     (`SwitchPartyMonSlots`, `BtlController_EmitChosenMonReturnValue`), after
     `Ctr3dsCanSwitchBattlerTo` passes;
   - any button press continues into the game's own path unchanged (create the
     task, start the fade, `OpenPartyMenuToChooseMon`).
   The panel shows "Choose a POKéMON to send out" and the party row takes the
   pick; `Ctr3dsBattleChoosingBattler` reports this state too, with a flag so
   the panel knows it is a send-out. Cancel is not offered, as the game's menu
   refuses it on send-out.

Gates: every new emit uses the existing pattern (choosing-battler check,
`gActiveBattler` set and restored, pending cleared in
`PlayerHandleChooseAction`).

---

## Phase B: Pokétch tiles on HOME

Five new HOME tiles, each a page in the launcher (`tab_extra.c` page enum,
before DEBUG), drawn by its own `view_*.c` like LINK. Launcher order puts the
player-facing ones first: TRAINER, CLOCK, DOWSING, BERRIES, DAY CARE, LINK,
SETTINGS, GAMEPLAY, EXTRAS, FOLLOWER, (DEBUG). Eleven of twelve cells.

6. **TRAINER** (`view_trainer.c`). The player's own card, front and back,
   tap to flip: `TrainerCard_GenerateCardForLinkPlayer` (`src/trainer_card.c:778`,
   writes only the struct passed) into a local card. `ui_card.c`: split
   `UiCardDraw` into `UiCardDrawCard(x, y, const struct TrainerCard *, name,
   back)`, with `UiCardDraw` a wrapper, so the art code is shared. A RECORDS
   button shows `GetGameStat` values: steps, battles, captures, eggs hatched,
   trades, shopping, and play time.
7. **CLOCK** (`view_clock.c`). `Ctr3dsGetClock`, play time from
   `gSaveBlock2Ptr`, `GetGameStat(GAME_STAT_STEPS)`,
   `VarGet(VAR_REPEL_STEP_COUNT)`, and for each egg in the party the steps left:
   cycles (`MON_DATA_FRIENDSHIP`) x 256 minus the day care's step counter,
   divided by `GetEggCyclesToSubtract()` (public, `src/egg_hatch.c:926`). Shown
   as "about N steps", because the counter wraps at 255 the first time.
8. **DOWSING** (`view_dowsing.c`). Only if the bag holds the Itemfinder
   (`CheckBagHasItem(ITEM_ITEMFINDER, 1)`), as in the game. A radar of the
   Itemfinder's own 15x11 range around `PlayerGetDestCoords`, blips for hidden
   items not yet found. Wrappers in `src/item_use.c`'s 3DS block around its
   static `IsHiddenItemPresentAtCoords` and `IsHiddenItemPresentInConnection`,
   so connected maps are searched the game's way. The loop over the range is
   ours, because `ItemfinderCheckForHiddenItems` writes a task.
9. **BERRIES** (`view_berries.c`). Every planted tree: berry name
   (`GetBerryInfo`), stage (`GetStageByBerryTreeId`), time to the next stage
   (`minutesUntilNextStage`), waterings (wrapper around the static
   `BerryTreeGetNumStagesWatered`), and place. The place: tree ids live in each
   map's object events, so build a 128-entry id -> mapsec table once, on first
   open, by walking `gMapGroups` headers' object events with
   `GetObjectEventBerryTreeId`-style reads (read only, cached in `.bss`).
   Never `CalcBerryYield` (RNG).
10. **DAY CARE** (`view_daycare.c`). `GetDaycareState` (public) for the
    headline; the two mons with the level they would have now, from a wrapper
    around the static `GetLevelAfterDaycareSteps` (works on a copy); how they
    get along, from a wrapper around the static `GetDaycareCompatibilityScore`
    (pure), in the old man's words; "An EGG is waiting" when it is.

---

## Phase C: PokéNav Plus on MAP (`tab_map.c`, `view_encounters.c`)

11. **Rematch marks.** For each `gRematchTable` entry with
    `gSaveBlock1Ptr->trainerRematches[i] != 0`, mark its mapsec
    (`Overworld_GetMapHeaderByGroupAndId(...)->regionMapSectionId`). Public
    `DoesSomeoneWantRematchIn` answers per map. A tapped route with rematches
    says "N want a rematch" in the caption where a landmark would go.
12. **Roamer.** If `gSaveBlock1Ptr->roamer.active` and the player has seen its
    species (the Pokédex area screen shows it on the same terms), mark its
    mapsec from the public `GetRoamerLocation`. The encounter list of that place
    gets it at the top of LAND and SURF, tagged ROAMING, with its level.
13. **Mass outbreak.** When `outbreakPokemonSpecies != SPECIES_NONE` (set only
    after the player watched the TV news), mark its map's mapsec, and the
    encounter list of that place gets it in LAND, tagged OUTBREAK, with
    `outbreakPokemonLevel` and `outbreakPokemonProbability`%.

Markers: small shapes drawn per mapsec with the `DrawPick` geometry
(`gRegionMapEntries`), before the player icon, one colour per kind, and a tiny
legend in the caption when any is on screen. Repaint key: the three marker sets
change without a touch (a rematch appears, the roamer moves), so fold a cheap
key of them into `UiMapStateKey`.

---

## Files

- `src/battle_controller_player.c`, `include/battle_controllers.h` (A)
- `src/item_use.c`, `src/berry.c`, `src/daycare.c` (+ their headers): small
  `#if PLATFORM_3DS` wrappers only (B)
- `3ds/ui/view_battle.c`, `bottom_screen.c` (`UiSetTab`), `ui_card.c`,
  `tab_extra.c`; new `view_trainer.c`, `view_clock.c`, `view_dowsing.c`,
  `view_berries.c`, `view_daycare.c`; `tab_map.c`, `view_encounters.c`
- Docs: README, cheatsheet file map, SECOND_SCREEN_PLAN catalogue (#3, #4, #5,
  #9, #1 outbreak/roamer), `3ds/DS_STYLE_PLAN.md` itself.

## Verification

- Compile checks and `PLATFORM_3DS=0` preprocessor identity for each `src/`
  file touched, per phase.
- A: RUN in a wild battle escapes; in a trainer battle shows the game's
  refusal; Wobbuffet/Wrap shows "can't escape". BAG jumps to the tab. Foe card
  shows stat drops after Growl/Leer. Faint: the panel asks, a tap sends out the
  chosen mon; pressing A instead opens the game's menu as before.
- B: TRAINER card matches the game's own trainer card; CLOCK egg steps count
  down while walking; DOWSING blips disappear when the item is picked up and
  match the Itemfinder's direction; BERRIES matches the trees on the routes;
  DAY CARE agrees with the old man's words and levels.
- C: a registered trainer who calls for a rematch appears on MAP; Latios/Latias
  mark moves after each map change; a TV outbreak marks its route and shows in
  its WILD list.
