# Second screen cheatsheet

Working reference for changing the 3DS bottom screen. Read this before touching
anything under `3ds/ui/`. Companion documents: the root `README-TECHNICAL.md` (why
the port is built this way), `3ds/SECOND_SCREEN_PLAN.md` (a proposed refactor and feature
catalogue, **not implemented**), `3ds/UI_SKIN_PLAN.md` (a proposed reskin and
re-layout, **not implemented**). This file describes the code as it actually is.

---

## 1. The one-paragraph model

The bottom screen is **game-side C**. It is compiled with Emerald's own headers
into `libpokeemerald.a`, so `gPlayerParty`, `GetMonData`, `gItems`, the fonts
and the mon icons are ordinary symbols. Nothing scrapes RAM and nothing is
reimplemented. It paints into one static `320x240` RGB565 buffer
(`sFb`, [ui_draw.c:22](ui/ui_draw.c#L22)), and the host uploads that buffer to a
PICA200 texture only when the UI says it changed. There is no heap, no view
stack, no widget library, and no text clipping. Everything is a static in a
file, drawn with rectangles and blits at hand-measured coordinates.

---

## 2. Frame path

```
Rp2350PresentFrame()                 3ds/host/main.c       (end of every game frame)
  if (sSubFrame == 0)                                      once per DISPLAYED frame
     hidScanInput(), set_speed()
  presenting = (sSubFrame + 1 >= sSpeed)                   decided once
  if (presenting)
     CtrVideoRenderBegin()           3ds/host/video.c     snapshot video state,
                                                           start rasteriser on core 2/1
  if (sSubFrame == 0)
     sample_touch(&touch)            3ds/host/main.c:84
     CtrBottomUpdate(&touch)  -----> 3ds/ui/bottom_screen.c:922   OVERLAPS the rasteriser
                                       UpdateInGameLatch()
                                       tab-bar tap  OR  UiXTouch(touch)
                                       UiPartyTick()      HP bar animation
                                       UiStateHash()      poll for change
                                       Redraw() if needed -> paints sFb
  if (presenting)
     CtrVideoPresent()               3ds/host/video.c
        if (second core && CtrBottomIsDirty())             still OVERLAPS it
           upload(bottom); CtrBottomClearDirty()           WHOLE, 320x240
        wait for the rasteriser                           `ppu.wait`
        upload(top)                                       WHOLE, every frame
        if (inline)                                        single-core path only
           if (idle && CtrBottomIsDirty())
              snapshot_bottom(); CtrBottomClearDirty()     320x240 -> stage
           if (mid-run)
              upload_bottom_slice()                        48 ROWS, 5 frames
     CtrSaveFlush(0)                 3ds/host/save.c      the save image
     CtrSettingsFlush(0)             3ds/host/settings.c  hands settings.bin to
                                                           the I/O thread
```

The rasteriser runs on a second core (core 2 on a New 3DS, core 1 otherwise)
while `CtrBottomUpdate` paints, which is why a full repaint no longer costs a
frame. It reads a private copy of VRAM, palette, OAM and the registers taken
just before the paint, so **anything a touch handler changes in video memory
shows up one frame later**, never half-drawn. If no second core can be had, or
the build is `CTR_PPU_THREAD=0`, it rasterises inline inside
`CtrVideoPresent()`, which is the old single-core path.

With a second core, the bottom screen is uploaded **whole, before the join**.
`CtrVideoPresent()` sends it while the rasteriser is still running, which is
safe because the rasteriser never touches the UI's framebuffer or the bottom
texture, so its ~2 ms comes out of what would otherwise be `ppu.wait`, and a
repaint reaches the panel on the frame it was painted.
`Ctr3dsRasteriserOnOwnCore()` ([bridge.h](bridge.h)) is how the game side
knows which path it is on.

On the inline path the bottom screen is uploaded **a slice per frame, not
whole**. The top screen has to hold 60fps and there is no idle time to hide the
upload in, so a repaint reaches the panel five frames after it was painted. See
`upload` and `BOT_CHUNK_ROWS` in [video.c](host/video.c) for why 48 rows, and
section 7 for what it cost before.

The two flushes are at the bottom for a reason: this is after the frame has been
presented. The save flush still writes there. The settings flush only hands a
snapshot to the I/O thread (`3ds/host/io_thread.c`), which does the card write
while the main thread waits, and the log does the same. Nothing on the touch
path may write to the card itself -- see section 13.

Key consequences:

- `CtrBottomUpdate` runs **once per displayed frame**, not per game frame. Under
  fast-forward the UI still updates 60 times a second while the game runs faster.
- It runs at the **end** of a game frame, after `CallCallbacks` and after
  `VBlankIntr` (`src/main.c`). The frame's own callback has already finished,
  which is why replacing `gMain.callback2` from here is safe (see the fly path).
- `CtrBottomInit()` is called from `main()` at [host/main.c:818](host/main.c#L818),
  after audio init and before `AgbMain()`.

---

## 3. The two-worlds rule

`include/gba/types.h` and `<3ds.h>` both typedef `u8`/`u16`/`u32`, and the
game's `include/` shadows libc's `string.h`. A translation unit including both
will not compile.

| Side | Files | May include |
|---|---|---|
| game | `src/**`, `3ds/gba_mem.c`, `3ds/tweaks.c`, **`3ds/ui/**`** | game headers only, never `<3ds.h>` |
| host | `3ds/host/**`, `rp2350/ppu.c` | libctru only, never game headers |

Anything crossing the seam is declared in [3ds/bridge.h](bridge.h) in **stdint
types only**. `bridge.h` includes neither side's headers and must stay that way.

---

## 4. File map

| File | Lines | Owns |
|---|---|---|
| [ui/bottom_screen.c](ui/bottom_screen.c) | 947 | Tab list, tab bar, dispatch, overlays, the shiny notice and its animation, the shared animation clock, repaint policy, `CtrBottom*` entry points |
| [ui/ui_shell.h](ui/ui_shell.h) | 164 | Layout constants, `UI_COL_*` palette, every per-tab entry point declaration |
| [ui/ui_draw.c](ui/ui_draw.c) / [.h](ui/ui_draw.h) | 979 / 213 | Framebuffer, blitters, window frames, icons, HP bar, sparkle art (in gold, or any ramp via `UiSparkleRamp`), `UiHit`, `UiHoldRepeat` |
| [ui/ui_text.c](ui/ui_text.c) / [.h](ui/ui_text.h) | 369 / 54 | Emerald font rendering at 1x and 2x, numbers, ASCII to game encoding (plus the UTF-8 e-acute, so a literal can say Pokémon) |
| [ui/tab_party.c](ui/tab_party.c) | 944 | 2x3 party grid, cheat tag strip, per-mon detail view with the move panel and the IV/EV spread, HP and mon-icon animation |
| [ui/tab_bag.c](ui/tab_bag.c) | 676 | Pockets, item list, details, USE button, party target picker. **The only tab that writes game state** |
| [ui/tab_map.c](ui/tab_map.c) | 699 | Region map decode and cache, player tracking, fly-from-map |
| [ui/tab_dex.c](ui/tab_dex.c) | 528 | Dex list with cursor and scroll, entry screen |
| [ui/tab_extra.c](ui/tab_extra.c) | 809 | Page 1 port settings, page 2 gameplay tweaks, page 3 quality of life, page 4 the debug menu (compiled out by `CTR_DEBUG_MENU`) |
| [ui/matchup.c](ui/matchup.c) / [.h](ui/matchup.h) | 230 / 59 | Reads about the opposing mon: type effectiveness for the party badges, `UiCatchableOpponent`, and `UiShinyOpponent` behind the notice |
| [ui/ui_quickball.c](ui/ui_quickball.c) / [.h](ui/ui_quickball.h) | 352 / 68 | The quick-throw strip: which ball to offer, the panel, and the throw. **The second thing here that writes game state** |
| [ui/ui_title.c](ui/ui_title.c) / [.h](ui/ui_title.h) | 128 / 35 | TOUCH TO START on the title screen: the art, drawn in the PRESS START banner's lettering, its blink (on the banner's clock at half the rate, `TITLE_BLINK_FRAMES`), and the tap that counts as START. The only thing here that is drawn or touchable before the game starts |
| [ui/tab_trophy.c](ui/tab_trophy.c) | 555 | The TROPHY tab: MAIN and POST-GAME page buttons with their counts, the achievements list in category colours (`UiAchCategoryRamp` lives here), hidden rows, its NEW tags (which last the visit they are seen on) and paging. Reads everything through `AchActive()` |
| [ui/ui_achtoast.c](ui/ui_achtoast.c) / [.h](ui/ui_achtoast.h) | 193 / 56 | The achievement toast: the third overlay, y 0..40, in the unlocked achievement's category colours (gold for a batch), with a VIEW button into the TROPHY tab |

Game side outside `ui/`: [achievements.c](achievements.c) /
[.h](achievements.h) hold what each achievement is, when it unlocks, and the
provider interface the TROPHY tab and the toast read through. See
[ACHIEVEMENTS_PLAN.md](ACHIEVEMENTS_PLAN.md).

Host side that matters to the UI: [host/main.c](host/main.c) (touch sampling,
every `Ctr3dsGet*`/`Ctr3dsSet*` toggle), [host/video.c](host/video.c) (upload,
and the rasteriser's worker thread that runs alongside the paint),
[host/settings.c](host/settings.c) (persistence), and
[host/achievements.c](host/achievements.c) (the per-playthrough achievement
bits, written the way settings are).

---

## 5. Per-tab contract

Every tab is a pair of functions declared in [ui_shell.h](ui/ui_shell.h), plus
an optional state key:

```c
void UiXxxDraw(void);                        // paint the content area, y in 0..191
void UiXxxTouch(const CtrTouchState *t);     // handle a touch
u32  UiXxxStateKey(void);                    // optional, see section 7
```

`Draw` may assume the framebuffer was just cleared to `UI_COL_BG` and that the
tab bar is painted **after** it returns, so it must not draw below
`UI_CONTENT_H`. Overlays are painted after the tab and before the bar.

### Overlays

An overlay is drawn over whichever tab just painted, and claims its rect of
touches before any tab sees them. There are three. The first two are worth
reading as a pair because they answer the same question differently, and the
third is what copying them looks like:

- The **shiny notice** ([bottom_screen.c:137](ui/bottom_screen.c#L137)) is the
  pattern: a 240x112 modal panel centred in the content area, with a DISMISS
  button, keyed on the encounter rather than on a bare flag so the next shiny
  still gets its own notice. It lives in the shell because the shell owns
  overlay paint order.
- The **quick-throw strip** ([ui_quickball.c](ui/ui_quickball.c)) is a 320x40
  band along the bottom of the content area, offering back the ball the player
  last threw. It lives in its OWN file and the shell calls four functions --
  `Active` / `Draw` / `Touch` / `StateKey` -- which is the shape to copy for a
  third overlay. Its geometry starts at y 152 for one reason: the notice ends
  there, so the two abut exactly and neither has to paint over the other's
  border in the case where both are up, which is a catchable shiny.
- The **achievement toast** ([ui_achtoast.c](ui/ui_achtoast.c)) is that
  copy: a 320x40 band along the TOP of the content area, in its own file, with
  the same `Active` / `Draw` / `Touch` / `StateKey` / `Tick` calls. Its `Touch`
  returns TRUE for VIEW and the shell does the tab switch, because the shell
  owns `sTab`. It ends at y 40, where the notice begins, so the
  three tile the 192px content area and can all be up at once. It is bound to
  a rare event (an unlock) and expires after four seconds, which is the tight
  bracket the paragraph below asks of any overlay. Paint order puts it before
  the notice, so the shiny alert is still the one that survives if geometry
  ever overlaps.

An overlay that is up for a COMMON state rather than a rare one has a cost the
notice does not pay. Every tab's layout is hand-fitted to a 192px content area,
so the strip is bound to the action-selection window
(`Ctr3dsPlayerIsChoosingAction()`) rather than to "there is a wild battle": it
appears while the top screen is already asking what to do and goes again the
moment the player answers. If a third overlay cannot find a bracket that tight,
that is an argument against the overlay, not for a bigger one.

It is an overlay rather than a band the tabs make room for because every tab's
layout is hand-fitted to a 192px content area. Reserving space would mean
re-fitting five tabs for a state that occurs once in 8192 encounters. `UiWindowFrame`'s centre tiles are opaque, so a
panel genuinely covers what is behind it rather than floating over readable
content.

Four rules, all of them learned the hard way on this one:

1. **Paint last**, after the tab's `Draw` and before the tab bar.
2. **Take touches first**, on the whole rect, `justReleased` or not. Otherwise
   a press that never becomes a release, or a drag begun on the panel, carries
   through into what it is covering. Absorb everything; act only on the control.
3. **Give it a real control.** "Tap anywhere to dismiss" throws the panel away
   under a stray touch while the player is still reading it.
4. **Let it expire on its own.** The shiny panel closes itself on
   `gBattleOutcome`, so a catch, a faint, a flee or a run clears it whether or
   not anyone pressed anything. An overlay that can only be dismissed by hand is
   an overlay that gets left up.

And fold an "is it up" bit into `UiStateHash`, **in a slot of its own**, or it
will not appear until something else happens to dirty the screen. The strip is
the sharper version of this: nothing else in that hash moves when action
selection opens or closes, so without `top[6]` it would never be drawn at all.

Finally, `UiOverlayActive()` is not bookkeeping. A tab that defers drawing to
the shell's animated layer paints those pieces into its own paint while it is
TRUE, and the reason is that the layer paints over a snapshot which now
contains the overlay. The party grid's bottom row of mon icons sits under the
strip, and its top row under the achievement toast, and either would otherwise
be redrawn straight through the panel on every animation step. The overlay is painted after the tab, so nothing the tab paints
can show through it. What happens next depends on the path:

- **Second core:** `CtrBottomUpdate` asks for a **full** repaint on every frame
  `UiPartyTick` moves anything while an overlay is up. `DrawCell` bakes the
  icons at the live frame and the HP block at its sliding value, so the grid
  keeps animating under the panel. The snapshot still holds nothing that moves
  on the cheap path: while an overlay is up, the party's moving parts are only
  ever in a full paint.
- **Single core:** the icons are baked at frame 0 and the HP block at whatever
  value the last full paint caught, and nothing repaints for them. The layer
  has nothing to draw for the party, so `CtrBottomUpdate` also skips asking for
  the cheap animated redraw while the strip or the toast is up, or the host
  would upload an unchanged screen five times a second. (The notice needs no
  such exclusion: its sparkles take the layer, so a cheap redraw with it up
  still has something to draw.) A bar that starts sliding under an
  overlay therefore stalls near its old value on this path until something
  else forces a repaint.

**Do not size a panel in pixels and hope.** `UiWindowFrame` takes 8px TILES, so
pick tile counts that centre exactly: 30x14 tiles is 240x112, and (320-240)/2
and (192-112)/2 are both 40.

### Adding a tab

The tab bar is `tabW = 320 / visibleCount`. Five tabs is 64px wide each; six is
53px, which is about the practical floor for a fingertip, and TROPHY is the
sixth, so the bar is full. Do not add a seventh. `SECOND_SCREEN_PLAN.md`
proposes converting EXTRA into a launcher instead, which is now the only way to
add a view.

1. Add to `enum UiTab` in [ui_shell.h:19](ui/ui_shell.h#L19), before `UI_TAB_COUNT`.
2. Declare `UiXxxDraw` / `UiXxxTouch` in the same header.
3. Add a row to `sTabs[]` at [bottom_screen.c:59](ui/bottom_screen.c#L59):
   `{ "NAME", FLAG_... }`, or flag `0` for always available.
4. Add a `case` to the `switch` in `Redraw()` ([:825](ui/bottom_screen.c#L825))
   and to the one in `CtrBottomUpdate()` ([:998](ui/bottom_screen.c#L998)).
5. Create `3ds/ui/tab_xxx.c`. It is picked up automatically by the `3ds/ui/*.c`
   glob in [build_objs.sh:113](build_objs.sh#L113). **See the naming hazard in
   section 12.**

Tab visibility mirrors `BuildNormalStartMenu()` (`src/start_menu.c`): a tab
gated on a progress flag must not appear before the player has it.
`Ctr3dsGetShowAllTabs()` overrides every gate for testing.

---

## 6. Touch

`CtrTouchState` ([bridge.h:46](bridge.h#L46)):

```c
typedef struct {
    int16_t x, y;          // 0..319 / 0..239
    uint8_t touching;
    uint8_t justPressed;
    uint8_t justReleased;
} CtrTouchState;
```

Dispatch in `CtrBottomUpdate` ([bottom_screen.c:957](ui/bottom_screen.c#L957)),
in order:

- **Before the game** (`!sInGame`) nothing below sees a touch at all. The one
  exception is the title screen: while PRESS START is up, a release anywhere
  goes to `UiTitleTouch` ([ui_title.c](ui/ui_title.c)) and counts as START.
  It starts nothing itself. It raises a flag that `Task_TitleScreenPhase3`
  (`src/title_screen.c`) reads beside its own START test, so the transition is
  the title screen's own code.
- An **active overlay** claims its whole rect first. The shiny panel takes every
  touch inside its 240x112 centre rect, release or not, so a press that never
  becomes a release, or a drag begun on the panel, cannot carry through into the
  tab underneath. Only its DISMISS button acts; the rest absorbs and ignores.
  The tab bar is deliberately left live, so the panel can be carried between
  tabs rather than trapping the player on one.
- `justReleased && y >= UI_CONTENT_H` switches tab. The tab bar is handled by
  the shell, tabs never see it.
- `y < UI_CONTENT_H` calls the active tab's `Touch`, **on every frame**, not only
  on release.

Two things follow, and both are load bearing:

1. **`sample_touch` latches the last contact point** ([host/main.c:100](host/main.c#L100)).
   `hidTouchRead` returns `(0,0)` on the release frame, so the coordinates are
   held. That also means `t->x`/`t->y` keep the last tap's position forever
   after release.
2. Because of (1), a tab's `Touch` is called every frame with stale coordinates
   once the player has tapped inside the content area. **Every handler therefore
   opens with `if (!t->justReleased) return;`** Do not remove that guard unless
   you are deliberately implementing a drag or a press-and-hold, in which case
   gate on `t->touching` yourself, or use `UiHoldRepeat` below, which does.

Acting on release rather than press means a touch that slides off a control does
not fire it. Keep that convention.

Hit testing is one helper, [ui_draw.c:606](ui/ui_draw.c#L606):

```c
int UiHit(const CtrTouchState *t, int x, int y, int w, int h);
```

Order matters: test overlays and pagers **before** the controls underneath them
(see `UiExtraTouch` at [tab_extra.c:679](ui/tab_extra.c#L679), which tests the
pager first so nothing can sit under it).

`Ctr3dsUiModifierHeld()` is a held 3DS button (X/Y/ZL/ZR, bound in EXTRA) used
as a "jump by 5" modifier. See `CursorStep()` at [tab_dex.c:246](ui/tab_dex.c#L246).

### Press and hold

`UiHoldRepeat` ([ui_draw.c:619](ui/ui_draw.c#L619)) is the one exception to the
`justReleased` guard, and it is why the guard moved down a few lines in the two
list tabs. Both scroll controls in DEX ([tab_dex.c:486](ui/tab_dex.c#L486)) and
BAG ([tab_bag.c:602](ui/tab_bag.c#L602)) run through it:

```c
static UiHold sHoldUp, sHoldDn;   // one counter per control, beside its state

if (UiHoldRepeat(&sHoldUp, t, PAGE_UP_X, PAGE_Y, PAGE_W, PAGE_H))
{
    MoveCursor(-CursorStep());
    return;
}
```

- **Call it above the handler's `justReleased` guard.** A held control has to
  act on frames where nothing has been released, which is exactly what that
  guard exists to throw away.
- **It still fires once on the release of a plain tap**, so a control converted
  to it behaves as it did before for anyone who taps. A press that got as far as
  repeating does not act again when it is lifted, so a hold does not end in one
  extra step.
- **The counter is rebuilt from the current frame**, never trusted across
  frames: sliding off the control stops the repeat, and `justPressed` zeroes it.
  Without that reset a finger still down when the tab is swapped out (the
  handler simply stops being called) would leave a counter parked past the delay
  and make the *next* press repeat instantly.
- **`UI_HOLD_DELAY` / `UI_HOLD_PERIOD` / `UI_HOLD_FAST`** are in frames, and a
  frame here is a *displayed* frame, so the rate is the same under
  fast-forward. Two rates rather than one because the national dex is 386 rows.
- **Every repeat is a full repaint** (section 7), and on DEX it is also one
  `UiMonPic` decompress, because each step lands on a new species. That is the
  real ceiling on the repeat rate; it is not a free-running scroll.

### Established interaction idioms

- **Tap to select, tap again to open.** Party grid and dex list both do this.
  Cheap on a resistive panel and keeps a one-tap mis-touch harmless.
- **A destructive or stateful action takes a second deliberate tap** on a
  dedicated button (BAG's USE, MAP's YES/NO confirm).
- **One column, several tenants.** The party detail view's left column shows the
  stat block, a tapped move's details, or the IV/EV spread
  ([tab_party.c:516](ui/tab_party.c#L516)), never two at once, while the moves
  list beside it survives all three. Two rules make that legible: the transient
  tenant (the move panel, opened by a tap on a specific row) is tested first in
  `DrawDetail`, and the persistent one has a button that reports its own state
  ([:612](ui/tab_party.c#L612), dim frame off, doubled accent outline on). A
  mode with no on-screen state is a mode the player cannot tell they left on.
- **A control that is not drawn must not be tappable.** The IV/EV button is not
  drawn for an empty party slot, so its hit test carries the same species check
  ([tab_party.c:816](ui/tab_party.c#L816)). Without it the toggle would flip
  invisibly and surface on the next mon opened.
- **BACK buttons** are per-view rects, currently in three different places:
  [tab_party.c:106](ui/tab_party.c#L106) (38x22),
  [tab_dex.c:96](ui/tab_dex.c#L96) (42x22),
  [tab_bag.c:104](ui/tab_bag.c#L104) (56x20 cancel).
- **Known bug:** modal flags (`sDetailOpen`, `sEntryOpen`, `sView`) are file
  statics that survive a tab switch, so leaving a detail view by tapping another
  tab and coming back re-enters it. Fixing this is step 0 of
  `SECOND_SCREEN_PLAN.md`. If you add a modal, you inherit the same bug.

---

## 7. Repaint policy

A full repaint is 76,800 pixels of software fill. Two separate flags in
[bottom_screen.c](ui/bottom_screen.c):

- `sNeedsRepaint`: the framebuffer contents are stale.
- `sDirty`: the host has not uploaded the current contents yet.

Do not conflate them. Three ways to get a repaint:

**1. Push.** Call `UiMarkDirty()` after changing anything the screen depends on.
Every touch handler that changes state does this. This is the normal route.

**2. Poll.** `UiStateHash()` ([bottom_screen.c:652](ui/bottom_screen.c#L652)) is
recomputed every frame and compared. This is for state that changes with no
touch at all: taking damage, levelling up, the player changing the window border
in Options, being handed the Pokedex.

`UiStateHash` folds:

| Slot | Contents |
|---|---|
| `top[0]` | `UiFrameId()` |
| `top[1]` | `sInGame` |
| `top[2]` | show-all-tabs override, plus the three unlock flags **only if `SaveDataLive()`** |
| `top[3]` | `UiMatchupOpponentKey()` |
| `top[4]` | the active tab's own key, dispatched by `sTab` |
| `top[5]` | whether the shiny notice is up |
| `top[6]` | `UiQuickBallStateKey()`, zero while the quick-throw strip is down |
| `top[7]` | `UiTitleStateKey()`: zero unless the title's PRESS START is up, otherwise which half of TOUCH TO START's blink is showing, **only while `!sInGame`**. Timed off the banner's own frame count (`Ctr3dsTitlePromptClock()`), so it keeps a fixed phase with the top screen |
| `top[8]` | `AchActive()->stateKey()`: the unlocked count and whether anything is unseen, on every tab, because the TROPHY tab's dot depends on it and an unlock can land on any of them. Reads only the provider's own bits, so it is safe before there is a save block |
| `top[9]` | `UiAchToastStateKey()`, zero while the achievement toast is down and different for every toast, so two in a row still repaint between them |
| then | 6 party mons x 5 fields (species, HP, max HP, level, status), **only while the PARTY tab or BAG's target picker is up** |

The party fields used to be folded on every tab, so in a battle each hit
repainted BAG, MAP, DEX and EXTRA as well, for screens that show none of it.
MAP's fly row is the one other thing that depends on the party, and
`UiMapStateKey` folds exactly that itself.

### Rules for writing a state key

- **A key must be O(what is on screen), not O(what is in the data.)** The dex
  key walks the whole dex, so it is only asked for while the DEX tab is up.
  That is why `top[4]` is a dispatch, not an XOR of everything.
- **Never share a slot between two keys.** Two contributions that happen to
  cancel show up as a panel that stops updating, which is the exact failure the
  hash exists to prevent. See the comment at [tab_map.c:583](ui/tab_map.c#L583).
- **Most new views need no key at all.** Static data (a learnset, a type chart,
  base stats) changes only under the view's own touch handler, which already
  calls `UiMarkDirty()`. IVs are in this class too: they are fixed when the mon
  is created and can never go stale.
- **Key only what is actually on screen, and only while it is.**
  `UiPartyStateKey()` ([tab_party.c:1032](ui/tab_party.c#L1032)) folds in the
  selected mon's EV total *only* while the IV/EV panel is open. EVs are the
  awkward case the party hash misses: they move after a battle without
  necessarily moving level, HP or status with them, so a full-health mon that
  lands the last hit would leave the panel stale. Six reads is the right price
  for a panel that is up; it is the wrong price on the four tabs that cannot
  show it.
- **`GetMonData` decrypts in place** (`src/pokemon.c:3745`). Hashing many mons
  through it costs a decrypt round trip each. `MON_DATA_PERSONALITY`,
  `MON_DATA_OT_ID` and `MON_DATA_SANITY_HAS_SPECIES` sit before
  `MON_DATA_ENCRYPT_SEPARATOR` (`include/pokemon.h:8-19`) and answer from the
  plaintext header, so a `personality ^ otId` fold is a plain load.

**3. Tick.** An animation asks for its own repaints. There are two, both called once per
frame from `CtrBottomUpdate` and both returning TRUE while they still want
frames, which the shell turns into `sNeedsRepaint`:

| Tick | Runs while |
|---|---|
| `UiPartyTick(visible)` ([tab_party.c](ui/tab_party.c)) | the PARTY tab is up: mon icons cycle their two frames, HP bars slide |
| `NoticeTick()` ([bottom_screen.c](ui/bottom_screen.c)) | the shiny panel is up |

`UiPartyTick` flips its icons on `UiAnimStepped()`, the shared step clock:
every 6 frames with a second core, every 12 without. Its HP bars slide every
frame. `NoticeTick` depends on the path. On the single-core path it steps on
the same clock. With a second core it counts frames of its own and runs the
animation as first written, a 64-frame twinkle and a glint across the panel as
it opens, returning TRUE only on the glint's frames and on the 24 frames in 64
where some corner changes size.

Four rules, and the third is the one that keeps this affordable:

1. **Once per frame, not once per redraw.** A tick called from `Redraw` stalls
   exactly when it is the thing that ought to be causing the redraw.
2. **Count calls, not milliseconds.** `CtrBottomUpdate` runs once per DISPLAYED
   frame, so a call is a 60th of a second even under fast-forward, and an
   animation does not speed up with the game. `UiHold`
   ([ui_draw.h](ui/ui_draw.h)) and both ticks all do this.
3. **Return FALSE the moment the thing being animated is off screen.** A
   repaint is 76,800 pixels of software fill plus a blocking texture upload on
   the host ([video.c](host/video.c)), so a tick that returns TRUE
   unconditionally is a decision to pay that on every frame of the game.
   `NoticeTick` returns FALSE whenever the panel is down, which is why it costs
   nothing on the frames it is not running.
4. **On the single-core path, advance on `UiAnimStepped()`, never on your own
   frame counter.** See below: there, a private period is a private repaint
   budget, and they add up. With a second core a repaint costs the frame
   nothing, so a tick may count its own frames, as `NoticeTick` does there, as
   long as it still returns TRUE only on frames where the picture changes.
   Branch on `Ctr3dsRasteriserOnOwnCore()` and keep the stepped version for the
   other path.

`UiPartyTick` takes its tab's visibility as an argument rather than reading
`sTab` itself, which is rule 3 made hard to skip: the shell has to say whether
anyone is looking. Off screen it returns immediately, and adopts the party's
real HP on the frame the tab comes back rather than sliding on arrival for
damage taken while it was hidden. It resyncs on the ARRIVAL, not on every
hidden frame -- `GetMonData` decrypts in place, so a per-frame resync across
the other four tabs would cost more than the repaints the gate saves.

### What an animation actually costs

**Repaints cost frames, and every attempt so far to say WHY has been wrong.**
This section is the record of that, because the wrong answers were all
plausible and two of them shipped.

The observed relationship holds:

> **fps = 3600 / (60 + repaints per second)**

Both data points fit. The notice repainting every frame ran the game at **30fps**
(model 30.0); the party icons every sixth frame ran it at **53** (model 51.4).
**But fitting is not explaining.** The formula was read as "a repaint costs one
whole VBlank in the upload", the upload was sliced across five frames to fix it,
and the frame rate did not move. Then the profiler was finally read:

| Stage | Mean | |
|---|---|---|
| `upload.top.copy` + `.flush` + `.xfer` | **441 µs** | whole top upload, every frame |
| `upload.bot.flush` + `.xfer` | **206 µs** | one bottom slice |
| `frameend` | **311 µs** | and this one is the tell |

**The entire bottom upload path is about 1 ms per repaint.** At five repaints a
second that is 5 ms in every 1000 and it cannot move the frame rate at all. The
slicing was aimed at something that was never the cost.

`frameend` at 311 µs is the second lesson: `C3D_FrameEnd` is **not** the VBlank
wait. `C3D_FRAME_SYNCDRAW` waits at `C3D_FrameBegin`, which went uninstrumented
for the whole investigation while a comment above the wrong call asserted
otherwise. **`framebegin` is the frame's slack** -- read it first.

So: the cost is per-repaint work that is neither the upload nor the sync, which
leaves the **paint** (`Redraw()`), and that is now instrumented too. Profile
before optimising, and be suspicious of a model that only fits.

Then the missing stages were added and the answer fell out:

| Stage | Mean | |
|---|---|---|
| `ppu` | **~9000 µs** | every frame, the GBA rasteriser |
| `framebegin` | **~5700 µs** | every frame -- **this is the slack** |
| `paint` | **~4900 µs** | per bottom-screen repaint |
| `upload.bot.copy` | 500 µs | per repaint |

**A frame has 5.7 ms spare and a full repaint costs 5.6 ms of it**, 87% of that
being `paint`. It fits or misses depending on how the PPU's 7-10 ms lands that
frame, which is why the symptom was 55fps rather than 30.

### And then the rasteriser moved to another core

Everything above was measured with the rasteriser and the paint taking turns on
core 0. The rasteriser now runs on a second core, started just before
`CtrBottomUpdate` and collected after it (section 2), so the two overlap. **A
repaint shorter than the rasteriser now costs the frame nothing.** That is what
finally fixed the battle stutter, where the quick-throw strip repaints at least
twice a turn and the shiny notice repaints on open, dismiss and expiry.

Read these instead of `framebegin` alone:

| Stage | What it says |
|---|---|
| `ppu` | the rasteriser's own time, measured on its core |
| `ppu.wait` | how long core 0 then sat waiting for it. Close to `ppu` on a frame with no repaint, near zero when the paint and the bottom upload took as long as the rasteriser |
| `ppu.snap` | the ~99 KB copy of the video state the rasteriser reads |
| `frame` | the displayed frame's period, sync point to sync point. The worst over 20 ms means a dropped frame |

Every 600 frames the log also says how many frames missed VBlank, if any did.

#### Measured after the move

The same build (`7db93d4`) in three places, from logs that each included battle
repaints: about 80 seconds in each Azahar run, five minutes on the console.

| Stage | Azahar, 100% clock | Azahar, 300% clock | New 3DS XL |
|---|---|---|---|
| `ppu` mean / worst | ~9,000 / 18,650 µs | ~3,000 / 6,215 µs | ~4,500 / 8,581 µs |
| `ppu.snap` mean | ~310 µs | ~165 µs | ~720 µs (worst 1,398) |
| `paint` mean | 4,000 to 7,900 µs | 1,450 to 2,400 µs | 2,400 to 3,900 µs |
| `framebegin` mean (spare time) | ~6,000 µs | ~13,000 µs | ~10,300 µs |
| `frame` worst | 33,442 µs, 5 missed in one window | 16,741 µs | 16,762 µs, none missed |

Azahar emulates the Old 3DS's 268 MHz clock even in New 3DS mode
(`cpu_clock_percentage` defaults to 100), so an Azahar log shows the rasteriser
at about twice what it costs on a New 3DS. Its five missed frames came from the
rasteriser alone running past a frame (`ppu` worst 18,650 µs), which no amount
of overlap with the paint can hide. Raised to 300% it errs the other way and
shows about two thirds of the console's cost; by the same arithmetic, about 200%
would match the console. The setting is per game, in
`custom/000400000FF3D500.ini` under Azahar's config directory
(`~/Library/Application Support/Azahar/config/` on a Mac; the title ID comes from
`UniqueId 0xFF3D5` in `emerald3ds.rsf`). Edit it only while Azahar is closed,
because Azahar rewrites its config on exit.

`ppu.snap` is where Azahar misleads most: 0.7 ms on the console against 0.16 ms
at 300%. It is the one piece of the rasteriser's cost still on core 0, so it is
the first thing to trim if core 0 ever needs time back.

#### What a repaint costs on the second-core path

Since then the bottom upload has moved inside the overlap too: whole-screen,
before the join (section 2), where it used to be sliced and after it. That
gave the bottom screen its animations back
([SECOND_SCREEN_ANIMATION_PLAN.md](SECOND_SCREEN_ANIMATION_PLAN.md)). Core 0's
share of the overlap is now paint plus upload. Measured on the New 3DS XL at
`59a0ba6`, over about two and a half minutes that included wild battles with the
shiny panel up:

| Case | Core 0 inside the overlap | Frame |
|---|---|---|
| A sparkle step | ~0.07 ms paint + ~1.6 ms upload | hidden behind the rasteriser |
| A party grid step (icons and HP blocks) | ~0.9 ms paint + ~1.6 ms upload | hidden behind the rasteriser |
| A full repaint with the shiny panel up (so every step under an overlay) | ~4.1 ms paint (4.7 worst) + ~1.6 ms upload | about 1 ms past the join |

The longest frame was 16,766 µs and none missed VBlank. `ppu.wait` averaged
2.6 ms even in the busiest window, at about 33 uploads a second, and
`framebegin` stayed at 10.2 to 10.9 ms, the same spare time as before the
animations came back. The upload is about 0.7 ms in windows of full paints
alone and 1.45 to 1.75 ms in battle windows made mostly of animation steps,
where the copy alone averages about 1 ms (2.1 ms worst). Azahar at 300% shows
that upload as 0.28 ms, about a sixth of the console's cost, so read it on the
console. The plan's [Measured](SECOND_SCREEN_ANIMATION_PLAN.md#measured)
section has the full table.

The rules below are the **single-core path's**, and that path is still live:
the port falls back to it when no second core is available, and
`make -C 3ds CTR_PPU_THREAD=0` builds it deliberately, so the formula above
still describes that build exactly. Where the second-core path does something
different, the rule says so. The game side asks `Ctr3dsRasteriserOnOwnCore()`
which path it is on.

### So the screen does not repaint to animate

`UiSnapshot` / `UiRestoreRect` ([ui_draw.h](ui/ui_draw.h)) keep a copy of the
last full paint. An animation step puts back the rects it is about to redraw and
draws only those -- six 32x32 icons, or four 16x14 sparkles. Measured 18x
cheaper: **~270 µs against 4900**.

The rule that makes it correct, and it is not optional:

> **The snapshot must be taken BEFORE anything that animates is drawn.**

`Redraw` paints the still screen, snapshots, then calls `DrawAnimatedLayer`.
Bake a moving element into the snapshot and every later restore paints it back:
mon icons blit with index 0 transparent, so the old frame shows through the
holes in the new one. That is why `DrawCell` no longer draws its icon and
`DrawNotice` no longer draws its sparkles -- both moved into the animated layer.

An animation whose change is not confined to rects it can name must ask for a
full repaint instead. `UiPartyAnimOnly()` is how `UiPartyTick` tells the shell
which. On the grid an icon flip and a sliding HP bar both live on the animated
layer (the HP block has two restore rects of its own), so it answers TRUE and
the shell takes the cheap path. The detail view's HP readout is not on that
layer, so a slide there answers FALSE and the shell rebuilds, and so does
anything that moves under an overlay on the second-core path (section 5).

The notice's glint is the same rule from the other side. It crosses the text,
so it cannot be a restore rect: `DrawNotice` paints it into the panel, and
`NoticeTick` asks for a full repaint on each of its 24 frames and on one more
after the last, so the band is out of the snapshot before the cheap sparkle
steps resume.

- **Measure, do not reason, about the drawing primitives either.** `UiFillRect`
  pairing pixels into 32-bit stores is **2.14x**; the identical change to
  `UiClear` is **0.93x**, because one long store loop is something the compiler
  already emits well. Both were "obviously" faster.
- **On the single-core path, keep the step period longer than a slice run.**
  Five frames to upload, so a step every twelve leaves seven idle. With a
  second core the upload is whole and lands on the frame it was painted, so
  there is no run to stay clear of, and the step is six frames.

**So on the single-core path there is one clock, `UI_ANIM_STEP_FRAMES`, and
everything shares it.** Repaints coalesce through a single `sNeedsRepaint`, but
only when they land on the same frames. Two animations on private periods ask
on different frames and cost close to double; on the shared clock, a shiny
panel over an animating party grid still costs 5 repaints a second, not 10.
That makes the frame rate a property of this screen rather than of how many
things happen to be moving.

With a second core the clock is six frames, the pace of Emerald's own party
menu, and only the party's icons use it. The notice counts frames of its own:
repaints on different frames cost nothing there, so there is nothing for it
to coalesce into.

Corollaries worth keeping:

- **A moving thing needs a rate its motion survives.** On the single-core path
  the notice's opening glint, a 4px band crossing 224px, would take eleven
  seconds to cross at 5 steps a second, so that path opens with a burst
  instead. Continuous motion and a low repaint budget are incompatible, so
  prefer state changes there. With a second core the glint is back, at one
  full repaint per frame for its 24 frames.
- A `u16` counter wraps: after about eighteen minutes counting frames, or three
  and a half hours counting 12-frame steps. If a cycle length divides 65536 the
  wrap lands on a boundary and the twinkle does not jump; `NOTICE_FRAME_CYCLE`
  (64) and `NOTICE_STEP_CYCLE` (8) are powers of two for that reason. Pick a
  power of two. On the frame path the wrap also replays the glint, which is
  harmless.

---

## 8. Drawing API

All coordinates are pixels unless the name says tiles. Everything clamps against
`0..UI_W/UI_H` only; **there is no clip rectangle**, so a wide string paints over
its neighbours (section 9).

### Geometry ([ui_draw.h](ui/ui_draw.h))

```c
void UiClear(u16 color);
void UiFillRect(int x, int y, int w, int h, u16 color);
void UiRect(int x, int y, int w, int h, u16 color);        // 1px outline
int  UiHit(const CtrTouchState *t, int x, int y, int w, int h);
bool8 UiHoldRepeat(UiHold *h, const CtrTouchState *t,     // section 6
                   int x, int y, int w, int hgt);
```

### Panels

```c
void UiWindowFrame(int tx, int ty, int wTiles, int hTiles);  // TILES, min 2x2
u8   UiFrameId(void);
u16  UiThemeText(void);
u16  UiThemeShadow(void);
```

`UiWindowFrame` draws the player's chosen border out of Emerald's 20 option-menu
frames, as a 3x3 nine-slice. Because those run from near-white to near-dark,
**text drawn on a frame must use `UiThemeText()` / `UiThemeShadow()`**, never a
fixed colour. `UI_COL_*` is for the port's own chrome (the tab bar), which is
not on a frame.

The one deliberate exception is a colour that carries its own dark outline. An
unlocked achievement's title on the TROPHY tab is printed in its category's body
colour with that category's dark edge as the shadow (`UiAchCategoryRamp`,
`ui_shell.h`): the edge outlines it on the near-white frames and the mid-tone
body carries it on the near-dark ones, the same pairing the shiny notice's gold
headline uses. A new fixed colour on a frame needs a ramp of that shape, and
checking on the lightest and the darkest frame in Options.

Coordinates are in whole 8px tiles, so panel layouts have to divide cleanly:
`320 = 40 tiles`, `UI_CONTENT_H = 192 = 24 tiles`.

`UiFrameId()` prefers `Ctr3dsLiveWindowFrameType()` (`src/option_menu.c:33`)
over the save block, so the border previews live while the options menu is open.
It also returns 0 when `gSaveBlock2Ptr` is NULL. Fold it into any redraw trigger.

### Game art

```c
void UiMonIcon(int x, int y, u16 species, u32 personality);   // 32x32, frame 0
void UiMonIconFrame(int x, int y, u16 species, u32 personality, u8 frame);
void UiItemIcon(int x, int y, u16 itemId);                    // 32x32
void UiMonPic(int x, int y, u16 species);                     // 64x64, cached
void UiPokeball(int x, int y);                                // 7x7, generic
void UiBallIcon(int x, int y, u16 itemId);                    // 16x16, cached
void UiFootprint(int x, int y, u16 species, u16 color);       // 16x16
void UiTypeIcon(int x, int y, u8 type);                       // 32x16
void UiStatusIcon(int x, int y, u8 ailment);                  // 32x8, AILMENT_*
void UiArrow(int x, int y, bool8 up, u16 fill);               // 11x7
void UiChevron(int x, int y);                                 // 6x10, menu cursor
void UiHpBar(int x, int y, int w, u32 hp, u32 maxHp);         // 8px tall
```

`UiHpBar` takes `hp` explicitly rather than reading the mon, because the party
tab animates it while the BAG picker shows the true value. It colours itself
through the game's own `GetHPBarLevel`, so it changes colour at exactly the
points the battle bar does.

### Low level

```c
u16  UiBgr555ToRgb565(u16 bgr555);
void UiLoadPal(u16 *dst565, const u16 *srcGbaPal, int count);
void UiBlit4bppTile(int x, int y, const u8 *tile, const u16 *pal565, int transparent0);
void UiBlit8bppTile(int x, int y, const u8 *tile, const u16 *pal565, int transparent0);
u16 *UiFb(void);
```

GBA graphics are 4bpp tiles with BGR555 palettes. Convert the palette **once**
with `UiLoadPal`, then per-pixel work is a table lookup. 8bpp tiles index the
whole 256-entry BG palette, not a 16-colour bank.

---

## 9. Text

**There is one font and one size.** `gFontNormalLatinGlyphs` at `UI_GLYPH_H` 15
is all the ROM has; the game never needed another on a 240px screen. For a
headline that must be read rather than looked for, `UiTextBig` scales those same
glyphs 2x nearest-neighbour ([ui_text.h:35](ui/ui_text.h#L35)); it costs four
times the fill per glyph, so it is not a general-purpose call. Pair it with
`UiTextBigWidth` for centring, and `UI_GLYPH_BIG_H` (30) for row pitch.

**Strings are game-encoded (`charmap.txt`), EOS-terminated, not ASCII.**
`GetSpeciesName()`, `GetItemName()`, `gMoveNames[]`, `gRegionMapEntries[].name`
and every other game table already return that encoding. For your own literals:

```c
u8 label[40];
UiText(x, y, UiAscii(label, "GAME SPEED", sizeof(label)), UiThemeText(), UiThemeShadow());
```

```c
int UiText(int x, int y, const u8 *str, u16 fg, u16 shadow);       // returns advance
int UiTextWidth(const u8 *str);
int UiTextRight(int xRight, int y, const u8 *str, u16 fg, u16 shadow);
int UiNum(int x, int y, s32 value, u16 fg, u16 shadow);
int UiNumRight(int xRight, int y, s32 value, u16 fg, u16 shadow);
int UiNumWidth(s32 value);
u8 *UiAscii(u8 *dst, const char *ascii, int dstSize);
```

`UI_GLYPH_H` is 15, `UI_LINE_H` is 16, `UI_TEXT_MAX` caps a single string at 128
characters so an unterminated one cannot run away.

The font is decoded here rather than through the game's `DecompressGlyphTile()`,
because that function reads a lookup table the text engine regenerates at
runtime from whatever colours it last printed with. Calling it produces
invisible text. Do not "simplify" `ui_text.c` back onto it.

### The text trap

**There is no clipping, wrapping, ellipsis or truncation.** Every panel width in
the tree is hand-measured against the longest known game string. For example
BAG's list panel is 24 tiles because that leaves exactly 108px, the width of the
widest item description line in the game ([tab_bag.c:45](ui/tab_bag.c#L45)).

That does not survive player-authored text: nicknames, OT names, box names. If
you add a view showing any of those, either measure and truncate yourself or
implement `UiClipPush/Pop` first (step 1 of `SECOND_SCREEN_PLAN.md`; the
blitters already do per-pixel bounds tests, so it is roughly four one-line edits
and zero extra per-pixel cost).

---

## 10. Reading game state safely

**Always go through the game's own accessors.** `GetMonData`, `GetMonAbility`,
`GetMoney`, `GetGameStat`, `GetSetPokedexFlag`, `FlagGet`, `VarGet`,
`BagGetItemIdByPocketPosition`. Money, coins and game stats are XOR-encrypted;
the accessors are not optional. Reading a raw field means the UI can disagree
with the game's own screens.

**`gSaveBlock1Ptr` and `gSaveBlock2Ptr` start NULL** (`src/load_save.c:41-42`)
and are only assigned once a file is loaded. Every `FlagGet` goes through
`gSaveBlock1Ptr`, so before that point it is a null dereference plus a field
offset. Azahar tolerated this for months; a real ARM11 faulted on the first
hardware boot. Gate any save-block read with:

```c
static bool8 SaveDataLive(void);   // bottom_screen.c:86
```

Note this is **not** the same question as `sInGame`, which latches on reaching
`CB2_Overworld` once and stays true through battles and menus. `sInGame` decides
whether to draw at all; `SaveDataLive()` decides whether it is safe to read.

**Randomiser.** If the view lists species that come from static data rather than
from a live mon, route them through `Ctr3dsMapWildSpecies()` (wild encounter
tables) or `Ctr3dsMapSpecies()` ([tweaks.h](tweaks.h)) or the list lies when the
randomiser is on.

**Statics are the memory model.** The UI layer is deliberately heap-free.
Caches are file statics: `UiMonPic` caches one species'
expanded sheet, `UiWindowFrame` caches the converted palette keyed on frame id,
`tab_map.c` holds the whole decompressed region map in `.bss`.

**Size-check before any `LZDecompressWram`.** It is bounded only by the size word
in its own input, so an overrun lands in the neighbouring statics.
`UiMonPic` was hit by exactly this: five species ship a four-frame front sheet
that decompresses to 8192 bytes while `gMonFrontPicTable` reports the size of
one frame, and the 6KB overrun repainted the cached window-frame palette. The
symptom was every other tab's border changing colour. See
[ui_draw.c:275](ui/ui_draw.c#L275) and [tab_map.c:125](ui/tab_map.c#L125).

---

## 11. Writing game state

Two things write: the BAG tab, and the quick-throw strip. Their gates are the
design, not a detail.

### Out of battle: four gates ([tab_bag.c:153](ui/tab_bag.c#L153))

```c
static bool8 CanUseItemNow(void)
{
    if (gMain.inBattle)                  return FALSE;
    if (gMain.callback2 != CB2_Overworld) return FALSE;
    if (ArePlayerFieldControlsLocked())  return FALSE;
    if (ScriptContext_IsEnabled())       return FALSE;
    return TRUE;
}
```

Mutating party or bag data mid-script can contradict whatever the script is
about to do. Any new write path must pass the same four.

### In battle: through the controller ([src/battle_controller_player.c:272](../src/battle_controller_player.c#L272))

`Ctr3dsQueueBattleItem(item, partySlot)` does **not** apply the effect itself. It
requires `gBattlerControllerFuncs[player] == HandleInputChooseAction`
(`Ctr3dsPlayerIsChoosingAction()`), rejects link/frontier/recorded battles,
saves and restores `gActiveBattler` and `gBattlerInMenuId`, and registers
`B_ACTION_USE_ITEM` so the item costs a turn and the opponent responds, exactly
as the d-pad route does. Copy this shape for any future battle write.

The quick-throw strip ([ui_quickball.c](ui/ui_quickball.c)) is what copying it
looks like: it calls that one function and has **no gate of its own**, because
everything it would need to refuse for -- the wrong moment, a barred battle
type, a full party and box -- is already inside. A second copy of those checks
out here is a second copy free to be subtly wrong.

Note what this means for the Safari Zone. `Ctr3dsPlayerIsChoosingAction()` asks
about the PLAYER controller, and the Safari Zone runs
`src/battle_controller_safari.c`, whose identically named action handler is a
different static function at a different address. So it is FALSE there and
neither the touch bag nor the strip can throw a Safari Ball. That is a real
limitation rather than an oversight, and any new battle write inherits it.

### Persisted state that is not save data

The last ball thrown is a byte of `settings.bin`, not of the save block. The
write is one fenced line in `HandleAction_UseItem()` (`src/battle_util.c`),
which is the single point every route the player can choose a ball by passes
through -- the d-pad bag, the touch BAG tab and the strip all arrive as
`B_ACTION_USE_ITEM`.

Two things about that are worth copying. It calls a setter from **battle logic**
rather than from `CtrBottomUpdate()`, which is safe only because
`CtrSettingsMarkDirty()` queues and `CtrSettingsFlush()` hands the write to the
I/O thread from the frame loop -- see section 13, and the header of
`host/settings.c` for what happened when a setter wrote synchronously. This one
is also why the write had to leave the frame altogether: a new kind of ball
thrown is a settings change, so its write used to land about a second after the
throw, in the middle of the catch animation. And the value crosses the seam as a
raw number that the host does **not** range-check, because the valid range is
`FIRST_BALL..LAST_BALL` in a game header the host may not include; the check
lives in `UiQuickBallItem()` where those constants are. That is the exception to
`settings.c`'s "range-check rather than trust the file" rule, and the only one.

### Classify by the game's tables, not by item id

`ItemTargeting()` ([tab_bag.c:188](ui/tab_bag.c#L188)) drives everything off
`GetItemEffectType()` and `GetItemBattleUsage()`, so it classifies every item of
a class the same way and cannot fall behind the data. Items needing a move
choice as well as a target are refused outright rather than defaulting to slot 0.

`GetItemFieldFunc()` is deliberately **not** used: the game's field-use flows are
coupled to the bag menu's task and callback context and render onto the top
screen, so driving one from here would fight the overworld for BG layers.

### Leaving the overworld

`DoFly()` ([tab_map.c:315](ui/tab_map.c#L315)) is the reference for replacing
`gMain.callback2` from the bottom screen. It is safe only because
`CtrBottomUpdate` runs at the end of a frame. Before leaving you **must** call:

```c
PlayRainStoppingSoundEffect();
CleanupOverworldWindowsAndTilemaps();
```

The overworld frees its graphics on the way **in**, not on the way out, so
whatever takes the callback away has to do it first. Skipping it does not fail
visibly: the first fly looks perfect and leaks 0x2D80 bytes, which is under ten
flies before a 0x1C000 heap has nothing left. Then `InitWindows` returns a null
nobody checks and the next draw writes to address 0. All twenty of the game's own
overworld exits do this.

---

## 12. Build

```sh
make tools && make generated             # decomp tools + generated headers
python3 tools/generate_wasm_assets.py    # -> build/assets
rp2350/gen_sound_assets.sh               # one time
3ds/build_objs.sh                        # game sources -> 3ds/build/libpokeemerald.a
make -C 3ds                              # -> 3ds/emerald3ds.{cia,3ds}
```

**A change under `3ds/ui/` needs `3ds/build_objs.sh` rerun, then `make -C 3ds`.**
A change under `3ds/host/` needs only `make -C 3ds`. A change to `bridge.h`
needs both: `build_objs.sh` recompiles every game-side source unconditionally,
and `3ds/Makefile` tracks header dependencies with `-MMD -MP`, so the host
objects that include it rebuild too.

That second half was not true until recently. The host rule had no header
dependencies at all and only `main.o` carried a `FORCE` prerequisite (for the
build stamp), so `make -C 3ds` after a `bridge.h` edit happily relinked stale
`settings.o`, `video.o`, `audio.o`, `save.o`, `log.o` and `ppu.o`. A macro or
enum whose value changed would then have been compiled two different ways into
one binary, with no error at any stage. If you ever see a symptom that has no
cause in the source, `make -C 3ds clean` first and see whether it survives.

Diagnostics build (traces, boot splash, liveness bars). The flag must be passed
to **both** or you get a half-instrumented build:

```sh
CTR_BOOT_DIAG=1 3ds/build_objs.sh && make -C 3ds CTR_BOOT_DIAG=1
```

### Object-name collision hazard

[build_objs.sh:113](build_objs.sh#L113) globs `3ds/ui/*.c` non-recursively and
writes `$OBJ/$(basename).o` into the **same** object directory as all of
`src/*.c`, with `3ds/ui` globbed last. A file named `3ds/ui/pokedex.c` would
silently overwrite `src/pokedex.o` and delete the game's Pokedex from the
archive, with no error at any stage.

**Mandatory prefixes:** `ui_*` for shared code, `tab_*` for tab roots, `view_*`
for pushed views. Keep the directory flat; subdirectories are not compiled.

---

## 13. Adding a host-side setting

The pattern is Apply/Set/Get, and it exists so `CtrSettingsLoad()` can restore a
value without writing the file back out during the load that produced it.

1. **`3ds/bridge.h`**: declare `void Ctr3dsSetFoo(int)` and `int Ctr3dsGetFoo(void)`,
   plus any `CTR_FOO_*` constants. Comment what it means and why it exists.
2. **`3ds/host/main.c`** (or `video.c` if it is a display concern): a static, then

   ```c
   void Ctr3dsApplyFoo(int v) { sFoo = clamp(v); }        // mutate, no persist
   void Ctr3dsSetFoo(int v)   { int b = sFoo; Ctr3dsApplyFoo(v);
                                if (sFoo != b) CtrSettingsMarkDirty(); }
   int  Ctr3dsGetFoo(void)    { return sFoo; }
   ```

   `CtrSettingsMarkDirty()` queues; it does not write. `CtrSettingsFlush()`,
   called from `Rp2350PresentFrame()` beside `CtrSaveFlush()`, waits for a
   second of quiet, snapshots every setting, and hands the snapshot to the I/O
   thread (`3ds/host/io_thread.c`), which does the card write while the main
   thread waits for VBlank. Only the closing path writes synchronously. Setters
   run from `CtrBottomUpdate()`, i.e. from the middle of a game frame, and a
   card write is a blocking FS round trip however few bytes it carries -- on a
   console the player sees that. Never call the writer from a setter.

   Range-check inside `Apply`, never trust the caller: a corrupt settings byte
   must leave the default standing.
3. **`3ds/host/settings.c`**: append a `uint8_t` to `struct CtrSettings`, bump
   `SETTINGS_VERSION`, add a `SETTINGS_Vn_SIZE` short-read migration, add the
   `extern` and the load/save lines. Keep the struct's every byte spoken for
   with explicit `pad`, or `settings_put()` writes uninitialised stack to the
   card. Choose the sense so that a zero byte means the old default.
4. **`3ds/ui/tab_extra.c`**: add the control, and fold the value into
   `UiExtraStateKey()` ([:544](ui/tab_extra.c#L544)) in a bit range nothing else
   claims -- but only if it can change with **no touch on this tab**, the way
   the shiny test does when its encounter fires. A plain toggle needs no slot:
   its own handler calls `UiMarkDirty()`, which is why `phoneCallsOff` and
   `quickBallOff` are absent from that hash rather than overlooked.

The file is `sdmc:/3ds/emerald3ds/settings.bin`. It is opened once at boot and
rewritten in place, on a `CTR_SETTINGS_QUIET_MS` debounce so a pass through the
settings costs one write. Two things it deliberately does NOT copy from
`save.c`, both because they were measured on a console and found expensive for
no gain at 24 bytes:

- **No `.tmp` and rename.** The payload is one sector, so there is no torn
  state to protect against, and the magic/version check turns anything odd into
  "use the defaults". The remove-and-rename half alone measured 51-56 ms.
- **No reopen per write.** Creating, closing, deleting and renaming each mutate
  the directory and each is its own round trip to the FS process.

A failed write is also not retried: one attempt per change, or a read-only card
would turn one tap into an FS attempt on every frame for the rest of the
session.

The struct is currently **24 bytes at v9 with no padding left**. Every version
from v5 on has grown by claiming bytes its predecessor wrote as explicit zero
padding, which is why v6, v8 and v9 needed no migration at all -- same size, and
each new field means at zero exactly what that file already meant. That padding
existed to stop the compiler rounding the struct up to its 4-byte alignment and
`settings_put()` then putting uninitialised stack on the card. There is none
left, so **the next field added grows the struct to 25 and must bring explicit
padding back with it.**

**Three settings deliberately break the pattern**, and all three are worth
knowing before you copy it:

- **A setting that expires does not persist.** `Ctr3dsSetShinyTest` has no
  `Apply` and never calls `CtrSettingsMarkDirty` ([host/main.c:329](host/main.c#L329)),
  because it disarms itself when the encounter fires. A saved "armed" would go
  off in some later session the player had forgotten arming it in. Skip step 3
  entirely for anything like that; fast-forward is the older precedent.
- **A setting behind `CTR_DEBUG_MENU` must be neutralised, not just hidden**
  ([bridge.h:195](bridge.h#L195)). Hiding the control leaves the value, and two
  of the debug settings persist, so a shipping build could inherit "show every
  tab" or a muted PSG channel from a debug session with no control to undo it.
  Guard in **`Apply`**, not in `Get`: `CtrSettingsLoad()` calls `Apply`
  directly and never asks the getter, and forcing the stored byte also covers
  readers that touch the static array themselves, as `CtrAudioFrame` does for
  the stereo downmix. Choose the neutral value carefully; for the audio A/B
  switches it is ON, because ON is the real mixer.
- **A value written by the game, not by a button, is not range-checked here.**
  `lastBall` is set from `HandleAction_UseItem()` (`src/battle_util.c`) and its
  valid range is `FIRST_BALL..LAST_BALL`, a game constant `settings.c` may not
  include -- copying the numbers across the seam would be a second definition
  free to drift from the first. So `Ctr3dsApplyLastBall` stores whatever it is
  given and `UiQuickBallItem()` checks it game-side, where the constants are.
  This is the ONLY exception to "range-check rather than trust the file"; a
  corrupt byte simply fails that test and the strip falls back to the first ball
  in the pocket.

---

## 14. Layout constants

```c
CTR_BOTTOM_WIDTH   320        // bridge.h
CTR_BOTTOM_HEIGHT  240
UI_TABBAR_H        48         // ui_shell.h, the bar along the bottom
UI_CONTENT_H       192        // 24 tiles, everything above the bar
UI_W / UI_H        320 / 240  // ui_draw.h
```

`UI_TABBAR_H` and `UI_CONTENT_H` are load bearing: the five tabs, the
encounters view and both overlays below are all fitted to them.
`UI_SKIN_PLAN.md` moves them only while re-fitting every one of those against
wireframes. Do not change them casually.

The three overlays divide that 192px content area between them, and their
numbers are load bearing against **each other**:

```c
UI_AT_Y  / UI_AT_H     0 /  40   // ui_achtoast.h, the toast:         y 0..40
NOTICE_Y / NOTICE_H   40 / 112   // bottom_screen.c, the shiny notice: y 40..152
UI_QB_Y  / UI_QB_H   152 /  40   // ui_quickball.h, the strip:        y 152..192
```

40 and 152 each appear twice on purpose. Move any of them and two overlap, and
then the one drawn later eats the other's border row -- which is visible,
because `UiWindowFrame`'s centre tiles are opaque. All three are expressed in
TILES (`UI_AT_TY`, `NOTICE_TY`, `UI_QB_TY`) because that is what `UiWindowFrame`
takes, so any new value has to land on an 8px boundary as well as clear the
other panels. There is no room left for a fourth.

Per-view constants are `#define`d at the top of each tab file, derived from each
other rather than tabulated twice (`MOVE_ROW_Y(i)`, `SPD_X(i)`, `CellTop(i)`).
When you move a control, move the constant, and check that the touch handler
uses the same expression the draw code does.

**A grid is only shareable while the pages sharing it have the same number of
rows.** `tab_extra.c` kept page 1 and page 2 on one set of `ROW*_Y` constants
after the tab-unlock override moved off page 1, which left page 1's first label
against the top frame and 32px dead under its last button while page 2 stayed
full. It now has two grids: `ROW*_LABEL_Y` for page 2's four rows, `P1_ROW_Y(i)`
for page 1's three. They still share the COLUMNS, which is what keeps the pages
looking like one panel, and both still end on the interior floor at y=183. `tab_party.c` gets this right by
computing both from `CellH()`/`CellTop()`, which change when the cheat tag strip
appears.

---

## 15. Pitfalls, collected

| Symptom | Cause |
|---|---|
| Data abort at boot, address near 0x1300 | Reading `gSaveBlock1Ptr` before a file is loaded. Gate on `SaveDataLive()`. |
| A **dangling** pointer (freed but not nulled, or a buffer still registered with `SetBgTilemapBuffer`) | **Cannot crash this port.** `gHeap` is `EWRAM_DATA u8 gHeap[HEAP_SIZE]` (`src/malloc.c`) and EWRAM is the static `gGbaMem` array, so freed memory stays mapped forever. A dangling read returns stale bytes and draws garbage; it never aborts. `CleanupOverworldWindowsAndTilemaps()` leaves BG1/2/3 registered on freed buffers for exactly this reason and is fine. Only **NULL** faults, because 0 is the one unmapped address, so hunt `FREE_AND_SET_NULL`, not `Free`. |
| An `Alloc` that returns **NULL** under heap pressure | The other way to get a null deref, and the reason the fly leak was fatal: 89 file-scope allocations in `src/` are used with no null check, which is vanilla and fine while the heap holds. Heap is `0x1C000`, unchanged by the port. The fix for this class is never "add 89 checks", it is "find the leak": every allocation that escapes its free. |
| Data abort with a **small or struct-sized FAR**, Read: *is it actually a bug?* | Freeing a struct while its sprites still live is only fatal if an `AnimateSprites()` runs **before** something calls `ResetSpriteData()`. Vanilla's universal idiom is "free, then hand off to a *setup* CB2 that resets sprites first", and that is safe: a sweep of every `FREE_AND_SET_NULL` of a state pointer in `src/` found this holds nearly everywhere. Only two shapes break it: (a) the free repeats across several frames while the scene's own loop keeps running (the battle-teardown bug), and (b) the free hands control back to an **already-running** main loop instead of a setup CB2 (the naming screen, which is the *only* `DoNamingScreen` caller passing `BattleMainCB2`; every other in-battle menu returns through `ReshowBattleScreenAfterMenu`). Check which shape you have before changing any teardown. Most candidates are false positives, and editing a working one is the bigger risk. |
| Data abort with a **small or struct-sized FAR**, Read | A null pointer plus a field offset -- FAR *is* the offset, so it is 0 only when the field is the struct's first member. `offsetof` the FAR against every struct the code frees and it names the pointer outright. The naming screen was one: `MainState_Exit` frees `sNamingScreen` while the sprites and helper tasks it created are still running, and `SpriteCB_Cursor` reads `currentPage` at offset **0x1E22** every frame, which was the reported FAR exactly. Note the trap: that screen ALREADY had a `VBLANK_REQUIRE(sNamingScreen)` guard, which fixed the VBlank reader and left the `AnimateSprites()` one. Guarding readers one at a time is how these survive. |
| Data abort with **FAR exactly 00000000**, Read | A null pointer dereferenced at offset 0. On a GBA this is free -- no MMU, address 0 is the BIOS, the read returns junk nobody looks at -- so vanilla code does it in places and gets away with it. On the ARM11 it is fatal. `FreeResetData_ReturnToOvOrDoEvolutions` (`src/battle_main.c`) was one: it freed the battle sprite data on every frame of the end-of-battle fade while those sprites were still animating, and `SpriteCB_EnemyShadow` read `gBattleSpritesDataPtr->battlerData` (first member, so offset 0) straight through the NULL. It presented as "running from a wild battle sometimes crashes" -- only outcomes that leave the opponent standing, and only the 61 species with a non-zero `gEnemyMonElevation`. |
| Every tab's border changes colour after viewing a dex entry | Decompress overrun into neighbouring statics. Size-check first. |
| Invisible text | Using the game's `DecompressGlyphTile()` instead of `ui_text.c`'s own decoder. |
| Text overruns into the next panel | No clipping exists. Measure, or add `UiClipPush`. |
| A panel stops updating | Two state-key contributions cancelling in one hash slot. |
| A readout goes stale until you switch tabs | State changes without a touch and has no state key. |
| Every tap lands at (0,0) | Reading touch coordinates without the latch, or dropping the `justReleased` guard. |
| Detail view reopens after a tab switch | Modal file statics survive the switch. Known bug. |
| Heap exhaustion after a few flies | Left the overworld without `CleanupOverworldWindowsAndTilemaps()`. |
| A wild Pokémon turns into a Bad Egg | Wrote `MON_DATA_PERSONALITY` into an existing mon. It is the substructure order *and* half the encryption key, and `SetBoxMonData` does not re-encrypt for it (the field is below `MON_DATA_ENCRYPT_SEPARATOR`). Create the mon with the personality you want instead: [3ds/tweaks.c:297](tweaks.c#L297). |
| A `src/` feature silently disappears | `3ds/ui/*.c` basename collided with a `src/*.c` object. |
| A playthrough gets another save's achievements | Conditions were read while the save in memory belonged to someone else. A New Game sets the trainer ID in Birch's speech while the old save's flags are still loaded, until `NewGameInitData()` clears them; a soft reset reloads the card's save under whatever was being played. So a playthrough is adopted only on a `CB2_Overworld` frame, and nothing is evaluated unless the save block's trainer ID matches it ([achievements.c](achievements.c), `Current` and `Adopt`). |
| Saved achievements come back as the wrong ones | An achievement's id was changed or reused. The id is its bit in `achievements.bin`, so ids are permanent: a new achievement takes the next unused id, and a retired one's id is never handed out again. Row order is free. The EXTRA debug row counts duplicate ids. |
| Mon icons punch through an overlay every few frames | The tab redrew them on the shell's animated layer, which paints over a snapshot that already contains the overlay. Fold the overlay into `UiOverlayActive()` so the tab paints the icons into its own paint instead (live on the second-core path, which then repaints fully for each step; still on the single-core path). |
| A missing prototype links, then fails at link | `build_objs.sh` passes `-Wno-implicit-function-declaration`. A call across the seam with no declaration compiles silently. |
| Host-side change did nothing | Forgot `3ds/build_objs.sh`, or passed `CTR_BOOT_DIAG` to only one of the two builds. |
| The game pauses for a moment whenever you touch the second screen | Something on the touch path is doing blocking work in the frame. Read `log.txt` for `slow <stage>` lines: `CtrLogSlow` ([bridge.h](bridge.h)) reports any timed stage over 50 ms. The file only exists with `CTR_DEBUG_MENU` on. |
| The frame rate drops while something on the bottom screen is animating | First check the boot log says `rasteriser on core 2` (or core 1). On the single-core path it is expected and quantified: `fps = 3600 / (60 + repaints per second)` (section 7). With the rasteriser on its own core a repaint should cost nothing, so read `ppu.wait` and `frame`. Read `log.txt` for `prof <stage>` lines rather than guessing -- `CtrProfile` ([bridge.h](bridge.h)) reports the mean and worst of each stage in MICROseconds every 600 samples, which is what `CtrLogSlow`'s 50 ms threshold and 1 ms clock cannot see. `paint` is the software fill, `upload.bot.copy/flush/xfer` the host's three upload stages, and `framebegin` is the VBlank wait, so a `framebegin` near zero means the frame had no slack left. |
| Profiler numbers that make no sense, or a crash inside `CtrProfile` | Called from a thread other than the main one. `CtrProfile` and `CtrLogSlow` keep unlocked static tables. The rasteriser's worker and the I/O thread measure themselves and let the main thread report. |
| The top screen shows garbage or a torn picture for a frame | Something made the rasteriser read live memory while the game or the paint was writing it. In threaded mode `ppu_set_memory()` must point at the snapshot `CtrVideoRenderBegin()` fills, never at `gGbaMem` ([host/video.c](host/video.c)). |
| The last lines before a crash are missing from `log.txt` | Expected now, within about a frame: lines are queued for the I/O thread rather than flushed on the spot. Boot is still written synchronously. |
| A symptom with no cause anywhere in the source | A stale host object. `make -C 3ds clean` and rebuild before reading any more code. |

---

## 16. Verification checklist

- Existing tabs unchanged, all controls still reachable, settings persist across
  a relaunch.
- New views checked against the game's **own** screens, not against your reading
  of the data: trainer card against the in-game trainer card, berry stages
  against the trees, encounter lists against the real tables with the randomiser
  both off and on.
- Write paths verified against their gates, not only their happy paths: refused
  during a battle, during a script, with field controls locked, and outside
  `CB2_Overworld`.
- Overlays verified against the states they must NOT appear in, which for a
  battle overlay is the long tail rather than the obvious case: trainer battles,
  the Battle Frontier, Wally's tutorial, Birch's bag on Route 101, link and
  recorded battles, and after `gBattleOutcome` is set. `UNCATCHABLE_BATTLE` in
  [ui/matchup.c](ui/matchup.c) is one flag test covering most of them, so use it
  rather than assembling a second list.
- A settings version bump verified by **loading the previous version's file**,
  not only by writing the new one. Keep a copy before changing the struct.
- **Teardown paths on hardware specifically.** Emulators are far more forgiving
  of a read through a null pointer than a real ARM11 is, and vanilla frees data
  out from under live sprite and task callbacks in more than one place -- battle
  teardown and the naming screen both did, and the 24 `VBLANK_REQUIRE` guards
  across 10 files mark the ones already found. When a crash reports a low FAR,
  look for a pointer the game nulls on the way out of a mode rather than for
  something the port did, and `offsetof` the FAR against the freed structs to
  name it. Prefer destroying the leftovers (`ResetSpriteData()` / `ResetTasks()`
  before the free) over guarding each reader: the readers are reached through
  function-pointer tables and helpers, so the list is never as short as it
  looks, and a per-reader guard is what left the naming screen still crashing.
- On hardware, not only in an emulator (`AGENTS.md`). The boot log and the frame
  600 audio health report stay clean. On the single-core path repaint frequency
  has not visibly risen; on the second-core path `ppu.wait` stays well above
  zero on most frames and no "missed VBlank" line appears.
