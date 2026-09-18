#!/usr/bin/env python3
"""Find pointers that are freed while a sprite or task callback can read them.

Why this matters only on hardware that is not a GBA
---------------------------------------------------
Code frees a file-scope pointer and sets it to NULL (FREE_AND_SET_NULL), while
code that reads through it is still scheduled. That code is a sprite callback
that AnimateSprites() runs, or a task that RunTasks() runs.

On a GBA, this has no effect. There is no MMU, and address 0 is the BIOS. The
junk from the read goes to graphics that are cleared soon. On the ARM11,
address 0 is not mapped, so the same read is a fatal data abort. The FAR is
then the offset of the field.

Note this difference. The `gHeap` array (src/malloc.c) is static, in the static
gGbaMem, so freed memory stays mapped. Thus a dangling pointer cannot fault. It
only draws junk. Only NULL faults. Thus FREE_AND_SET_NULL is the dangerous
call, and a plain Free() is safe. This script looks for the dangerous call.

What this script cannot decide
------------------------------
The question that decides a site is:

    After the free, does AnimateSprites() run before some code calls
    ResetSpriteData()?

The usual vanilla pattern is "free, then go to a setup CB2 that resets the
sprites first", which is safe. Only a return to a main loop that already runs
is dangerous. For example, naming_screen.c returned directly into
BattleMainCB2. A person must read the callback chain to decide, so each raw hit
below is a candidate, not a bug.

Thus this is a regression gate, not a bug list. The REVIEWED table below holds
the sites that a person checked. The script fails only on a site that nobody
checked yet.

Two limits to know before reading the output:

- The "was it reset first" test reads only the body of the function that frees.
  It does not follow into callers. Roulette shows the cost: Task_ExitRoulette
  calls ResetSpriteData() and then FreeRoulette(), so the site is safe, but the
  free is inside FreeRoulette() where no reset is visible. Expect that shape as
  a false positive, and check the caller before believing a finding.
- reaches() fans out through the call graph, so a large scene reports most of
  its callbacks as live readers. The list says "these could read it", not
  "these do read it after the free".

    python3 tools/audit_dangling.py [--all] [src/foo.c ...]
"""

import glob
import re
import sys

FUNC_HDR = re.compile(r'^(?:static\s+)?[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\([^;]*\)\s*$')

# A file-scope pointer definition, in any of the spellings this tree uses:
#
#     EWRAM_DATA static struct NamingScreenData *sNamingScreen = NULL;
#     static EWRAM_DATA struct PokemonStorageSystemData *sStorage = NULL;
#     COMMON_DATA u16 *gOverworldTilemapBuffer_Bg1 = NULL;
#     } *sRoulette = NULL;                 // an anonymous struct
#     } static EWRAM_DATA *sPokedexAreaScreen = NULL;
#     } *sMoveRelearnerStruct = {0};
#
# The rule is "a line that starts at column 0, thus is not a statement, and
# declares *NAME". The `(` case is excluded so that a function definition such
# as `const struct MapConnection *GetMapConnection(u8 dir)` does not match.
#
# The earlier version of this script allowed only `static EWRAM_DATA struct X
# *p`. That is the one order this tree does NOT use, so it skipped 24 of the 47
# files that free a pointer, and every crash this port has found was in a
# skipped file. Measure the corpus, not just the findings: main() prints it.
# The second alternative is the pointer-to-array form, which wraps the name
# in parentheses: `COMMON_DATA u16 (*gContestMonPixels)[][32] = {0};`.
# It is kept separate rather than allowing a bare `)` after the name,
# because that would also match a parameter in a function definition.
PTR_DEF = r'^(?:\}|[A-Za-z_])[^;=\n]*(?:\*\s*%s\s*(?:=|;|\[)|\(\s*\*\s*%s\s*\))'

# Sites that a person read and cleared, with the key (file, pointer, freeing
# function). The first entries come from the sweep of 2026-09. Each entry tells
# why it is not a bug.
REVIEWED = {
    ('src/battle_dome.c', 'sInfoCard', 'Task_HandleInfoCardInput'):
        'STATE_CLOSE_CARD destroys every sInfoCard->spriteIds[] first',
    ('src/battle_factory_screen.c', 'sFactorySelectScreen', 'Select_Task_Exit'):
        'hands off to CB2_ReturnToFieldContinueScript, a setup path',
    ('src/battle_factory_screen.c', 'sFactorySwapScreen', 'Swap_Task_Exit'):
        'hands off to CB2_ReturnToFieldContinueScript, a setup path',
    ('src/berry_crush.c', 'sGame', 'QuitBerryCrush'):
        'MainTask is the freer itself; link-only path',
    ('src/contest_util.c', 'sContestResults', 'FreeContestResults'):
        'caller sets CB2_ReturnToFieldContinueScriptPlayMapMusic, a setup path',
    ('src/credits.c', 'sCreditsData', 'Task_UpdatePage'):
        'FIXED: destroys Task_ShowMons, the only live reader, before the free',
    ('src/easy_chat.c', 'sEasyChatScreen', 'FreeEasyChatScreenStruct'):
        'Task_InitEasyChatScreen is an init task, gone before the free',
    ('src/easy_chat.c', 'sScreenControl', 'FreeEasyChatScreenControl'):
        'as above',
    ('src/easy_chat.c', 'sWordData', 'FreeEasyChatScreenWordData'):
        'as above',
    ('src/evolution_scene.c', 'sEvoStructPtr', 'Task_EvolutionScene'):
        'the other readers are mutually exclusive task variants',
    ('src/evolution_scene.c', 'sEvoStructPtr', 'Task_TradeEvolutionScene'):
        'as above',
    ('src/frontier_pass.c', 'sPassData', 'FreeFrontierPassData'):
        'hands off to CB2_FrontierPass, a setup path',
    ('src/frontier_pass.c', 'sPassGfx', 'FreeFrontierPassGfx'):
        'as above',
    ('src/hall_of_fame.c', 'sHofMonPtr', 'Task_Hof_TrySaveData'):
        'Task_Hof_InitMonData is an init task in the same slot, not concurrent',
    ('src/hall_of_fame.c', 'sHofMonPtr', 'Task_Hof_HandleExit'): 'as above',
    ('src/hall_of_fame.c', 'sHofMonPtr', 'Task_HofPC_HandleExit'): 'as above',
    ('src/pokedex_cry_screen.c', 'sCryMeterNeedle', 'FreeCryScreen'):
        'destroys the needle sprite, the only reader, before the free',
    ('src/pokedex_cry_screen.c', 'sDexCryScreen', 'FreeCryScreen'): 'as above',
    ('src/pokenav.c', 'gPokenavResources', 'FreePokenavResources'):
        'hands off to CB2_ReturnToField*, a setup path',
    ('src/slot_machine.c', 'sSlotMachine', 'SlotTask_FreeDataStructures'):
        'returns to prevMainCb, which is a CB2_ReturnToField* setup path',
    ('src/union_room_chat.c', 'sDisplay', 'FreeDisplay'):
        'link-only; Task_ReceiveChatMessage is destroyed on the same path',
    ('src/use_pokeblock.c', 'sMenu', 'FeedPokeblockToMon'):
        'PreparePokeblockFeedScene() reaches ResetSpriteData() in the same frame',
    ('src/use_pokeblock.c', 'sMenu', 'CloseUsePokeblockMenu'):
        'destroys all three reader sprites before the free',

    # --- sweep of 2026-09-18, after the corpus fix made these visible at all.
    # Every one of them was invisible to this script before that.
    ('src/battle_anim_utility_funcs.c', 'sAnimStatsChangeData', 'StatsChangeAnimation_Step3'):
        'destroys its own sprites before the free; the anim task ends with it',
    ('src/battle_factory_screen.c', 'sFactorySelectMons', 'CB2_InitSelectScreen'):
        'a setup callback: the scratch buffer goes as setup ends, then it hands '
        'off to CB2_SelectScreen',
    ('src/berry_blender.c', 'sBerryBlender', 'CB2_CheckPlayAgainLink'):
        'FIXED 2026-09-18: the callback returns after the free, before '
        'AnimateSprites(). SpriteCB_PlayerArrow reads bg_X with no test',
    ('src/berry_blender.c', 'sBerryBlender', 'CB2_CheckPlayAgainLocal'):
        'FIXED 2026-09-18: as above, and this is the path a solo blend takes',
    ('src/contest.c', 'gContestResources', 'FreeContestResources'):
        'CB2_ContestMain calls AnimateSprites() BEFORE RunTasks(), so the '
        'sprites of that frame ran before the free, and its tail reads nothing '
        'through the pointer',
    ('src/dodrio_berry_picking.c', 'sStatusBar', 'FreeStatusBar'):
        'destroys its sprites before the free; link-only',
    ('src/mirage_tower.c', 'sBgShakeOffsets', 'DoMirageTowerDisintegration'):
        'destroys the reading task before the free',
    ('src/mirage_tower.c', 'sFallingFossil', 'Task_FossilFallAndSink'):
        'destroys its sprite before the free',
    ('src/mirage_tower.c', 'sMirageTowerPulseBlend', 'ClearMirageTowerPulseBlendEffect'):
        'destroys the pulse-blend task before the free',
    ('src/pokedex_area_screen.c', 'sPokedexAreaScreen', 'Task_HandlePokedexAreaScreenInput'):
        'destroys the reading task before the free',
    ('src/pokemon_storage_system.c', 'sStorage', 'FreePokeStorageData'):
        'CB2_PokeStorage returns on NULL, and the guard sits BEFORE '
        'AnimateSprites() exactly because SpriteCB_HeldMon reads sStorage (ff770eb)',
    ('src/region_map.c', 'sFlyMap', 'CB_ExitFlyMap'):
        'hands off to CB2_ReturnToPartyMenuFromFlyMap, a setup path',
    ('src/roulette.c', 'sRoulette', 'FreeRoulette'):
        'Task_ExitRoulette calls ResetSpriteData() before FreeRoulette(), which '
        'this script cannot see because it reads only the freeing function. The '
        'CB2 tail is guarded separately',
    ('src/trade.c', 'sTradeAnim', 'DoTradeAnim_Cable'):
        'FIXED 2026-09-18: CB2_InGameTrade returns after DoTradeAnim() frees, '
        'before RunTasks() and AnimateSprites()',
    ('src/trade.c', 'sTradeAnim', 'DoTradeAnim_Wireless'):
        'FIXED 2026-09-18: covered by the same two callback guards; link-only',
    ('src/trade.c', 'sTradeAnim', 'CB2_FreeTradeAnim'):
        'FIXED 2026-09-18: the callback returns after the free, before RunTasks()',
    ('src/trainer_card.c', 'sData', 'CloseTrainerCard'):
        'SetMainCallback2(sData->callback2) reads the pointer BEFORE the free, '
        'and the task then destroys itself',
}


def parse_functions(path):
    lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
    funcs, i = {}, 0
    while i < len(lines):
        m = FUNC_HDR.match(lines[i])
        if m and i + 1 < len(lines) and lines[i + 1].strip() == '{':
            name, j, depth, body = m.group(1), i + 2, 1, []
            while j < len(lines) and depth > 0:
                depth += lines[j].count('{') - lines[j].count('}')
                if depth > 0:
                    body.append(lines[j])
                j += 1
            funcs[name] = ('\n'.join(body), i + 1)
            i = j
        else:
            i += 1
    return funcs


def table_members(text, funcs):
    """Names listed in an array initializer. This is necessary: the
    naming-screen crash came through sPageSwapSpriteFuncs[], which a grep and a
    call-only graph both miss."""
    out = set()
    for m in re.finditer(r'=\s*\{([^;]*?)\}\s*;', text, re.S):
        out |= {n for n in re.findall(r'\b([A-Za-z_]\w*)\b', m.group(1)) if n in funcs}
    return out


def reaches(funcs, ptr, tbl):
    reach = {f for f, (b, _) in funcs.items()
             if re.search(r'\b' + re.escape(ptr) + r'\s*(->|\[)', b)}
    changed = True
    while changed:
        changed = False
        for f, (b, _) in funcs.items():
            if f in reach:
                continue
            for g in reach:
                if re.search(r'\b' + re.escape(g) + r'\s*\(', b) or \
                   (g in tbl and re.search(r'\b' + re.escape(g) + r'\b', b)):
                    reach.add(f)
                    changed = True
                    break
    return reach


def signatures(text):
    """A sprite callback takes `struct Sprite *`, and a task takes `u8 taskId`.
    Without this test, the usual struct field `callback` in mail.c looks like a
    sprite callback."""
    sig = {}
    for m in re.finditer(r'\b(\w+)\s*(\([^;{)]*\))\s*[;{]', text):
        params = m.group(2)
        if re.search(r'\(\s*struct\s+Sprite\s*\*', params):
            sig[m.group(1)] = 'sprite'
        elif re.search(r'\(\s*u8\s+\w*[Tt]ask\w*\s*\)|\(\s*u8\s+taskId\s*\)', params):
            sig[m.group(1)] = 'task'
    return sig


def pointer_defs(paths):
    """Map a pointer name to the files that define it, across the whole tree."""
    defs = {}
    for path in paths:
        text = open(path, encoding='utf-8', errors='replace').read()
        cands = set(re.findall(r'\*\s*(\w+)\s*(?:=|;|\[)', text))
        cands |= set(re.findall(r'\(\s*\*\s*(\w+)\s*\)', text))
        for name in cands:
            esc = re.escape(name)
            if re.search(PTR_DEF % (esc, esc), text, re.M):
                defs.setdefault(name, set()).add(path)
    return defs


def entry_points(text, funcs, tbl, sig):
    """The scheduled readers: sprite callbacks that AnimateSprites() runs, and
    tasks that RunTasks() runs.

    Three registration forms matter, and the earlier version of this script
    modelled only the first two:

      sprite->callback = Fn      an assignment, or a .callback field in a table
      CreateTask(Fn, ...)        535 sites in src/
      gTasks[id].func = Fn       790 sites in src/, so the MAJORITY form

    Task_ExitRoulette, which frees sRoulette, is installed only by the third
    form. Missing it is why this script reported nothing for roulette.c.

    A name that merely sits in a function-pointer table is an entry too, if its
    signature fits: the naming-screen crash came through sPageSwapSpriteFuncs[].
    """
    sprites = {m.group(1) for m in re.finditer(r'(?:->|\.)\s*callback\s*=\s*&?(\w+)', text)}
    tasks = {m.group(1) for m in re.finditer(r'\bCreateTask\s*\(\s*&?(\w+)', text)}
    tasks |= {m.group(1) for m in
              re.finditer(r'\bgTasks\s*\[[^\]]*\]\s*\.\s*func\s*=\s*&?(\w+)', text)}
    tasks |= {m.group(1) for m in
              re.finditer(r'\b(?:SetTaskFuncWithFollowupFunc|SwitchTaskToFollowupFunc)'
                          r'\s*\([^;)]*?&?(\w+)\s*[,)]', text)}
    sprites |= {n for n in tbl if sig.get(n) == 'sprite'}
    tasks |= {n for n in tbl if sig.get(n) == 'task'}
    sprites = {n for n in sprites if n in funcs and sig.get(n) == 'sprite'}
    tasks = {n for n in tasks if n in funcs and sig.get(n) == 'task'}
    return sprites, tasks


def audit(paths, defs=None):
    if defs is None:
        defs = pointer_defs(paths)
    out = []
    for path in paths:
        text = open(path, encoding='utf-8', errors='replace').read()
        freed = {p for p in re.findall(r'(?:TRY_)?FREE_AND_SET_NULL\(\s*(\w+)\s*\)', text)
                 if p in defs}
        if not freed:
            continue
        funcs = parse_functions(path)
        tbl = table_members(text, funcs)
        sig = signatures(text)
        sprites, tasks = entry_points(text, funcs, tbl, sig)
        for ptr in sorted(freed):
            # A pointer defined in another file is usually read there too, so
            # fold that file in for this pointer only. Without it a global that
            # is freed away from its definition is never analysed, and
            # gBattleSpritesDataPtr -- the first crash this port fixed -- is
            # exactly that: defined in battle_main.c, freed in
            # battle_gfx_sfx_util.c.
            pfuncs, ptbl, psp, ptk = funcs, tbl, sprites, tasks
            for other in sorted(defs.get(ptr, ())):
                if other == path:
                    continue
                otext = open(other, encoding='utf-8', errors='replace').read()
                ofuncs = parse_functions(other)
                otbl = table_members(otext, ofuncs)
                osp, otk = entry_points(otext, ofuncs, otbl, signatures(otext))
                pfuncs = {**ofuncs, **pfuncs}
                ptbl = ptbl | otbl
                psp = psp | osp
                ptk = ptk | otk
            reach = reaches(pfuncs, ptr, ptbl)
            sp = sorted(e for e in psp if e in reach)
            tk = sorted(e for e in ptk if e in reach)
            if not sp and not tk:
                continue
            for f, (body, line) in pfuncs.items():
                if not re.search(r'(?:TRY_)?FREE_AND_SET_NULL\(\s*' + re.escape(ptr) + r'\s*\)', body):
                    continue
                # Judge EVERY free in the body, not only the first. Before, a
                # function that frees at two points was assessed on the first
                # alone, and CB2_CheckPlayAgainLink in berry_blender.c does
                # exactly that.
                chunks = re.split(r'(?:TRY_)?FREE_AND_SET_NULL\(\s*' + re.escape(ptr) + r'\s*\)', body)
                left_sp, left_tk, pre = [], [], ''
                for chunk in chunks[:-1]:
                    pre += chunk          # everything before THIS free
                    if 'ResetSpriteData(' not in pre:
                        left_sp = [e for e in sp if e != f]
                    if 'ResetTasks(' not in pre:
                        left_tk = [e for e in tk if e != f]
                    if left_sp or left_tk:
                        break
                if not left_sp and not left_tk:
                    continue
                # The discriminator that a person needs: where does control go
                # next?
                handoff = re.findall(r'SetMainCallback2\(\s*([\w>.\-]+)\s*\)', body)
                out.append(dict(path=path, ptr=ptr, freer=f, line=line,
                                sprites=left_sp, tasks=left_tk,
                                handoff=handoff))
    return out


def corpus(paths, defs):
    """How many files and pointers this script can actually see.

    Print it every run. The script reported "no unreviewed sites" for a year
    while it could see only 23 of 47 files, and a clean report from a tool that
    reads half the tree is worse than no tool, because it is believed.
    """
    files = ptrs = 0
    for path in paths:
        text = open(path, encoding='utf-8', errors='replace').read()
        freed = set(re.findall(r'(?:TRY_)?FREE_AND_SET_NULL\(\s*(\w+)\s*\)', text))
        if not freed:
            continue
        files += 1
        ptrs += len(freed & set(defs))
    total_files = total_ptrs = 0
    for path in paths:
        text = open(path, encoding='utf-8', errors='replace').read()
        freed = set(re.findall(r'(?:TRY_)?FREE_AND_SET_NULL\(\s*(\w+)\s*\)', text))
        if freed:
            total_files += 1
            total_ptrs += len(freed)
    return files, total_files, ptrs, total_ptrs


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('-')]
    show_all = '--all' in sys.argv
    tree = sorted(glob.glob('src/**/*.c', recursive=True))
    paths = args or tree
    # Always index the whole tree, even when the caller names one file: a
    # pointer can be defined in a file other than the one that frees it.
    defs = pointer_defs(tree + sorted(glob.glob('include/**/*.h', recursive=True)))
    seen_f, all_f, seen_p, all_p = corpus(tree, defs)
    print(f'corpus: {seen_f}/{all_f} files, {seen_p}/{all_p} freed pointers visible')
    if seen_f < all_f:
        print('  WARNING: some files are invisible to this script. Fix PTR_DEF.')
    print()
    findings = audit(paths, defs)

    new = [f for f in findings if (f['path'], f['ptr'], f['freer']) not in REVIEWED]
    known = [f for f in findings if (f['path'], f['ptr'], f['freer']) in REVIEWED]

    if new:
        print(f'=== {len(new)} UNREVIEWED site(s) ===\n')
        for f in new:
            print(f"  {f['path']}:{f['line']}  {f['ptr']}  freed in {f['freer']}()")
            if f['sprites']:
                print(f"      live sprite callbacks: {', '.join(f['sprites'])}")
            if f['tasks']:
                print(f"      live tasks:            {', '.join(f['tasks'])}")
            print(f"      hands off to:          {', '.join(f['handoff']) or '(no SetMainCallback2 here)'}")
            print('      -> Does an AnimateSprites() run before the next ResetSpriteData()?')
            print('         A *setup* CB2 resets first and is safe; an already-running')
            print('         main loop animates first and is a real bug.\n')
    else:
        print('No unreviewed sites.\n')

    if show_all and known:
        print(f'=== {len(known)} reviewed and cleared ===')
        for f in known:
            print(f"  {f['path']}:{f['line']} {f['ptr']} in {f['freer']}()")
            print(f"      {REVIEWED[(f['path'], f['ptr'], f['freer'])]}")

    live = {(f['path'], f['ptr'], f['freer']) for f in findings}
    stale = [k for k in REVIEWED if k not in live]
    if stale and not args:
        print(f'note: {len(stale)} REVIEWED row(s) match no current site. The shape is')
        print('      gone, so the row is dead weight and would silently clear the')
        print('      shape if it came back. Delete them:')
        for k in stale:
            print(f'        {k}')
        print()

    print(f'{len(new)} unreviewed, {len(known)} reviewed '
          f'(re-run with --all to list the reviewed ones)')
    return 1 if new else 0


if __name__ == '__main__':
    sys.exit(main())
