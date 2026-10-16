#!/usr/bin/env python3
"""Fail when product code still shows UI text through `Pick(L"...", L"...")`.

New UI text belongs in tools/l10n/catalog.tsv and is read with
`l10n::Get(l10n::StringId::X)` (see tools/l10n/gen_strings.py). Dynamic
Pick calls (runtime tables) and Cn(...) composition helpers stay allowed.

    python tools/l10n/check_no_pick.py   (exit 1 with the offending call sites)
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
from migrate_pick import TOKEN, parse_pick_call  # noqa: E402

EXEMPT_PREFIX = (
    "src/bench/",
    "src/common/zh_hant.cpp",
    "src/common/localization.h",
    "src/common/localization.cpp",
    "src/app/selftest_1b2.cpp",
    # Service-side / shared code compiled into binaries without the string
    # table (index service, tests): the runtime Pick helper works there.
    "src/fs/fs_enum.cpp",
    "src/fs/fs_net_cache.cpp",
    "src/ipc/shell_client.cpp",
    "src/common/diagnostics_exporter.cpp",
)
CALL = re.compile(r"\b((?:\w+::)*)Pick\s*\(")


def main():
    bad = []
    for path in sorted((ROOT / "src").rglob("*")):
        rel = path.relative_to(ROOT).as_posix()
        if path.suffix not in (".cpp", ".h") or rel.startswith(EXEMPT_PREFIX):
            continue
        if path.name.endswith(("_test.cpp", "_test.h")):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if "Pick(" not in text:
            continue
        masked, last = [], 0
        for m in TOKEN.finditer(text):
            masked.append((last, m.start(), False))
            masked.append((m.start(), m.end(), True))
            last = m.end()
        masked.append((last, len(text), False))
        for start, end, is_masked in masked:
            if is_masked:
                continue
            for call in CALL.finditer(text, start, end):
                if call.group(1) not in ("", "l10n::", "pulse::l10n::"):
                    continue  # other Pick APIs (e.g. ColorPickerPopup::Pick)
                if parse_pick_call(text, call.end() - 1):
                    line = text.count("\n", 0, call.start()) + 1
                    sample = text[call.start():call.start() + 70].splitlines()[0]
                    bad.append(f"{rel}:{line}: {sample}")
    if bad:
        print("Pick(L\"…\", L\"…\") calls in product code (put the text in tools/l10n/catalog.tsv):")
        for entry in bad:
            print("   ", entry)
        sys.exit(1)
    print("no literal Pick() calls in product code")


if __name__ == "__main__":
    main()
