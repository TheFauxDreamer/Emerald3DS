# 3DS port roadmap

Outstanding work on the port, and the reasoning behind the decisions already
taken. Companion to `3ds/README.md`, which describes how the port is built and
why it is structured the way it is.

---

# Part A — Top-screen sharpness

## Context

The top screen looks soft, and the obvious question is whether the GBA image can
be rendered at 2x for sharpness.

Two findings shape everything below.

**1. 2x is impossible.** `240x160 * 2 = 480x320`; the top screen is `400x240`.
It exceeds *both* dimensions, so it cannot be done at any setting — not by
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

Because filling the panel is not a priority, pixel-perfect **1x** is viable —
the only genuinely artifact-free option, since one GBA pixel maps to exactly one
3DS pixel with no resampling. Vertical 1.5x is otherwise a fixed 2:3 ratio and
cannot be improved.

## A.1 — Emulator settings (no code; do this first)

Quit Azahar first — it rewrites its config on exit — then in
`~/Library/Application Support/Azahar/config/qt-config.ini`, setting each key's
matching `\default=false` alongside it:

- `filter_mode=false` — nearest present instead of bilinear. Biggest single win.
- `resolution_factor=4` — rasterises the quad at 4x internally so nearest-sampled
  GBA pixels stay crisp when enlarged. Valid range 0–18.
- `use_integer_scaling=true` — avoids fractional window rescaling.

Judge the result before building anything. This may resolve the complaint
entirely, in which case A.2 is optional polish.

## A.2 — Selectable top-screen scale

Only `3ds/host/video.c` and `3ds/Makefile` change. This is host-side, so unlike
`CTR_BOOT_DIAG` it does **not** need mirroring into `3ds/build_objs.sh` — no
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
reallocation concern — `gfxSetScreenFormat` already sizes the top framebuffer
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

## A — Verification

```sh
for s in 1 2 3; do make -C 3ds clean && make -C 3ds CTR_TOP_SCALE=$s; done
```

- **Mode 2 is the regression test** — must be pixel-identical to today: 360x240
  centred, 20px bars. Any difference means the one-scale-to-two refactor is wrong.
- **Mode 1** — 240x160 centred with wide borders, and *perfectly* sharp: no
  uneven row or column widths anywhere. This is the reference for "correct"; if
  it is not crisp, the remaining blur is Azahar's, not the port's.
- **Mode 3** — same physical size as mode 2, but vertical edges on sprites and
  text noticeably cleaner. Check fill cost: wide mode doubles top-screen fill,
  and since `C3D_FrameEnd` blocks on VBlank a regression shows as sluggishness.
- **Unaffected each time** — bottom screen, tab switching, party grid, touch.

## A — Risks

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

# Part B — Bottom-screen UI (Stages 3–5 outstanding)

Stages 0–2 are implemented: `ui_draw.c` (4bpp blitter, BGR555→RGB565, Emerald
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

## Stage 3 — BAG and item use

The only stage that mutates game state, so the gate is the design.

- List the current pocket with `BagGetItemIdByPocketPosition()` and
  `BagGetQuantityByPocketPosition()`; names via `GetItemName()`, flavour text via
  `GetItemDescription()`. Pocket switcher across the top.
- Flow: pick a mon in PARTY → BAG → tap item → apply to the selected mon.
- **Safety gate**, re-checked immediately before mutation:
  - `gMain.callback2 == CB2_Overworld` (`include/overworld.h:131`)
  - `!ArePlayerFieldControlsLocked()` (`include/script.h:36`)
  - `!ScriptContext_IsEnabled()` (`include/script.h:38`)
  - not in battle — **confirm the exact flag first**; `gMain.inBattle` did not
    grep cleanly in `include/main.h`.
- Apply `PokemonUseItemEffects(mon, item, partyIndex, 0, FALSE)`. Note the
  **inverted return**: `FALSE` means the item *did* have an effect. Only then
  `RemoveBagItem(item, 1)`.
- If the gate fails, say so on screen rather than silently ignoring the tap.

Why this point is safe: `CtrBottomUpdate` runs from `Rp2350PresentFrame()`, at
the end of `WasmRunFrame()` *after* `VBlankIntr()` — the frame's callbacks have
finished and the next has not started.

Item use changes the save; the deferred flush in `3ds/host/save.c` covers it.
Ensure the party hash changes so the grid repaints.

## Stage 4 — MAP

Location readout, no new graphics decoding: current area via
`GetMapNameGeneric()` with `GetCurrentRegionMapSectionId()`, player coordinates,
and nearby landmarks via `GetLandmarkName()`. A rendered region map with a
player marker is a follow-on once the 4bpp blit path has proven itself.

## Stage 5 — DEX

Seen/caught counts from `GetHoennPokedexCount()` / `GetNationalPokedexCount()`,
a scrollable list filtered by `GetSetPokedexFlag()`, and per-entry category,
height, weight and description from `struct PokedexEntry`.

## B — Verification

Build with diagnostics on so failures are visible:

```sh
CTR_BOOT_DIAG=1 3ds/build_objs.sh && make -C 3ds CTR_BOOT_DIAG=1
```

- **Stage 3 is the real test.** Damage a Pokémon, use a Potion from the touch
  screen, then open Emerald's own party menu: HP must match and the bag count
  must have decremented. Then verify the gate — tap the same item during a
  battle and mid-cutscene and confirm nothing happens.
- **Stage 4** — walk between areas, including indoors and caves, and confirm the
  name tracks.
- **Regression each stage** — top screen unaffected, and
  `~/Library/Application Support/Azahar/sdmc/3ds/emerald3ds/pokeemerald.sav`
  still updates after an in-game save.

## B — Risks

- **Item use is the only state-mutating path.** Get the gate right before
  anything else in Stage 3 — a wrong gate corrupts saves rather than crashing.
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

## Debugging notes

A crash log gives only a raw PC. Resolve it against the ELF from that same CI
run (published as the `emerald3ds-elf` artifact alongside `emerald3ds.map`).

`svcOutputDebugString` output needs `Debug.Emulated:Debug` in Azahar's
`log_filter`, or the traces are discarded before reaching the log.
