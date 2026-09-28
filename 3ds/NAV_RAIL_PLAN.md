# Nav rail plan

**Status: stage 1 done.** Written 2026-09-28.

The bottom screen moves its nav bar from the bottom edge to a rail on the
right, after the layout of
[pokeemerald-3Ds-dualscreen](https://github.com/ZallaxDev/pokeemerald-3Ds-dualscreen):
options on the right, what they show on the left. No feature, page or control
is removed; only positions change.

| | Bar (now) | Rail |
|---|---|---|
| Nav | 320x48 along the bottom, 4 x 60 + HOME 80 | 64x240 on the right, 5 slots of 48 |
| Content | 320x192 (40 x 24 tiles) | 256x240 (32 x 30 tiles), the same pixel count |

`CTR_UI_RAIL` (3ds/build_objs.sh, default 0) selects the layout, and
`3ds/ui/ui_shell.h` gives `UI_CONTENT_W`, `UI_CONTENT_H` and `UI_RAIL_W` for
it. With 0 every file must preprocess to the same code as before its stage.
With 1, a tab that has not moved yet loses its right 64px under the rail (the
shell draws the rail last), so each intermediate build still works.

## Stages

1. **The switch, the rail and touch routing.** Done: ui_shell.h, ui_navbar.c
   (slots from `SlotX/SlotY/SlotW/SlotH`), bottom_screen.c
   (`UI_TOUCH_IN_NAV`), build_objs.sh.
2. **One tab per commit**, each with a 256x240 layout behind the flag. The
   list below is the order.
3. **Flip the default** to 1, then delete the bar layout constants. Update
   SECOND_SCREEN_CHEATSHEET.md ("Adding a tab", the layout sections), the
   README and the ROADMAP.

## Files to move (stage 2)

Counts are references to `CTR_BOTTOM_WIDTH` / `UI_CONTENT_H` at the start;
most coordinates are literals beside them.

- [ ] tab_extra.c (1414 lines): the HOME launcher (4x4 grid of tiles) and
      pages SETTINGS, GAMEPLAY, EXTRAS, FOLLOWER, NAV BAR, DEBUG. SETTINGS rows
      are 276px wide centred in 320: four buttons become 54px (4 x 54 + 3 x 8
      = 240 inside the frame). The RENDER and MUSIC controls take a line of
      their own. Check rows narrow the hint column.
- [ ] view_trainer.c, view_clock.c, view_dowsing.c, view_berries.c,
      view_daycare.c, view_friendship.c, view_frontier.c, ui_link.c: the game
      pages of HOME.
- [ ] tab_party.c (1258): the grid and the detail view.
- [ ] tab_bag.c (671): the two panels and the target picker.
- [ ] tab_map.c (1172): the region map is wider than 256 (pan or scale), the
      caption, FLY and ESCAPE.
- [ ] view_encounters.c (950): the WILD list.
- [ ] tab_dex.c (520), tab_trophy.c (555), ui_card.c (531).
- [ ] view_battle.c (931): the battle panel.
- [ ] Overlays: ui_quickball.c, ui_achtoast.c, the shiny notice in
      bottom_screen.c. Check that none covers the rail.
- [ ] ui_title.c: the title screen's bottom screen (drawn without the bar).

## Rules

- A primary button or rail slot stays at or above the 53px finger floor.
- Game side only (3ds/ui/, 3ds/build_objs.sh). No change in src/.
- Check each commit with the clang recipe at `CTR_UI_RAIL` 0 and 1, and
  compare the flag-0 preprocessed text with HEAD.
