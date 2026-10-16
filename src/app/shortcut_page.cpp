#include "shortcut_page.h"

#include "app_internal.h"
#include "global_search_controller.h"
#include "keymap.h"
#include "shortcut_settings.h"

namespace pulse {

void ApplyKeymapPrefs(AppState& s) {
    app::Keymap keymap = app::Keymap::FromPrefs(s.appPrefs.shortcuts);
    keymap.SetGlobalHotkey(app::KeyChord{static_cast<uint8_t>(s.appPrefs.global_search_modifiers & 0x0F),
                                         static_cast<uint16_t>(s.appPrefs.global_search_key & 0xFF)},
                           s.appPrefs.global_search_enabled);
    app::SetActiveKeymap(std::move(keymap));
}

namespace {

app::ShortcutSettings& Model(AppState& s) {
    auto& model = s.settings.Shortcuts();
    model.SetNotifyWindow(s.hwnd);
    model.Sync(s.appPrefs.shortcuts,
               app::KeyChord{static_cast<uint8_t>(s.appPrefs.global_search_modifiers & 0x0F),
                             static_cast<uint16_t>(s.appPrefs.global_search_key & 0xFF)},
               s.appPrefs.global_search_enabled);
    return model;
}

bool PageShown(AppState& s) {
    return IsSettingsTab(ActiveTab(s)) && s.settings.page() == 7;
}

// The page changed the keymap: write prefs, save, apply to every window.
void CommitShortcutChanges(AppState& s) {
    app::ShortcutSettings::Change change;
    if (!s.settings.Shortcuts().TakeChange(change)) return;
    s.appPrefs.shortcuts = std::move(change.lines);
    if (change.global_changed) {
        s.appPrefs.global_search_modifiers = change.global.mods;
        s.appPrefs.global_search_key = change.global.vk;
    }
    if (!s.isolatedTest) s.appPrefs.Save();
    if (change.global_changed) ApplyGlobalSearchSettings(s);  // re-registers, then ApplyKeymapPrefs
    else ApplyKeymapPrefs(s);
}

void Refresh(AppState& s) {
    // Filters and popovers change the page height; an opened popover is
    // scrolled into view (it may flip above its row, so settle in a few steps).
    const float w = static_cast<float>(s.compositor.Width());
    const float h = static_cast<float>(s.compositor.Height());
    for (int step = 0; step < 3; ++step) {
        auto vm = BuildVm(s, false);
        const float maximum = s.renderer.SettingsMaxScroll(vm, w, h);
        const float delta = s.renderer.SettingsShortcutsRevealDelta(vm, w, h);
        s.settings.SetScroll(s.settings.scroll() + delta, maximum);
        if (delta > -0.5f && delta < 0.5f) break;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

} // namespace

void FillShortcutsVm(AppState& s, ui::WindowViewModel& vm) {
    auto& model = Model(s);
    model.EnsureProbed();
    if (vm.settings_page != 7) model.CloseTransient();
    vm.settings_shortcut_issues = model.IssueCount();
    if (vm.settings_page == 7) vm.settings_shortcuts = model.View();
}

void ShortcutsAction(AppState& s, int index) {
    auto& model = Model(s);
    if (index == ui::kScSearch) SetFocus(s.hwnd);
    model.Click(index);
    CommitShortcutChanges(s);
    Refresh(s);
}

bool ShortcutsPageKey(AppState& s, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (!PageShown(s)) return false;
    auto& model = Model(s);
    if (!model.WantsKeys()) return false;
    if (!model.HandleKey(msg, wparam, lparam)) return false;
    CommitShortcutChanges(s);
    Refresh(s);
    return true;
}

bool ShortcutsPageHotkey(AppState& s, LPARAM lparam) {
    if (!PageShown(s)) return false;
    auto& model = Model(s);
    const app::KeyChord chord{static_cast<uint8_t>(LOWORD(lparam) & 0x0F), static_cast<uint16_t>(HIWORD(lparam) & 0xFF)};
    if (!model.HandleHotkeyChord(chord)) return false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

} // namespace pulse
