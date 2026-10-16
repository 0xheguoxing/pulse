// shortcut_settings.h — the state behind Settings > 快捷键 (page 7): filter,
// search, the open popover (record / range / info / tester / more), the
// conflict probe and the undo snapshot. UI thread only, except the probe
// worker, which hands its result over under a mutex.
//
// The page edits a Keymap copy and reports each change through TakeChange();
// the caller (shortcut_page.cpp) writes AppPrefs, saves and applies it, so
// prefs stay the single source of truth (Sync() follows them back).
#pragma once

#include <windows.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "keymap.h"
#include "../ui/shortcuts_view.h"

namespace pulse::app {

class ShortcutSettings {
public:
    ShortcutSettings();
    ~ShortcutSettings();
    ShortcutSettings(const ShortcutSettings&) = delete;
    ShortcutSettings& operator=(const ShortcutSettings&) = delete;

    // Repainted when the probe finishes and when the toast expires.
    void SetNotifyWindow(HWND hwnd);
    // Follows AppPrefs::shortcuts and the global hotkey; cheap when unchanged.
    void Sync(const std::vector<std::wstring>& lines, KeyChord global, bool global_enabled);
    // The first call starts the background RegisterHotKey probe.
    void EnsureProbed();
    std::shared_ptr<const ui::ShortcutsView> View();
    int IssueCount();  // red + amber, for the sidebar badge

    // HitTestResult::SettingsShortcuts index (ui::ShortcutsHit).
    void Click(int index);
    // Keyboard messages while the page is shown; true when used.
    bool HandleKey(UINT msg, WPARAM wparam, LPARAM lparam);
    // A chord that arrived as WM_HOTKEY (Pulse's own global hotkey) while
    // recording; true when used.
    bool HandleHotkeyChord(KeyChord chord);
    bool WantsKeys() const noexcept;
    void CloseTransient();  // the page was left: drop popover and focus

    struct Change {
        std::vector<std::wstring> lines;  // AppPrefs::shortcuts
        KeyChord global{};
        bool global_changed = false;
    };
    bool TakeChange(Change& out);

    // Prefs line keeping the IME warnings the user switched off.
    static constexpr const wchar_t* kImeIgnoredId = L"_ImeIgnored";
    const Keymap& keymap() const noexcept { return map_; }
    const ShortcutEnv& env() const noexcept { return env_; }
    void SetEnvForTesting(ShortcutEnv env);

private:
    struct Snapshot {
        Keymap map;
        std::vector<KeyChord> ime_ignored;
    };
    void StartProbe();
    void TakeProbeResult();
    void Rebuild();
    void BuildPop(ui::ShortcutPopView& p);
    void OpenRow(KeyAction action, int slot);
    void OpenRecord(KeyAction action, int slot);
    void OpenInfo(KeyAction action, int slot);
    void OpenRange(KeyAction action);
    void ClosePop();
    void FeedChord(KeyChord chord);
    void SaveRecord();
    void SaveRange();
    void ApplyChord(KeyAction action, int slot, KeyChord chord, const ChordVerdict& verdict);
    void Remember();
    void Changed(std::wstring toast, bool undo = true);
    void Toast(std::wstring text, bool undo);
    void Undo();
    void ResetAllKeys();
    void CopyToClipboard();
    void PasteFromClipboard();
    ChordVerdict RecordVerdict() const;
    std::vector<std::wstring> PrefsLines() const;
    const BindingIssue* IssueFor(KeyAction action, KeyChord chord) const;
    std::vector<KeyChord> DisplayChords(KeyAction action) const;

    HWND notify_ = nullptr;
    Keymap map_;
    ShortcutEnv env_;
    std::vector<std::wstring> synced_lines_;
    KeyChord synced_global_{};
    bool synced_global_enabled_ = false;
    bool synced_ = false;

    std::mutex mutex_;
    std::vector<KeyChord> probe_result_;
    bool probe_ready_ = false;
    std::atomic<bool> probing_{false};
    bool probed_once_ = false;
    std::thread worker_;

    int filter_ = 0;
    std::wstring query_;
    bool search_focused_ = false;
    bool finding_ = false;
    KeyChord find_chord_{};

    ui::ShortcutPopView::Kind pop_ = ui::ShortcutPopView::None;
    KeyAction pop_action_ = KeyAction::Count;
    int pop_slot_ = -1;
    KeyChord rec_chord_{};
    uint8_t rec_held_ = 0;
    uint8_t range_mods_ = 0;
    KeyChord test_chord_{};
    std::vector<KeyChord> pop_suggestions_;

    std::optional<Snapshot> undo_;
    std::wstring toast_;
    bool toast_undo_ = false;
    ULONGLONG toast_until_ = 0;

    bool changed_ = false;
    bool global_changed_ = false;
    std::vector<BindingIssue> issues_;
    std::shared_ptr<const ui::ShortcutsView> view_;
    bool dirty_ = true;
    bool view_probing_ = false;
};

} // namespace pulse::app
