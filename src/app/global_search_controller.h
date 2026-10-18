#pragma once
namespace pulse {
struct AppState;
void ApplyGlobalSearchSettings(AppState& s);
void ToggleGlobalSearch(AppState& s);
// Settings > Search master switch: asks before turning it off (and whether to
// delete the index), saves the preference and applies it.
void ToggleSearchSwitch(AppState& s);
void ApplySearchSwitch(AppState& s, bool purge_index);
void ShutdownGlobalSearch(AppState& s);
bool HandleGlobalSearchHotkeyCapture(AppState& s, unsigned key);
}
