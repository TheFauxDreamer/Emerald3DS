# TOUCH TO START on the bottom screen

**Status: proposed, not implemented.** Line numbers below were taken on
2026-09-12 at `ea832a8`; the function names are the stable anchors.

## Context

On the title screen the top screen flashes the game's "PRESS START" banner, while
the 3DS bottom screen sits blank (black) until the player reaches the overworld,
and every touch is ignored before that. The request: show "TOUCH TO START" on the
bottom screen in the same style as the banner, and make a tap there start the game
exactly as pressing START does.

What the exploration found:

- The banner is sprite art, `graphics/title_screen/press_start.png`
  (`gTitleScreenPressStartGfx` / `gTitleScreenPressStartPal`). It uses a custom
  7px-tall bitmap font with 2px strokes, a 1px black outline around every stroke,
  and rows shaded white, white, lavender, grey, grey. It only contains the letters
  P R E S T A, so the whole word START (including its T) can be copied verbatim,
  but O, U, C and H have to be drawn to the same rules.
- The blink is `SpriteCB_PressStartCopyrightBanner` (`src/title_screen.c:409`):
  it toggles `sprite->invisible` every 16 frames while `sAnimate` is TRUE.
- START is taken in `Task_TitleScreenPhase3` (`src/title_screen.c:780`), which
  only runs while `gMain.callback2 == MainCB2`.
- The bottom screen paints its pre-game screen in `Redraw()`'s `!sInGame` branch
  (`3ds/ui/bottom_screen.c:666`), and `CtrBottomUpdate()` drops every touch while
  `!sInGame` (`:795`). `SoftReset` is a no-op on this port, so the title is only
  ever reached before `sInGame` latches.
- The top screen defaults to 1.5x, so the banner shows 10.5px tall. Drawn at 2x
  on the bottom screen, the prompt is 232x14.

## Approach

The bottom screen follows the real banner sprite's visibility, so the two blink
together, and a tap sets a one-frame flag that the title task reads next to its
existing START check. That way the title's own code runs the transition, so the
fade and music are identical and no second copy of that logic exists.

### 1. `src/title_screen.c` + `include/title_screen.h` (all `#if PLATFORM_3DS`)

Header, following the `include/option_menu.h` pattern:

```c
#if PLATFORM_3DS
enum { CTR3DS_TITLE_PROMPT_NONE, CTR3DS_TITLE_PROMPT_LIT, CTR3DS_TITLE_PROMPT_DARK };
u8   Ctr3dsTitlePromptState(void);
void Ctr3dsTitleTouchStart(void);
#endif
```

In `title_screen.c`, placed after `CreateCopyrightBanner` and before `#undef
sAnimate` so it can use that name:

- `Ctr3dsTitlePromptState()`: NONE unless `gMain.callback2 == MainCB2 &&
  FuncIsActiveTask(Task_TitleScreenPhase3)`. The callback test is what makes the
  prompt disappear the moment START is taken or the music runs out, because the
  task lingers while `MainCB2` stops running it. After that it scans `gSprites`
  for the first sprite that is `inUse`, has `callback ==
  SpriteCB_PressStartCopyrightBanner` and `sAnimate == TRUE` (PRESS START, not
  the copyright line), and returns DARK or LIT from its `invisible` bit.
- `Ctr3dsTitleTouchStart()` sets a `static bool8 sTouchedStart`.
- `Task_TitleScreenPhase3`: the 3DS build reads and clears the flag at the top
  of every run, unconditionally, so a tap that lands on the same frame as a
  button press cannot stay pending into a later visit (B on the main menu returns
  here). It then ORs the flag into the existing condition. The `#else` branch
  keeps the original line exactly as it is, so the GBA/`make compare` build does
  not change:

```c
#if PLATFORM_3DS
    bool8 touchedStart = sTouchedStart;

    sTouchedStart = FALSE;
    if (JOY_NEW(A_BUTTON) || JOY_NEW(START_BUTTON) || touchedStart)
#else
    if (JOY_NEW(A_BUTTON) || JOY_NEW(START_BUTTON))
#endif
```

The flag is only ever set when the prompt is up at the end of a frame, and the
next frame's `MainCB2` runs this task, so it is always consumed.

### 2. New `3ds/ui/ui_title.c` + `ui_title.h`

Same shape as `ui_quickball.[ch]`, the documented pattern for a self-contained
piece the shell calls into. Picked up by the `3ds/ui/*.c` glob, and the `ui_`
prefix avoids the object-name collision hazard.

- The art is `static const char *const sArt[7]`, 116 columns, where `' '` is
  transparent and `'1'`..`'5'` are palette indices. It reads as the letters in
  source. START and the T come verbatim from `press_start.png` (x 83..124 and
  91..100, rows 1..7). O, U, C and H follow the banner's rule: a 1px black
  outline on every pixel touching a stroke, with enclosed counters left black.
  The word gap is 7 columns, as between PRESS and START. Preview:

```
111111111111111111111 111111111111111 1111       111111111111111111       111111111111111111  11  1111111 1111111111
155555555155555551551 155155555551551 1551       155555555155555551       155555551555555551 1551 155555511555555551
111155111155111551551 15515511111155111551       111155111155111551       155111111111551111155551155111551111551111
   1441  144111441441 1441441    144444441          1441  144111441       144444441  1441  1441144144111441  1441
   1331  133111331331113313311111133111331          1331  133111331       111111331  1331 13311113333133311  1331
   1331  1333333313333333133333331331 1331          1331  133333331       133333331  1331 13333311333113331  1331
   1111  1111111111111111111111111111 1111          1111  111111111       111111111  1111 11111111111111111  1111
```

- Colours come from the game's own `gTitleScreenPressStartPal` through
  `UiLoadPal` (converted once into a static, first draw), not hardcoded.
- `UiTitleDraw()`: only when `Ctr3dsTitlePromptState() == LIT`, paints each
  non-transparent pixel as a 2x2 `UiFillRect`, centred on the full 320x240 screen
  at (44, 113). There is no tab bar before the game, so the content-area limit
  does not apply.
- `UiTitleTouch(t)`: `t && t->justReleased && state != NONE` calls
  `Ctr3dsTitleTouchStart()`. A tap anywhere counts, and it acts on release
  (the codebase convention). It still works during the dark half of the blink.
- `UiTitleStateKey()`: returns the 0/1/2 state for the repaint hash.

### 3. `3ds/ui/bottom_screen.c`

- `#include "ui_title.h"`.
- `Redraw()` `!sInGame` branch: call `UiTitleDraw()` after `UiClear(0)`. Update
  the lifecycle comment at `:502` ("blank on the title screen") to match.
- `CtrBottomUpdate()`: `if (!sInGame) { UiTitleTouch(touch); touch = NULL; }` in
  place of the bare `touch = NULL`, with the comment updated.
- `UiStateHash()`: `u32 top[8]`, `top[7] = sInGame ? 0 : UiTitleStateKey();` in a
  slot of its own (per the cheatsheet's no-shared-slots rule). Nothing else in the
  hash moves when the banner blinks, so without this slot the prompt would never
  be drawn. It reads only `gMain`, `gTasks` and `gSprites`, so it is safe before
  save data exists.

Cost: one blank repaint (`UiClear` plus about 600 2x2 fills) every 16 frames,
only on the title screen, overlapped with the rasteriser on core 2. The
five-frame sliced upload fits inside the 16-frame toggle. Because slices upload
top down and the prompt sits in the third slice, the bottom trails the top by
about 3 frames.

### 4. Docs: `3ds/SECOND_SCREEN_CHEATSHEET.md`

- Section 4 file map: a row for `ui/ui_title.c`.
- Section 6 dispatch: before the game the only touch taken is the title prompt.
- Section 7 `UiStateHash` slot table: `top[7]`.

## Out of scope (deliberately)

Touch does not skip the logo-shine phases, the intro cutscene or the copyright
screen. Before PRESS START appears there is nothing drawn to tap, which follows
the cheatsheet's "a control that is not drawn must not be tappable" rule. The
bottom background stays black, as it is now.

## Verification

1. Build: `3ds/build_objs.sh && make -C 3ds` (both `src/` and `3ds/ui/` change;
   `bridge.h` does not).
2. Azahar (300% clock): boot through the intro. When PRESS START appears on the
   top screen, TOUCH TO START appears centred on the bottom, blinking in step.
   Check the letters against the top banner.
3. Tap the bottom screen: the music fades, the screen fades to white and the main
   menu opens, identical to pressing START, and the bottom goes blank at once.
4. Taps during the logo shine and the intro do nothing.
5. Press B on the main menu to return to the title: the prompt returns and does
   not start on its own (the stale-flag case). Also press START on the same frame
   as a tap, then B back: no auto-start.
6. Leave the title idle until the music ends: it returns to the intro and the
   prompt disappears.
7. Continue into the game: the tabs behave exactly as before.
8. Repeat 2 and 3 on the New 3DS XL.
9. The non-3DS `Task_TitleScreenPhase3` line is textually unchanged. If the GBA
   toolchain is set up, `make compare` confirms it.
