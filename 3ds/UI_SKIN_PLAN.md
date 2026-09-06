# A designed bottom screen, built from images

**Status: proposed, not implemented.** Companion documents:
[SECOND_SCREEN_CHEATSHEET.md](SECOND_SCREEN_CHEATSHEET.md) (the code as it
actually is, read it first), [SECOND_SCREEN_PLAN.md](SECOND_SCREEN_PLAN.md)
(feature and refactor catalogue, also not implemented).

## Context

The bottom screen works but was never designed. It is drawn entirely from
`UiFillRect` / `UiRect` primitives, and its whole visual identity is borrowed:
every panel is one of Emerald's 20 option-menu window frames, and every ink
colour is read back out of `gStandardMenuPalette` at runtime
([ui_draw.c:203](ui/ui_draw.c#L203)). That was the right call while the port was
proving it could read game state at all, and it is why the code is littered with
defensive decisions (outlines on every arrow, a chevron in theme colours, a Poke
Ball that carries its own dark edge) whose only purpose is to survive being drawn
on 20 backgrounds that run from near-white to near-black.

The cost is that nothing can be composed. There is no ownership of the
background, so no gradient, no shadow, no anti-aliased edge, no shape that is
not an axis-aligned rectangle. Buttons are 1px outlines, reimplemented three
separate times ([tab_extra.c:182](ui/tab_extra.c#L182),
[tab_map.c:452](ui/tab_map.c#L452), [tab_bag.c:421](ui/tab_bag.c#L421)), and the
three BACK buttons are three different sizes.

The decision taken here is to **stop borrowing and own the look**: one custom
skin, built from real image assets, across all five tabs, with the layout itself
driven by wireframes drawn at 320x240 rather than by the accumulated
hand-measured constants. The player's Frame option keeps governing the top
screen exactly as it always did; it stops governing the bottom one.

Intended outcome: a bottom screen that looks deliberately designed, built from
PNGs that can be repainted in any image editor without touching a line of C.

An earlier revision of this document froze `UI_TABBAR_H` and `UI_CONTENT_H` and
reskinned without re-laying-out. Both are open now, which is what puts the tab
internals in scope and what makes the wireframe the source of truth rather than
a sketch.

---

## The pipeline already exists (verified, not assumed)

No romfs, no tex3ds, no new tool, no Makefile change, no build-script change.

```c
const u8  sBtnPixels[] = INCGFX_U8 ("3ds/graphics/skin/button.png", ".8bpp", "-plain");
const u16 sBtnPal[]    = INCGFX_U16("3ds/graphics/skin/button.png", ".gbapal");
```

| Stage | Behaviour | Evidence |
|---|---|---|
| `tools/generate_wasm_assets.py` | rglobs every `.c`/`.h`/`.inc` in the tree for `INCGFX_*`, runs `gbagfx` per match into `build/assets/`. `3ds/ui/*.c` is already in scope; nothing new is registered. | `source_files()`, `generate_incgfx()` |
| `gbagfx png -> 8bpp -plain` | **Linear, row-major, one byte per pixel.** No tile grid, no 8px alignment, any width, 256 colours. | `WritePlainImage`, [gfx.c:508](../tools/gbagfx/gfx.c#L508) |
| `gbagfx png -> gbapal` | Writes the PNG's PLTE as BGR555. | `HandlePngToGbaPaletteCommand` |
| `preproc -g build/assets` | Resolves `root + source + args_as_path + extension` and emits the bytes inline. | `CFile::TryConvertIncgfx`, `c_file.cpp:538` |
| CI | Already runs `generate_wasm_assets.py` before `build_objs.sh`. | `.github/workflows/build-3ds.yml` |

`-plain` is an established in-tree pattern, not a novelty: `src/pokemon.c:1362`
uses it for Spinda's spots.

### Four hard rules for every asset

| Rule | Why | Where confirmed |
|---|---|---|
| **Indexed PNG, colour type 3** | `ReadPng` rejects RGB and RGBA outright | [convert_png.c:90](../tools/gbagfx/convert_png.c#L90) |
| **Bit depth 8** | a depth-4 file is re-packed as one flat bitstream that ignores per-row padding, so any odd width shears silently | `ConvertBitDepth`, [convert_png.c:47](../tools/gbagfx/convert_png.c#L47) |
| **Palette index 0 is transparent, and nothing else may use it** | there is no alpha channel and no blending anywhere in the blitters | `UiBlit4bppTile`, [ui_draw.c:75](ui/ui_draw.c#L75) |
| **256 colours maximum per sheet** | one byte per pixel | `WritePlainImage` |

Aseprite, GIMP and Photoshop all export this (Aseprite: Indexed mode, then Save
As PNG; GIMP: Image > Mode > Indexed, then export). The `skin.py check`
subcommand below fails loudly on a file that misses any of the four, so a bad
export is caught as an error rather than as garbled art.

Two consequences that shape what gets drawn:

- **No soft edges against unknown backgrounds.** 1-bit alpha means
  anti-aliasing has to be baked against a known ground. That is precisely why
  the skin takes ownership of the backdrop instead of sitting on the player's
  chosen frame: once it owns the ground, baked AA is correct everywhere. This is
  the concrete payoff of replacing the frames rather than coexisting with them.
- **No full-screen 320x240 images.** `preproc` expands each asset to C *source
  text*, so 76,800 bytes becomes roughly 460KB of source recompiled on every
  build. The backdrop is a 64x64 tile for that reason. If a unique full-screen
  image is ever genuinely needed, the escape hatch is `".8bpp.lz"` plus the
  game's own `LZDecompressWram` at init.

---

## Step 0: the regeneration trap, before any drawing

[generate_wasm_assets.py:27](../tools/generate_wasm_assets.py#L27):

```python
def run_gbagfx(input_path, output_path, options=()):
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        return          # never rebuilds
```

**Editing a PNG does not regenerate its asset.** `make clean-assets` works but
nukes every asset in the tree and forces a full regen. Add
`3ds/graphics/skin/refresh.sh`: `rm -rf build/assets/3ds/graphics/skin`, then run
`generate_wasm_assets.py`. Three lines, and it is the difference between a
five-second art loop and a baffling one.

Prove the pipeline with a single throwaway asset before writing any drawing
code. `build/assets/3ds/graphics/skin/button.png_plain.8bpp` must be exactly
`width * height` bytes. A wrong size there means the bit depth is not 8, which
is the one failure mode that would otherwise surface as garbled art rather than
as an error.

---

## Step 1: wireframe intake

`3ds/graphics/skin/wireframe/` holds one PNG per surface, committed as the
design record: `shell.png` (the bar and content split), `party.png`,
`party_detail.png`, `bag.png`, `map.png`, `dex.png`, `extra_p1.png`,
`extra_p2.png`. Each is exactly 320x240, or an integer 2x / 4x multiple.

These are reference only and are never compiled: `generate_wasm_assets.py`
converts a PNG only when some `INCGFX_*` line names it, so an unreferenced file
is inert. Any colour format, any bit depth, layers flattened. The four hard
rules above do not apply to them.

**Legend.** Each widget is drawn as a flat block of one solid colour, one colour
per widget class, so the probe can classify as well as measure. The exact values
are recorded once in `skin.py`:

| Class | Meaning |
|---|---|
| panel | a nine-sliced container |
| button | a tappable control (probe emits both the draw rect and the `UiHit` rect) |
| text | a label or readout slot; its width is the truncation budget |
| art | a game-art blit: mon icon, item icon, dex pic, type icon, HP bar |
| chrome | arrow, pip, divider, scrollbar |

`skin.py probe <file>` decodes the PNG with stdlib `zlib`, finds the connected
regions of each legend colour, and prints a table of `x, y, w, h` plus a
suggested `#define` name. That table is what becomes the tab's constants, so
what was drawn and what the code does cannot drift.

`shell.png` is drawn and settled first: `UI_TABBAR_H` and `UI_CONTENT_H` are
consumed by all five tab files, so every other wireframe is drawn against
whatever split it establishes.

Text slots are subject to the font, not the other way round. There is one font
and two sizes: `UI_GLYPH_H` 15 with `UI_LINE_H` 16, and `UiTextBig` at 2x
nearest-neighbour for `UI_GLYPH_BIG_H` 30. Wireframe text blocks have to be
drawn to those heights.

---

## Step 2: the art assets

All under `3ds/graphics/skin/`, so everything the port owns stays under `3ds/`
alongside `3ds/ui/` and `3ds/host/`. Index 0 is transparent in every sheet,
matching the convention `UiBlit4bppTile` already uses.

| File | Size | Bytes | Purpose |
|---|---|---|---|
| `bg.png` | 64x64 | 4,096 | content backdrop, tiled, so it must tile seamlessly |
| `panel.png` | 48x48 | 2,304 | nine-slice panel, 16px margins |
| `button.png` | 48x144 | 6,912 | nine-slice button, three states stacked (idle / active / pressed), 48px each |
| `tabbar.png` | 64x96 | 6,144 | tab cell, two states stacked, plus the bar's own ground |
| `icons.png` | 120x24 | 2,880 | five 24x24 tab icons, in `enum UiTab` order |
| `chrome.png` | 64x64 | 4,096 | atlas: arrows, pager pips, scrollbar, dividers |

Roughly 26KB of const data, plus six 512-byte palettes. Negligible against a
64MB system mode.

`tabbar.png`'s cell width follows `320 / visibleCount`: 64px at five tabs, 53px
at six, which is about the practical floor for a fingertip. The sizes above are
the starting set; the wireframes may move them, which is exactly why nothing on
the C side hardcodes a dimension.

**Dimensions come from the PNGs, never from a table typed twice.**
`3ds/graphics/skin/skin.py` is pure stdlib (`zlib` plus `struct` to write IHDR /
PLTE / tRNS / IDAT / IEND directly), matching `generate_wasm_assets.py`'s
existing no-dependency discipline, with four subcommands:

- `gen` writes correctly-formatted placeholder PNGs at every required size, so
  the screen is functional from the first build and the art is repainted in
  place afterwards.
- `check` validates the four hard rules, plus that no visible pixel uses
  index 0.
- `header` reads the committed PNGs and emits `3ds/ui/ui_gfx_sheets.h` carrying
  each sheet's real width, height and nine-slice margins. Repainting at a new
  size means rerunning `header`; the C side never carries a hand-typed
  dimension.
- `probe` is step 1's wireframe measurement.

The script is committed for reproducibility and for regenerating the set after a
palette change, but **the build never invokes it**. CI is untouched, and the
PNGs are ordinary indexed images that open in any editor.

`gen` owns one master palette shared by every sheet, so the set is coherent by
construction rather than by discipline.

---

## Step 3: the sprite layer

### `3ds/ui/ui_gfx.h` / `ui_gfx.c`

Game-side, like the rest of `3ds/ui/`. Draws into the same `sFb` that
[ui_draw.c:22](ui/ui_draw.c#L22) owns, so it is a peer of `UiBlit4bppTile`, not
a replacement. The `ui_*` prefix is mandatory: `build_objs.sh` writes every
object into one flat directory, so a `3ds/ui/` basename colliding with a `src/`
one silently deletes that game feature (cheatsheet section 12).

```c
enum UiSheetId { UI_SHEET_BG, UI_SHEET_PANEL, UI_SHEET_BUTTON,
                 UI_SHEET_TABBAR, UI_SHEET_ICONS, UI_SHEET_CHROME,
                 UI_SHEET_COUNT };

void UiGfxInit(void);                                    // from CtrBottomInit
void UiBlitSheet(int x, int y, int sheet);
void UiBlitPart(int x, int y, int sheet, int sx, int sy, int w, int h);
void UiTileFill(int x, int y, int w, int h, int sheet);
void UiNineSlice(int x, int y, int w, int h, int sheet, int band);
```

- The sheet table is `const struct { const u8 *px; const u16 *pal; u16 w, h; }
  sSheets[UI_SHEET_COUNT]`, with a parallel `static u16 sPal565[COUNT][256]` in
  `.bss` that `UiGfxInit` fills once via the existing `UiLoadPal`. No
  const-cast, no per-draw conversion, no cache-invalidation logic. 3KB of
  `.bss`.
- Every entry point clamps against `0..UI_W` / `0..UI_H` exactly the way
  `UiFillRect` ([ui_draw.c:47](ui/ui_draw.c#L47)) already does, so an off-screen
  or oversized request is a no-op rather than an overrun.
- `band` selects one state out of a stacked strip, so the three button states
  are one sheet and one palette rather than three of each.
- Nine-slice **tiles** its edges rather than stretching them. For a 1px edge the
  two are identical; for a thicker edge, tiling keeps a texture readable where
  stretching would smear it.

### `3ds/ui/ui_skin.h`

The skin's ink, dim, shadow, accent and ground colours as RGB565 constants in
one place, replacing the ad-hoc `UI_COL_*` block at
[ui_shell.h:31](ui/ui_shell.h#L31), and taking over `UI_TABBAR_H` /
`UI_CONTENT_H` from [ui_shell.h:14](ui/ui_shell.h#L14). The HP-bar, shiny-gold
and Poke Ball colours stay where they are: those are the game's own art colours,
hardcoded from the art for reasons documented at their definitions, and are
correct as they stand.

---

## Step 4: the pivot, where a total overhaul becomes a small diff

Two function bodies change and about 98 call sites do not.

**`UiWindowFrame(tx, ty, wTiles, hTiles)`** ([ui_draw.c:165](ui/ui_draw.c#L165))
has **10 call sites** across four tabs, all of them panels. Reimplement its body
as a `UiNineSlice` of `UI_SHEET_PANEL` and every one of them is reskinned
untouched. Add `UiPanel(x, y, w, h)` in pixels for new code and make the
tile-granular function a one-line wrapper, so the 8px grid stops being a
constraint on anything written from here on.

**`UiThemeText()` / `UiThemeShadow()`** ([ui_draw.c:203](ui/ui_draw.c#L203))
have **about 88 call sites**. Return the skin's ink colours instead of reading
`gStandardMenuPalette` and every label on the screen becomes consistent in one
edit.

Then drop `top[0] = UiFrameId()` from `UiStateHash()`
([bottom_screen.c:304](ui/bottom_screen.c#L304)). Once the frame no longer
drives the bottom screen it is a stale input to the repaint hash. `UiFrameId()`
itself stays: it is still correct, and the top screen still uses the setting.

Replace the `UiClear` in `Redraw()` with a `UiTileFill` of the backdrop.

**At the end of this step all five tabs already look new, with no per-tab edits
at all.** That is the checkpoint worth building to before anything else.

---

## Step 5: one shared button, replacing three

Buttons exist three times with three different idioms, and the BACK buttons are
38x22 ([tab_party.c:106](ui/tab_party.c#L106)), 42x22
([tab_dex.c:96](ui/tab_dex.c#L96)) and 56x20
([tab_bag.c:104](ui/tab_bag.c#L104)). Add one widget to `ui_gfx`:

```c
void UiButton(int x, int y, int w, int h, const u8 *label, int state);
```

`DrawButtonH` ([tab_extra.c:182](ui/tab_extra.c#L182)) becomes a wrapper, and
the MAP / BAG / DEX / PARTY copies become calls. This is the reuse win that
keeps the art set small: one nine-slice, three bands, every button on the
screen.

**Press feedback** is what makes the third band mean anything. Today a tap has
no visual response at all until release. `ui_draw.c` gains
`UiSetPointer(const CtrTouchState *)`, called once per frame from
`CtrBottomUpdate`, and a no-argument `UiPressed(x, y, w, h)` any drawer can
consult without being handed the touch state. `CtrBottomUpdate` then marks dirty
on `justPressed || justReleased`. About 15 lines.

Mind the touch latch here. `sample_touch` holds the last contact point because
`hidTouchRead` returns `(0,0)` on the release frame
([host/main.c:100](host/main.c#L100)), which is why every handler opens with
`if (!t->justReleased) return;`. `UiPressed` must gate on `t->touching`, not on
coordinates alone, or every control under the last tap point draws as held
forever.

---

## Step 6: the shell, then the five tabs

**Shell.** `DrawTabBar` ([bottom_screen.c:379](ui/bottom_screen.c#L379))
currently draws flat rectangles. It becomes the bar's ground, a per-cell art
state, an icon from `icons.png` and the label beneath it, at whatever
`UI_TABBAR_H` `shell.png` established.

**Then one tab per pass**, in this order, each landing as its own coherent
screen: EXTRA first (pure chrome, no game art, so it validates the widget set),
then PARTY (the most game art and the only animation), then BAG, then DEX, then
MAP last (the region map decode and cache is the most delicate thing on the
screen).

For each: run `skin.py probe` on its wireframe, replace that tab's `#define`
block with the probed constants, and update the draw and touch code together.
Keep the existing convention of constants derived from each other
(`MOVE_ROW_Y(i)`, `SPD_X(i)`, `CellTop(i)`) rather than tabulated twice, and
check that the touch handler uses the same expression the draw code does.

**The trap specific to re-laying-out these tabs is that there is no clipping.**
Every panel width in the tree is hand-measured against the longest known game
string; BAG's list panel is 24 tiles because that leaves exactly 108px, the
width of the widest item description line in the game
([tab_bag.c:45](ui/tab_bag.c#L45)). Narrowing any panel that shows game text
means re-measuring that string, or implementing `UiClipPush/Pop` first (step 1
of `SECOND_SCREEN_PLAN.md`; the blitters already do per-pixel bounds tests, so
it is roughly four one-line edits and zero extra per-pixel cost).

---

## Optional, and cheap only because this touches all five tabs anyway

`sDetailOpen`, `sEntryOpen` and `sView` are file statics that survive a tab
switch, so leaving a detail view by tapping another tab and coming back
re-enters it. Known bug, step 0 of `SECOND_SCREEN_PLAN.md`. Folding the reset
into the tab switch is a few lines while the tab files are already open. Out of
scope unless explicitly taken up.

---

## Files

**New:** `3ds/graphics/skin/skin.py`, `refresh.sh`, the six sheet PNGs,
`wireframe/*.png`; `3ds/ui/ui_gfx.c`, `ui_gfx.h`, `ui_gfx_sheets.h`
(generated by `skin.py header`), `ui_skin.h`.

**Modified:** [ui_draw.c](ui/ui_draw.c) (two function bodies, `UiPanel`, pointer
state), [ui_draw.h](ui/ui_draw.h), [ui_shell.h](ui/ui_shell.h) (the colour block
and the layout constants move out), [bottom_screen.c](ui/bottom_screen.c)
(backdrop, tab bar, hash slot, `UiGfxInit`, dirty-on-press), and each of the
five `3ds/ui/tab_*.c`.

**Untouched:** the Makefiles, `build_objs.sh`, the `.rsf`, CI, and every `src/`
file. `3ds/ui/*.c` is already globbed by
[build_objs.sh:113](build_objs.sh#L113), and `INCGFX` lines there are already in
`generate_wasm_assets.py`'s scope.

**[SECOND_SCREEN_CHEATSHEET.md](SECOND_SCREEN_CHEATSHEET.md) needs four edits
when this lands**, since it describes the code as it actually is: its header
line calling this document "not implemented"; section 5's overlay note, which
cites this document for the layout constants being load bearing; section 8, the
drawing API, which gains `UiPanel`, `UiButton` and the `ui_gfx` calls and loses
the "text on a frame must use `UiThemeText()`" rule once frames no longer apply
here; and section 14, which cites this document as the authority for
`UI_TABBAR_H` and `UI_CONTENT_H` being load bearing.

---

## Verification

```sh
python3 3ds/graphics/skin/skin.py check     # the four format rules
python3 3ds/graphics/skin/skin.py header    # after any size change
bash 3ds/graphics/skin/refresh.sh           # rm + regenerate, see step 0
bash 3ds/build_objs.sh && make -C 3ds
```

- **Pipeline, before any drawing code.** Confirm
  `build/assets/3ds/graphics/skin/button.png_plain.8bpp` is exactly
  `width * height` bytes. This is the one failure mode that would otherwise
  surface as garbled art rather than as an error.
- **In Azahar, per step**, since each leaves the screen coherent: after step 4
  every tab is reskinned with no per-tab edit; after step 5 a held button
  visibly depresses; after each pass in step 6, that tab matches its wireframe.
- **Clipping**, which is where a new blitter fails silently rather than loudly:
  the BAG target picker ([tab_bag.c:440](ui/tab_bag.c#L440)) and the PARTY cells
  ([tab_party.c:268](ui/tab_party.c#L268)) both draw panels at computed offsets
  near the screen edge. Watch those two rather than the static layouts.
- **Repaint cost.** The screen is hash-gated, so a repaint is rare but full, and
  step 4 replaces a flat `UiClear` with a tiled fill plus a palette lookup over
  the whole content area. Confirm the frame rate is unchanged while walking with
  the PARTY tab up, which is the case that repaints on every HP change.
- **Touch parity per tab.** Every control still reachable, and nothing tappable
  that is not drawn: the IV/EV button carries a species check for exactly that
  reason ([tab_party.c:816](ui/tab_party.c#L816)).
- **On hardware, not only in an emulator** (`AGENTS.md`). The two bugs this
  codebase has hit hardest, the null save-block read and the decompress overrun,
  were both invisible in Azahar.
- **Do not regress the GBA path.** Everything here is new files under `3ds/`
  plus edits confined to `3ds/ui/`, none of which the matching build compiles,
  so `make compare` stays byte-identical. Worth running once at the end anyway.

---

## Risks and non-goals

- **1-bit alpha.** Index 0 is transparent; there is no blending. Anti-aliased
  edges therefore have to be baked against a known background, which the skin
  can do precisely because it now owns that background.
- **Compile time.** Each `INCGFX` array is expanded to C source text by
  `preproc`. At the sizes above this is a few hundred KB total and unnoticeable;
  it would stop being unnoticeable at full-screen images, which is the second
  reason the backdrop is a tile.
- **`UI_TABBAR_H` and `UI_CONTENT_H` moving is the expensive part.** Five tab
  files derive their layout from `UI_CONTENT_H`. Changing it is affordable here
  only because step 6 re-fits all five against wireframes anyway. Change the
  constant with the tab it re-fits, never ahead of it.
- **The 20 frames do not disappear**, they stop applying here. If the bottom
  screen ends up looking disconnected from a player's chosen frame on the top
  screen, the fallback is additive: the skin keeps its shapes and takes its fill
  from the frame palette.
