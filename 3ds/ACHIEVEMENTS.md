# Achievements

Every achievement on the TROPHY tab, in the order the tab shows them. The
tables in [achievements.c](achievements.c) are the source, and CI checks that
this list still matches them (see
[Keeping this list current](#keeping-this-list-current)).
[ACHIEVEMENTS_PLAN.md](ACHIEVEMENTS_PLAN.md) is the design record.

**83 achievements**: 65 main, 18 post-game.

- **Two pages.** MAIN holds everything that can be done up to the Hall of Fame.
  POST-GAME holds what only opens up after it. Its rows read "Hidden
  Achievement" until you enter the Hall of Fame, unless you have already
  earned them.
- **Six colours,** one per category: Story gold, Legendary green, Pokémon red,
  Battle purple, Extras blue, Contests pink.
- **Kept per playthrough,** keyed on the trainer ID. Loading a save unlocks
  everything it has already done, in one toast.
- **Counters** show on a locked row whose goal is between 2 and 999.
- **Three are watched as they happen,** because they leave nothing in the save
  to read afterwards: Shining Star, Tide's Out and Play Me a Tune. A save from
  before this build cannot catch up on them, and RESYNC on the debug page
  forgets them.

A flag or stat in the last column is read through the game's own accessor
(`FlagGet`, `GetGameStat`). "Reaches" means the unlock comes once the value is
at least the goal.

## Main

### Story (gold)

| Id | Achievement | Description | Unlocks when |
|---|---|---|---|
| 48 | A Journey Begins | Choose your first Pokémon | `FLAG_SYS_POKEMON_GET` |
| 49 | Friendly Rivalry | Beat your rival on Route 103 | `FLAG_DEFEATED_RIVAL_ROUTE103` |
| 50 | Field Research | Get a Pokédex from Prof. Birch | `FLAG_SYS_POKEDEX_GET` |
| 51 | Hit the Ground Running | Get the Running Shoes | `FLAG_SYS_B_DASH` |
| 0 | Stone Badge | Beat Roxanne in Rustboro City | `FLAG_BADGE01_GET` |
| 53 | Connected | Get a PokéNav in Rustboro | `FLAG_SYS_POKENAV_GET` |
| 59 | A Cut Above | Get HM01 Cut in Rustboro | `FLAG_RECEIVED_HM_CUT` |
| 52 | Special Delivery | Deliver Mr. Stone's letter to Steven | `FLAG_DELIVERED_STEVEN_LETTER` |
| 1 | Knuckle Badge | Beat Brawly in Dewford Town | `FLAG_BADGE02_GET` |
| 54 | Package Deal | Deliver the Devon Goods to Capt. Stern | `FLAG_DELIVERED_DEVON_GOODS` |
| 55 | Pedal Power | Get a Bike from Rydel in Mauville | `FLAG_RECEIVED_BIKE` |
| 2 | Dynamo Badge | Beat Wattson in Mauville City | `FLAG_BADGE03_GET` |
| 58 | Hot Pursuit | Stop Team Magma on Mt. Chimney | `FLAG_DEFEATED_EVIL_TEAM_MT_CHIMNEY` |
| 3 | Heat Badge | Beat Flannery in Lavaridge Town | `FLAG_BADGE04_GET` |
| 4 | Balance Badge | Beat your father in Petalburg | `FLAG_BADGE05_GET` |
| 60 | Surf's Up | Get HM03 Surf from Wally's father | `FLAG_RECEIVED_HM_SURF` |
| 61 | Take Flight | Get HM02 Fly on Route 119 | `FLAG_RECEIVED_HM_FLY` |
| 5 | Feather Badge | Beat Winona in Fortree City | `FLAG_BADGE06_GET` |
| 63 | The Best Ball | Find the Master Ball in the Aqua Hideout | `FLAG_ITEM_AQUA_HIDEOUT_B1F_MASTER_BALL`, the item ball in Aqua Hideout B1F |
| 6 | Mind Badge | Beat Tate and Liza in Mossdeep | `FLAG_BADGE07_GET` |
| 7 | Rain Badge | Beat Juan in Sootopolis City | `FLAG_BADGE08_GET` |
| 62 | HM Collector | Get all eight HMs | All eight `FLAG_RECEIVED_HM_*` flags. Counter out of 8 |
| 8 | Rival No More | Beat Wally at Victory Road | `FLAG_DEFEATED_WALLY_VICTORY_ROAD` |
| 9 | Champion | Enter the Hall of Fame | `FLAG_SYS_GAME_CLEAR`, which also reveals the POST-GAME page |

### Legendary (green)

The `FLAG_DEFEATED_*` flags are set whether the encounter ends in a catch or a
knockout, so these still unlock with the randomiser on.

| Id | Achievement | Description | Unlocks when |
|---|---|---|---|
| 11 | Sky High | Face Rayquaza atop Sky Pillar | `FLAG_DEFEATED_RAYQUAZA` |
| 76 | Puzzle Master | Complete all three Regi puzzles | `FLAG_SYS_REGIROCK_PUZZLE_COMPLETED` (Rock Smash in the Desert Ruins), `FLAG_SYS_BRAILLE_REGICE_COMPLETED` (a lap around the Island Cave's walls) and `FLAG_SYS_REGISTEEL_PUZZLE_COMPLETED` (Flash in the Ancient Tomb). Counter out of 3 |
| 14 | Rock Solid | Face Regirock in the Desert Ruins | `FLAG_DEFEATED_REGIROCK` |
| 15 | Cold Snap | Face Regice in the Island Cave | `FLAG_DEFEATED_REGICE` |
| 16 | Iron Will | Face Registeel in the Ancient Tomb | `FLAG_DEFEATED_REGISTEEL` |

### Pokémon (red)

| Id | Achievement | Description | Unlocks when |
|---|---|---|---|
| 23 | Gotcha! | Catch your first wild Pokémon | `GAME_STAT_POKEMON_CAPTURES` reaches 1 |
| 57 | It's Evolving! | Evolve a Pokémon for the first time | `GAME_STAT_EVOLVED_POKEMON` reaches 1 |
| 78 | Teaching an Old Dog New Tricks | Use any move tutor | Any of the ten `FLAG_MOVE_TUTOR_TAUGHT_*` flags, set when one of Hoenn's one-time tutors teaches a move. The first is in Slateport's Fan Club. The Battle Frontier's two Battle Point tutors set no flag, so they do not count |
| 66 | In Good Hands | Leave a Pokémon at the Day Care | `GAME_STAT_USED_DAYCARE` reaches 1 |
| 56 | Full House | Have six Pokémon in your party | Six party slots hold a Pokémon, eggs not counted. Paused in the Battle Factory, whose rentals fill the party |
| 26 | Hatchling | Hatch an Egg | `GAME_STAT_HATCHED_EGGS` reaches 1 |
| 65 | Ancient History | Revive a fossil at Devon Corp. | `FLAG_RECEIVED_REVIVED_FOSSIL_MON`, so the randomiser cannot hide it |
| 19 | Researcher | Own 25 kinds in the Hoenn Pokédex | Hoenn Pokédex owned count reaches 25. Counter |
| 20 | Field Worker | Own 50 kinds in the Hoenn Pokédex | Hoenn Pokédex owned count reaches 50. Counter |
| 25 | First Bite | Hook something while fishing | `GAME_STAT_FISHING_ENCOUNTERS` reaches 1 |
| 28 | Growing Up | See 50 evolutions | `GAME_STAT_EVOLVED_POKEMON` reaches 50. Counter |
| 27 | Breeder | Hatch 30 Eggs | `GAME_STAT_HATCHED_EGGS` reaches 30. Counter |
| 24 | Collector | Catch 100 wild Pokémon | `GAME_STAT_POKEMON_CAPTURES` reaches 100. Counter |
| 21 | Dex Enthusiast | Own 100 kinds in the Hoenn Pokédex | Hoenn Pokédex owned count reaches 100. Counter |
| 22 | Dex Expert | Own 150 kinds in the Hoenn Pokédex | Hoenn Pokédex owned count reaches 150. Counter |
| 64 | Maxed Out | Raise a Pokémon to level 100 | A party Pokémon at level 100. Paused in the Battle Factory, whose Open Level rentals are level 100 |
| 29 | Shining Star | Catch a shiny Pokémon | Watched as it happens: the one hook in `src/`, in `Cmd_givecaughtmon` |

### Battle (purple)

| Id | Achievement | Description | Unlocks when |
|---|---|---|---|
| 31 | Nothing Happened | Use Splash in battle | `GAME_STAT_USED_SPLASH` reaches 1 |
| 30 | Battle Hardened | Fight 100 trainer battles | `GAME_STAT_TRAINER_BATTLES` reaches 100. Counter |

### Extras (blue)

| Id | Achievement | Description | Unlocks when |
|---|---|---|---|
| 80 | What Do We Have Here! | Find any hidden item | Any `FLAG_HIDDEN_ITEM_*` flag, which only picking the item up sets. Route 104 alone hides five |
| 79 | You've Got Mail! | Receive any mail | Any Mail item in the bag or the PC's item storage, or any written Mail, held by a party Pokémon or kept in the PC mailbox. Petalburg's Mart is the first to sell it, and the in-game trades' Plusle, Horsea and Meowth arrive holding a letter |
| 81 | Mystery Communication | Enable Mystery Gift | `FLAG_SYS_MYSTERY_GIFT_ENABLE`: answer the questionnaire on any Poké Mart counter with LINK TOGETHER WITH ALL, once you have the Pokédex |
| 33 | Cable Car | Ride the cable car up Mt. Chimney | `GAME_STAT_RODE_CABLE_CAR` reaches 1 |
| 34 | Hot Springs | Soak in the Lavaridge hot springs | `GAME_STAT_ENTERED_HOT_SPRINGS` reaches 1 |
| 38 | Home Base | Set up a Secret Base | The player's own Secret Base slot is in use (`secretBases[0].secretBaseId`) |
| 36 | Green Thumb | Plant 25 berries | `GAME_STAT_PLANTED_BERRIES` reaches 25. Counter |
| 77 | Play Me a Tune | Use the White or Black Flute | Watched as it happens: `FLAG_SYS_ENC_UP_ITEM` or `FLAG_SYS_ENC_DOWN_ITEM` is set, read every frame because the next map load clears both. The Glass Workshop on Route 113 makes both flutes from volcanic ash |
| 40 | Jackpot! | Hit a jackpot at the Game Corner | `GAME_STAT_SLOT_JACKPOTS` reaches 1 |
| 35 | On Safari | Enter the Safari Zone | `GAME_STAT_ENTERED_SAFARI_ZONE` reaches 1 |
| 37 | Blender | Make 25 Pokéblocks | `GAME_STAT_POKEBLOCKS` reaches 25. Counter |
| 39 | Lucky Number | Win the Lilycove lottery | `GAME_STAT_WON_POKEMON_LOTTERY` reaches 1 |
| 68 | Tide's Out | Enter Shoal Cave at low tide | Watched as it happens: standing in Shoal Cave's entrance room with its low-tide layout. Low tide is 03:00 to 08:59 and 15:00 to 20:59 on the game's clock |
| 32 | Marathon | Walk 100,000 steps | `GAME_STAT_STEPS` reaches 100,000. No counter, since it would change on every step |
| 67 | Seasoned Traveller | Visit every route and town in Hoenn | Stood in all 50 map sections from Littleroot Town to Route 134. Counter, and with three or fewer left the description names them. Older saves count the towns they have visited and the routes where they found an item or beat a trainer |

### Contests (pink)

| Id | Achievement | Description | Unlocks when |
|---|---|---|---|
| 41 | Star Performer | Win a Pokémon Contest | `GAME_STAT_WON_CONTEST` reaches 1 |
| 42 | Ribbon Collector | Earn 10 ribbons | `GAME_STAT_RECEIVED_RIBBONS` reaches 10. Counter |

## Post-game

Hidden until you enter the Hall of Fame.

### Story (gold)

| Id | Achievement | Description | Unlocks when |
|---|---|---|---|
| 10 | A Bigger Journey Begins | Get the National Pokédex | `FLAG_SYS_NATIONAL_DEX` |
| 74 | New Adventures Await | Get all three tickets and the Old Sea Map | The Eon, Aurora and Mystic Tickets and the Old Sea Map in the bag. Dad hands them over after the Hall of Fame, and until the row is earned it says "Ask any ferry attendant for the rest", for saves already past that scene. Counter out of 4 |
| 75 | A New Frontier | Unlock the Battle Frontier | `FLAG_SYS_FRONTIER_PASS`, set when the reception gate hands over the Frontier Pass on your first visit |

### Legendary (green)

| Id | Achievement | Description | Unlocks when |
|---|---|---|---|
| 12 | Terra Firma | Face Groudon in the Terra Cave | `FLAG_DEFEATED_GROUDON` |
| 13 | Deep Blue | Face Kyogre in the Marine Cave | `FLAG_DEFEATED_KYOGRE` |
| 17 | Eon Chaser | Catch the roaming Latias or Latios | Latias or Latios owned in the Pokédex. Catching the roamer sets no flag |
| 69 | Southern Secret | Catch the Lati on Southern Island | `FLAG_CAUGHT_LATIAS_OR_LATIOS` |
| 70 | Faraway Friend | Catch Mew on Faraway Island | `FLAG_CAUGHT_MEW` |
| 71 | Out of This World | Catch Deoxys on Birth Island | `FLAG_BATTLED_DEOXYS`, which despite its name is set only on a catch |
| 72 | Rainbow Wing | Catch Ho-Oh atop Navel Rock | `FLAG_CAUGHT_HO_OH` |
| 73 | Silver Wing | Catch Lugia deep in Navel Rock | `FLAG_CAUGHT_LUGIA` |
| 18 | Odd Tree | Deal with the tree by the Frontier | `FLAG_DEFEATED_SUDOWOODO` |

### Pokémon (red)

| Id | Achievement | Description | Unlocks when |
|---|---|---|---|
| 82 | An Impossible Task | Complete the National Pokédex | `HasAllMons()`, the game's own test and the one its Pokédex diploma uses: every kind owned except Mew, Lugia, Ho-Oh, Celebi, Jirachi and Deoxys. The one row that cannot be earned yet, because the Kanto and Johto starters and legends need a trade and this port cannot trade yet |

### Battle (purple)

| Id | Achievement | Description | Unlocks when |
|---|---|---|---|
| 43 | Tower Climber | Win 7 in a row at the Battle Tower | `GAME_STAT_BATTLE_TOWER_SINGLES_STREAK` reaches 7. Counter |
| 44 | Silver Symbol | Earn a Frontier Silver Symbol | Any of the seven `FLAG_SYS_*_SILVER` flags |
| 45 | Silver Set | Earn all 7 Silver Symbols | All seven `FLAG_SYS_*_SILVER` flags. Counter out of 7 |
| 46 | Gold Symbol | Earn a Frontier Gold Symbol | Any of the seven `FLAG_SYS_*_GOLD` flags |
| 47 | Frontier Legend | Earn all 7 Gold Symbols | All seven `FLAG_SYS_*_GOLD` flags. Counter out of 7 |

## Keeping this list current

Change this file in the same commit as the tables in
[achievements.c](achievements.c): a new row, a moved one, a changed title,
description or condition. `python3 3ds/check_achievements_md.py` compares the
two. It checks every row's page, category, position, id, title and
description, the totals line at the top, and that no id is repeated or too big
for the store. The `achievements-list` job in
[build-3ds.yml](../.github/workflows/build-3ds.yml) runs it on every push. The
"Unlocks when" column is prose, so no script can check it. Keep it true by hand.

- **Ids are permanent.** An achievement's id is its bit in `achievements.bin`,
  so a new one takes the next unused id and a retired one's id is never handed
  out again. Row order is free. The next unused id is **83**.
- **Text has to fit:** 212px for a title and 268px for a description, measured
  by the debug page, since the TROPHY tab does not clip.
