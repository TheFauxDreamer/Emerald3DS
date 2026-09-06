# Second screen cheatsheet

Working reference for changing the 3DS bottom screen. Read this before touching
anything under `3ds/ui/`. Companion documents: `3ds/README.md` (why the port is
built this way), `3ds/SECOND_SCREEN_PLAN.md` (a proposed refactor and feature
catalogue, **not implemented**), `3ds/UI_SKIN_PLAN.md` (a proposed visual
reskin, **not implemented**). This file describes the code as it actually is.

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
Rp2350PresentFrame()                 3ds/host/main.c:522   (end of every game frame)
  if (sSubFrame == 0)                                      once per DISPLAYED frame
     hidScanInput()
     sample_touch(&touch)            3ds/host/main.c:84
     CtrBottomUpdate(&touch)  -----> 3ds/ui/bottom_screen.c:293
                                       UpdateInGameLatch()
                                       tab-bar tap  OR  UiXTouch(touch)
                                       UiPartyTick()      HP bar animation
                                       UiStateHash()      poll for change
                                       Redraw() if needed -> paints sFb
  if (++sSubFrame >= sSpeed)
     CtrVideoPresent()               3ds/host/video.c:269
        if (CtrBottomIsDirty())      video.c:301
           upload(CtrBottomFramebuffer()); CtrBottomClearDirty()
```

Key consequences:

- `CtrBottomUpdate` runs **once per displayed frame**, not per game frame. Under
  fast-forward the UI still updates 60 times a second while the game runs faster.
- It runs at the **end** of a game frame, after `CallCallbacks` and after
  `VBlankIntr` (`src/main.c`). The frame's own callback has already finished,
  which is why replacing `gMain.callback2` from here is safe (see the fly path).
- `CtrBottomInit()` is called from `main()` at [host/main.c:636](host/main.c#L636),
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
| [ui/bottom_screen.c](ui/bottom_screen.c) | 351 | Tab list, tab bar, dispatch, repaint policy, `CtrBottom*` entry points |
| [ui/ui_shell.h](ui/ui_shell.h) | 106 | Layout constants, `UI_COL_*` palette, every per-tab entry point declaration |
| [ui/ui_draw.c](ui/ui_draw.c) / [.h](ui/ui_draw.h) | 609 / 114 | Framebuffer, blitters, window frames, icons, HP bar, `UiHit` |
| [ui/ui_text.c](ui/ui_text.c) / [.h](ui/ui_text.h) | 318 / 42 | Emerald font rendering, numbers, ASCII to game encoding |
| [ui/tab_party.c](ui/tab_party.c) | 673 | 2x3 party grid, cheat tag strip, per-mon detail view, HP animation |
| [ui/tab_bag.c](ui/tab_bag.c) | 654 | Pockets, item list, details, USE button, party target picker. **The only tab that writes game state** |
| [ui/tab_map.c](ui/tab_map.c) | 699 | Region map decode and cache, player tracking, fly-from-map |
| [ui/tab_dex.c](ui/tab_dex.c) | 520 | Dex list with cursor and scroll, entry screen |
| [ui/tab_extra.c](ui/tab_extra.c) | 624 | Three pages of port settings, gameplay tweaks, audio A/B |
| [ui/matchup.c](ui/matchup.c) / [.h](ui/matchup.h) | 143 / 31 | Type effectiveness for the party grid's badges |

Host side that matters to the UI: [host/main.c](host/main.c) (touch sampling,
every `Ctr3dsGet*`/`Ctr3dsSet*` toggle), [host/video.c](host/video.c) (upload),
[host/settings.c](host/settings.c) (persistence).

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
`UI_CONTENT_H`.

### Adding a tab

The tab bar is `tabW = 320 / visibleCount`. Five tabs is 64px wide each; six is
53px, which is about the practical floor for a fingertip. Do not add a seventh.
`SECOND_SCREEN_PLAN.md` proposes converting EXTRA into a launcher instead.

1. Add to `enum UiTab` in [ui_shell.h:19](ui/ui_shell.h#L19), before `UI_TAB_COUNT`.
2. Declare `UiXxxDraw` / `UiXxxTouch` in the same header.
3. Add a row to `sTabs[]` at [bottom_screen.c:56](ui/bottom_screen.c#L56):
   `{ "NAME", FLAG_... }`, or flag `0` for always available.
4. Add a `case` to the `switch` in `Redraw()` ([:249](ui/bottom_screen.c#L249))
   and to the one in `CtrBottomUpdate()` ([:293](ui/bottom_screen.c#L293)).
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

Dispatch in `CtrBottomUpdate` ([bottom_screen.c:293](ui/bottom_screen.c#L293)):

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
   you are deliberately implementing a drag, in which case gate on
   `t->touching` yourself.

Acting on release rather than press means a touch that slides off a control does
not fire it. Keep that convention.

Hit testing is one helper, [ui_draw.c:606](ui/ui_draw.c#L606):

```c
int UiHit(const CtrTouchState *t, int x, int y, int w, int h);
```

Order matters: test overlays and pagers **before** the controls underneath them
(see `UiExtraTouch` at [tab_extra.c:601](ui/tab_extra.c#L601), which tests the
pager first so nothing can sit under it).

`Ctr3dsUiModifierHeld()` is a held 3DS button (X/Y/ZL/ZR, bound in EXTRA) used
as a "jump by 5" modifier. See `CursorStep()` at [tab_dex.c:243](ui/tab_dex.c#L243).

### Established interaction idioms

- **Tap to select, tap again to open.** Party grid and dex list both do this.
  Cheap on a resistive panel and keeps a one-tap mis-touch harmless.
- **A destructive or stateful action takes a second deliberate tap** on a
  dedicated button (BAG's USE, MAP's YES/NO confirm).
- **BACK buttons** are per-view rects, currently in three different places:
  [tab_party.c:101](ui/tab_party.c#L101) (38x22),
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

Do not conflate them. Two ways to get a repaint:

**1. Push.** Call `UiMarkDirty()` after changing anything the screen depends on.
Every touch handler that changes state does this. This is the normal route.

**2. Poll.** `UiStateHash()` ([bottom_screen.c:153](ui/bottom_screen.c#L153)) is
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
| then | 6 party mons x 5 fields (species, HP, max HP, level, status) |

### Rules for writing a state key

- **A key must be O(what is on screen), not O(what is in the data.)** The dex
  key walks the whole dex, so it is only asked for while the DEX tab is up.
  That is why `top[4]` is a dispatch, not an XOR of everything.
- **Never share a slot between two keys.** Two contributions that happen to
  cancel show up as a panel that stops updating, which is the exact failure the
  hash exists to prevent. See the comment at [tab_map.c:583](ui/tab_map.c#L583).
- **Most new views need no key at all.** Static data (a learnset, a type chart,
  base stats) changes only under the view's own touch handler, which already
  calls `UiMarkDirty()`.
- **`GetMonData` decrypts in place** (`src/pokemon.c:3745`). Hashing many mons
  through it costs a decrypt round trip each. `MON_DATA_PERSONALITY`,
  `MON_DATA_OT_ID` and `MON_DATA_SANITY_HAS_SPECIES` sit before
  `MON_DATA_ENCRYPT_SEPARATOR` (`include/pokemon.h:8-19`) and answer from the
  plaintext header, so a `personality ^ otId` fold is a plain load.

`UiPartyTick()` ([tab_party.c:127](ui/tab_party.c#L127)) is the one animation.
It must be called once per frame, not once per redraw, or it stalls whenever the
screen happens not to be repainting. It returns TRUE while a bar is still
moving, which the shell turns into a repaint request. It currently runs on every
tab, so a moving HP bar forces a full repaint even with the MAP tab up.

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

Coordinates are in whole 8px tiles, so panel layouts have to divide cleanly:
`320 = 40 tiles`, `UI_CONTENT_H = 192 = 24 tiles`.

`UiFrameId()` prefers `Ctr3dsLiveWindowFrameType()` (`src/option_menu.c:33`)
over the save block, so the border previews live while the options menu is open.
It also returns 0 when `gSaveBlock2Ptr` is NULL. Fold it into any redraw trigger.

### Game art

```c
void UiMonIcon(int x, int y, u16 species, u32 personality);   // 32x32
void UiItemIcon(int x, int y, u16 itemId);                    // 32x32
void UiMonPic(int x, int y, u16 species);                     // 64x64, cached
void UiPokeball(int x, int y);                                // 7x7
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
static bool8 SaveDataLive(void);   // bottom_screen.c:83
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

Only BAG writes today, and its gates are the design, not a detail.

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
needs both.

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
                                if (sFoo != b) CtrSettingsSave(); }
   int  Ctr3dsGetFoo(void)    { return sFoo; }
   ```

   Range-check inside `Apply`, never trust the caller: a corrupt settings byte
   must leave the default standing.
3. **`3ds/host/settings.c`**: append a `uint8_t` to `struct CtrSettings`, bump
   `SETTINGS_VERSION`, add a `SETTINGS_Vn_SIZE` short-read migration, add the
   `extern` and the load/save lines. Keep the struct's every byte spoken for
   with explicit `pad`, or `CtrSettingsSave` writes uninitialised stack to the
   card. Choose the sense so that a zero byte means the old default.
4. **`3ds/ui/tab_extra.c`**: add the control, and fold the value into
   `UiExtraStateKey()` ([:440](ui/tab_extra.c#L440)) in a bit range nothing else
   claims.

The file is `sdmc:/3ds/emerald3ds/settings.bin`, written atomically through a
`.tmp` and a rename.

---

## 14. Layout constants

```c
CTR_BOTTOM_WIDTH   320        // bridge.h
CTR_BOTTOM_HEIGHT  240
UI_TABBAR_H        48         // ui_shell.h, the bar along the bottom
UI_CONTENT_H       192        // 24 tiles, everything above the bar
UI_W / UI_H        320 / 240  // ui_draw.h
```

`UI_TABBAR_H` and `UI_CONTENT_H` are declared load bearing by
`UI_SKIN_PLAN.md`. Do not change them casually.

Per-view constants are `#define`d at the top of each tab file, derived from each
other rather than tabulated twice (`MOVE_ROW_Y(i)`, `SPD_X(i)`, `CellTop(i)`).
When you move a control, move the constant, and check that the touch handler
uses the same expression the draw code does. `tab_party.c` gets this right by
computing both from `CellH()`/`CellTop()`, which change when the cheat tag strip
appears.

---

## 15. Pitfalls, collected

| Symptom | Cause |
|---|---|
| Data abort at boot, address near 0x1300 | Reading `gSaveBlock1Ptr` before a file is loaded. Gate on `SaveDataLive()`. |
| Every tab's border changes colour after viewing a dex entry | Decompress overrun into neighbouring statics. Size-check first. |
| Invisible text | Using the game's `DecompressGlyphTile()` instead of `ui_text.c`'s own decoder. |
| Text overruns into the next panel | No clipping exists. Measure, or add `UiClipPush`. |
| A panel stops updating | Two state-key contributions cancelling in one hash slot. |
| A readout goes stale until you switch tabs | State changes without a touch and has no state key. |
| Every tap lands at (0,0) | Reading touch coordinates without the latch, or dropping the `justReleased` guard. |
| Detail view reopens after a tab switch | Modal file statics survive the switch. Known bug. |
| Heap exhaustion after a few flies | Left the overworld without `CleanupOverworldWindowsAndTilemaps()`. |
| A `src/` feature silently disappears | `3ds/ui/*.c` basename collided with a `src/*.c` object. |
| Host-side change did nothing | Forgot `3ds/build_objs.sh`, or passed `CTR_BOOT_DIAG` to only one of the two builds. |

---

## 16. Verification checklist

- Both mixer configurations still link: default, and `CTR_M4A_ASM=1 3ds/build_objs.sh`.
- Existing tabs unchanged, all controls still reachable, settings persist across
  a relaunch.
- New views checked against the game's **own** screens, not against your reading
  of the data: trainer card against the in-game trainer card, berry stages
  against the trees, encounter lists against the real tables with the randomiser
  both off and on.
- Write paths verified against their gates, not only their happy paths: refused
  during a battle, during a script, with field controls locked, and outside
  `CB2_Overworld`.
- On hardware, not only in an emulator (`AGENTS.md`). The boot log and the frame
  600 audio health report stay clean, and repaint frequency has not visibly
  risen.
