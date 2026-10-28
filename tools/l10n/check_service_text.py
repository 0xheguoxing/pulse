#!/usr/bin/env python3
"""Check that Simplified Chinese text produced by Pulse's helper processes has an
English translation in src/common/service_text.cpp.

The index service, network agent, pulse_shell and preview host report
Simplified Chinese; the UI localizes it with l10n::ServiceText(). Every Han
literal in the helper sources must be either an exact entry, a literal piece of
a pattern, or fully matched by a pattern. As in ServiceErrorText, only the
message before " | " counts (the rest is the item path); it is split on " · "
first, as ServiceText does.

Usage: python tools/l10n/check_service_text.py   (exit code 1 on missing text)
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TABLE = ROOT / "src/common/service_text.cpp"
SOURCES = [
    "src/app/ai_launcher.cpp",
    "src/index/index_client.cpp",
    "src/index/index_config.cpp",
    "src/index/index_engine.cpp",
    "src/index/index_migration.cpp",
    "src/index/index_shard.cpp",
    "src/index/network_agent_main.cpp",
    "src/index/network_index.cpp",
    "src/ipc/shell_client.cpp",
    "src/preview_host/preview_properties.cpp",
    "src/shell_host/main.cpp",
]
SEPARATOR = " \u00b7 "
ERROR_BAR = " | "
HAN = re.compile(r"[\u3400-\u9fff\uf900-\ufaff]")
WIDE_LITERAL = re.compile(r'L"((?:[^"\\\n]|\\.)*)"')
PLACEHOLDER = re.compile(r"\{n\}|\{\}")


def unescape(body: str) -> str:
    def repl(m: re.Match[str]) -> str:
        esc = m.group(0)
        if esc.startswith("\\u"):
            return chr(int(esc[2:], 16))
        return {"\\\\": "\\", '\\"': '"', "\\n": "\n", "\\t": "\t"}.get(esc, esc)
    return re.sub(r"\\u[0-9A-Fa-f]{4}|\\.", repl, body)


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    out = []
    for line in text.splitlines():
        # Drop // comments that are not inside a string literal.
        in_str = False
        i = 0
        while i < len(line):
            c = line[i]
            if c == "\\" and in_str:
                i += 2
                continue
            if c == '"':
                in_str = not in_str
            elif not in_str and line.startswith("//", i):
                line = line[:i]
                break
            i += 1
        out.append(line)
    return "\n".join(out)


def load_table() -> tuple[set[str], list[str]]:
    text = strip_comments(TABLE.read_text(encoding="utf-8"))
    exact: set[str] = set()
    patterns: list[str] = []
    for m in re.finditer(r'\{\s*L"((?:[^"\\]|\\.)*)"\s*,\s*(?:L"(?:[^"\\]|\\.)*"\s*)+\}', text):
        zh = unescape(m.group(1))
        if PLACEHOLDER.search(zh):
            patterns.append(zh)
        else:
            exact.add(zh)
    return exact, patterns


def pattern_regex(pattern: str) -> re.Pattern[str]:
    parts = PLACEHOLDER.split(pattern)
    kinds = PLACEHOLDER.findall(pattern)
    rx = ""
    for i, part in enumerate(parts):
        rx += re.escape(part)
        if i < len(kinds):
            rx += r"[0-9,]+" if kinds[i] == "{n}" else r".+"
    return re.compile(rx + r"\Z")


def known(fragment: str, exact: set[str], patterns: list[str],
          pieces: list[str], regexes: list[re.Pattern[str]]) -> bool:
    if fragment in exact:
        return True
    if any(rx.match(fragment) for rx in regexes):
        return True
    # A literal concatenated around runtime values must be a pattern piece.
    core = fragment.strip()
    return any(core and core in piece for piece in pieces)


def main() -> int:
    exact, patterns = load_table()
    regexes = [pattern_regex(p) for p in patterns]
    pieces = [piece.strip() for p in patterns for piece in PLACEHOLDER.split(p) if piece.strip()]
    missing: list[tuple[str, str]] = []
    checked = 0
    for rel in SOURCES:
        text = strip_comments((ROOT / rel).read_text(encoding="utf-8"))
        for m in WIDE_LITERAL.finditer(text):
            literal = unescape(m.group(1))
            if not HAN.search(literal):
                continue
            for fragment in literal.split(ERROR_BAR, 1)[0].split(SEPARATOR):
                if not HAN.search(fragment):
                    continue
                checked += 1
                if not known(fragment, exact, patterns, pieces, regexes) and \
                        not known(fragment.strip(), exact, patterns, pieces, regexes):
                    missing.append((rel, fragment))
    for rel, fragment in missing:
        print(f"[MISSING] {rel}: {fragment!r}")
    print(f"{checked} fragments checked, {len(exact)} exact entries, {len(patterns)} patterns, "
          f"{len(missing)} missing")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())