# A designed bottom screen, built from images

## Context

The bottom screen works but was never designed. It is drawn entirely from
`UiFillRect` / `UiRect` primitives, and its whole visual identity is borrowed:
every panel is one of Emerald's 20 option-menu window frames, and every ink
colour is read back out of `gStandardMenuPalette`. That was the right call while
the port was proving it could read game state at all, and it is why the code is
littered with defensive decisions (outlines on every arrow, a chevron in theme
colours, a Poke Ball that carries its own dark edge) whose only purpose is to
survive being drawn on 20 backgrounds that run from near-white to near-black.

The cost is that nothing can be composed. There is no ownership of the
background, so no gradient, no shadow, no anti-aliased edge, no shape that is
not an axis-aligned rectangle.

The decision taken here is to **stop borrowing and own the look**: one custom
skin, built from real image assets, across all five tabs. The player's Frame
option keeps governing the top screen exactly as it always did; it stops
governing the bottom one.

Intended outcome: a bottom screen that looks deliberately designed, built from
PNGs that can be repainted in any image editor without touching a line of C.

---

## The pipeline already exists (verified, not assumed)

No romfs, no new tool, no Makefile change, no build-script change.

```c
const u8 sBtnPixels[] = INCGFX_U8("3ds/graphics/skin/button.png", ".8bpp", "-plain");
const u16 sBtnPal[]   = INCGFX_U16("3ds/graphics/skin/button.png", ".gbapal");
```

What each stage does, and where it was confirmed:

| Stage | Behaviour | Evidence |
|---|---|---|
| `tools/generate_wasm_assets.py` | rglobs every `.c`/`.h`/`.inc` in the tree for `INCGFX_*`, runs `gbagfx` per match into `build/assets/`. `3ds/ui/*.c` is already in scope; nothing new is registered. | `source_files()`, `generate_incgfx()` |
| `gbagfx png -> 8bpp -plain` | **Linear, row-major, one byte per pixel.** No tile grid, no 8px alignment, any width, 256 colours. | `WritePlainImage`, `tools/gbagfx/gfx.c:508` |
| `gbagfx png -> gbapal` | Writes the PNG's PLTE as BGR555. | `HandlePngToGbaPaletteCommand` |
| `preproc -g build/assets` | Resolves `root + source + args_as_path + extension` and emits the bytes inline. | `CFile::TryConvertIncgfx`, `c_file.cpp:538` |
| CI | Already runs `generate_wasm_assets.py` before `build_objs.sh`. | `.github/workflows/build-3ds.yml` |

`-plain` is an established in-tree pattern, not a novelty: `src/pokemon.c:1362`
uses it for Spinda's spots.

**Two hard input constraints.** `ReadPng` (`convert_png.c:90`) accepts only
colour type 3 (indexed) or grayscale, and it does **not** call
`png_set_packing`, so a bit-depth-4 PNG would come back packed two pixels per
byte and be silently misread as 8bpp. Every asset must therefore be an
**indexed PNG at bit depth 8**. The generator below enforces both.

---

## Asset set

All under a new `3ds/graphics/skin/`, so everything the port owns stays under
`3ds/` alongside `3ds/ui/` and `3ds/host/`. Index 0 is transparent in every
sheet, matching the convention `UiBlit4bppTile` already uses.

| File | Size | Bytes | Purpose |
|---|---|---|---|
| `bg.png` | 64x64 | 4,096 | Content-area backdrop, tiled |
| `panel.png` | 48x48 | 2,304 | Nine-slice panel, 16px margins |
| `button.png` | 48x96 | 4,608 | Nine-slice button, 3 states stacked (idle / active / pressed), 32px each |
| `tabbar.png` | 64x96 | 6,144 | Tab cell, 2 states stacked, plus the bar's own ground |
| `icons.png` | 120x24 | 2,880 | Five 24x24 tab icons: party, bag, map, dex, extra |
| `chrome.png` | 64x64 | 4,096 | Atlas: arrows, pager pips, scrollbar, dividers |

Roughly **24 KB** of const data, plus six 512-byte palettes. Negligible against
a 64MB system mode.

Deliberately a 64x64 tiling backdrop rather than a 320x240 image. A full-screen
8bpp background is 76,800 bytes, which `preproc` would expand to about 460 KB of
C source text recompiled on every build. If the design later genuinely needs a
unique full-screen backdrop, the escape hatch already exists: `".8bpp.lz"` plus
the game's own `LZDecompressWram` at init. Not needed for a skin built from
patterns, panels and nine-slices.

---

## New code

### `3ds/ui/ui_gfx.h` / `ui_gfx.c` — the sprite layer

Game-side, like the rest of `3ds/ui/`. Draws into the same `sFb` that
`ui_draw.c` owns, so it is a peer of `UiBlit4bppTile`, not a replacement.

```c
enum UiSheetId { UI_SHEET_BG, UI_SHEET_PANEL, UI_SHEET_BUTTON,
                 UI_SHEET_TABBAR, UI_SHEET_ICONS, UI_SHEET_CHROME,
                 UI_SHEET_COUNT };

struct UiNine { u8 l, t, r, b; };   // margins in pixels

void UiGfxInit(void);                                    // from CtrBottomInit
void UiBlitSheet(int x, int y, int sheet);
void UiBlitPart(int x, int y, int sheet, int sx, int sy, int w, int h);
void UiTileFill(int x, int y, int w, int h, int sheet);
void UiNineSlice(int x, int y, int w, int h, int sheet,
                 int bandY, int bandH, const struct UiNine *m);
```

- The sheet table is `const struct { const u8 *px; const u16 *pal; u16 w, h; }
  sSheets[UI_SHEET_COUNT]`, with a parallel `static u16 sPal565[COUNT][256]` in
  `.bss` that `UiGfxInit` fills once via the existing `UiLoadPal`. No const-cast,
  no per-draw conversion, no cache-invalidation logic. 3 KB of `.bss`.
- Every entry point clamps against `0..UI_W` / `0..UI_H` exactly the way
  `UiFillRect` (`ui_draw.c:47`) already does, so an off-screen or oversized
  request is a no-op rather than an overrun.
- `bandY`/`bandH` select one state out of a stacked strip, so the three button
  states are one sheet and one palette rather than three of each.
- Nine-slice **tiles** its edges rather than stretching them. For a 1px edge the
  two are identical; for a thicker edge, tiling keeps a texture readable where
  stretching would smear it.

### `3ds/ui/ui_skin.h` — design tokens

The skin's ink, dim, shadow, accent and ground colours as RGB565 constants, in
one place, replacing the ad-hoc `UI_COL_*` block currently in `ui_shell.h:31-52`.
The HP-bar and Poke Ball colours stay where they are: those are the game's own
art colours and are correct as they stand.

---

## The pivot: two functions change, ~98 call sites do not

This is what makes a total visual overhaul a small diff.

**`UiWindowFrame(tx, ty, wTiles, hTiles)`** (`ui_draw.c:164`) has **10 call
sites** across four tabs, all of them panels. Reimplement its body as a
`UiNineSlice` of `UI_SHEET_PANEL` and every one of them is reskinned untouched.
Add `UiPanel(x, y, w, h)` in pixels for new code, and make the tile-granular
function a one-line wrapper, so the 8px grid stops being a constraint on
anything written from here on.

**`UiThemeText()` / `UiThemeShadow()`** (`ui_draw.c:202-210`) have **~88 call
sites**. Return the skin's ink colours instead of reading `gStandardMenuPalette`
and every label on the screen becomes consistent in one edit.

Also drop `top[0] = UiFrameId()` from `UiStateHash()`
(`bottom_screen.c:159`). Once the frame no longer drives the bottom screen it
is a stale input to the repaint hash. `UiFrameId()` itself stays: it is still
correct, and the top screen still uses the setting.

---

## Per-surface work

Ordered so the screen is coherent after every step, never half-reskinned.

**1. Foundation.** `ui_gfx`, `ui_skin.h`, the pivot above, `UiGfxInit()` from
`CtrBottomInit`, and `UiClear` replaced by a `UiTileFill` of the backdrop in
`Redraw()` (`bottom_screen.c:267`). At the end of this step all five tabs
already look new, with no per-tab edits at all.

**2. Tab bar.** `DrawTabBar` (`bottom_screen.c:226`) currently draws flat
rectangles. Becomes: the bar's ground, a per-cell art state, a 24x24 icon from
`icons.png`, and the label beneath it. **`UI_TABBAR_H` stays at 48 and
`UI_CONTENT_H` stays at 192** — every tab's pixel layout is derived from those,
and moving them would cascade through five files for no design gain.

**3. Buttons.** `DrawButtonH` (`tab_extra.c:140`) is the single chokepoint every
button on the EXTRA tab passes through, both pages, including the pager and the
new MUSIC toggle. One function becomes a `UiNineSlice` plus a centred label.

**4. Press feedback.** Currently a tap has no visual response at all until
release. `ui_draw.c` gains `UiSetPointer(const CtrTouchState *)`, called once per
frame from `CtrBottomUpdate`, and a no-argument `UiPressed(x, y, w, h)` any
drawer can consult without being handed the touch state. `CtrBottomUpdate` then
marks dirty on `justPressed || justReleased`. About 15 lines, and it is what
makes the art's third state mean anything.

**5. Per-tab refinement.** Chrome art for `UiArrow` and the dex/bag scroll
indicators, panel headers, and the PARTY tab's cheat-tag strip
(`tab_party.c:38`). Cosmetic polish on a screen that is already consistent.

---

## The generator: `3ds/graphics/skin/gen_skin.py`

Pure stdlib, matching `generate_wasm_assets.py`'s existing discipline: `zlib`
plus `struct` to write IHDR / PLTE / tRNS / IDAT / IEND directly. No Pillow, no
new CI dependency.

**Run once, PNGs committed.** The script is committed for reproducibility and
for regenerating the whole set after a palette change, but the build does not
invoke it. CI is untouched, and the PNGs are ordinary indexed images that open
in any editor. Repainting a button means editing `button.png` and rebuilding.

The script owns one master 64-colour palette shared by every sheet, so the art
is coherent by construction rather than by discipline, and emits colour type 3
at bit depth 8 to satisfy the two `ReadPng` constraints above.

---

## Verification

```sh
python3 3ds/graphics/skin/gen_skin.py     # once, or after a palette change
python3 tools/generate_wasm_assets.py     # picks up the new INCGFX lines
bash 3ds/build_objs.sh && make -C 3ds
```

- **Pipeline, before any drawing code.** Confirm
  `build/assets/3ds/graphics/skin/button.png_plain.8bpp` is exactly
  `48 * 96 = 4608` bytes. A wrong size here means the PNG's bit depth is not 8,
  which is the one failure mode that would otherwise surface as garbled art
  rather than an error.
- **In Azahar**, per step, since each leaves the screen coherent: after step 1
  every tab is reskinned with no per-tab edits; after step 2 the bar has icons;
  after step 3 EXTRA's buttons are art on both pages; after step 4 a held button
  visibly depresses.
- **Clipping**, which is where a new blitter fails silently rather than loudly:
  the BAG target picker (`tab_bag.c:440`) and the PARTY cells
  (`tab_party.c:268`) both draw panels at computed offsets near the screen edge.
  Watch those two rather than the static layouts.
- **Repaint cost.** The screen is hash-gated, so a repaint is rare, but step 1
  replaces a flat `UiClear` with a tiled fill plus palette lookups over the full
  320x192. Confirm the frame rate is unchanged while walking with the PARTY tab
  up, which is the case that repaints on every HP change.
- **Do not regress the GBA path.** Everything here is new files under `3ds/`
  plus edits confined to `3ds/ui/`, none of which the matching build compiles,
  so `make compare` stays byte-identical. Worth running once at the end anyway.

---

## Risks and non-goals

- **1-bit alpha.** Index 0 is transparent; there is no blending. Anti-aliased
  edges therefore have to be baked against a known background, which the skin
  can do precisely because it now owns that background. This is the concrete
  payoff of replacing the frames rather than coexisting with them.
- **`UI_TABBAR_H` is load-bearing.** Five files derive their layout from
  `UI_CONTENT_H`. Explicitly out of scope.
- **Compile time.** Each `INCGFX` array is expanded to C source text by
  `preproc`. At the sizes above this is a few hundred KB total and unnoticeable;
  it would stop being unnoticeable at full-screen images, which is the second
  reason the backdrop is a tile.
- **The 20 frames do not disappear**, they stop applying here. If the bottom
  screen ends up looking disconnected from a player's chosen frame on the top
  screen, the fallback is the tinted variant considered and rejected above, and
  it is additive: the skin would keep its shapes and take its fill from the
  frame palette.
