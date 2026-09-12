# Give the bottom screen its animations back

**Status: proposed, not started.** Written 2026-09-12 against `ea832a8`, and
re-checked against `893409a`, after TOUCH TO START. Every line reference below
was read against that tree; re-check them before starting.
Companion documents: [SECOND_SCREEN_CHEATSHEET.md](SECOND_SCREEN_CHEATSHEET.md)
(the code as it actually is; read section 7 first),
[UI_SKIN_PLAN.md](UI_SKIN_PLAN.md) (a reskin that shares the repaint path, see
[How this meets the skin plan](#how-this-meets-the-skin-plan)), and
[README-TECHNICAL.md](../README-TECHNICAL.md) ("What a bottom-screen repaint
costs").

**In one paragraph.** While everything ran on one core, every bottom-screen
repaint cost the game a frame, so the bottom screen's animations were slowed,
cut, or frozen to protect the top screen. Since `7db93d4` the rasteriser runs on
a second core and a New 3DS XL has about 10 ms to spare each frame. This plan
restores everything that was visibly lost, in four stages, and keeps the old
tuning for the one path that still has no second core.

## Contents

- [Context](#context)
- [Decisions](#decisions)
- [What was cut](#what-was-cut)
- [TOUCH TO START](#touch-to-start)
- [What it will cost](#what-it-will-cost)
- [How this meets the skin plan](#how-this-meets-the-skin-plan)
- [Changes, in stages](#changes-in-stages)
- [Verification](#verification)
- [Later, not in scope](#later-not-in-scope)

## Context

Between `38d8870` ("shiny animation") and `92f3947` ("fps battle fix"), the
bottom screen gave up animation for frame rate. The measured rule then was
`fps = 3600 / (60 + repaints per second)`: a full repaint was 4.9 ms against a
frame with 5.7 ms of slack, because the paint and the ~9 ms rasteriser took
turns on core 0.

`7db93d4` moved the rasteriser to core 2 (core 1 on an Old 3DS), started just
before `CtrBottomUpdate` and collected after it, so the two overlap
([host/main.c:710](host/main.c#L710) to [:735](host/main.c#L735)). Measured on a
New 3DS XL at that commit: rasteriser ~4.5 ms mean and 8.6 ms worst, full paint
2.4 to 3.9 ms, about 10 ms spare per frame, and no dropped frames in five
minutes of play. The budget those compromises were protecting is no longer the
budget.

The outcome wanted: the bottom screen moves the way it was first written to,
with no cost to the game's frame rate, on every console where the rasteriser has
its own core.

## Decisions

- **Keep the single-core tuning behind a guard.** Without a second core
  (`CTR_PPU_THREAD=0`, or no thread could be started) every repaint still costs a
  whole VBlank, so restoring the rates there would bring back the stutter the
  second core fixed. The guard is one bridge function, and that path's code
  already exists, so nothing is rewritten for it.
- **Keep BATTLE ANIM, reword its hint.** It still buys frames on the single-core
  path, and some players may simply prefer still bars and icons in battle.
  Removing it would also mean retiring a byte of `settings.bin`.
- **Restore all of it, in stages, upload first.** The whole-screen upload is the
  largest visible win, and every per-frame animation only looks smooth once it
  is in. The overlay change also fixes a real bug (stalled HP bars), so it
  belongs in whatever else is decided.
- **Measure on the console, not on paper.** The costs below come from a New 3DS
  XL log. Azahar at the default clock overstates the rasteriser about 2x, and at
  300% it understates it (cheatsheet section 7, "Measured after the move").

## What was cut

Visible losses, oldest first. "Before" is how the code was first written.

| # | What | Before | Now | What the player sees today |
|---|---|---|---|---|
| 1 | Bottom upload sliced (`66e6da5`) | whole screen in one frame | 48 rows a frame, five frames per picture ([host/video.c:579](host/video.c#L579)); a new picture waits for a run in flight ([:741](host/video.c#L741)) | every bottom animation shows at 12 fps at most; a tap or tab switch lands up to 5 frames late, up to 9 if a run is in flight, and wipes down in bands |
| 2 | Icon pace (`daa46b6`) | swap every 6 frames, `ICON_ANIM_FRAMES`, the pace of Emerald's own party menu | every 12 frames on the shared clock ([ui/bottom_screen.c:245](ui/bottom_screen.c#L245)) | icons visibly slower than the in-game party menu |
| 3 | Shiny twinkle (`daa46b6`) | every frame, `NOTICE_CYCLE` 64 frames: each size held 4/4/6/4/4 frames, then dark for 42; corners 16 frames apart | 8 steps of 12 frames ([:293](ui/bottom_screen.c#L293), [:352](ui/bottom_screen.c#L352)); each size held 12 frames; corners 24 apart | five changes a second |
| 4 | Shiny glint (`daa46b6`) | a slanted 4 px gold band crossing the panel behind the text over 24 frames (`DrawSweep`, `NOTICE_SWEEP_END`, `SWEEP_W`, `SWEEP_SLANT`) | removed; a two-step "burst" stands in ([:298](ui/bottom_screen.c#L298)) | the effect is gone |
| 5 | Party frozen under an overlay (`ccc5fd7`, `0d221a3`, `5219f5b`, `92f3947`) | icons kept animating under the shiny panel, and bars kept sliding | all six icons baked at frame 0 while the shiny panel or the quick-throw strip is up ([ui/tab_party.c:584](ui/tab_party.c#L584), [:889](ui/tab_party.c#L889)), and the HP block baked at whatever value it had ([:620](ui/tab_party.c#L620)) | the grid stops moving at every action select of a catchable battle, and **a bar that starts sliding under an overlay stalls near its old value** until something else forces a repaint. That part is a bug, not a trade-off |
| 6 | BATTLE ANIM toggle (`92f3947`) | none | EXTRA page 3, default ON ([ui/tab_extra.c:431](ui/tab_extra.c#L431)) | its hint, "off = smoother battles", stops being true |

Efficiency changes from the same commits stay exactly as they are, because they
cost nothing visible and reverting them would only cost time: the snapshot and
restore-rect cheap path (`ccc5fd7`), the 32-bit fills (`15efa7b`), the matchup
memo and the HP block on the animated layer (`92f3947`).

## TOUCH TO START

The title screen's prompt ([ui/ui_title.c](ui/ui_title.c)) arrived after this
plan was written. It is a bottom-screen animation too, but it runs before the
game starts and was never cut for frame rate, so it is not a row above. The plan
touches it in one place:

- **Stage 2 leaves it alone.** It blinks on the title banner's own frame count
  (`Ctr3dsTitlePromptClock()`, 32 frames lit and 32 dark), not on
  `UiAnimStepped()`, and it repaints through its own hash slot, `top[7]`, as a
  full `paint.blank` (the `!sInGame` branch of `Redraw`). Halving
  `UI_ANIM_STEP_FRAMES` does not change its pace.
- **Stage 1 improves it.** The prompt sits in the third 48-row slice, so today
  it lights about 3 frames after PRESS START does on the top screen
  ([TOUCH_TO_START_PLAN.md](TOUCH_TO_START_PLAN.md), "Cost"). With the
  whole-screen upload the two light on the same frame.

## What it will cost

Per-stage costs from the New 3DS XL log: rasteriser 4.5 ms mean, 8.6 ms worst,
on core 2; full paint 2.4 to 3.9 ms; animation step 0.9 to 1.2 ms; bottom copy
0.8 ms; flush plus transfer about 0.25 ms per 48 rows. A whole-screen upload is
therefore about 2 ms.

Where it lands matters more than how big it is. Today the bottom copy and
transfer run **after** the join ([host/video.c:741](host/video.c#L741)), so they
are serial on core 0. They read only the UI's own framebuffer, which is final
once `CtrBottomUpdate` returns, and the rasteriser never touches it or the
bottom texture. Moved **before** the wait at
[host/video.c:693](host/video.c#L693), the upload sits inside the ~4.2 ms that
core 0 currently spends waiting (`ppu.wait`).

| Case | Core 0 inside the overlap | Frame, estimated |
|---|---|---|
| An animation step (icons, twinkle, HP slide) | ~1 ms paint + ~2 ms upload | hidden behind the rasteriser |
| A full repaint (glint frame, overlay change) | 2.4 to 3.9 ms paint + ~2 ms upload | spills 0.25 to 1.75 ms past the join: about 1 + 0.7 + max(8.6, 6) + 0.6 ≈ 11 ms worst |
| Budget | | 16.7 ms |

Two caveats:
- **Fast-forward.** `CtrBottomUpdate` runs on subframe 0 but the render is only
  started on the presenting subframe ([host/main.c:699](host/main.c#L699)), so
  under fast-forward the paint overlaps nothing. That is not a blocker, since the
  rasteriser only runs on presented frames, but it is not free there.
- **An Old 3DS** runs the rasteriser on core 1 at 80% of that core. It is not
  measured and not a target, but it takes the second-core path, so read its log
  before calling it done there.

## How this meets the skin plan

[UI_SKIN_PLAN.md](UI_SKIN_PLAN.md) repaints through the same `Redraw` and
snapshot path, and two of its sections depend on the upload this plan changes:

- **Its step 5, "Press feedback, on the animated layer"**, notes that a press
  shows 1 to 5 frames late because of the 48-row slicing, and defers the fix as
  host-side work outside that plan. Stage 1 here is that fix: the whole picture
  lands on the frame it was painted.
- **Its "What a repaint may cost now"** holds a repaint to being free while
  `paint` finishes before `ppu`. With the upload moved before the join, core 0's
  window becomes paint plus upload, so a full repaint can run up to ~1.75 ms past
  the rasteriser. That spends spare time rather than dropping a frame, but it
  means the skin's "no slower than today" paint budget should be read against
  this plan's numbers once both land.

Neither plan blocks the other. If the skin lands first, the notice's drawing in
stage 3 is written against its panel art instead of `UiWindowFrame`; the clock,
the upload and the overlay changes are unaffected.

## Changes, in stages

Each stage builds and runs on its own, so a console log after any of them says
which change cost what.

| Stage | Contents | Why here |
|---|---|---|
| 1 | The guard, and the whole-screen upload | the only stage that adds work to every repaint, so it gets its own console check |
| 2 | The clock, and the party under an overlay | icon pace, and the stalled-bar fix |
| 3 | The shiny panel | twinkle and glint |
| 4 | The BATTLE ANIM hint, and the docs | wording only |

### Stage 1: the guard and the upload

**Guard.** Add to [bridge.h](bridge.h), in stdint types like everything else
there:

```c
// TRUE when the GBA rasteriser runs on its own core (core 2 on a New 3DS,
// core 1 otherwise). Fixed at start-up, before CtrBottomInit(). When FALSE the
// bottom screen keeps its single-core tuning: every repaint still costs a
// VBlank on that path, see SECOND_SCREEN_ANIMATION_PLAN.md.
int Ctr3dsRasteriserOnOwnCore(void);
```

Implement it in [host/video.c](host/video.c) as `sPpuCore >= 0`
([:181](host/video.c#L181)). `ppu_thread_start()` ([:263](host/video.c#L263))
settles that value inside `CtrVideoInit()`, which `main()` runs before
`CtrBottomInit()`, so it is constant for the session. Every later change applies
only when it returns TRUE.

**Upload.** In `CtrVideoPresent()` ([host/video.c:665](host/video.c#L665)), on
the second-core path:

- When `CtrBottomIsDirty()`, upload the whole bottom screen **before**
  `LightEvent_Wait(&sPpuDone)` ([:693](host/video.c#L693)), then
  `CtrBottomClearDirty()`.
- Use the existing `upload()` ([:527](host/video.c#L527)) with `sBotStage`,
  `BOT_TEX_W`, `CtrBottomFramebuffer()`, `CTR_BOTTOM_WIDTH` x
  `CTR_BOTTOM_HEIGHT`, `&sBotTex` and `kProfBot`. That is exactly the call
  `66e6da5` replaced, so the profile stage names do not change.
- The top upload stays after the join; it needs the rasteriser's output.
- The inline path keeps `snapshot_bottom()` ([:587](host/video.c#L587)) and
  `upload_bottom_slice()` ([:600](host/video.c#L600)) as they are.
- Rewrite the `upload()` comment ("The TOP screen's path, and now only that",
  [:523](host/video.c#L523)) and the `BOT_CHUNK_ROWS` comment
  ([:579](host/video.c#L579)) to say which path uses which, and why.

### Stage 2: the clock, and the party under an overlay

**Clock.** `UI_ANIM_STEP_FRAMES` ([ui/bottom_screen.c:245](ui/bottom_screen.c#L245))
becomes a function of the guard: 6 on the second-core path, 12 otherwise. The
icons already flip on `UiAnimStepped()`
([ui/tab_party.c:248](ui/tab_party.c#L248)), so they get the game's pace back
with no change of their own.

**Party under an overlay.** On the second-core path:

- `DrawCell()` ([ui/tab_party.c:584](ui/tab_party.c#L584)) and the detail view
  ([:889](ui/tab_party.c#L889)) bake `sIconFrame` rather than frame 0 while
  `UiOverlayActive()`.
- `CtrBottomUpdate()` asks for a **full** repaint whenever `UiPartyTick()` moves
  anything while `UiOverlayActive()`
  ([ui/bottom_screen.c:881](ui/bottom_screen.c#L881)). Today that branch skips
  the cheap redraw under the strip ([:894](ui/bottom_screen.c#L894)), and under
  the shiny panel it runs a cheap redraw that draws only sparkles and uploads an
  unchanged screen.
- The baked HP block ([ui/tab_party.c:620](ui/tab_party.c#L620)) already reads
  the sliding value, so the bars slide again and the stall is gone.
- The overlay is painted after the tab, so nothing can draw through it. The rule
  that the snapshot holds nothing that moves still holds: while an overlay is
  up, the party's moving parts are in a full paint, and the cheap path is not
  used for them.

### Stage 3: the shiny panel

On the second-core path only:

- `NoticeTick()` ([ui/bottom_screen.c:311](ui/bottom_screen.c#L311)) counts
  frames rather than steps.
- Restore the per-frame twinkle: `NOTICE_CYCLE` 64 and the original
  `TwinkleSize` thresholds from `38d8870` (`git show 38d8870 --
  3ds/ui/bottom_screen.c`), as a second table beside the step-based one
  ([:352](ui/bottom_screen.c#L352)). Keep the cycle a power of two, for the
  reason the comment above `NOTICE_CYCLE` gives.
- Return TRUE only on frames where some corner's size actually changes, so a
  frame with nothing new uploads nothing.
- Restore `DrawSweep()` from the same commit, drawn in `DrawNotice()`
  ([:428](ui/bottom_screen.c#L428)) behind the rule and the text while the frame
  count is under `NOTICE_SWEEP_END` (24). Each glint frame asks for a full
  repaint, plus one more after the last so the band leaves the snapshot before
  the cheap sparkle path resumes.
- No burst on this path, as in the original.

The single-core path keeps the step-based twinkle and the burst unchanged.

### Stage 4: the hint and the docs

- **BATTLE ANIM** ([ui/tab_extra.c:431](ui/tab_extra.c#L431)): on the
  second-core path the hint reads "off = still in battle" instead of "off =
  smoother battles". The toggle, its setting and its default are unchanged.
- **Cheatsheet section 7:** "So there is one clock", "Keep the step period
  longer than a slice run" and "A moving thing needs a rate its motion survives"
  ([SECOND_SCREEN_CHEATSHEET.md:582](SECOND_SCREEN_CHEATSHEET.md#L582),
  [:594](SECOND_SCREEN_CHEATSHEET.md#L594)) become rules of the single-core
  path. Say what the second-core path does instead.
- **Cheatsheet section 2:** the frame path shows the upload after the join; move
  it before.
- **README-TECHNICAL.md:** one sentence that the bottom upload is whole-screen
  and overlapped on the second-core path.
- **TOUCH_TO_START_PLAN.md:** its "Cost" paragraph says the bottom prompt trails
  the top by about 3 frames. Add that the trail is gone on the second-core path.
  Its "16-frame toggle" there predates the 32-frame blink, which the file's own
  status note already records.
- **Comments that state a single-core rule as universal**, rewritten to say which
  path they describe:
  - the clock comment at [ui/bottom_screen.c:245](ui/bottom_screen.c#L245) and
    the repaint figures at [:762](ui/bottom_screen.c#L762);
  - "Half the game's pace is the price" at
    [ui/tab_party.c:174](ui/tab_party.c#L174), and the 4.9 ms at
    [:297](ui/tab_party.c#L297);
  - [ui/ui_draw.h:157](ui/ui_draw.h#L157), [ui/view_encounters.c:107](ui/view_encounters.c#L107)
    and [ui/ui_shell.h:80](ui/ui_shell.h#L80).
- **Comments that are simply out of date**, fixed while the files are open:
  - [ui/ui_shell.h:84](ui/ui_shell.h#L84) says the shiny notice is the only
    overlay; the quick-throw strip is one too.
  - [ui/ui_shell.h:103](ui/ui_shell.h#L103) says the icons change every six
    frames, and [:122](ui/ui_shell.h#L122) says only sliding slots are redrawn;
    `UiPartyRedrawAnimated` redraws all six.
  - [ui/tab_party.c:569](ui/tab_party.c#L569), [:574](ui/tab_party.c#L574) and
    [:887](ui/tab_party.c#L887) name `UiPartyRedrawIcons`, since renamed.
  - [SECOND_SCREEN_CHEATSHEET.md:575](SECOND_SCREEN_CHEATSHEET.md#L575) names
    `UiPartyIconOnly()`, now `UiPartyAnimOnly()`, and says a slide forces a full
    rebuild; it takes the cheap path now.
  - "Dark for most of the cycle" at
    [ui/bottom_screen.c:350](ui/bottom_screen.c#L350) is wrong for the step
    version, where each corner is lit five steps of eight.

## Verification

**Build.** devkitPro is not installed on the development Mac, so the default
build is checked by the `build-3ds` CI workflow. The UI changes are gated at
runtime, so one binary exercises both tunings; only `host/video.c`'s inline path
is compile-time, behind `CTR_PPU_THREAD`. CI does not build that variant, so
build `make -C 3ds CTR_PPU_THREAD=0` once wherever the CIA is built.

Game-side files (`3ds/ui/*.c`, `src/*.c`) can still be syntax-checked on the Mac
with clang, run from the repository root:

```sh
clang --target=armv6k-none-eabihf -march=armv6k -mfloat-abi=hard -ffreestanding \
  -fsyntax-only -std=gnu11 -isystem <dir with a stub string.h> -iquote include \
  -DMODERN=1 -DRP2350=1 -DPLATFORM_3DS=1 -DCTR_BOOT_DIAG=0 -D__INTELLISENSE__ <file>
```

`-D__INTELLISENSE__` takes the IDE branch in `include/global.h`, which turns
`INCBIN` into `{0}`, so no generated assets are needed. The stub `string.h`
declares `memcpy`, `memset`, `strlen` and the like, because the bare-metal
target has no libc headers. Judge the result by clang's exit status, not by
filtering its output: without the stub the first include is fatal, the file's
own diagnostics never appear, and that reads as a clean pass. Host files
(`3ds/host/*.c`) still need devkitPro or CI.

**Console log, after stage 1 and again after stage 4.** On the New 3DS XL, then
in Azahar, play several wild battles including a shiny test encounter (EXTRA
debug page), with quick throws and back-outs, then read
`sdmc:/3ds/emerald3ds/log.txt`:

- every `frame` worst is about 16.76 ms, with no "missed VBlank" lines;
- `ppu.wait` stays well above zero on most frames, which is core 0 still
  finishing before the rasteriser;
- `upload.bot.*` samples once per repaint rather than once per slice.

**Stop condition.** If the stage 1 log shows any missed frame, or `ppu.wait` near
zero, do not start stage 2. Read `upload.bot.copy/flush/xfer` first. The copy is
the one part that can be removed outright (see [Later](#later-not-in-scope)).

**What to look at:**

- icons swap at the same pace as the in-game START > POKéMON menu;
- the shiny panel opens with the glint crossing behind "SHINY!", and the corners
  twinkle smoothly;
- a tab switch appears in one frame, with no band wiping down the screen;
- on the title screen, TOUCH TO START lights on the same frame as PRESS START on
  the top screen;
- HP bars slide smoothly on the bottom screen;
- under the quick-throw strip and the shiny panel, icons keep moving and bars
  keep sliding, and nothing draws over either panel;
- with `CTR_PPU_THREAD=0`, the bottom screen behaves exactly as today: 12-frame
  clock, the burst, sliced upload, frozen icons under overlays.

## Later, not in scope

Three further savings exist if the upload ever needs to be cheaper. None is
needed for the numbers above.

- **Upload only the dirty 8-row bands.** Any 8-row band is contiguous in both
  the stage and the tiled texture (the note above `BOT_CHUNK_ROWS`). Estimated at
  about 1.0 ms per party step and 0.4 ms per sparkle step.
- **Start the transfer asynchronously.** `GX_DisplayTransfer` waited on before
  `C3D_FrameBegin` gives core 0 the transfer time back.
- **Paint straight into the 512-wide linear stage.** That removes the 0.8 ms
  copy, which exists only because the UI's framebuffer is a 320-wide array in
  ordinary memory.
