# Event islands plan

**Status: implemented, not yet tested on a console.** Written 2026-09-13
against `5ee5f1c` on the `achievements` branch ("achievements 8"), and amended
the same day against `c2f8d00` on `main` for existing post-game saves and the
ticket counter. This file is now the design record. Line references were read
then; the script and function names are the stable anchors.

**How it landed, where that differs from the text below:**

- **A full bag stops the hand-over at the first item that doesn't fit,** with
  one "The BAG is full" message rather than one per item left. Nothing that
  wasn't given gets flags, as planned. `giveitem` itself says nothing on a full
  bag (`EventScript_NoRoomForItem` only sets `VAR_RESULT`), which is why the
  script shows `gText_TooBadBagIsFull` itself.
- **No catch check, from the second commit on.** The first skipped an item
  whose island Pokémon was already caught, as the Mystery Gift scripts do. A
  legit save showed that can only hurt: its legendaries had been transferred
  from Sapphire, which sets none of the island flags, so it was fine, but a
  save with a catch flag set and the item missing (a save editor can make one)
  would have been stuck below 4/4 on New Adventures Await for good.
- **One give helper for all four,** `Ctr3ds_EventScript_GiveEventItemIfMissing`.
  It takes the item in `VAR_0x8000`, the scratch var `giveitem` overwrites
  anyway, so a caller sees nothing a plain `giveitem` wouldn't do.
- **The fence check is "no content changes", not "byte-identical".** Without
  the define, the preprocessed `event_scripts.s` differs from the unmodified
  tree's in 17 blank lines and in cpp line markers shifted by the include's
  three-line fence. No label, instruction or data line differs.
- **At Lilycove the Old Sea Map comes first,** because the game checks it
  before the other first-time tickets. The talk that hands the items over ends
  with Mr. Briney sailing the player to Faraway Island, as it would have for a
  Mystery Gift map. The next talk is the sailor's shared first-time scene for
  the other three, with a choice of island. That scene marks all three shown,
  and after it the ordinary menu lists all four islands.

## Context

Emerald shipped four event islands, but only ever opened them through
distributions that ended years ago:
- **Southern Island** (Latias or Latios) needs the Eon Ticket, which Emerald
  could only get by Record Mixing (`src/record_mixing.c:977`).
- **Faraway Island** (Mew) needs the Old Sea Map. Birth Island (Deoxys) needs
  the Aurora Ticket. Navel Rock (Ho-Oh and Lugia) needs the Mystic Ticket. All
  three came by Mystery Gift (`data/scripts/gift_*.inc`).

The player should receive all four items on becoming Champion, and each
island's Pokémon should have an achievement for catching it. A post-game
counter achievement tracks the four items, and while it is incomplete it says
where to get the rest. Decided:
- **Delivery:** Dad hands them over with the S.S. Ticket in the post-Hall-of-Fame
  scene at home. That scene runs once, so for saves already past it, any S.S.
  Tidal ferry attendant (Slateport, Lilycove or the Battle Frontier) hands over
  any that are missing. Only Lilycove sails to the islands, but a Battle
  Frontier player may never talk to that attendant: they can sail from
  Slateport, use the Frontier's own dock, or Fly.
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
- **The items stay put:** all four are key items, which never leave the bag
  once in it. They can't be deposited (`TryDepositItem`,
  `src/item_menu.c:2252`), tossed or sold, and no script removes them.
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
  conditions, and the four callers.
- **One "needed" check per item:** `Ctr3ds_EventScript_EonTicketNeeded` and
  three siblings, each setting `VAR_RESULT`. An item is needed unless the player
  already has both it and its enable flag. Catches are not consulted, unlike the
  gift scripts: an island catch leaves its item in the bag, so a catch check
  never decides anything on a legit save and could only strand one whose flags
  and bag disagree.
- **`Ctr3ds_EventScript_AnyEventTicketNeeded`:** FALSE if `FLAG_SYS_GAME_CLEAR`
  is unset. Otherwise it runs the four checks, stopping at the first TRUE. Every
  caller is already gated on the Hall of Fame, but this way the file refuses
  before it whatever calls it.
- **One "try give" per item:**
  - Skip it if not needed.
  - If it is already in the bag, only set the flags.
  - Otherwise `giveitem`, which plays the obtain message and fanfare.
  - If the bag is full, show `gText_TooBadBagIsFull` and set no flags, so the
    ferry can hand it over later. `Common_EventScript_ShowBagIsFull` isn't used
    because it ends the script.
  - On success, set `FLAG_ENABLE_SHIP_*`, plus `FLAG_RECEIVED_*` for the three
    that have one.
- **Three entry points:**
  - **`Ctr3ds_EventScript_EventTicketsFromDad`:** if any item is needed, Dad says
    "Oh, and these came for you, too" (tickets and an old map, no sender), gives
    them, then points at the Lilycove port.
  - **`Ctr3ds_EventScript_EventTicketsAtLilycove`:** if any item is needed, the
    attendant says they were left at the port for the player, then gives them.
    Lilycove's own first-time ticket dialogue follows.
  - **`Ctr3ds_EventScript_EventTicketsAtOtherPort`:** the same, then one line
    saying the ferry to those islands sails from LILYCOVE.
  - The two harbor entry points share one internal block (the message and the
    four try-gives), so that text is written once.
- **Text:** in Gen 3 style (upper-case names, `…`, about 30 characters a line),
  in the same file.

## 3. The four call sites (one fenced `call` each)

- **`data/scripts/players_house.inc`,
  `PlayersHouse_1F_EventScript_GetSSTicketAndSeeLatiTV`:** calls
  `...FromDad` after
  `msgbox PlayersHouse_1F_Text_PortsInSlateportLilycove`, so Dad's ferry line
  leads into it. The scene is shared by both houses and runs once, before
  `FLAG_RECEIVED_SS_TICKET`.
- **`data/maps/LilycoveCity_Harbor/scripts.inc`,
  `LilycoveCity_Harbor_EventScript_FerryAttendant`:** calls `...AtLilycove`
  after the `FLAG_SYS_GAME_CLEAR` check and before the `Get*State` calls. Those
  then see the new items, so the game's own "first time showing this ticket"
  dialogue plays straight after.
- **`data/maps/SlateportCity_Harbor/scripts.inc`,
  `SlateportCity_Harbor_EventScript_AskForTicket`:** calls `...AtOtherPort` as
  its first line, before "May I see your ticket". The label is only reached
  through the `FLAG_SYS_GAME_CLEAR` check in `..._FerryAttendant`. Its "choose
  again" loop goes back to `..._ChooseDestination`, so the hand-over can't
  repeat.
- **`data/maps/BattleFrontier_OutsideWest/scripts.inc`,
  `BattleFrontier_OutsideWest_EventScript_FerryAttendant`:** calls
  `...AtOtherPort` after `faceplayer`, before "May I see your ticket". The
  Frontier is only reachable after the Hall of Fame, and its loop goes back to
  `..._ChooseFerryDestination`, not to the top.
- All four files are included by `data/event_scripts.s`, so section 1's
  `assemble_s` define covers them.

## 4. Achievements (`3ds/achievements.c`)

### The ticket counter

One row in `sPostStory` (gold, hidden until the Hall of Fame like the rest of
POST-GAME), after A Bigger Journey Begins:

| id | Title | Description | Hint while locked | Counts |
|---|---|---|---|---|
| 74 | New Adventures Await | Get all three tickets and the Old Sea Map | Ask any ferry attendant for the rest | `sEventItems` in the bag, goal 4 |

- **A new kind, `ACH_ITEM_LIST`:** how many of the `count` items in `list` are
  in the bag. `Value()` loops `CheckBagHasItem(d->list[i], 1)`, the game's own
  accessor, which handles the encrypted quantities.
  - The `list` and `count` field comments name both list kinds.
  - The file gains `item.h` and `constants/items.h`, and the header comment's
    list of accessors gains `CheckBagHasItem`.
- **`sEventItems`:** `ITEM_EON_TICKET`, `ITEM_AURORA_TICKET`,
  `ITEM_OLD_SEA_MAP` and `ITEM_MYSTIC_TICKET`, next to `sHmFlags`. Its comment
  says:
  - Key items never leave the bag, so the count only goes up.
  - Inside the Battle Pyramid, `CheckBagHasItem` answers for the Pyramid bag
    (`src/item.c:136`), so a locked row reads low there. That can't unlock
    anything falsely, and unlocks are never taken back.
- **Why the items and not the `FLAG_ENABLE_SHIP_*` flags:** the items are what
  the player sees, and the old Cable Club Eon Ticket script sets the flag even
  when a full bag made `giveitem` fail (`data/scripts/cable_club.inc`,
  `CableClub_EventScript_DistributeEonTicket`).
- **The hint:** a new `const char *hint` in `struct AchDef`, NULL everywhere
  else. The new `ITEM_LIST(i, t, d, h, l, g)` macro takes it (NULL for none).
  In `LocalGet`, after the hidden early return, a locked row with a hint shows
  the hint as its description. This is the same kind of swap as Seasoned
  Traveller's "Still to visit".
  - Before the Hall of Fame, the row is hidden like the rest of POST-GAME.
  - On an existing post-game save it reads "Ask any ferry attendant for the
    rest" at 0/4.
  - A new save fills it during Dad's scene, and it then shows the plain
    description.
- **Width check:** the debug page already measures the `v.desc` it is
  showing, so it measures the hint while locked. `AchDebugRealText` stays as
  it is.

### The catches

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
- The next unused id becomes 75, and the "(69 at the time of writing)" comment
  above the tables says so.
- The total becomes 75 achievements: 59 main and 16 post-game.

## 5. Docs

- **`3ds/ACHIEVEMENTS_PLAN.md`:** a "ninth commit" section covering the six
  rows, `ACH_ITEM_LIST`, the hint field, the ticket delivery, and the first
  fenced data change.
- **`README.md`:**
  - The count becomes 75.
  - One sentence says Dad hands over the four event items after the Hall of
    Fame, opening Southern Island, Faraway Island, Birth Island and Navel Rock.
    On an existing post-game save, any S.S. Tidal ferry attendant hands them
    over instead.
- **`README-TECHNICAL.md`:** where it describes the `#if PLATFORM_3DS`
  principle, note that data scripts can be fenced now (`assemble_s`), and name
  the one file that uses it.
- **`3ds/ROADMAP.md`**, Part E: 69 becomes 75.

## Verification

- **Local preprocessing:** run `tools/preproc` then cpp then `preproc -ie` on
  `data/event_scripts.s`.
  - With `-DPLATFORM_3DS=1`, the new labels and the four calls appear, and
    preproc converts every `.string` without error.
  - Without the define, the output matches the unmodified tree's apart from
    blank lines and cpp line markers (see "How it landed"). That proves the
    fence.
  - A full local assemble isn't possible: clang's assembler rejects GNU-only
    constructs in the game's macros (`waitstate`), so CI's devkitARM run is the
    assembly check.
- **C and text:** run the `STRICT=1` syntax check on `achievements.c`, the id
  check (75 unique, all old ids unchanged), and the font-width check on the new
  titles (212px), descriptions and the hint (268px each).
- **Commit and CI:** commit with no attribution trailer. Push, then watch
  `build-3ds`.
- **On a console or in Azahar:**
  - **Before the Hall of Fame:** the Lilycove attendant still says the ferry
    isn't available.
  - **A save just before the Elite Four:** after the credits, Dad gives the
    S.S. Ticket then the four items, and New Adventures Await toasts in gold.
    At Lilycove, the first talk is the Old Sea Map's scene, straight to Faraway
    Island. The next is the sailor's scene for the other three, with a choice,
    and after that the ordinary menu offers all four islands.
  - **An existing post-game save**, on a copy for each attendant:
    - POST-GAME shows New Adventures Await at 0/4 with the ferry hint.
    - **Slateport:** gives the items, says the island ferry sails from
      Lilycove, then shows the usual destination menu. The counter reaches 4/4
      and toasts.
    - **Battle Frontier dock:** the same.
    - **Lilycove:** gives the items, then the Old Sea Map's first-time scene,
      then sails to Faraway Island.
    - Talking to any of them again gives nothing more.
  - **Catching each Pokémon** gives its green toast. A catch made earlier is
    picked up on load.
  - **POST-GAME** shows 16, hidden until the Hall of Fame.
