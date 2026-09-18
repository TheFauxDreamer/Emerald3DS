# The NULL dereference crashes, and how to find the rest

**Status: 15 crashes fixed, the sweep not yet run.** Written 2026-09-18 against
`ab0412e`. This is the record of one bug class that keeps stopping the console,
what is fixed, what the tools can and cannot see, and what to do next.

Read [SECOND_SCREEN_CHEATSHEET.md](SECOND_SCREEN_CHEATSHEET.md) section 15 for
how to triage a crash screen. This file is about finding the crashes before a
player does.

## Contents

- [Why this class exists](#why-this-class-exists)
- [The five sub-classes](#the-five-sub-classes)
- [What is fixed](#what-is-fixed)
- [The tools, and what each one cannot see](#the-tools-and-what-each-one-cannot-see)
- [Coverage](#coverage)
- [Next steps](#next-steps)
- [How to verify a fix](#how-to-verify-a-fix)
- [Out of scope](#out-of-scope)

## Why this class exists

A GBA has no MMU. Address 0 is the BIOS. Thus a read or a write through a NULL
pointer is harmless: the read gives an old opcode that nothing uses, and the
write does nothing. Vanilla code does this in many places and passes every test
on real hardware.

The ARM11 of the 3DS maps nothing below the code segment, which starts at
`0x00100000`. The same access is a data abort and stops the process. The FAR on
the crash screen is the offset of the field, so it is 0 only when the field is
the first member of its struct.

Note two things that follow from this:

- A **dangling** pointer cannot crash this port. `gHeap` is a static array
  inside `gGbaMem` ([src/malloc.c](../src/malloc.c)), so freed memory stays
  mapped. A stale read draws junk. Only NULL faults. Thus `FREE_AND_SET_NULL`
  is the dangerous call, and a plain `Free()` is safe.
- An emulator proves nothing here. Azahar tolerates a read through NULL. Test
  this class on the console.

## The five sub-classes

| # | Shape | Example |
|---|---|---|
| 1 | An optional pointer that is NULL in some game state | `sStorage->movingMonSprite` with no Pokemon held |
| 2 | A global that is NULL until something sets it | `gMPlay_PokemonCry` before the first cry |
| 3 | A literal NULL given to a pointer parameter | `GetLocalWildMon(FALSE)` |
| 4 | State freed while a scheduled reader still runs | the naming screen, and 8 more |
| 5 | An `Alloc` that returns NULL under heap pressure | out of scope, see the last section |

Sub-class 4 is the one that keeps coming back. It has four reader shapes, and
the last two were only understood in September 2026:

- a **sprite callback** that `AnimateSprites()` runs
- a **task** that `RunTasks()` runs
- a **VBlank callback**, which needs `VBLANK_REQUIRE` ([include/main.h](../include/main.h))
- the **main callback's own tail**: the task frees the state inside
  `RunTasks()`, and the rest of that callback then runs in the same frame

The last shape now accounts for 5 of the 15 crashes. Watch for it.

## What is fixed

| Commit | Site | What the player did |
|---|---|---|
| `583006b` | `SpriteCB_EnemyShadow`, battle teardown | ran from a wild battle |
| `286b6ed` | `SpriteCB_Cursor`, naming screen | confirmed a blank nickname |
| `0f9763a` | `Cmd_playanimation` | broke a Substitute |
| `8641b00` | `StopCryAndClearCrySongs` | entered a double battle as the first cry |
| `f973516` | `GetLocalWildMon` | walked with a follower |
| `2bce46c` | `SetMovingMonPriority` | moved the PC cursor |
| `75794ca` | `InitBoxMonSprites` | opened MOVE ITEMS |
| `ff770eb` | `CB2_PokeStorage` tail | left the PC |
| `984ee06` | `CB2_Roulette` tail | left the Roulette |
| `984ee06` | `VBlankCB_TradeAnim` | finished a trade |
| `984ee06` | `GetMapConnection` | used DIVE underwater |
| `dbc4e81` | `gAnimDisableStructPtr`, 5 sites | used Rollout, Ice Ball or Fury Cutter in a Contest |
| `ab0412e` | `CB2_CheckPlayAgainLocal` and `Link` tails | finished a Berry Blender session |
| `ab0412e` | `CB2_FreeTradeAnim` tail | finished any trade |
| `ab0412e` | `CB2_InGameTrade` tail | finished an NPC trade |

The first 8 were each found by a player. The last 7 were found by reading.

Three of these came from one file, `src/trade.c`, through three different
reader shapes. One fix in a file does not clear the file.

## The tools, and what each one cannot see

### `tools/audit_dangling.py`

Finds sub-class 4. Run it with no arguments. It prints its corpus first, then
any site that nobody has reviewed:

```sh
python3 tools/audit_dangling.py          # candidates nobody has cleared
python3 tools/audit_dangling.py --all    # also list the cleared ones
```

It reports **candidates, not bugs**. A person answers the question it prints,
by reading the callback chain. Vanilla's safe idiom is "free, then hand off to
a setup CB2 that resets the sprites first". Most candidates are that, and
editing a working teardown is the larger risk.

Before 2026-09-18 this script could see only 23 of the 47 files that free a
pointer, and 70 of 111 pointers, because its declaration test allowed only one
of the six spellings this tree uses. Every crash the port had found was in a
file it skipped. It now sees 47/47 and 111/111. **The corpus line prints on
every run. If it is not 47/47, something regressed.**

What it still cannot see:

- **The main callback tail and the VBlank callback.** It models only sprite
  callbacks and tasks. All 5 tail crashes were found by hand. This is the
  largest remaining gap, and it is next step 1.
- **Resets in a caller.** The "was it reset first" test reads only the body of
  the freeing function. `Task_ExitRoulette` calls `ResetSpriteData()` and then
  `FreeRoulette()`, so the site is safe, but the reset is invisible from inside
  `FreeRoulette()`. Expect that shape as a false positive.
- **Which reader actually runs.** `reaches()` follows the call graph, so a
  large scene lists most of its callbacks. The list means "these could read
  it", not "these do read it after the free".

### The instrumented wasm build

`-fsanitize=null` puts one test before each dereference and calls
`__ubsan_handle_type_mismatch_v1` when the pointer is 0. With
`-fsanitize-recover=null` the handler returns and the access still happens. In
wasm, address 0 is inside the linear memory and is readable, so the run
continues. Thus **one run reports every site it touches**, instead of stopping
at the first.

```sh
make wasm WASM_SANITIZE="-fsanitize=null -fsanitize-recover=null"
node tools/wasm_replay.mjs tools/wasm_replays/<route>.txt
```

[web/app.js](../web/app.js) supplies the handler. It logs each site once, and
`window.pokeemerald.nullDerefs()` prints the summary.
`tools/wasm_replay.mjs` writes the console to `console.log` in its output
folder, so the list arrives as an artifact.

These parts are verified: clang emits the handler for both wasm32 and ARM,
`-fsanitize=null` adds only the null test, the checks survive `-O2`, and
`tools/preproc` passes the `#` line markers through, so a site names the real
`src/*.c` file and line.

**The sweep has never run.** It needs `wasm-ld` and the decompilation tools,
which are not on the machine that wrote this.

## Coverage

| Sub-class | State |
|---|---|
| 3, a literal NULL argument | Reported exhausted across `src/`, `3ds/` and `include/`. The method was checked against `f973516^` and `0f9763a^` and found both. Two latent leftovers are not reachable in normal play. |
| 4, freed state | Swept for all four reader shapes. 2 candidates are open, below. |
| 1 and 2, optional and uninitialised pointers | Partly swept. Two proven findings are open, below. Cases that need OAM exhaustion are held back on purpose. |
| Anything behind `#if PLATFORM_3DS` | **Not swept at all.** |

That last row is the one to keep in mind. `src/` has 352 references to
`PLATFORM_3DS` across 60 files: the followers, the day care yard, the level
cap, the event tickets. The wasm build does not define `PLATFORM_3DS`, so the
sweep cannot reach any of it. The follower crash `f973516` was in exactly that
code. Only next step 6 covers it.

## Next steps

In order of value for the effort.

### 1. Teach the detector the callback tail shape

The shape that caused 5 of the 16 crashes is the one shape the detector cannot
see. Add two passes to `tools/audit_dangling.py`:

- **Main callback tail.** Find each function that calls `RunTasks()` and then
  does more work, where that later work reads a file-scope pointer that one of
  the file's own tasks frees. Report whether an `== NULL` early return already
  guards it. `CB2_NamingScreen` and `CB2_PokeStorage` are the model.
- **VBlank callback.** Find each VBlank callback that reads a freeable pointer
  and has no `VBLANK_REQUIRE`. There are 25 guards across 11 files, and the
  25th is the one added to `src/trade.c` in `984ee06`. That file had none while
  needing one, and only a person noticed.

Consider also following one level up for the reset test, which would remove the
`FreeRoulette` class of false positive.

### 2. The two proven findings

Both are sub-class 1, both have a sibling in the same file that already tests
the pointer, which is the evidence that the NULL state is real:

- [src/pokemon_storage_system.c:3919](../src/pokemon_storage_system.c) reads
  `sStorage->displayMonSprite->oam.mosaic`. Three siblings test that pointer.
  It runs every frame from pressing A on a Pokemon until the mosaic ends.
- `CreateMonMarkingComboSprite` and `CreateMonMarkingAllCombosSprite`
  ([src/mon_markings.c](../src/mon_markings.c)) return NULL. Two of the three
  call sites ignore that: `src/pokemon_storage_system.c:3875` and
  `src/pokenav_conditions_gfx.c:675`. The third,
  `src/pokemon_summary_screen.c:4078`, tests it, which is the evidence.
  Separately, `RemoveAndCreateMonMarkingsSprite`
  (`src/pokemon_summary_screen.c:4092`) gives the stored pointer to
  `DestroySprite` with no test, which is the same hazard by a different route.

**Held back on purpose**, until the sweep proves they occur: the rain sprite
loop (`src/field_weather_effect.c:724`), the PC `cursorSprite` and
`cursorShadowSprite`, and `CreatePartyMonsSprites`. All three need OAM
exhaustion before the pointer can be NULL. Guarding them is churn in vanilla
code on evidence that is still theoretical.

### 3. The two open candidates

`FreeBattleSpritesData` and `FreeMonSpritesGfx`
([src/battle_gfx_sfx_util.c](../src/battle_gfx_sfx_util.c)). The end of battle
is safe, because `583006b` reordered
`FreeResetData_ReturnToOvOrDoEvolutions` to call `ResetSpriteData()` first. The
two functions have 9 and 19 other callers. Each caller needs the same question
asked of it. Note that `CB2_FreeTradeAnim` calls `FreeMonSpritesGfx()` with no
reset before it, so at least one other caller has the shape.

### 4. Run the sweep

```sh
brew install llvm                        # supplies wasm-ld
make tools && make generated             # needs libpng
python3 tools/generate_wasm_assets.py
make wasm                                # confirm it builds with no flag first
```

Then build with `WASM_SANITIZE` and drive it. Extend
`tools/wasm_replays/` from `mudkip_starter.txt`, which already reaches a
started save. Aim the routes at the states this class lives in: the PC in all
its modes, a wild battle, a double battle, a trainer battle with a send-out
cry, the party menu, the summary screen, the Pokedex, the bag, the naming
screen, a Contest with Rollout, the Roulette, the Berry Blender, and the
overworld for a few thousand frames.

The result is a list of `file:line` with hit counts. That is the first time
this class will have had one.

### 5. Make the detector a gate

Add `python3 tools/audit_dangling.py` to
[.github/workflows/build-3ds.yml](../.github/workflows/build-3ds.yml), beside
the `check_achievements_md.py` step. It needs no toolchain and already exits
non-zero on an unreviewed site. Today nothing runs it.

### 6. The opt-in 3DS diagnostic build

This is the only item that covers `PLATFORM_3DS` code. With it, a crash names
its own file and line in `log.txt` instead of needing a FAR and a PC resolved
against a CI ELF.

- Add `CTR_UBSAN="${CTR_UBSAN:-0}"` to
  [3ds/build_objs.sh](build_objs.sh), on the model of `CTR_BOOT_DIAG` directly
  above it. Append the flags to `CFLAGS` in `compile_c()`. Keep it off by
  default. Never ship it.
- Add `3ds/host/ubsan.c` with the handler, built by [3ds/Makefile](Makefile).
- **Log synchronously.** `log_to_file()` ([host/log.c](host/log.c)) queues
  lines for the I/O thread, and the abort comes in the same frame, so a queued
  line dies with the process. `log_write()` is the path that flushes. Add an
  entry point that reaches it directly. Dedupe each site in the handler as
  well, because `LOG_MAX_LINES` is 512 and every other log line shares it.
- devkitARM's GCC is unverified for this flag. GCC has emitted
  `__ubsan_handle_type_mismatch_v1` since GCC 8 and takes the same two flags.
  Defining the handler here is what keeps `libubsan` out of the link.

### 7. One convention, and the cheatsheet

The fixes use four different treatments today: `#if PLATFORM_3DS` (`583006b`),
`#if WASM || RP2350` (`ff770eb`), `#ifdef UBFIX` (the rest), and no fence at
all (`f973516`). Thus a grep cannot list the class.

Settle on `#ifdef UBFIX` with a `// UB NULL:` comment tag, which is what every
fix from `dbc4e81` on uses. Leave `f973516` unfenced, because giving a real
pointer is correct on every target. Retag the other two.

Then rewrite the two "small FAR" rows in
[SECOND_SCREEN_CHEATSHEET.md](SECOND_SCREEN_CHEATSHEET.md) section 15. Name the
sub-classes, say that the sweep exists and how to run it, and say that a new
guard carries the tag. A row that grows by one sentence for each crash is the
reactive method written down.

## How to verify a fix

1. **The matching ROM must not change.** `make compare` cannot be the check:
   `tools/agbcc` is not in the tree and CI never builds the ROM. Preprocess each
   changed file before and after with `MODERN` undefined, where an
   `#ifdef UBFIX` block disappears, and confirm the output is the same apart
   from whitespace. Locally this needs a stub `string.h` and
   `-D__INTELLISENSE__`, because [include/global.h](../include/global.h) line 4
   includes the real header. A new `VBLANK_REQUIRE` legitimately adds
   `((void)0);`, as all 24 existing uses do.
2. **Syntax with `UBFIX` on.** Preprocess with `-DMODERN=1 -D__INTELLISENSE__`
   and pipe to `clang --target=arm-none-eabi -march=armv6k -fsyntax-only`. Do
   not use the host target, because mach-O refuses the `section` attributes.
3. **`python3 tools/audit_dangling.py`** reports no unreviewed site, and the
   corpus line still says 47/47 and 111/111.
4. **Record the verdict.** A site that is cleared gets a row in the `REVIEWED`
   table with the reason. A site that is fixed says so.
5. **On the console.** Walk the thing that crashed. An emulator tolerates a read
   through NULL, so a clean Azahar run proves nothing.

## Out of scope

An `Alloc` that returns NULL under heap pressure. 89 file-scope allocations in
`src/` are used with no test, which is vanilla and is safe while the heap
holds. The heap is `0x1C000` and the port does not change it. The fix for that
class is never "add 89 tests". It is "find the leak": the allocation that
escapes its free.
