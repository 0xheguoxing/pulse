#!/usr/bin/env python3
"""Checks that every menu command is linked in the action list.

src/app/actions/action_inventory.h links each MenuCmd (src/app/context_menu.h)
and each kCmdGit* (src/app/git_ui.h) to an action, or gives the reason it has
none. KeyAction coverage is checked at compile time (action_inventory.cpp).

Reports gaps and exits 0; --strict exits 1 when anything is missing.
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
APP = ROOT / "src" / "app"


def read(path):
    return path.read_text(encoding="utf-8-sig")


def menu_commands():
    text = read(APP / "context_menu.h")
    body = text.split("enum MenuCmd", 1)[1].split("};", 1)[0]
    names = re.findall(r"^\s*(Cmd\w+)", body, re.M)
    names += re.findall(r"constexpr int (kCmdGit\w+)", read(APP / "git_ui.h"))
    return names


def inventory():
    text = read(APP / "actions" / "action_inventory.h")
    actions = re.findall(r'^\s*\{"([a-z_.]+)", ActionRisk::', text, re.M)
    body = text.split("kMenuCmdLinks[]", 1)[1].split("};", 1)[0]
    links = re.findall(r'\{(\w+), \{"([a-z_.]*)"(?:, "([^"]*)")?\}\}', body)
    return actions, links


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--strict", action="store_true", help="exit 1 on any gap")
    args = parser.parse_args()

    commands = menu_commands()
    actions, links = inventory()
    known = set(actions)
    linked = {}
    problems = []
    for command, action_id, why in links:
        if command in linked:
            problems.append(f"{command}: linked twice")
        linked[command] = action_id
        if action_id and action_id not in known:
            problems.append(f"{command}: unknown action '{action_id}'")
        if not action_id and not why:
            problems.append(f"{command}: no action and no reason")
    for command in commands:
        if command not in linked:
            problems.append(f"{command}: not linked to an action (add it to kMenuCmdLinks)")
    for command in linked:
        if command not in commands:
            problems.append(f"{command}: linked but no longer a menu command")

    with_action = sum(1 for c in commands if linked.get(c))
    print(f"actions: {len(actions)}  menu commands: {len(commands)}  "
          f"linked to an action: {with_action}  excluded: {len(commands) - with_action - sum(c not in linked for c in commands)}")
    for p in problems:
        print("  " + p)
    if not problems:
        print("every menu command is linked")
    return 1 if problems and args.strict else 0


if __name__ == "__main__":
    sys.exit(main())
