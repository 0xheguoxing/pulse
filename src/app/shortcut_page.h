// shortcut_page.h — Settings > 快捷键 glue between AppState, the keymap and
// the settings page (app::ShortcutSettings holds the page state).
#pragma once

#include <windows.h>

namespace pulse {
struct AppState;
namespace ui { struct WindowViewModel; }

// Builds the active keymap from AppPrefs::shortcuts and the global hotkey.
void ApplyKeymapPrefs(AppState& s);
// BuildVm while Settings is open: the sidebar badge and, on page 7, the view.
void FillShortcutsVm(AppState& s, ui::WindowViewModel& vm);
// A click on the page (HitTestResult::SettingsShortcuts index).
void ShortcutsAction(AppState& s, int index);
// Keyboard messages before any shortcut handling: the recorder, the key
// tester, 按键查找 and the search box take raw keys. True when used.
bool ShortcutsPageKey(AppState& s, UINT msg, WPARAM wparam, LPARAM lparam);
// WM_HOTKEY for Pulse's own global hotkey while the recorder listens.
bool ShortcutsPageHotkey(AppState& s, LPARAM lparam);
}
