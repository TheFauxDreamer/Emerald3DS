#!/usr/bin/env python3
"""Checks that 3ds/ACHIEVEMENTS.md still lists exactly what 3ds/achievements.c
defines. That is every achievement, in display order, on the correct page and
under the correct category. The id, title and description must be the same,
word for word. It also checks what C cannot check at compile time: the ids are
unique and fit the store.

Run from any directory: python3 3ds/check_achievements_md.py
Exits 1 with a diff when the two do not agree. It uses only the Python 3
standard library.
"""

import difflib
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
C_FILE = HERE / "achievements.c"
MD_FILE = HERE / "ACHIEVEMENTS.md"
BRIDGE = HERE / "bridge.h"

SECTIONS = {"ACH_SECTION_MAIN": "Main", "ACH_SECTION_POSTGAME": "Post-game"}
CATEGORIES = {
    "ACH_CAT_STORY": "Story",
    "ACH_CAT_LEGEND": "Legendary",
    "ACH_CAT_POKEMON": "Pokémon",
    "ACH_CAT_BATTLE": "Battle",
    "ACH_CAT_EXTRA": "Extras",
    "ACH_CAT_CONTEST": "Contests",
}


def fail(msg):
    print(f"check_achievements_md: {msg}", file=sys.stderr)
    sys.exit(1)


def c_string(literal):
    """The contents of a C string literal, with octal escapes read as UTF-8."""
    raw = literal[1:-1].encode("latin-1").decode("unicode_escape")
    return raw.encode("latin-1").decode("utf-8")


def read_string_expr(text, pos, macros):
    """Adjacent literals and string macros ("Get a " POKEDEX) from pos, up to
    the next comma or closing parenthesis. Returns (value, pos after it)."""
    out = []
    token = re.compile(r'\s*(?:("(?:[^"\\]|\\.)*")|([A-Za-z_]\w*))')
    while True:
        m = token.match(text, pos)
        if not m:
            break
        if m.group(1):
            out.append(c_string(m.group(1)))
        elif m.group(2) in macros:
            out.append(macros[m.group(2)])
        else:
            break
        pos = m.end()
    if not out:
        fail(f"expected a string at: {text[pos:pos + 40]!r}")
    return "".join(out), pos


def rows_from_c():
    src = C_FILE.read_text(encoding="utf-8")
    code = re.sub(r"//[^\n]*", "", src)

    macros = {
        m.group(1): c_string(m.group(2))
        for m in re.finditer(r'^#define\s+(\w+)\s+("(?:[^"\\]|\\.)*")\s*$', code, re.M)
    }

    tables = {}
    for m in re.finditer(r"static const struct AchDef (\w+)\[\] =\s*\{(.*?)^\};", code, re.S | re.M):
        body, rows = m.group(2), []
        for row in re.finditer(r"\b[A-Z_]+\(\s*(\d+)\s*,", body):
            title, pos = read_string_expr(body, row.end(), macros)
            if not body[pos:].lstrip().startswith(","):
                fail(f"id {row.group(1)}: expected a comma after the title")
            desc, _ = read_string_expr(body, body.index(",", pos) + 1, macros)
            rows.append((int(row.group(1)), title, desc))
        tables[m.group(1)] = rows

    groups = re.findall(r"GROUP\((ACH_SECTION_\w+),\s*(ACH_CAT_\w+),\s*(\w+)\)", code)
    if not groups:
        fail("found no GROUP() rows in achievements.c")

    out = []
    for section, category, table in groups:
        if table not in tables:
            fail(f"GROUP names {table}, which is not an AchDef table")
        for ach_id, title, desc in tables[table]:
            out.append((SECTIONS[section], CATEGORIES[category], ach_id, title, desc))
    return out


def rows_from_md():
    page = category = None
    out, stated = [], {}
    for n, line in enumerate(MD_FILE.read_text(encoding="utf-8").splitlines(), 1):
        if line.startswith("## "):
            page = next((p for p in SECTIONS.values() if line[3:].startswith(p)), None)
            category = None
        elif line.startswith("### "):
            category = next((c for c in CATEGORIES.values() if line[4:].startswith(c)), None)
        elif re.match(r"\|\s*\d+\s*\|", line):
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if page is None or category is None:
                fail(f"ACHIEVEMENTS.md:{n}: a row outside a page and category heading")
            out.append((page, category, int(cells[0]), cells[1], cells[2]))

        m = re.search(r"\*\*(\d+) achievements\*\*: (\d+) main, (\d+) post-game", line)
        if m:
            stated["totals"] = tuple(int(x) for x in m.groups())
        m = re.search(r"next unused id is \*\*(\d+)\*\*", line)
        if m:
            stated["next"] = int(m.group(1))
    return out, stated


def main():
    c_rows = rows_from_c()
    md_rows, stated = rows_from_md()
    problems = []

    limit = int(re.search(r"#define CTR_ACH_BYTES\s+(\d+)", BRIDGE.read_text()).group(1)) * 8
    ids = [r[2] for r in c_rows]
    for dup in sorted({i for i in ids if ids.count(i) > 1}):
        problems.append(f"id {dup} is used more than once in achievements.c")
    for big in sorted(i for i in ids if i >= limit):
        problems.append(f"id {big} does not fit the store (ids run 0 to {limit - 1})")

    main_count = sum(1 for r in c_rows if r[0] == "Main")
    totals = (len(c_rows), main_count, len(c_rows) - main_count)
    if stated.get("totals") != totals:
        problems.append(f"ACHIEVEMENTS.md should say **{totals[0]} achievements**: "
                        f"{totals[1]} main, {totals[2]} post-game")
    if "next" not in stated or stated["next"] <= max(ids):
        problems.append(f"ACHIEVEMENTS.md's next unused id must be above {max(ids)}, the highest in use")

    if md_rows != c_rows:
        render = lambda rows: [" | ".join(str(x) for x in r) for r in rows]
        diff = difflib.unified_diff(render(md_rows), render(c_rows),
                                    "3ds/ACHIEVEMENTS.md", "3ds/achievements.c", lineterm="", n=1)
        problems.append("the list differs from the tables (page | category | id | title | description):\n"
                        + "\n".join(diff))

    if problems:
        fail("\n".join(problems))
    print(f"ACHIEVEMENTS.md matches achievements.c: {totals[0]} achievements, "
          f"{totals[1]} main, {totals[2]} post-game")


if __name__ == "__main__":
    main()
