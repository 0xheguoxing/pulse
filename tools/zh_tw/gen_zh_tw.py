#!/usr/bin/env python3
"""Deprecated shim: the string pipeline moved to tools/l10n/gen_strings.py.

tools/l10n/catalog.tsv is now the source of truth (zh-CN + en-US + zh-TW in one
file); strings_zh_tw.rcinc, strings.rcinc, string_ids.h, the StringId enum and
zh_hant_table.inc are all generated from it. This shim keeps old commands
working: `python tools/zh_tw/gen_zh_tw.py [--check]` runs the new generator.
"""
import runpy
import sys
from pathlib import Path

target = Path(__file__).resolve().parents[1] / "l10n" / "gen_strings.py"
sys.argv[0] = str(target)
runpy.run_path(str(target), run_name="__main__")
