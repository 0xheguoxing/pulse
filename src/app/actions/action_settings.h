// action_settings.h - Pulse's settings as actions (全面打通).
//
// One row per setting the assistant and the command palette may change. A row
// changes the setting the way the user's click does (the same ToggleUi index
// or controller setter), so saving and applying stay in one place. Choices
// hold the prefs' values; the setters take the choice's index like the
// segmented control's hit index.
#pragma once

#include "../../common/localization.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace pulse {
struct AppState;
}

namespace pulse::app::actions {

struct SetChoice {
    const char* key = nullptr;
    l10n::StringId label{};
    int value = 0;
};

inline constexpr uint8_t kSetSys = 1;   // 影响系统: Windows sees it (registry, shell)

struct SetDef {
    const char* id;
    l10n::StringId label;
    int page;                                // settings page (SelectPage)
    uint8_t flags;
    int toggle;                              // a switch: the click's index; -1 uses set (0 / 1)
    int (*get)(AppState&);
    void (*set)(AppState&, int);             // the choice's index (switch: 0 off, 1 on)
    std::array<SetChoice, 4> choices;        // none: a switch
};

std::span<const SetDef> SettingDefs();
const SetDef* FindSetting(std::string_view id);
bool IsSwitch(const SetDef& d);
// The value as the card and the palette show it: 开 / 关, or the control's label.
std::wstring SettingText(const SetDef& d, int v);
std::wstring SettingsPageTitle(int page);
// Sets d to the choice at `index` (switch: 0 / 1) the way a click would.
void ApplySetting(AppState& s, const SetDef& d, int index);
// The choice index of a prefs value (-1: none).
int ChoiceIndex(const SetDef& d, int v);

} // namespace pulse::app::actions
