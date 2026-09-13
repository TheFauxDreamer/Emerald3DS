# Event islands plan

**Status: planned, not implemented.** Written 2026-09-13 against `5ee5f1c` on
the `achievements` branch ("achievements 8"). Line references were read then;
the script and function names are the stable anchors.

## Context

Emerald shipped four event islands, but only ever opened them through
distributions that ended years ago:
- **Southern Island** (Latias or Latios) needs the Eon Ticket, which Emerald
  could only get by Record Mixing (`src/record_mixing.c:977`).
- **Faraway Island** (Mew) needs the Old Sea Map. Birth Island (Deoxys) needs
  the Aurora Ticket. Navel Rock (Ho-Oh and Lugia) needs the Mystic Ticket. All
  three came by Mystery Gift (`data/scripts/gift_*.inc`).

The player should receive all four items on becoming Champion, and each
island's Pokémon should have an achievement for catching it. Decided:
- **Delivery:** Dad hands them over with the S.S. Ticket in the post-Hall-of-Fame
  scene at home. The Lilycove ferry attendant hands over any that are missing,
  for saves already past that scene.
- **Toggle:** none; always on. It only opens areas the game shipped with, and
  only after the Hall of Fame.

What the game already does:
- **The ferry gate:** the Lilycove attendant sails to an island only after
  `FLAG_SYS_GAME_CLEAR`, and only with both the item in the bag and its
  `FLAG_ENABLE_SHIP_*` flag (`LilycoveCity_Harbor_EventScript_Get*State`,
  `ScriptMenu_CreateLilycoveSSTidalMultichoice` in `src/script_menu.c:430`). The
  gift scripts set exactly that, plus a `FLAG_RECEIVED_*` flag for the three
  Mystery Gift items.
- **The islands:** all four maps and encounters are complete in this tree.
- **Catch flags:** each island's script sets a flag only on a catch:
  `FLAG_CAUGHT_LATIAS_OR_LATIOS`, `FLAG_CAUGHT_MEW`, `FLAG_CAUGHT_HO_OH` and
  `FLAG_CAUGHT_LUGIA`. Deoxys uses `FLAG_BATTLED_DEOXYS`, which despite its name
  is set only on the catch branch (`data/maps/BirthIsland_Exterior/scripts.inc:91`).
  Flags still work with the randomiser on, and past catches are already in the
  save.

## 1. Let data scripts be fenced (`3ds/build_objs.sh`)

- `assemble_s()` passes `-DPLATFORM_3DS=1` to its cpp stage, as the `m4a_1.s`
  line already does. Script changes can then sit behind `#if PLATFORM_3DS` like
  the `src/` hooks.
- **Safe:** no header the data files include tests `PLATFORM_3DS`. They include
  only `config.h` and `constants/*`, and `constants/gba_constants.inc`, the one
  guarded `.inc`, is not among them. So nothing else in the data changes.
- The top-level Makefile's other builds don't define it, so they stay vanilla.

## 2. The ticket script (new `data/scripts/ctr3ds_event_tickets.inc`)

- **Include:** included from `data/event_scripts.s` after `players_house.inc`,
  fenced. The header comment covers what the distributions set, the ferry's two
  conditions, and the two callers.
- **One "needed" check per item:** `Ctr3ds_EventScript_EonTicketNeeded` and
  three siblings, each setting `VAR_RESULT`. An item is needed unless the player
  already has both it and its enable flag, or has already caught its Pokémon
  (both of Ho-Oh and Lugia for the Mystic Ticket), mirroring the gift scripts'
  guards.
- **`Ctr3ds_EventScript_AnyEventTicketNeeded`:** runs the four checks, stopping
  at the first TRUE.
- **One "try give" per item:**
  - Skip it if not needed.
  - If it is already in the bag, only set the flags.
  - Otherwise `giveitem`, which plays the obtain message and fanfare.
  - If the bag is full, show `gText_TooBadBagIsFull` and set no flags, so the
    ferry can hand it over later. `Common_EventScript_ShowBagIsFull` isn't used
    because it ends the script.
  - On success, set `FLAG_ENABLE_SHIP_*`, plus `FLAG_RECEIVED_*` for the three
    that have one.
- **Two entry points:**
  - **`Ctr3ds_EventScript_EventTicketsFromDad`:** if any item is needed, Dad says
    "Oh, and these came for you, too" (tickets and an old map, no sender), gives
    them, then points at the Lilycove port.
  - **`Ctr3ds_EventScript_EventTicketsAtHarbor`:** if any item is needed, the
    attendant says they were left at the port for the player, then gives them.
- **Text:** in Gen 3 style (upper-case names, `…`, about 30 characters a line),
  in the same file.

## 3. The two call sites (one fenced `call` each)

- **`data/scripts/players_house.inc`,
  `PlayersHouse_1F_EventScript_GetSSTicketAndSeeLatiTV`:** after
  `msgbox PlayersHouse_1F_Text_PortsInSlateportLilycove`, so Dad's ferry line
  leads into it. The scene is shared by both houses and runs once, before
  `FLAG_RECEIVED_SS_TICKET`.
- **`data/maps/LilycoveCity_Harbor/scripts.inc`,
  `LilycoveCity_Harbor_EventScript_FerryAttendant`:** after the
  `FLAG_SYS_GAME_CLEAR` check and before the `Get*State` calls. Those then see
  the new items, so the game's own "first time showing this ticket" dialogue
  plays straight after.

## 4. Achievements (`3ds/achievements.c`)

Five `FLAG` rows in `sPostLegends` (green, hidden until the Hall of Fame like
the rest of POST-GAME), after Eon Chaser and before Odd Tree:

| id | Title | Description | Flag |
|---|---|---|---|
| 69 | Southern Secret | Catch the Lati on Southern Island | `FLAG_CAUGHT_LATIAS_OR_LATIOS` |
| 70 | Faraway Friend | Catch Mew on Faraway Island | `FLAG_CAUGHT_MEW` |
| 71 | Out of This World | Catch Deoxys on Birth Island | `FLAG_BATTLED_DEOXYS` (catch only, commented) |
| 72 | Rainbow Wing | Catch Ho-Oh atop Navel Rock | `FLAG_CAUGHT_HO_OH` |
| 73 | Silver Wing | Catch Lugia deep in Navel Rock | `FLAG_CAUGHT_LUGIA` |

- A comment above the rows says where the tickets come from.
- Eon Chaser's comment stops saying the Eon Ticket is out of reach.
- The next unused id becomes 74.
- The total becomes 74 achievements: 59 main and 15 post-game.

## 5. Docs

- **`3ds/ACHIEVEMENTS_PLAN.md`:** a "ninth commit" section covering the five
  rows, the ticket delivery, and the first fenced data change.
- **`README.md`:**
  - The count becomes 74.
  - One sentence says Dad hands over the four event items after the Hall of
    Fame, opening Southern Island, Faraway Island, Birth Island and Navel Rock.
- **`README-TECHNICAL.md`:** where it describes the `#if PLATFORM_3DS`
  principle, note that data scripts can be fenced now (`assemble_s`), and name
  the one file that uses it.
- **`3ds/ROADMAP.md`**, Part E: 69 becomes 74.

## Verification

- **Local preprocessing:** run `tools/preproc` then cpp then `preproc -ie` on
  `data/event_scripts.s`.
  - With `-DPLATFORM_3DS=1`, the new labels and the two calls appear, and
    preproc converts every `.string` without error.
  - Without the define, the output is byte-identical to the unmodified tree's.
    That proves the fence.
  - A full local assemble isn't possible: clang's assembler rejects GNU-only
    constructs in the game's macros (`waitstate`), so CI's devkitARM run is the
    assembly check.
- **C and text:** run the `STRICT=1` syntax check on `achievements.c`, the id
  check (74 unique, all old ids unchanged), and the font-width check on the new
  titles and descriptions.
- **Commit and CI:** commit with no attribution trailer. Push, then watch
  `build-3ds`.
- **On a console or in Azahar:**
  - **Before the Hall of Fame:** the Lilycove attendant still says the ferry
    isn't available.
  - **A save just before the Elite Four:** after the credits, Dad gives the
    S.S. Ticket then the four items. Lilycove then offers all four islands, with
    each first-time ticket line.
  - **An existing post-game save:** the Lilycove attendant hands them over once,
    then sails.
  - **Catching each Pokémon** gives its green toast. A catch made earlier is
    picked up on load.
  - **POST-GAME** shows 15, hidden until the Hall of Fame.
