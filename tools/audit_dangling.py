#!/usr/bin/env python3
"""Find pointers freed while a sprite or task callback can still read them.

Why this only matters off-GBA
-----------------------------
A file-scope pointer is freed and set to NULL (FREE_AND_SET_NULL) while code
that dereferences it is still scheduled -- a sprite callback run by
AnimateSprites(), or a task run by RunTasks().

On a GBA that is free: no MMU, address 0 is the BIOS, and the junk read back
feeds graphics that are about to be wiped. On the ARM11 address 0 is unmapped,
so the same read is a fatal data abort, FAR = the field's offset.

Note the asymmetry, which is the opposite of the usual intuition: `gHeap` is a
static array (src/malloc.c) inside the static gGbaMem, so freed memory stays
mapped forever. A *dangling* pointer therefore cannot fault -- it draws garbage.
Only NULL faults. So FREE_AND_SET_NULL is the dangerous call and a bare Free()
is the safe one, and that is what this script keys on.

Three real bugs were found this way: battle_main.c (battle teardown),
naming_screen.c (MainState_Exit) and credits.c (Task_UpdatePage case 10).

What this script cannot decide
------------------------------
The question that actually settles a site is:

    after the free, does an AnimateSprites() run before something calls
    ResetSpriteData()?

Vanilla's universal idiom is "free, then hand off to a *setup* CB2 that resets
sprites first", which is safe. Only a handoff to an ALREADY-RUNNING main loop is
dangerous -- naming_screen.c returned straight into BattleMainCB2, which is why
it alone crashed. Deciding that needs a human to read the callback chain, so
every raw hit below is a candidate, not a bug. The last sweep found ~12
candidates and all but three were false positives.

So this is a REGRESSION GATE, not a bug list: reviewed sites live in REVIEWED
below, and the script fails only on a site nobody has looked at yet.

    python3 tools/audit_dangling.py [--all] [src/foo.c ...]
"""

import glob
import re
import sys

FUNC_HDR = re.compile(r'^(?:static\s+)?[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\([^;]*\)\s*$')

# Sites already read and cleared, keyed (file, pointer, freeing function).
# Seeded from the sweep of 2026-09; each entry says why it is not a bug.
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
    ('src/cable_car.c', 'sCableCar', 'CB2_EndCableCar'):
        'ResetSpriteData() and ResetTasks() before the free',
    ('src/fldeff_cut.c', 'sCutGrassSpriteArrayPtr', 'CutGrassSpriteCallbackEnd'):
        'destroys its own sprites first',
    ('src/frontier_pass.c', 'sMapData', 'FreeFrontierMap'):
        'ResetTasks() before the free',
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
    """Names listed in an array initializer. Load-bearing: the naming-screen
    crash was reached through sPageSwapSpriteFuncs[], which a grep and a
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
    """A sprite callback takes `struct Sprite *`, a task takes `u8 taskId`.
    Without this, mail.c's ordinary struct field named `callback` reads as a
    sprite callback."""
    sig = {}
    for m in re.finditer(r'\b(\w+)\s*(\([^;{)]*\))\s*[;{]', text):
        params = m.group(2)
        if re.search(r'\(\s*struct\s+Sprite\s*\*', params):
            sig[m.group(1)] = 'sprite'
        elif re.search(r'\(\s*u8\s+\w*[Tt]ask\w*\s*\)|\(\s*u8\s+taskId\s*\)', params):
            sig[m.group(1)] = 'task'
    return sig


def audit(paths):
    out = []
    for path in paths:
        text = open(path, encoding='utf-8', errors='replace').read()
        freed = {p for p in re.findall(r'(?:TRY_)?FREE_AND_SET_NULL\(\s*(\w+)\s*\)', text)
                 if re.search(r'^\s*(?:static\s+)?(?:EWRAM_DATA\s+)?'
                              r'(?:struct\s+\w+|\w+)\s*\*\s*' + re.escape(p) + r'\b',
                              text, re.M)}
        if not freed:
            continue
        funcs = parse_functions(path)
        tbl = table_members(text, funcs)
        sig = signatures(text)
        sprites = {m.group(1) for m in re.finditer(r'(?:->|\.)\s*callback\s*=\s*&?(\w+)', text)
                   if m.group(1) in funcs and sig.get(m.group(1)) == 'sprite'}
        tasks = {m.group(1) for m in re.finditer(r'\bCreateTask\s*\(\s*&?(\w+)', text)
                 if m.group(1) in funcs and sig.get(m.group(1)) == 'task'}
        for ptr in sorted(freed):
            reach = reaches(funcs, ptr, tbl)
            sp = sorted(e for e in sprites if e in reach)
            tk = sorted(e for e in tasks if e in reach)
            if not sp and not tk:
                continue
            for f, (body, line) in funcs.items():
                if not re.search(r'(?:TRY_)?FREE_AND_SET_NULL\(\s*' + re.escape(ptr) + r'\s*\)', body):
                    continue
                pre = body.split('FREE_AND_SET_NULL(' + ptr)[0]
                left_sp = [] if 'ResetSpriteData(' in pre else [e for e in sp if e != f]
                left_tk = [] if 'ResetTasks(' in pre else [e for e in tk if e != f]
                if not left_sp and not left_tk:
                    continue
                # The discriminator a human needs: where does control go next?
                handoff = re.findall(r'SetMainCallback2\(\s*([\w>.\-]+)\s*\)', body)
                out.append(dict(path=path, ptr=ptr, freer=f, line=line,
                                sprites=left_sp, tasks=left_tk,
                                handoff=handoff))
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('-')]
    show_all = '--all' in sys.argv
    paths = args or sorted(glob.glob('src/**/*.c', recursive=True))
    findings = audit(paths)

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

    print(f'{len(new)} unreviewed, {len(known)} reviewed '
          f'(re-run with --all to list the reviewed ones)')
    return 1 if new else 0


if __name__ == '__main__':
    sys.exit(main())
