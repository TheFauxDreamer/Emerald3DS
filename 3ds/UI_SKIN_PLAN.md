# A designed bottom screen, built from images

**Status: proposed, not implemented.** Re-verified against the tree at `7db93d4`
on 2026-09-12: every line reference below was re-read, and the parts about the
two overlays, the encounters view, the repaint path, repaint cost and press
feedback were rewritten for what landed after the first draft. Companion
documents: [SECOND_SCREEN_CHEATSHEET.md](SECOND_SCREEN_CHEATSHEET.md) (the code
as it actually is, read it first), [SECOND_SCREEN_PLAN.md](SECOND_SCREEN_PLAN.md)
(feature and refactor catalogue, also not implemented).

## Context

The bottom screen works but was never designed. It is drawn entirely from
`UiFillRect` / `UiRect` primitives, and its whole visual identity is borrowed:
every panel is one of Emerald's 20 option-menu window frames, and every ink
colour is read back out of `gStandardMenuPalette` at runtime
([ui_draw.c:289](ui/ui_draw.c#L289)). That was the right call while the port was
proving it could read game state at all, and it is why the code is littered with
defensive decisions (outlines on every arrow, a chevron in theme colours, a Poke
Ball that carries its own dark edge, a shiny notice that paints its own dark
ground so its gold can be read) whose only purpose is to survive being drawn on
20 backgrounds that run from near-white to near-black.

The cost is that nothing can be composed. There is no ownership of the
background, so no gradient, no shadow, no anti-aliased edge, no shape that is
not an axis-aligned rectangle. Buttons are 1px outlines, drawn by two local
helpers and about a dozen inline `UiRect` calls across seven files (step 5), and
the BACK buttons come in three sizes.

The decision taken here is to **stop borrowing and own the look**: one custom
skin, built from real image assets, across all five tabs, the encounters view
and both overlays, with the layout itself driven by wireframes drawn at 320x240
rather than by the accumulated hand-measured constants. The player's Frame
option keeps governing the top screen exactly as it always did; it stops
governing the bottom one.

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
| `gbagfx png -> gbapal` | Writes the PNG's PLTE as BGR555, **as many entries as the PLTE has**, not always 256. | `HandlePngToGbaPaletteCommand` |
| `preproc -g build/assets` | Resolves `root + source + args_as_path + extension` and emits the bytes inline. `build_objs.sh` passes `-g` for every game-side source, `3ds/ui/` included. | `CFile::TryConvertIncgfx`, `c_file.cpp:538`; [build_objs.sh:80](build_objs.sh#L80) |
| CI | Already runs `generate_wasm_assets.py` before `build_objs.sh`. | `.github/workflows/build-3ds.yml:74`, `:80` |

`-plain` is an established in-tree pattern, not a novelty: `src/pokemon.c:1362`
uses it for Spinda's spots.

### Four hard rules for every asset

| Rule | Why | Where confirmed |
|---|---|---|
| **Indexed PNG, colour type 3** | `ReadPng` rejects RGB and RGBA outright | [convert_png.c:90](../tools/gbagfx/convert_png.c#L90) |
| **Bit depth 8** | a depth-4 file is re-packed as one flat bitstream that ignores per-row padding, so any odd width shears silently | `ConvertBitDepth`, [convert_png.c:48](../tools/gbagfx/convert_png.c#L48) |
| **Palette index 0 is transparent, and nothing else may use it** | there is no alpha channel and no blending anywhere in the blitters | `UiBlit4bppTile`, [ui_draw.c:123](ui/ui_draw.c#L123) |
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
design record. Each is exactly 320x240, or an integer 2x / 4x multiple.

| File | Surface | What it must show |
|---|---|---|
| `shell.png` | the bar and the content split | settled first, see below |
| `overlays.png` | shiny notice and quick-throw strip, together | their geometry is coupled (see "The overlays" below) |
| `party.png` | the 2x3 grid | **both** cell heights: 64px with no cheat tags, 56px under the 24px tag strip ([tab_party.c:46](ui/tab_party.c#L46)) |
| `party_detail.png` | the per-mon detail view | one left-column rect that all three tenants must fit: stats, a tapped move's info, the IV/EV spread |
| `bag.png` | pockets, list, details, USE | |
| `bag_picker.png` | the target picker | a 24px header over a 2x3 grid of 160x56 |
| `map.png` | the region map and caption band | the caption's FLY and WILD PKMN buttons and the YES/NO confirm |
| `encounters.png` | MAP's pushed view ([view_encounters.c](ui/view_encounters.c)) | header, 4x2 grid, pagers, BACK |
| `dex.png` | list and entry screen | |
| `extra_p1.png`, `extra_p2.png`, `extra_p3.png` | EXTRA's three shipping pages | page 4, the debug menu, exists only under `CTR_DEBUG_MENU` and reuses page 3's grid |

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
| art | a game-art blit: mon icon, item icon, dex pic, type icon, ball icon, HP bar |
| chrome | arrow, pip, divider, scrollbar |

`skin.py probe <file>` decodes the PNG with stdlib `zlib`, finds the connected
regions of each legend colour, and prints a table of `x, y, w, h` plus a
suggested `#define` name. That table is what becomes the tab's constants, so
what was drawn and what the code does cannot drift.

`shell.png` is drawn and settled first: `UI_TABBAR_H` and `UI_CONTENT_H` are
consumed by all five tab files, the encounters view and both overlays, so every
other wireframe is drawn against whatever split it establishes.

Text slots are subject to the font, not the other way round. There is one font
and two sizes: `UI_GLYPH_H` 15 with `UI_LINE_H` 16, and `UiTextBig` at 2x
nearest-neighbour for `UI_GLYPH_BIG_H` 30 ([ui_text.h:18](ui/ui_text.h#L18)).
Wireframe text blocks have to be drawn to those heights.

### The overlays

Two panels are drawn over whichever tab is up, and both are hand-fitted to the
current 192px content area:

| Overlay | Rect | Defined at |
|---|---|---|
| shiny notice | 240x112 at (40, 40), so y 40..152 | [bottom_screen.c:159](ui/bottom_screen.c#L159) (`NOTICE_*`) |
| quick-throw strip | 320x40 at (0, 152), so y 152..192 | [ui_quickball.h:29](ui/ui_quickball.h#L29) (`UI_QB_*`) |

They **abut exactly** at y 152 and the strip ends on `UI_CONTENT_H`, because the
one case where both are up (a catchable shiny) must not tear either border. Both
are expressed in tiles today because `UiWindowFrame` takes tiles.

- If `shell.png` moves `UI_CONTENT_H`, both overlays move **in the same
  commit** and must still abut. Once they draw through `UiPanel` in pixels the
  8px constraint goes away; derive both from `UI_CONTENT_H` rather than from
  literals, so the next move cannot separate them.
- The strip covers PARTY's bottom row of cells, which is why `UiOverlayActive()`
  makes that tab paint still icons while it is up. Draw `party.png` knowing that
  row is sometimes covered.

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

Roughly 26KB of const data, plus six palettes of at most 512 bytes each.
Negligible against a 64MB system mode.

`tabbar.png`'s cell width follows `320 / visibleCount`: 64px at five tabs, 53px
at six, which is about the practical floor for a fingertip. The sizes above are
the starting set; the wireframes may move them, which is exactly why nothing on
the C side hardcodes a dimension.

**Decide about the title bar before drawing `tabbar.png`.**
`SECOND_SCREEN_PLAN.md` (its step 0) turns the bar into a title bar at depth
greater than zero, which needs a back chevron and a title ground as well. If
that is still wanted, author those states into the same sheet now (that plan
suggests 64x160), or the bar gets drawn twice.

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
[ui_draw.c:23](ui/ui_draw.c#L23) owns, so it is a peer of `UiBlit4bppTile`, not
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

- The sheet table is `const struct { const u8 *px; const u16 *pal; u16 w, h,
  palCount; } sSheets[UI_SHEET_COUNT]`, with a parallel
  `static u16 sPal565[COUNT][256]` in `.bss` that `UiGfxInit` fills once via
  the existing `UiLoadPal` ([ui_draw.c:37](ui/ui_draw.c#L37)). Convert
  `palCount` entries, taken from `ARRAY_COUNT` of the `INCGFX` array, **not
  256**: `.gbapal` is only as long as the PNG's PLTE, and reading 256 would run
  past a shorter array. No const-cast, no per-draw conversion, no
  cache-invalidation logic. 3KB of `.bss`.
- Every entry point clamps against `0..UI_W` / `0..UI_H` exactly the way
  `UiFillRect` ([ui_draw.c:65](ui/ui_draw.c#L65)) already does, so an off-screen
  or oversized request is a no-op rather than an overrun. If `UiClipPush/Pop`
  (`SECOND_SCREEN_PLAN.md` step 1) lands first, they clamp against **the clip**
  instead, or a panel inside a clipped row paints over its neighbours.
- `band` selects one state out of a stacked strip, so the three button states
  are one sheet and one palette rather than three of each.
- Nine-slice **tiles** its edges rather than stretching them. For a 1px edge the
  two are identical; for a thicker edge, tiling keeps a texture readable where
  stretching would smear it.
- **`UiTileFill` of the backdrop is a row copy, not a palette lookup.**
  `UiGfxInit` expands `bg.png` to RGB565 once (64x64, 8KB of `.bss`), and the
  fill copies rows out of that. It covers 61,440 pixels on every full repaint,
  and step 4 explains why that has to cost about what `UiClear` does.

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

Two function bodies and one colour block change, and about 300 call sites do
not.

**`UiWindowFrame(tx, ty, wTiles, hTiles)`** ([ui_draw.c:251](ui/ui_draw.c#L251))
has **14 call sites in seven files**, every one of them a panel: the tabs, the
BAG picker's cells, MAP's caption band and its "Map unavailable" panel, the
encounters view ([view_encounters.c:432](ui/view_encounters.c#L432)), and both
overlays ([bottom_screen.c:432](ui/bottom_screen.c#L432),
[ui_quickball.c:197](ui/ui_quickball.c#L197)). Reimplement its body as a
`UiNineSlice` of `UI_SHEET_PANEL` and every one of them is reskinned untouched.
Add `UiPanel(x, y, w, h)` in pixels for new code and make the tile-granular
function a one-line wrapper, so the 8px grid stops being a constraint on
anything written from here on.

**`UiThemeText()` / `UiThemeShadow()`** ([ui_draw.c:289](ui/ui_draw.c#L289))
are called **about 198 times**. Return the skin's ink colours instead of reading
`gStandardMenuPalette` and every label on the screen becomes consistent in one
edit.

**The `UI_COL_*` block** is the third lever, and the easy one to overlook.
`UI_COL_DIM` (75 uses) and `UI_COL_ACCENT` (28) are drawn directly on panels as
secondary labels, button outlines and arrow fills, so moving the block into
`ui_skin.h` with the skin's values retargets all of them in the same edit.

Then drop `top[0] = UiFrameId()` from `UiStateHash()`
([bottom_screen.c:524](ui/bottom_screen.c#L524)). Once the frame no longer
drives the bottom screen it is a stale input to the repaint hash. `UiFrameId()`
itself stays: it is still correct, and the top screen still uses the setting.

Replace the in-game `UiClear(UI_COL_BG)` in `Redraw()`
([bottom_screen.c:679](ui/bottom_screen.c#L679)) with a `UiTileFill` of the
backdrop. The pre-game `UiClear(0)` a few lines above it stays black: that is
the blank screen under the title, not a skin surface.

**At the end of this step every surface already looks new, with no per-tab
edits at all.** That is the checkpoint worth building to before anything else.

### The snapshot is on the skin's side

`Redraw()` ([bottom_screen.c:646](ui/bottom_screen.c#L646)) no longer ends with
the tab bar. It paints the still screen (ground, tab, strip, notice, bar), takes
`UiSnapshot()`, and only then runs `DrawAnimatedLayer()` for the pieces that
move. An animation step (`RedrawAnimated`, [:754](ui/bottom_screen.c#L754))
puts a few rects back from the snapshot and redraws only those.

That fits the skin with no extra work: the backdrop and the panels are ground,
so they belong in the snapshot, and `UiRestoreRect` puts a textured ground back
under a mon icon exactly. The one rule to keep is the cheatsheet's: **nothing
that moves is drawn before `UiSnapshot()`.** Step 5's press state is the only
new thing here that moves, and it is drawn after.

### What a repaint may cost now

The frame model changed under this plan. The rasteriser now runs on a second
core (core 2 on a New 3DS, core 1 otherwise) while `CtrBottomUpdate` paints, so
**a repaint is free as long as `paint` finishes before `ppu` does.** Measured at
`7db93d4` on a New 3DS XL (cheatsheet section 7): `paint` 2.4 to 3.9 ms mean,
`ppu` about 4.5 ms mean. At the top of that range there is well under a
millisecond left before repaints start costing frames again. The single-core
fallback, taken when no second core is available or built deliberately with
`CTR_PPU_THREAD=0`, still pays `fps = 3600 / (60 + repaints per second)`.

So this step's budget is "no slower than today", measured rather than assumed:

- The backdrop fill is a row copy of a pre-expanded tile (step 3), not a
  per-pixel palette lookup, so it lands near `UiClear`'s cost.
- `UiNineSlice` over a full-area panel does one byte read and one palette
  lookup per pixel, which is the same work `UiWindowFrame`'s 4bpp blits do
  today, minus the nibble unpack. Expect it to be a wash; confirm it.
- Read `paint` on the console before and after this step. A rise is a
  regression to fix here, before step 5 builds on it.

---

## Step 5: one shared button, replacing all of them

Every button on the screen is a 1px `UiRect` outline, in three shapes:

| Kind | Where |
|---|---|
| named helper | `DrawButtonH` ([tab_extra.c:188](ui/tab_extra.c#L188)); `DrawBtn` ([tab_map.c:476](ui/tab_map.c#L476)), whose comment calls sharing it "premature" because it then had one other user; `DrawSpreadButton` ([tab_party.c:845](ui/tab_party.c#L845)) |
| inline outline | BAG pagers, USE ([tab_bag.c:432](ui/tab_bag.c#L432)) and CANCEL; DEX pagers and BACK ([tab_dex.c:417](ui/tab_dex.c#L417)); PARTY BACK ([tab_party.c:875](ui/tab_party.c#L875)); encounters pagers and BACK ([view_encounters.c:401](ui/view_encounters.c#L401)); THROW ([ui_quickball.c:248](ui/ui_quickball.c#L248)); DISMISS ([bottom_screen.c:488](ui/bottom_screen.c#L488)) |
| pager | a `UiArrow` centred in an outline, on BAG, DEX and the encounters view |

The BACK buttons are 38x22 ([tab_party.c:107](ui/tab_party.c#L107)), 42x22
([tab_dex.c:96](ui/tab_dex.c#L96), and the encounters view, which matched DEX
on purpose) and 56x20 ([tab_bag.c:108](ui/tab_bag.c#L108), BAG's CANCEL). Add
one widget to `ui_gfx`:

```c
void UiButton(int x, int y, int w, int h, const u8 *label, int state);
void UiButtonGlyph(int x, int y, int w, int h, int glyph, int state);   // pagers
```

`DrawButtonH` and `DrawBtn` become wrappers, and every inline outline above
becomes a call. The three bands already have meanings on screen: EXTRA, MAP and
the IV/EV toggle all mark the active choice with a doubled accent inset, which
is the `active` band. This is the reuse win that keeps the art set small: one
nine-slice, three bands, every button on the screen. If
`SECOND_SCREEN_PLAN.md`'s `ui_widgets.c` exists by the time this lands, the two
functions live there instead; the skin only needs them to exist once.

### Press feedback, on the animated layer

Today a tap has no visual response at all until release. The first draft of this
step drew the pressed band from inside `Redraw()` and repainted on press. That
is wrong on two counts now:

- It bakes the pressed state into the snapshot, and it doubles the full
  repaints every tap costs.
- The bottom screen reaches the panel in 48-row bands, five frames per picture,
  top to bottom ([video.c:579](host/video.c#L579)), and a new picture waits for
  a run already in flight. A full repaint on press shows 1 to 5 frames after the
  touch depending on its row, the tab bar last, and on a tap shorter than five
  frames it holds the release's result back by the remainder.

Instead, the pressed control is drawn **over** the snapshot, the way the party
icons are:

1. `UiButton` records each button it draws (rect, label or glyph, state) in a
   small static table, cleared at the top of `Redraw()`.
2. `ui_draw.c` gains `UiSetPointer(const CtrTouchState *)`, called once per
   frame from `CtrBottomUpdate` ([bottom_screen.c:775](ui/bottom_screen.c#L775)).
3. `DrawAnimatedLayer` gains a last step, after whichever tenant it ran: while
   the pointer is `touching`, find the recorded button under it, restore its
   rect from the snapshot and draw it in the pressed band.
4. `CtrBottomUpdate` asks for the cheap `RedrawAnimated` path on
   `justPressed`, on sliding off a button, and on `justReleased`, never for a
   full `Redraw`. The release still acts through the tab's handler exactly as
   today, and that handler's `UiMarkDirty()` produces the full repaint it
   always did.

Mind the touch latch. `sample_touch` holds the last contact point because
`hidTouchRead` returns `(0,0)` on the release frame
([host/main.c:96](host/main.c#L96)), which is why every handler opens with
`if (!t->justReleased) return;`. The press step must gate on `t->touching`, not
on coordinates alone, or the control under the last tap draws as held forever.

About 50 lines. Judge the latency on the console rather than in Azahar. If the
tab bar's five-frame delay reads as sluggish, the follow-up is host-side
(upload only the bands a press touched) and is not part of this plan.

---

## Step 6: the shell, then the tabs

**Shell, with both overlays.** `DrawTabBar`
([bottom_screen.c:620](ui/bottom_screen.c#L620)) currently draws flat
rectangles. It becomes the bar's ground, a per-cell art state, an icon from
`icons.png` and the label beneath it, at whatever `UI_TABBAR_H` `shell.png`
established. The notice and the strip are re-fitted in the same pass, because
both are positioned against `UI_CONTENT_H` (step 1, "The overlays").

**Then one surface per pass**, in this order, each landing as its own coherent
screen: EXTRA first (pure chrome, no game art, so it validates the widget set),
then PARTY (the most game art, and the animated layer's main tenant), then BAG
with its picker, then DEX, then MAP with the encounters view last (the region
map decode and cache is the most delicate thing on the screen).

For each: run `skin.py probe` on its wireframe, replace that file's `#define`
block with the probed constants, and update the draw and touch code together.
Keep the existing convention of constants derived from each other
(`MOVE_ROW_Y(i)`, `SPD_X(i)`, `CellTop(i)`) rather than tabulated twice, and
check that the touch handler uses the same expression the draw code does.
PARTY's animated layer restores rects computed from those same constants
(`UiPartyRedrawAnimated`, [tab_party.c:387](ui/tab_party.c#L387)), so it moves
with the cell or icons get drawn where the cell no longer is.

**The trap specific to re-laying-out these surfaces is that there is no
clipping.** Every panel width in the tree is hand-measured against the longest
known game string; BAG's list panel is 24 tiles because that leaves exactly
108px, the width of the widest item description line in the game
([tab_bag.c:45](ui/tab_bag.c#L45)). Narrowing any panel that shows game text
means re-measuring that string, or implementing `UiClipPush/Pop` first (step 1
of `SECOND_SCREEN_PLAN.md`; the blitters already do per-pixel bounds tests, so
it is roughly four one-line edits and zero extra per-pixel cost).

---

## Optional, and cheap only because this touches every surface anyway

Modal state lives in file statics that survive a tab switch, so leaving a view
by tapping another tab and coming back re-enters it: `sDetailOpen` (PARTY),
`sEntryOpen` (DEX), `sView` (BAG's picker), `sOpen`
([view_encounters.c:118](ui/view_encounters.c#L118), MAP's encounters view) and
MAP's fly confirm, `sConfirm` ([tab_map.c:148](ui/tab_map.c#L148)). Known bug,
step 0 of `SECOND_SCREEN_PLAN.md`. Folding the reset into the tab switch is a
few lines while those files are already open. Out of scope unless explicitly
taken up.

---

## Files

**New:** `3ds/graphics/skin/skin.py`, `refresh.sh`, the six sheet PNGs,
`wireframe/*.png`; `3ds/ui/ui_gfx.c`, `ui_gfx.h`, `ui_gfx_sheets.h`
(generated by `skin.py header`), `ui_skin.h`.

**Modified:** [ui_draw.c](ui/ui_draw.c) (two function bodies, `UiPanel`, pointer
state), [ui_draw.h](ui/ui_draw.h), [ui_shell.h](ui/ui_shell.h) (the colour block
and the layout constants move out), [bottom_screen.c](ui/bottom_screen.c)
(backdrop, tab bar, hash slot, `UiGfxInit`, the notice, the press step in the
animated layer), [ui_quickball.c](ui/ui_quickball.c) /
[.h](ui/ui_quickball.h) (the strip's geometry and THROW),
[view_encounters.c](ui/view_encounters.c), and each of the five
`3ds/ui/tab_*.c`.

**Untouched:** the Makefiles, `build_objs.sh`, the `.rsf`, CI, the host side,
and every `src/` file. `3ds/ui/*.c` is already globbed by
[build_objs.sh:106](build_objs.sh#L106), and `INCGFX` lines there are already in
`generate_wasm_assets.py`'s scope.

**When this lands, update the documents that describe the code as it is.**
[SECOND_SCREEN_CHEATSHEET.md](SECOND_SCREEN_CHEATSHEET.md): its header line
calling this document "not implemented"; section 5, the overlay notes and the
"do not size a panel in pixels" rule, which `UiPanel` retires; section 8, the
drawing API, which gains `UiPanel`, `UiButton` and the `ui_gfx` calls and loses
the "text on a frame must use `UiThemeText()`" rule once frames no longer apply
here; and section 14, the layout and overlay constants at whatever values
`shell.png` settled. In code, the comment above the notice's geometry
([bottom_screen.c:151](ui/bottom_screen.c#L151)) that still calls the content
height load bearing on this document's say-so.

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
  every surface is reskinned with no per-tab edit; after step 5 a held button
  visibly depresses and springs back on slide-off; after each pass in step 6,
  that surface matches its wireframe.
- **Clipping**, which is where a new blitter fails silently rather than loudly:
  the BAG target picker's cells ([tab_bag.c:441](ui/tab_bag.c#L441)) and the
  PARTY cells ([tab_party.c:543](ui/tab_party.c#L543)) both draw panels at
  computed offsets near the screen edge. Watch those two rather than the static
  layouts.
- **Repaint cost, on the console.** Build with `CTR_DEBUG_MENU` and read
  `log.txt`'s `prof` lines before and after step 4, and again after step 5:
  `paint` must not rise, `ppu.wait` must stay above zero on frames that repaint,
  `frame` worst must stay near 16.7 ms, and no "missed VBlank" line may appear.
  Three scenarios: the PARTY tab in a battle while taking damage (a sliding HP
  bar forces full repaints), the quick-throw strip up (it repaints at least
  twice a turn), and the shiny notice (EXTRA's shiny test). Azahar at its
  default clock doubles the rasteriser's cost and at 300% shows about two thirds
  of it, so its numbers are only a smoke test.
- **Overlays.** With a catchable shiny up, the notice and the strip still abut
  with neither border torn, on every tab, before and after `UI_CONTENT_H`
  moves.
- **Touch parity per surface.** Every control still reachable, and nothing
  tappable that is not drawn: the IV/EV button carries a species check for
  exactly that reason ([tab_party.c:1053](ui/tab_party.c#L1053)).
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
- **Repaint headroom is thin.** With the paint overlapping the rasteriser, the
  console had well under a millisecond to spare at the top of the measured
  range. Anything this plan adds to a full repaint is measured, not reasoned
  about (step 4).
- **`UI_TABBAR_H` and `UI_CONTENT_H` moving is the expensive part.** Five tab
  files, the encounters view and both overlays derive their layout from
  `UI_CONTENT_H`. Changing it is affordable here only because step 6 re-fits
  all of them against wireframes anyway. Change the constant with the surfaces
  it re-fits, never ahead of them.
- **The 20 frames do not disappear**, they stop applying here. If the bottom
  screen ends up looking disconnected from a player's chosen frame on the top
  screen, the fallback is additive: the skin keeps its shapes and takes its fill
  from the frame palette.
