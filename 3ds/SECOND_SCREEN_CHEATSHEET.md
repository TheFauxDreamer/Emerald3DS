# Second screen cheatsheet

Working reference for changing the 3DS bottom screen. Read this before touching
anything under `3ds/ui/`. Companion documents: the root `README.md` (why the
port is built this way), `3ds/SECOND_SCREEN_PLAN.md` (a proposed refactor and feature
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
Rp2350PresentFrame()                 3ds/host/main.c:551   (end of every game frame)
  if (sSubFrame == 0)                                      once per DISPLAYED frame
     hidScanInput()
     sample_touch(&touch)            3ds/host/main.c:85
     CtrBottomUpdate(&touch)  -----> 3ds/ui/bottom_screen.c:436
                                       UpdateInGameLatch()
                                       tab-bar tap  OR  UiXTouch(touch)
                                       UiPartyTick()      HP bar animation
                                       UiStateHash()      poll for change
                                       Redraw() if needed -> paints sFb
  if (++sSubFrame >= sSpeed)
     CtrVideoPresent()               3ds/host/video.c:269
        if (CtrBottomIsDirty())      video.c:313
           upload(CtrBottomFramebuffer()); CtrBottomClearDirty()
     CtrSaveFlush(0)                 3ds/host/save.c      the save image
     CtrSettingsFlush(0)             3ds/host/settings.c  settings.bin
```

The two flushes are at the bottom for a reason: this is the only point in the
frame where blocking on the SD card is affordable, because `CtrVideoPresent()`
has already waited for VBlank. Nothing on the touch path may write to the card
itself -- see section 13.

Key consequences:

- `CtrBottomUpdate` runs **once per displayed frame**, not per game frame. Under
  fast-forward the UI still updates 60 times a second while the game runs faster.
- It runs at the **end** of a game frame, after `CallCallbacks` and after
  `VBlankIntr` (`src/main.c`). The frame's own callback has already finished,
  which is why replacing `gMain.callback2` from here is safe (see the fly path).
- `CtrBottomInit()` is called from `main()` at [host/main.c:675](host/main.c#L675),
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
| [ui/bottom_screen.c](ui/bottom_screen.c) | 721 | Tab list, tab bar, dispatch, overlays, the shiny notice and its animation, repaint policy, `CtrBottom*` entry points |
| [ui/ui_shell.h](ui/ui_shell.h) | 112 | Layout constants, `UI_COL_*` palette, every per-tab entry point declaration |
| [ui/ui_draw.c](ui/ui_draw.c) / [.h](ui/ui_draw.h) | 778 / 172 | Framebuffer, blitters, window frames, icons, HP bar, sparkle art, `UiHit`, `UiHoldRepeat` |
| [ui/ui_text.c](ui/ui_text.c) / [.h](ui/ui_text.h) | 360 / 54 | Emerald font rendering at 1x and 2x, numbers, ASCII to game encoding |
| [ui/tab_party.c](ui/tab_party.c) | 947 | 2x3 party grid, cheat tag strip, per-mon detail view with the move panel and the IV/EV spread, HP animation |
| [ui/tab_bag.c](ui/tab_bag.c) | 676 | Pockets, item list, details, USE button, party target picker. **The only tab that writes game state** |
| [ui/tab_map.c](ui/tab_map.c) | 699 | Region map decode and cache, player tracking, fly-from-map |
| [ui/tab_dex.c](ui/tab_dex.c) | 528 | Dex list with cursor and scroll, entry screen |
| [ui/tab_extra.c](ui/tab_extra.c) | 704 | Page 1 port settings, page 2 gameplay tweaks, page 3 the debug menu (compiled out by `CTR_DEBUG_MENU`) |
| [ui/matchup.c](ui/matchup.c) / [.h](ui/matchup.h) | 210 / 43 | Reads about the opposing mon: type effectiveness for the party badges, and `UiShinyOpponent` behind the notice |

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
`UI_CONTENT_H`. Overlays are painted after the tab and before the bar.

### Overlays

An overlay is drawn over whichever tab just painted, and claims its rect of
touches before any tab sees them. The shiny notice
([bottom_screen.c:135](ui/bottom_screen.c#L135)) is the pattern: a 240x112 modal
panel centred in the content area, with a DISMISS button, keyed on the encounter
rather than on a bare flag so the next shiny still gets its own notice.

It is an overlay rather than a band the tabs make room for because every tab's
layout is hand-fitted to a 192px content area, which `UI_SKIN_PLAN.md` declares
load bearing. Reserving space would mean re-fitting five tabs for a state that
occurs once in 8192 encounters. `UiWindowFrame`'s centre tiles are opaque, so a
panel genuinely covers what is behind it rather than floating over readable
content.

Four rules, all of them learned the hard way on this one:

1. **Paint last**, after the tab's `Draw` and before the tab bar.
2. **Take touches first**, on the whole rect, `justReleased` or not — otherwise
   a press that never becomes a release, or a drag begun on the panel, carries
   through into what it is covering. Absorb everything; act only on the control.
3. **Give it a real control.** "Tap anywhere to dismiss" throws the panel away
   under a stray touch while the player is still reading it.
4. **Let it expire on its own.** The shiny panel closes itself on
   `gBattleOutcome`, so a catch, a faint, a flee or a run clears it whether or
   not anyone pressed anything. An overlay that can only be dismissed by hand is
   an overlay that gets left up.

And fold an "is it up" bit into `UiStateHash`, or it will not appear until
something else happens to dirty the screen.

**Do not size a panel in pixels and hope.** `UiWindowFrame` takes 8px TILES, so
pick tile counts that centre exactly: 30x14 tiles is 240x112, and (320-240)/2
and (192-112)/2 are both 40.

### Adding a tab

The tab bar is `tabW = 320 / visibleCount`. Five tabs is 64px wide each; six is
53px, which is about the practical floor for a fingertip. Do not add a seventh.
`SECOND_SCREEN_PLAN.md` proposes converting EXTRA into a launcher instead.

1. Add to `enum UiTab` in [ui_shell.h:19](ui/ui_shell.h#L19), before `UI_TAB_COUNT`.
2. Declare `UiXxxDraw` / `UiXxxTouch` in the same header.
3. Add a row to `sTabs[]` at [bottom_screen.c:57](ui/bottom_screen.c#L57):
   `{ "NAME", FLAG_... }`, or flag `0` for always available.
4. Add a `case` to the `switch` in `Redraw()` ([:396](ui/bottom_screen.c#L396))
   and to the one in `CtrBottomUpdate()` ([:476](ui/bottom_screen.c#L476)).
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

Dispatch in `CtrBottomUpdate` ([bottom_screen.c:427](ui/bottom_screen.c#L427)),
in order:

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

**2. Poll.** `UiStateHash()` ([bottom_screen.c:271](ui/bottom_screen.c#L271)) is
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
  calls `UiMarkDirty()`. IVs are in this class too: they are fixed when the mon
  is created and can never go stale.
- **Key only what is actually on screen, and only while it is.**
  `UiPartyStateKey()` ([tab_party.c:774](ui/tab_party.c#L774)) folds in the
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

Three rules, and the third is the one that keeps this affordable:

1. **Once per frame, not once per redraw.** A tick called from `Redraw` stalls
   exactly when it is the thing that ought to be causing the redraw.
2. **Count calls, not milliseconds.** `CtrBottomUpdate` runs once per DISPLAYED
   frame, so a call is a 60th of a second even under fast-forward, and an
   animation does not speed up with the game. `UiHold`
   ([ui_draw.h](ui/ui_draw.h)) and both ticks all do this.
3. **Return FALSE the moment the thing being animated is off screen.** A
   repaint is 76,800 pixels of software fill plus a blocking texture upload on
   the host ([video.c](../host/video.c)), so a tick that returns TRUE
   unconditionally is a decision to pay that on every frame of the game.
   `NoticeTick` returns FALSE whenever the panel is down, which is why a rare,
   brief, 60fps animation costs nothing on the frames it is not running.

`UiPartyTick` takes its tab's visibility as an argument rather than reading
`sTab` itself, which is rule 3 made hard to skip: the shell has to say whether
anyone is looking. Off screen it returns immediately, and adopts the party's
real HP on the frame the tab comes back rather than sliding on arrival for
damage taken while it was hidden. It resyncs on the ARRIVAL, not on every
hidden frame -- `GetMonData` decrypts in place, so a per-frame resync across
the other four tabs would cost more than the repaints the gate saves.

**An animation's rate is its repaint rate.** The party icons change every sixth
frame because that is the pace of the game's own `sAnim_0`
(`src/pokemon_icon.c`), so an idle party grid asks for ten repaints a second,
not sixty. Tie an animation to the rate its source art actually moves at and
the cost follows; pick a rate freely and you have picked a repaint budget
without noticing.

A `u16` frame counter wraps after eighteen minutes. If a cycle length divides
65536 the wrap lands on a boundary and nothing is visible; `NOTICE_CYCLE` is 64
for that reason. Pick a power of two.

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

**There is one font and one size.** `gFontNormalLatinGlyphs` at `UI_GLYPH_H` 15
is all the ROM has — the game never needed another on a 240px screen. For a
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
static bool8 SaveDataLive(void);   // bottom_screen.c:84
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

   `CtrSettingsMarkDirty()` queues; it does not write. The write happens in
   `CtrSettingsFlush()`, called from `Rp2350PresentFrame()` beside
   `CtrSaveFlush()`. Setters run from `CtrBottomUpdate()`, i.e. from the middle
   of a game frame, and the write is seven-plus blocking FS round trips however
   few bytes it carries -- on a console the player sees that. Never call the
   writer from a setter.

   Range-check inside `Apply`, never trust the caller: a corrupt settings byte
   must leave the default standing.
3. **`3ds/host/settings.c`**: append a `uint8_t` to `struct CtrSettings`, bump
   `SETTINGS_VERSION`, add a `SETTINGS_Vn_SIZE` short-read migration, add the
   `extern` and the load/save lines. Keep the struct's every byte spoken for
   with explicit `pad`, or `settings_write()` writes uninitialised stack to the
   card. Choose the sense so that a zero byte means the old default.
4. **`3ds/ui/tab_extra.c`**: add the control, and fold the value into
   `UiExtraStateKey()` ([:544](ui/tab_extra.c#L544)) in a bit range nothing else
   claims.

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

**Two settings deliberately break the pattern**, and both are worth knowing
before you copy it:

- **A setting that expires does not persist.** `Ctr3dsSetShinyTest` has no
  `Apply` and never calls `CtrSettingsMarkDirty` ([host/main.c:322](host/main.c#L322)),
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
| Host-side change did nothing | Forgot `3ds/build_objs.sh`, or passed `CTR_BOOT_DIAG` to only one of the two builds. |
| The game pauses for a moment whenever you touch the second screen | Something on the touch path is doing blocking work in the frame. Read `log.txt` for `slow <stage>` lines: `CtrLogSlow` ([bridge.h](bridge.h)) reports any timed stage over 50 ms. The file only exists with `CTR_DEBUG_MENU` on. |
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
- On hardware, not only in an emulator (`AGENTS.md`). The boot log and the frame
  600 audio health report stay clean, and repaint frequency has not visibly
  risen.
