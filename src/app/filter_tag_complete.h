// filter_tag_complete.h — the filter box's # completion (pulse_tag_redesign
// §7): typing # or -# (or | after one) lists the tags (color dot, name,
// count); ↑↓ pick, Enter / Tab / a click insert the name, Esc closes.
#pragma once
#include "app_runtime.h"

namespace pulse {
// After the filter text or caret changed.
void UpdateFilterTagSuggestions(AppState& s);
void CloseFilterTagSuggestions(AppState& s);
// The filter edit's WM_KEYDOWN; true when the list took the key.
bool FilterTagSuggestionsKey(AppState& s, WPARAM key);
// The WM_CHAR that follows a key the list took (Enter / Tab / Esc); true once.
bool FilterTagSuggestionsSwallowChar(WPARAM ch);
// A left click: true when it landed on the list (a row inserts its tag).
bool ClickFilterTagSuggestion(AppState& s, int x, int y);
void FillFilterTagSuggestionsVm(AppState& s, ui::WindowViewModel& vm);
}
