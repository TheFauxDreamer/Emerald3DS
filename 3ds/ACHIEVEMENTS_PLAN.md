# Achievements plan

**Status: proposed, not started.** Written 2026-09-12 against `04aebdd`, and
re-checked 2026-09-13 against `480dc6b`, after the second-screen animation
stages ([SECOND_SCREEN_ANIMATION_PLAN.md](SECOND_SCREEN_ANIMATION_PLAN.md)).
Every line reference below was read against that tree; re-check them before
starting.
Companion documents: [SECOND_SCREEN_CHEATSHEET.md](SECOND_SCREEN_CHEATSHEET.md)
(sections 5, 7, 10, 13 and 14 above all) and
[SECOND_SCREEN_PLAN.md](SECOND_SCREEN_PLAN.md), whose Tier 3 already lists
"achievement medals off `GetGameStat`".

## Context

The request: "native, basic (not hardcore) RetroAchievements", an achievements
page on the bottom screen, and an unlock notification that appears over
whichever tab is open. The research below found that real RetroAchievements is
possible but costly and unofficial for this port. **Decided: built-in
achievements behind an RA-ready provider interface, on a sixth bottom-screen
tab.**

## Research summary (why not real RA now)

- RA accepts **Casual (formerly softcore) unlocks from unrecognised clients**. The
  rc_client wiki says hardcore unlocks are "turned into softcore" when the
  User-Agent is unknown. RA staff say decomp/PC ports are not supported.
- Precedent: **HayatoG/tmc** (Minish Cap decomp, native Switch) ships rc_client in
  softcore and it works on hardware (`port/port_retroachievements.c`). It relies
  on two things this port lacks:
  1. **A user-supplied ROM to hash.** Emerald3DS has no ROM, so it would have to
     claim the retail hash.
  2. **Game RAM at retail offsets, passed straight through.** Here
     `EWRAM_DATA`/`IWRAM_DATA` are plain `.bss` (`include/gba/defines.h:10-17`).
     RA's Emerald set follows `gSaveBlock1Ptr`/`gSaveBlock2Ptr`, which move on
     every warp (`src/load_save.c:77`), so it would need a retail-address
     translation layer that rewrites pointer values. Building it needs a GBA
     `.map`, and CI has no agbcc.
- Other costs: curl + mbedtls portlibs (only citro2d/citro3d/ctru are linked,
  `3ds/Makefile:114`; the `.rsf` already grants `http:C`/`soc:U`/`ssl:C`), a
  network thread, swkbd login, and text-only badges because there is no libpng.
- The provider interface below is where an RA provider would plug in later.
  Nothing else in the UI would change.

## Design

### Layers

```
3ds/achievements.{c,h}      game side  definitions, evaluation, provider interface (local provider)
3ds/ui/tab_trophy.c         game side  the TROPHY tab (list + progress header)
3ds/ui/ui_achtoast.{c,h}    game side  unlock toast overlay (Active/Draw/Touch/StateKey/Tick shape)
3ds/host/achievements.c     host side  per-playthrough store on SD, flushed by the I/O thread
3ds/bridge.h                seam       store load/save + flush declarations (stdint only)
```

### Provider interface (`3ds/achievements.h`), RA-ready

```c
struct AchView { const char *title, *desc;      // ASCII, UI converts with UiAscii()
                 u16 progress, goal;             // goal 0 = no counter
                 u8 unlocked, unseen; };
struct AchProvider {
    u16   (*count)(void);
    void  (*get)(u16 i, struct AchView *out);
    u16   (*unlockedCount)(void);
    bool8 (*anyUnseen)(void);
    void  (*markAllSeen)(void);                 // called when the TROPHY tab is shown
    bool8 (*popToast)(u16 *index, u16 *batch);  // next toast; batch>1 = "N unlocked"
    u32   (*stateKey)(void);                    // for UiStateHash
};
const struct AchProvider *AchActive(void);      // &gAchLocal for now
void AchTick(void);                             // per displayed frame, from CtrBottomUpdate
```

The tab and toast only ever call `AchActive()`. A later RA provider would be a
game-side adapter over new bridge calls into a host-side rc_client. It would
bring its own per-logical-frame `rc_client_do_frame` hook in
`Rp2350PresentFrame`.

### Definitions and evaluation (`3ds/achievements.c`, modelled on `3ds/tweaks.c`)

- `static const struct AchDef sAchDefs[]` = { title, desc, kind, arg, goal }. The
  kinds are: `ACH_FLAG` (`FlagGet`), `ACH_FLAG_ANY`/`ACH_FLAG_ALL` (a flag range,
  for Frontier symbols), `ACH_STAT` (`GetGameStat`, `include/overworld.h:63`),
  `ACH_DEX_HOENN` (`GetHoennPokedexCount(FLAG_GET_CAUGHT)`), `ACH_DEX_NATIONAL`,
  `ACH_HOENN_COMPLETE` (`HasAllHoennMons`), and `ACH_EVENT` (set by a hook).
- **The bit index is the table index, and the table is append-only.** It is
  stored on the card (128-bit capacity), so reordering entries would move saved
  unlocks.
- **Round-robin**, one definition per `AchTick()`. The cost is constant and the
  latency is under a second for about 50 entries. Dex counts walk the dex only on
  their own turn.
- **Adopting a playthrough (important pitfall).** The key is
  `T1_READ_32(gSaveBlock2Ptr->playerTrainerId)` (the same read `3ds/tweaks.c:423`
  uses). A new ID is adopted **only on a frame where
  `gMain.callback2 == CB2_Overworld`**. A New Game sets the trainer ID during
  Birch's speech, while the previous save's flags are still loaded until
  `NewGameInitData` runs. Adopting early would copy the old playthrough's unlocks
  onto the new one. Everything is also gated on `gSaveBlock1Ptr/2Ptr != NULL`
  (the `SaveDataLive()` rule, cheatsheet section 10).
- **Backfill.** The first time a playthrough has no stored record, every
  condition already true is unlocked quietly, and one summary toast says "N
  unlocked from your save". After that, each new unlock gets its own toast,
  queued in a small ring and shown one after another.
- **One event hook.** "Catch a shiny" is a fenced `#if PLATFORM_3DS` line in
  `Cmd_givecaughtmon` (`src/battle_script_commands.c:10094`). It calls
  `Ctr3dsAchOnCaught(mon)`, which checks `IsMonShiny` and sets that
  achievement's event bit. This is the same pattern as the existing tweak hooks
  in that file.

### Initial set (about 50)

| Group | Entries | Condition |
|---|---|---|
| Badges | 8, one per gym | `FLAG_BADGE01_GET`..`08` |
| Story | Champion, National Dex, Wally at Victory Road | `FLAG_SYS_GAME_CLEAR`, `FLAG_SYS_NATIONAL_DEX`, `FLAG_DEFEATED_WALLY_VICTORY_ROAD` |
| Legends | Rayquaza, Groudon, Kyogre, the three Regis, Latias/Latios, Sudowoodo | `FLAG_DEFEATED_*` (resolved by catch or KO, so they also work with the randomiser on) |
| Dex | Hoenn caught 25/50/100/150/200, Hoenn complete, National 300 | dex counts |
| Stats | first egg, 30 eggs, 50 evolutions, 100 trainer battles, first catch, 100 catches, first fish, contest win, 10 ribbons, jackpot, 100k steps, Splash, cable car, hot springs, Safari, 25 berries planted, 25 Pokeblocks, secret base, lottery win, Battle Tower streak 7 | `GetGameStat(GAME_STAT_*)`, with a progress counter |
| Frontier | any Silver, all Silver, any Gold, all Gold | `FLAG_SYS_TOWER_SILVER`..`FLAG_SYS_PYRAMID_GOLD` |
| Event | Catch a shiny | hook above |

Titles must fit 150px and descriptions 288px. There is no clipping or wrapping
(cheatsheet section 9), so the implementation checks them with `UiTextWidth`
once, on the debug page.

### Persistence (host: `3ds/host/achievements.c`, copying `3ds/host/settings.c`)

- File `sdmc:/3ds/emerald3ds/achievements.bin`: header {magic `'E3AC'`, version,
  count}, then up to 8 records {u32 playerId, u32 lastUsed, u8 unlocked[16], u8
  unseen[16]}, replacing the least recently used. About 330 bytes, **every byte
  spoken for with explicit padding** (the settings.c rule). It is opened once and
  rewritten in place, with no temp-and-rename (the same one-sector reasoning as
  `settings.c`). A bad magic or version means "no records". It is never fatal.
- Bridge (stdint only):
  `int CtrAchStoreLoad(uint32_t id, uint8_t *unlocked, uint8_t *unseen, int n)`
  returns 1 if found, and
  `void CtrAchStoreSave(uint32_t id, const uint8_t *unlocked, const uint8_t *unseen, int n)`
  copies the record in and marks it dirty.
- The write discipline is copied exactly: `CtrAchFlush(force)` sits beside
  `CtrSettingsFlush` at all three call sites in `3ds/host/main.c` (the per-frame
  one at about :742 and the two close paths). `CtrAchDrain()` is added to
  `io_main` and to `CtrIoExit` (`3ds/host/io_thread.c`) and declared in
  `io_thread.h`. Load at boot beside `CtrSettingsLoad()` (main.c about :772). Add
  `host/achievements.c` to `HOST_SRCS` (`3ds/Makefile:54`). Both sides'
  `achievements.o` are safe: the game objects go to `3ds/build/obj` and the host
  objects to `3ds/build`.

### TROPHY tab (`3ds/ui/tab_trophy.c`)

- Shell wiring (cheatsheet "Adding a tab"): `UI_TAB_TROPHY` goes **before**
  `UI_TAB_EXTRA` in `enum UiTab` (`ui_shell.h:19`), so EXTRA stays rightmost. Add a
  `{ "TROPHY", 0 }` row to `sTabs[]`, which is always available like BAG and
  EXTRA. Add a `case` to both switches in `bottom_screen.c` (Redraw and
  `CtrBottomUpdate`), declare `UiTrophyDraw/Touch/StateKey` in `ui_shell.h`, and
  add a `top[4]` branch for `UiTrophyStateKey()`.
- Layout (320x192): a `UiWindowFrame` panel with a header ("ACHIEVEMENTS 12/51"
  and a thin gold progress bar in `UI_COL_SHINY`), then a list of 2-line rows at
  32px each. A row has a `UiSparkle` when unlocked (dim when locked), the title in
  theme text (dim when locked), the description dimmed, a right-aligned "37/50"
  counter, and a gold `NEW` tag while unseen. PAGE up/down buttons use
  `UiHoldRepeat` like DEX (`tab_dex.c`, `PAGE_*` constants). Definition order is
  kept so the list stays learnable.
- **NEW tags survive the visit they are seen on.** `UiTrophyTick(bool8 visible)`
  follows the `UiPartyTick(visible)` shape and is called every frame from
  `CtrBottomUpdate`. When the tab goes from hidden to visible, it copies the
  provider's unseen bits into a file-static mask, then calls `markAllSeen()`.
  Rows draw NEW from that mask, which is cleared when the tab is left. Marking
  seen inside Redraw instead would flip `top[9]` and repaint the tags away one
  frame later.

### Toast overlay (`3ds/ui/ui_achtoast.{c,h}`), the third overlay

- **Geometry: 40x5 tiles at TY 0, which is y 0..40.** It abuts the shiny notice
  (y 40..152) exactly, and the quick-throw strip (y 152..192), so all three can
  be up with no overlap. This goes into cheatsheet section 14's load-bearing
  constants.
- Content: dark ground with a gold rule (the `DrawNotice` idiom), a sparkle,
  "ACHIEVEMENT UNLOCKED" small, the title, and a **VIEW** button that switches to
  the TROPHY tab. The toast follows the four overlay rules: it paints after the
  tab, takes touches on its whole rect first, has a real control, and **expires
  on its own** after about 240 displayed frames (counted in `UiAchToastTick()`
  calls, the `UiHold` idiom, so it lasts the same time under fast-forward). While
  the TROPHY tab is on screen VIEW is neither drawn nor tappable.
- **What it costs depends on the path.** The toast itself is static: one repaint
  to appear and one to go. Over the PARTY grid on the second-core path, though,
  `CtrBottomUpdate` turns every party step into a full repaint while any overlay
  is up (cheatsheet section 5), so a 4-second toast there is about 40 full
  repaints. The cheatsheet's "What a repaint costs on the second-core path"
  measured that case at `59a0ba6`: about 4.1 ms of paint plus 1.6 ms of upload,
  finishing about 1 ms past the join, with no missed VBlank. On the single-core
  path the party is frozen under the toast instead, and nothing repaints for it.
- **Optional, second-core path only: open with the notice's glint.** `DrawSweep`
  would take a rect instead of the `NOTICE_IN_*` constants, and the toast would
  follow `NoticeTick`'s rule: a full repaint on each glint frame plus one more
  after the last, so the band leaves the snapshot (cheatsheet section 7). The
  single-core path skips it, since a sweep cannot survive the 12-frame clock.
- Shell wiring in `bottom_screen.c`:
  - `UiOverlayActive()` also returns TRUE for the toast, because the party grid's
    top-row icons sit inside y 0..40 (`CellTop(0)` is 0, or 24 with the cheat
    tag strip). `DrawCell` then bakes them via `OverlayIconFrame()`
    (`tab_party.c:323`).
  - `DrawAnimatedLayer` skips the party redraw while the toast is up, the same as
    for the strip: the party branch at `bottom_screen.c:887` gains
    `!UiAchToastActive()`.
  - The single-core branch in `CtrBottomUpdate`, `else if (!UiQuickBallActive())`
    at `bottom_screen.c:1044`, gains `!UiAchToastActive()` too. Otherwise it asks
    for cheap redraws that draw nothing for the party and upload an unchanged
    screen. The second-core branch at `:1031` needs no change, because it
    already asks `UiOverlayActive()`.
  - The toast inherits the single-core path's stalled bar: a bar that starts
    sliding under an overlay stalls until the next full repaint (the "How it
    landed" note in the animation plan). Here that lasts at most the toast's 4
    seconds, since its expiry repaints. That note's suggested fix, one full
    repaint when a slide under an overlay finishes, would cover the toast as
    well.
  - `Redraw` draws it after the notice. `CtrBottomUpdate` adds a hit-test branch
    beside the notice and strip, and calls `AchTick()` then `UiAchToastTick()`
    every frame while `sInGame`.
  - `UiStateHash` grows `top[8]` to `top[10]`: `top[8] = UiAchToastStateKey()` (0
    while down) and `top[9] = AchActive()->stateKey()` (unlocked count plus the
    any-unseen bit). Each gets a slot of its own (`top[7]` is now the title
    screen's key, from commit 04aebdd).
- **Unseen marker on the tab bar:** `DrawTabBar` draws a small gold dot at the
  TROPHY label's top right while `anyUnseen()`. This covers a toast that was
  missed or expired; `top[9]` keeps it current on every tab.

### Debug page (EXTRA, only with `CTR_DEBUG_MENU`)

- **ACH TEST** queues a fake toast using the first definition's title, the tmc
  idea. **ACH RESET** clears the current playthrough's record. Neither persists a
  setting, so neither needs neutralising. The same page runs the width check.

### Docs

- Cheatsheet: section 5 (a third overlay and the sixth tab), section 7 (the
  `top[8]`/`top[9]` rows), section 14 (the y 0/40/152 split), and a pitfall row
  for "adopt only on CB2_Overworld".
- README feature table: one row.
- ROADMAP: note the RA provider as future work, with the blockers listed above.

## Build and commit order

1. Host store + bridge + io_thread and Makefile wiring. Check that it compiles
   and that the boot logs the load.
2. `achievements.c` definitions, evaluation and backfill, plus the
   `Cmd_givecaughtmon` hook. Add `3ds/achievements.c` to the `build_objs.sh` loop
   next to `3ds/tweaks.c`.
3. The TROPHY tab.
4. The toast and tab-bar dot.
5. The debug controls and docs.

Rebuild with `3ds/build_objs.sh && make -C 3ds` (both, because `bridge.h`
changes). Commit after each verified step, following AGENTS.md.

## Verification

- Builds: devkitPro is not installed on the development Mac, so the `build-3ds`
  CI workflow is the build check and has to stay green. Syntax-check game-side
  files on the Mac with the clang command in the animation plan's Verification
  section. Host files (`3ds/host/*.c`) need CI. Build
  `make -C 3ds CTR_PPU_THREAD=0` once wherever the CIA is built, because CI does
  not build the single-core variant.
- Azahar, then the New 3DS XL:
  - **Existing save with badges:** loading it gives one "N unlocked from your
    save" toast, the TROPHY tab shows them unlocked with NEW tags, and the tags
    clear after viewing.
  - **Live unlock:** use ACH TEST, then earn a real one (Splash, or a first
    catch). The toast appears on PARTY, BAG, MAP, DEX and EXTRA, VIEW jumps to the
    tab, and the toast expires by itself. The dot stays until the tab is viewed.
  - **New Game over an existing save:** the old save's unlocks must NOT carry
    over (this checks the adoption gate). Soft reset and Continue: the old record
    comes back.
  - **Overlay stacking:** on the PARTY tab in a wild battle, a shiny (EXTRA shiny
    test) plus the quick-throw strip plus a toast (ACH TEST during action
    selection) must show three panels with no border overlap. On the
    second-core path the icons keep moving under all three. With
    `CTR_PPU_THREAD=0` they hold still at frame 0. On both paths nothing
    punches through a panel.
  - **Persistence:** unlocks and NEW tags survive a relaunch. A corrupted or
    truncated `achievements.bin` boots with defaults.
  - **Performance,** read against the animation plan's
    [Measured](SECOND_SCREEN_ANIMATION_PLAN.md#measured) baseline at
    `59a0ba6`. With `CTR_DEBUG_MENU` on and the stacking case above running,
    `log.txt` must show `frame` worst about 16.77 ms, no "missed VBlank" lines,
    and `ppu.wait` well above zero. There must be no `slow` lines on unlock (the
    write happens on the I/O thread). Read the upload on the console: Azahar at
    300% shows it at about a sixth of its real cost.
- Randomiser on: the legend and badge achievements still unlock.

## Sources

- [rc_client integration](https://github.com/RetroAchievements/rcheevos/wiki/rc_client-integration)
  (rcheevos wiki): User-Agent rules and the softcore demotion.
- [Hardcore compliance requirements](https://docs.retroachievements.org/general/hardcore-compliance-requirements.html)
  and [emulator support](https://docs.retroachievements.org/general/emulator-support-and-issues.html)
  (RA docs).
- [HayatoG/tmc](https://github.com/HayatoG/tmc), branch `switch-port`:
  `port/port_retroachievements.c` and `port/port_gba_mem.c`, plus issue #12 for
  the hardware validation notes.
- rcheevos `src/rcheevos/consoleinfo.c`: the GBA memory map (IWRAM at RA
  0x000000, EWRAM at 0x008000, save RAM at 0x048000).
- [RA forum: Ship of Harkinian support](https://retroachievements.org/forums/topic/32281)
  and [decompilation projects](https://retroachievements.org/forums/topic/31008):
  RA's stance on decomp ports.
