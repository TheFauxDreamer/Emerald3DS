#!/usr/bin/env python3
"""Turn a 3DS crash screen into names.

Luma3DS catches aborts in this port and shows a register dump (libctru gives a
process no exception handler of its own, so there is no runtime code to add --
this is a tool, not a feature). The dump gives raw numbers. This resolves them:

    PC / LR  ->  function + offset,   against the link map
    FAR      ->  which struct FIELD,  because FAR *is* the field's offset

That second half is the one that matters. Every crash of this class so far has
been a NULL pointer plus a field offset, so the FAR names the field -- and
therefore the pointer -- outright. It is how both the naming-screen bug
(currentPage, 0x1E22) and the battle-teardown bug (battlerData, 0) were found.

CI publishes what the map half needs: the `emerald3ds-elf` artifact carries
3ds/emerald3ds.elf and 3ds/build/emerald3ds.map (3ds/Makefile:88).

Usage
-----
    tools/ctr_sym.py --map 3ds/build/emerald3ds.map 0x001A2BD0 0x0017A68C
    tools/ctr_sym.py --far 0x1E22 --struct NamingScreenData
    tools/ctr_sym.py --selftest

Needs only clang (macOS ships it). It compiles a types-only translation unit for
a 32-bit ARM target so pointer size and alignment match the console -- compiling
for the host would give 8-byte pointers and every offset would be wrong.
"""

import argparse
import bisect
import glob
import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Headers that between them define the game's base types (u8/u16/MainCallback/
# struct Sprite/...). A types-only TU includes these, then the struct under test.
PRELUDE = ['global.h', 'main.h', 'sprite.h', 'task.h', 'pokemon.h',
           'battle.h', 'window.h', 'bg.h', 'text.h']

# The game's include/ shadows libc, and an -ffreestanding ARM target has no
# sysroot, so these three stubs are the whole libc it needs. Generated at run
# time rather than committed, so they cannot go stale.
STUBS = {
    'stdint.h': """#ifndef _S_STDINT
#define _S_STDINT
typedef unsigned char uint8_t;   typedef signed char int8_t;
typedef unsigned short uint16_t; typedef short int16_t;
typedef unsigned int uint32_t;   typedef int int32_t;
typedef unsigned long long uint64_t; typedef long long int64_t;
typedef unsigned int uintptr_t;  typedef int intptr_t;
#endif
""",
    'stddef.h': """#ifndef _S_STDDEF
#define _S_STDDEF
#define NULL ((void *)0)
typedef unsigned int size_t;
typedef int ptrdiff_t;
#endif
""",
    'string.h': """#ifndef _S_STRING
#define _S_STRING
typedef unsigned int size_t;
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
#endif
""",
    'limits.h': "#define CHAR_BIT 8\n#define INT_MAX 2147483647\n"
                "#define UINT_MAX 4294967295u\n#define SHRT_MAX 32767\n",
    'stdarg.h': "", 'stdio.h': "", 'stdlib.h': "",
}

TYPE_BLOCK = re.compile(
    r'^(?:typedef\s+)?(?:struct|union|enum)\b[^;{]*\{.*?\n\}\s*[\w \t\*]*;',
    re.S | re.M)


# ----------------------------------------------------------------- map ------

def load_map(path):
    """(address, symbol) pairs from a GNU ld -Map file, sorted by address."""
    syms = []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.match(r'\s+0x([0-9a-fA-F]+)\s+([A-Za-z_][\w.$]*)\s*$', line)
        if m:
            syms.append((int(m.group(1), 16), m.group(2)))
    syms.sort()
    return syms


def resolve(syms, addr):
    """Nearest preceding symbol, the way addr2line would."""
    i = bisect.bisect_right(syms, (addr, '￿')) - 1
    if i < 0:
        return None
    base, name = syms[i]
    return name, addr - base


# --------------------------------------------------------------- struct -----

def find_struct_file(name):
    """Where `struct <name> { ... }` is defined. Structs local to a .c are the
    interesting case -- NamingScreenData lives in src/naming_screen.c."""
    pat = re.compile(r'\bstruct\s+' + re.escape(name) + r'\s*\{', re.S)
    for pattern in ('include/**/*.h', 'src/**/*.c'):
        for path in sorted(glob.glob(os.path.join(REPO, pattern), recursive=True)):
            if pat.search(open(path, encoding='utf-8', errors='replace').read()):
                return path
    return None


def struct_fields(text, name):
    m = re.search(r'struct\s+' + re.escape(name) + r'\s*\{(.*?)\n\};', text, re.S)
    if not m:
        return []
    fields = []
    for line in m.group(1).split('\n'):
        line = re.sub(r'//.*', '', line).strip()
        if not line or line.startswith('#'):
            continue
        f = re.match(r'.*?\b(\w+)\s*(?:\[[^\]]*\])*\s*(?::\s*\d+\s*)?;$', line)
        if f:
            fields.append(f.group(1))
    return fields


def probe(name, path, fields, far, op):
    """Compile a types-only TU asserting each field is `op` far.

    Fields that violate the assertion are named in the compiler's own error
    text, so one compile identifies them -- far cheaper than bisecting each
    field's offset separately.
    """
    src = open(path, encoding='utf-8', errors='replace').read() if path else ''
    blocks = TYPE_BLOCK.findall(src) if path and path.endswith('.c') else []
    body = '\n'.join('#include "%s"' % h for h in PRELUDE) + '\n\n'
    body += '\n\n'.join(blocks) + '\n\n'
    body += '\n'.join(
        '_Static_assert(__builtin_offsetof(struct %s, %s) %s %#x, "FIELD=%s");'
        % (name, f, op, far, f) for f in fields)

    with tempfile.TemporaryDirectory() as td:
        inc = os.path.join(td, 'stubs')
        os.makedirs(inc)
        for fn, text in STUBS.items():
            open(os.path.join(inc, fn), 'w').write(text)
        cfile = os.path.join(td, 'probe.c')
        open(cfile, 'w').write(body)
        out = subprocess.run(
            ['clang', '-target', 'arm-none-eabi', '-ffreestanding', '-nostdinc',
             '-isystem', inc, '-iquote', os.path.join(REPO, 'include'),
             '-DMODERN=1', '-DRP2350=1', '-DPLATFORM_3DS=1',
             '-fsyntax-only', cfile],
            capture_output=True, text=True, cwd=REPO).stderr
    # Preserve declaration order; clang repeats each failure a few times.
    hit = {m.group(1) for m in re.finditer(r'FIELD=(\w+)', out)}
    unrelated = [l for l in out.split('\n')
                 if ' error: ' in l and 'static assertion' not in l]
    return [f for f in fields if f in hit], unrelated


def explain_far(far, name, path=None):
    path = path or find_struct_file(name)
    if not path:
        print(f'  could not find a definition of struct {name}')
        return
    rel = os.path.relpath(path, REPO)
    fields = struct_fields(open(path, encoding='utf-8', errors='replace').read(), name)
    if not fields:
        print(f'  struct {name} found in {rel} but no fields parsed')
        return

    exact, errs = probe(name, path, fields, far, '!=')
    if errs:
        print('  probe did not compile cleanly:')
        for e in errs[:3]:
            print('   ', e.strip())
        return
    if exact:
        print(f'  FAR {far:#x} is struct {name}.{exact[0]}   ({rel})')
        print(f'  => a NULL {name} pointer dereferenced at .{exact[0]}')
        return
    # Not a field start: report the field that contains it.
    inside, _ = probe(name, path, fields, far, '>')
    if inside:
        print(f'  FAR {far:#x} falls inside struct {name}.{inside[-1]}   ({rel})')
    else:
        print(f'  FAR {far:#x} is before the first field of struct {name}')


# ----------------------------------------------------------------- main -----

def selftest():
    """Both crashes solved this session, with the answers already established."""
    cases = [(0x1E22, 'NamingScreenData', 'currentPage'),
             (0x0,    'BattleSpriteData', 'battlerData')]
    ok = True
    for far, sname, expect in cases:
        path = find_struct_file(sname)
        fields = struct_fields(open(path, encoding='utf-8', errors='replace').read(), sname) if path else []
        got, errs = probe(sname, path, fields, far, '!=') if fields else ([], ['no fields'])
        good = got[:1] == [expect]
        ok &= good
        print(f"  {'PASS' if good else 'FAIL'}  FAR {far:#06x} in {sname:18s} "
              f"-> {got[0] if got else '(nothing)'}   expected {expect}")
        if errs:
            print('        probe errors:', errs[0].strip()[:100])
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--map', help='GNU ld .map from the emerald3ds-elf artifact')
    ap.add_argument('--far', help='faulting address from the crash screen')
    ap.add_argument('--struct', dest='sname', help='struct to resolve --far against')
    ap.add_argument('--file', help='file defining that struct (else searched for)')
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('addrs', nargs='*', help='PC / LR to resolve against --map')
    a = ap.parse_args()

    if a.selftest:
        return selftest()

    if a.map:
        syms = load_map(a.map)
        print(f'{len(syms)} symbols from {a.map}')
        for raw in a.addrs:
            addr = int(raw, 16)
            r = resolve(syms, addr)
            print(f'  {addr:#010x}  ' +
                  (f'{r[0]} + {r[1]:#x}' if r else '(before the first symbol)'))

    if a.far is not None:
        far = int(a.far, 16)
        if not a.sname:
            print('  --far needs --struct NAME (the pointer you suspect was NULL)')
            return 2
        explain_far(far, a.sname, a.file)

    if not a.map and a.far is None:
        ap.print_help()
    return 0


if __name__ == '__main__':
    sys.exit(main())
