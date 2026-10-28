#!/usr/bin/env python3
"""One-shot migration: rewrite `l10n::Pick(L"cn", L"en")` calls in product code to
`l10n::Get(l10n::StringId::X).c_str()` and append the strings to catalog.tsv.

Only calls whose two arguments are string-literal sequences are rewritten; other
Pick uses stay (tests and dynamic tables keep the runtime helper). Cn(...)
composition helpers are left alone on purpose. Run tools/l10n/gen_strings.py
afterwards to regenerate the string outputs.

    python tools/l10n/migrate_pick.py [--dry-run]
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
from gen_strings import CATALOG, Row, decode_cpp, id_name_for, read_catalog, write_catalog  # noqa: E402

DRY = "--dry-run" in sys.argv
EXEMPT_PREFIX = (
    "src/bench/",
    "src/common/zh_hant.cpp",
    "src/common/localization.h",
    "src/common/localization.cpp",
    "src/app/selftest_1b2.cpp",
)
TOKEN = re.compile(r'//[^\n]*|/\*.*?\*/|(L?)"((?:[^"\\\n]|\\.)*)"|\'(?:[^\'\\\n]|\\.)*\'', re.S)
CALL = re.compile(r"\b((?:\w+::)*)Pick\s*\(")
LITERAL = re.compile(r'(L?)"((?:[^"\\\n]|\\.)*)"')


def skip_ws(text, i):
    while i < len(text) and text[i] in " \t\r\n":
        i += 1
    return i


def parse_literal_arg(text, i):
    """Parse a sequence of concatenated string literals; return (decoded, end) or None."""
    parts = []
    while True:
        i = skip_ws(text, i)
        m = LITERAL.match(text, i)
        if not m:
            break
        parts.append(decode_cpp(m.group(2)))
        i = m.end()
    if not parts:
        return None
    return "".join(parts), i


def parse_pick_call(text, at):
    """at points at '(' of Pick(. Return (zh, en, end) or None."""
    i = at + 1
    arg1 = parse_literal_arg(text, i)
    if not arg1:
        return None
    i = skip_ws(text, arg1[1])
    if i >= len(text) or text[i] != ",":
        return None
    arg2 = parse_literal_arg(text, i + 1)
    if not arg2:
        return None
    i = skip_ws(text, arg2[1])
    if i >= len(text) or text[i] != ")":
        return None
    return arg1[0], arg2[0], i + 1


def rc_escape_cpp(text: str) -> str:
    """C++-decoded literal text -> RC string-table form ("" quotes, \\n escapes)."""
    return (text.replace("\\", "\\\\").replace('"', '""')
                .replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t"))


def derive_name(en: str, used: set) -> str:
    words = re.findall(r"[A-Za-z0-9]+", en)
    if not words:
        return None
    name = "".join(w[:1].upper() + w[1:] for w in words)[:60]
    if not name[0].isalpha():
        name = "S" + name
    base, n = name, 2
    while name in used:
        name = f"{base}_{n}"
        n += 1
    return name


def main():
    rows = read_catalog()
    used_names = {r.name for r in rows}
    used_ids_names = {r.id_name for r in rows}
    next_id = max(r.id for r in rows) + 1
    by_text = {(r.zh, r.en): r for r in rows}
    new_rows = []
    rewritten = 0
    skipped = []
    changed_files = []

    for path in sorted((ROOT / "src").rglob("*")):
        rel = path.relative_to(ROOT).as_posix()
        if path.suffix not in (".cpp", ".h") or rel.startswith(EXEMPT_PREFIX):
            continue
        if path.name.endswith(("_test.cpp", "_test.h")):
            continue
        with open(path, encoding="utf-8", errors="replace", newline="") as fh:
            text = fh.read()
        if "Pick(" not in text:
            continue
        # Mask comments/strings so we only match real call sites.
        masked = []
        last = 0
        for m in TOKEN.finditer(text):
            masked.append((last, m.start(), False))
            masked.append((m.start(), m.end(), True))
            last = m.end()
        masked.append((last, len(text), False))
        edits = []
        for start, end, is_masked in masked:
            if is_masked:
                continue
            for call in CALL.finditer(text, start, end):
                parsed = parse_pick_call(text, call.end() - 1)
                if not parsed:
                    skipped.append((rel, text[call.start():call.start() + 60].replace("\n", " ")))
                    continue
                zh, en, end_pos = parsed
                key = (rc_escape_cpp(zh), rc_escape_cpp(en))
                row = by_text.get(key)
                if row is None:
                    name = derive_name(en, used_names) or f"Text{next_id}"
                    used_names.add(name)
                    id_name = id_name_for(name)
                    base, n = id_name, 2
                    while id_name in used_ids_names:
                        id_name = f"{base}_{n}"
                        n += 1
                    used_ids_names.add(id_name)
                    row = Row(name, id_name, next_id, key[0], key[1], "")
                    next_id += 1
                    by_text[key] = row
                    new_rows.append(row)
                qual = call.group(1) or ""
                repl = f"{qual}Get({qual}StringId::{row.name}).c_str()"
                edits.append((call.start(), end_pos, repl))
        if edits:
            for start, end, repl in reversed(edits):
                text = text[:start] + repl + text[end:]
            if not DRY:
                with open(path, "w", encoding="utf-8", newline="") as fh:
                    fh.write(text)
            rewritten += len(edits)
            changed_files.append((rel, len(edits)))

    print(f"rewritten Pick calls: {rewritten} in {len(changed_files)} files")
    for rel, n in changed_files:
        print(f"   {rel}: {n}")
    print(f"new catalog rows: {len(new_rows)}")
    for row in new_rows:
        print(f"   {row.name} = {row.id_name} {row.id} zh={row.zh[:30]!r} en={row.en[:30]!r}")
    backslash = [r for r in new_rows if "\\" in r.zh or "\\" in r.en]
    if backslash:
        print(f"note: {len(backslash)} new rows contain backslash escapes")
    if skipped:
        print(f"skipped non-literal Pick calls: {len(skipped)}")
        for rel, sample in skipped[:15]:
            print(f"   {rel}: {sample}")
    if new_rows and not DRY:
        rows.extend(new_rows)
        write_catalog(rows)
        print(f"updated {CATALOG.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
