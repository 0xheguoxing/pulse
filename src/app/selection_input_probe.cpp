#include "app_input.h"
#include "app_commands.h"
#include "app_navigation.h"
#include "app_sidebar_refresh.h"
#include "app_runtime.h"
#include "app_ops_ui.h"
#include <algorithm>
#include <cstdio>

using namespace pulse;
namespace {
int CheckDeleteSelection(AppState& s, FILE* log) {
    wchar_t data[32768]{};
    if (!s.isolatedTest || !GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", data, ARRAYSIZE(data))) return 2;
    auto* tab = ActiveTab(s);
    const std::wstring fixture = fs::NormalizePath(std::wstring(data) + L"\\files");
    if (!tab || fs::NormalizePath(tab->current_path) != fs::NormalizePath(fixture) || tab->EntryCount() != 5) {
        fprintf(log, "[ERROR] fixture guard path=%ls expected=%ls count=%zu\n",
                tab ? tab->current_path.c_str() : L"", fixture.c_str(), tab ? tab->EntryCount() : 0);
        return 2;
    }
    for (int i = 0; i < 5; ++i) {
        const std::wstring expected(1, static_cast<wchar_t>(L'a' + i));
        if (tab->EntryAt(i).name != expected + L".txt") {
            fprintf(log, "[ERROR] fixture row %d name=%ls\n", i, tab->EntryAt(i).name.c_str());
            return 2;
        }
    }
    s.appPrefs.confirm_recycle_delete = false;
    int failures = 0;
    const auto pump = [] {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        Sleep(10);
    };
    const auto remove = [&](std::vector<int> indices, int remaining, const wchar_t* expected, const char* label) {
        tab->SelectIndices(indices);
        const auto paths = SelectedFullPaths(*tab);
        DeleteSelected(s, false);
        const auto deadline = GetTickCount64() + 15000;
        bool completed = false;
        while (GetTickCount64() < deadline) {
            pump();
            completed = !tab->loading && tab->EntryCount() == static_cast<size_t>(remaining);
            for (const auto& path : paths) completed &= GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
            if (completed) break;
        }
        const auto settled = GetTickCount64() + 500;
        while (GetTickCount64() < settled) pump();
        const std::wstring actual = tab->selected_index >= 0 ? tab->EntryAt(tab->selected_index).name : L"";
        const bool okay = completed && actual == expected && (remaining || tab->SelectedCount() == 0);
        fprintf(log, "[%s] %s actual=%ls index=%d remaining=%zu\n", okay ? "PASS" : "FAIL", label,
                actual.c_str(), tab->selected_index, tab->EntryCount());
        fflush(log);
        failures += !okay;
        return okay;
    };
    if (!remove({2}, 4, L"d.txt", "real recycle command: middle selection follows successor")) return 1;
    if (!remove({3}, 3, L"d.txt", "real recycle command: last selection follows predecessor")) return 1;
    if (!remove({1, 2}, 1, L"a.txt", "real recycle command: multiple deletion follows surviving neighbor")) return 1;
    if (!remove({0}, 0, L"", "real recycle command: deleting final item clears selection")) return 1;
    for (wchar_t ch = L'a'; ch <= L'e'; ++ch) {
        const auto path = fixture + L"\\" + std::wstring(1, ch) + L".txt";
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return 2;
        CloseHandle(file);
    }
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    fs::EnumerateDirectory(fixture, *entries);
    std::sort(entries->begin(), entries->end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    tab->SetSnapshot(entries);
    for (const auto& name : {L"c.txt", L"e.txt"}) {
        int index = -1;
        for (int i = 0; i < static_cast<int>(tab->EntryCount()); ++i) if (tab->EntryAt(i).name == name) index = i;
        if (index < 0) return 2;
        tab->SelectOnly(index);
        if (!DeleteFileW((fixture + L"\\" + name).c_str())) return 2;
        const bool applied = ApplyNotifyToVisible(s, fixture, {FILE_ACTION_REMOVED, name, {}});
        const auto actual = tab->selected_index >= 0 ? tab->EntryAt(tab->selected_index).name : L"";
        const bool okay = applied && actual == L"d.txt";
        fprintf(log, "[%s] directory-notify handler: applied=%d removed=%ls selected=%ls\n",
                okay ? "PASS" : "FAIL", applied ? 1 : 0, name, actual.c_str());
        fflush(log);
        failures += !okay;
    }
    return failures ? 1 : 0;
}

template<class Check>
void CheckFilteredRanges(Check check) {
    // In-memory rows only: validate the same path list consumed by operations,
    // without dispatching delete/move or touching files/preferences.
    for (bool descending : {false, true}) {
        for (int mode = 0; mode < 5; ++mode) {
            app::Pane pane;
            auto& tab = pane.view;
            tab.current_path = L"C:\\pulse-selection-fixture";
            auto entries = std::make_shared<std::vector<fs::DirEntry>>(3);
            const wchar_t* names[] = {L"a.keep", L"b.secret", L"c.keep"};
            for (int i = 0; i < 3; ++i) {
                auto& entry = (*entries)[i];
                entry.name = names[i];
                entry.full_path = tab.current_path + L"\\" + names[i];
                entry.size = i == 1 ? 1000 : 10;
                entry.attrs = mode == 4 && i == 1 ? FILE_ATTRIBUTE_HIDDEN : FILE_ATTRIBUTE_NORMAL;
            }
            if (descending) std::reverse(entries->begin(), entries->end());
            tab.SetSnapshot(entries);
            app::PlacesCatalog places;
            places.persist = false;
            app::ColorTag tag;
            tag.name = L"keep";
            tag.paths = {(*entries)[0].full_path, (*entries)[2].full_path};
            places.tags.push_back(std::move(tag));
            places.TagsReordered();
            if (mode == 0) tab.filter_text = L"*.keep";
            if (mode == 1) tab.filter_text = L"#keep";
            if (mode == 2 || mode == 3) {
                tab.compare_marks = std::make_shared<const std::vector<uint8_t>>(std::initializer_list<uint8_t>{1, 0, 1});
                tab.compare_diff_only = true;
            }
            if (mode == 3) tab.filter_text = L"*.keep #keep";
            ui::PaneViewModel view;
            app::FillPaneViewModel(view, pane, &places);
            check(view.EntryCount() == 2 && view.SourceIndex(0) == 0 && view.SourceIndex(1) == 2,
                  "filter pipeline exposes only expected rows (name/tag/diff/combined/hidden, ascending/descending)");
            for (bool reverse : {false, true}) {
                const int from = reverse ? 2 : 0;
                const int to = reverse ? 0 : 2;
                tab.SelectOnly(from);
                tab.SelectRange(from, to, &view);
                auto paths = SelectedFullPaths(tab);
                auto expected = std::vector<std::wstring>{(*entries)[0].full_path, (*entries)[2].full_path};
                std::sort(paths.begin(), paths.end());
                std::sort(expected.begin(), expected.end());
                uint64_t bytes = 0;
                int files = 0, folders = 0;
                tab.SelectionSizeSummary(&bytes, &files, &folders);
                check(paths == expected && tab.SelectedCount() == 2 && bytes == 20 && files == 2 && folders == 0,
                      "forward/reverse Shift range exports only visible operation paths and matching status totals");
                tab.SelectOnly(from);
                tab.MoveFocus(to, true, &view);
                check(tab.SelectedIndices() == std::vector<int>({0, 2}) && tab.selected_index == to,
                      "keyboard focus extension uses the displayed range");
            }
            tab.SelectOnly(1);
            tab.MoveFocus(2, true, &view);
            check(tab.SelectedIndices() == std::vector<int>({2}) && tab.selection_anchor == 2,
                  "removed anchor restarts range at visible target");
        }
    }
    app::Pane pane;
    auto entries = std::make_shared<std::vector<fs::DirEntry>>(3);
    for (auto& entry : *entries) entry.attrs = FILE_ATTRIBUTE_NORMAL;
    pane.view.SetSnapshot(entries);
    ui::PaneViewModel view;
    app::FillPaneViewModel(view, pane);
    auto groups = std::make_shared<ui::ListGroups>(3);
    for (int i = 0; i < 3; ++i) { (*groups)[i].first = i; (*groups)[i].count = 1; }
    (*groups)[1].collapsed = true;
    view.groups = groups;
    view.view_mode = ui::ViewMode::Details;
    pane.view.SelectRange(0, 2, &view);
    check(pane.view.SelectedIndices() == std::vector<int>({0, 2}), "range excludes collapsed group members");
    pane.view.SelectOnly(1);
    pane.view.MoveFocus(2, true, &view);
    check(pane.view.SelectedIndices() == std::vector<int>({2}), "collapsed anchor restarts range");
    view.filter_map = std::make_shared<const ui::PaneViewModel::FilterMap>();
    pane.view.SelectRange(0, 2, &view);
    check(pane.view.SelectedCount() == 0, "empty filtered view cannot select source rows");
}

// Explorer-style keyboard selection through the real key handler. GetKeyState
// reads this thread's key table, so SetKeyboardState stands in for held keys.
int CheckKeyboardSelection(AppState& s, FILE* log) {
    auto* tab = ActiveTab(s);
    if (!tab || tab->EntryCount() < 4) {
        fprintf(log, "[ERROR] keyboard fixture needs 4 rows, got %zu\n", tab ? tab->EntryCount() : 0);
        return 2;
    }
    ui::PaneViewModel view;
    app::FillPaneViewModel(view, *s.pane, &s.places);
    const auto row = [&](int v) { return view.SourceIndex(v); };
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label);
        fflush(log);
        failures += !ok;
    };
    const auto key = [&](UINT vk, bool ctrl, bool shift) {
        BYTE keys[256]{};
        GetKeyboardState(keys);
        keys[VK_CONTROL] = keys[VK_LCONTROL] = ctrl ? 0x80 : 0;
        keys[VK_SHIFT] = keys[VK_LSHIFT] = shift ? 0x80 : 0;
        SetKeyboardState(keys);
        HandleKeyDown(&s, s.hwnd, WM_KEYDOWN, vk, 0);
        keys[VK_CONTROL] = keys[VK_LCONTROL] = keys[VK_SHIFT] = keys[VK_LSHIFT] = 0;
        SetKeyboardState(keys);
    };
    const auto rows = [&](std::initializer_list<int> views) {
        std::vector<int> out;
        for (int v : views) out.push_back(row(v));
        std::sort(out.begin(), out.end());
        return out;
    };
    tab->SelectOnly(row(0));
    key(VK_DOWN, true, false);
    key(VK_DOWN, true, false);
    check(tab->SelectedIndices() == rows({0}) && tab->FocusIndex() == row(2),
          "Ctrl+Down moves focus without changing the selection");
    ui::PaneViewModel ring;
    app::FillPaneViewModel(ring, *s.pane, &s.places);
    check(ring.focus_ring_source == row(2), "the unselected focused row gets the focus ring");
    key(VK_SPACE, true, false);
    check(tab->SelectedIndices() == rows({0, 2}), "Ctrl+Space toggles the focused row on");
    key(VK_DOWN, true, true);
    check(tab->SelectedIndices() == rows({0, 2, 3}), "Ctrl+Shift+Down adds the range from the anchor");
    HandleListRowClick(s, row(3), true, false);
    FinishListRowClick(s);
    check(tab->SelectedIndices() == rows({0, 2}) && tab->FocusIndex() == row(3) &&
          tab->selection_anchor == row(3), "Ctrl-click deselects, keeps focus and becomes the anchor");
    HandleListRowClick(s, row(1), false, true);
    check(tab->SelectedIndices() == rows({1, 2, 3}), "Shift-click replaces with anchor..row");
    key(VK_HOME, true, false);
    check(tab->SelectedIndices() == rows({1, 2, 3}) && tab->FocusIndex() == row(0),
          "Ctrl+Home moves focus only");
    key(VK_SPACE, false, true);
    check(tab->SelectedIndices() == rows({0, 1, 2, 3}), "Shift+Space selects anchor..focus");
    key(VK_DOWN, false, false);
    check(tab->SelectedIndices() == rows({1}), "plain Down continues from the moved focus");
    tab->SelectOnly(row(0));
    HandleListRowClick(s, row(2), true, false);
    HandleListRowClick(s, row(3), true, true);
    check(tab->SelectedIndices() == rows({0, 2, 3}), "Ctrl+Shift-click adds the range");
    key(VK_UP, false, true);
    check(tab->SelectedIndices() == rows({2}), "Shift+Up replaces with anchor..focus");
    return failures ? 1 : 0;
}
}
// Runs only in an explicitly isolated --shot test instance with fixture files.
int RunSelectionInputProbe(AppState& s, const wchar_t* output) {
    FILE* log = nullptr;
    if (_wfopen_s(&log, output, L"w") || !log) return 2;
    if (GetEnvironmentVariableW(L"PULSE_TEST_DELETE_SELECTION_ONLY", nullptr, 0)) {
        const int result = CheckDeleteSelection(s, log);
        std::fclose(log);
        return result;
    }
    if (GetEnvironmentVariableW(L"PULSE_TEST_KEYBOARD_SELECTION_ONLY", nullptr, 0)) {
        const int result = CheckKeyboardSelection(s, log);
        std::fclose(log);
        return result;
    }
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label);
        std::fflush(log);
        failures += !ok;
    };
    CheckFilteredRanges(check);
    if (GetEnvironmentVariableW(L"PULSE_TEST_FILTERED_SELECTION_ONLY", nullptr, 0)) {
        extern int RunNavigationAuditProbe(AppState&, FILE*);
        failures += RunNavigationAuditProbe(s, log);
        std::fclose(log);
        return failures ? 1 : 0;
    }
    const auto pump_sidebar = [&](auto ready) {
        const auto deadline = GetTickCount64() + 10000;
        while (GetTickCount64() < deadline) {
            TickSidebarRefresh(s, GetTickCount64());
            if (ready()) return true;
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Sleep(10);
        }
        return false;
    };
    check(pump_sidebar([&] { return !s.sidebar.drives.empty(); }), "sidebar loads asynchronously after worker startup");
    if (!s.sidebar.drives.empty()) {
        s.sidebar.drives.front().detail = L"stale-capacity";
        RequestSidebarRefresh(s);
        check(pump_sidebar([&] { return s.sidebar.drives.front().detail != L"stale-capacity"; }),
              "background capacity request replaces stale display without rebuilding sidebar");
    }
    auto* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || tab->EntryCount() < 3) { std::fclose(log); return 3; }
    tab->ClearSelection();
    SetFocus(s.hwnd);
    SendMessageW(s.hwnd, WM_CHAR, L'a', 0);
    check(tab->selected_index >= 0 && tab->EntryAt(tab->selected_index).name == L"apple.txt",
          "WM_CHAR selects first matching fixture");
    SendMessageW(s.hwnd, WM_CHAR, L'a', 0);
    check(tab->selected_index >= 0 && tab->EntryAt(tab->selected_index).name == L"apricot.txt",
          "WM_CHAR cycles to next matching fixture");
    ToggleQuickPreview(s);
    const auto original = s.quickPreview.item().path;
    check(s.quickPreview.visible() && original.find(L"apricot.txt") != std::wstring::npos,
          "preview opens selected fixture");
    SetFocus(s.hwnd);
    s.listTypeAhead = {};
    SendMessageW(s.hwnd, WM_CHAR, L'b', 0);
    const auto now = GetTickCount64();
    TickQuickPreviewSelection(s, now);
    check(s.quickPreview.item().path == original, "selection update is debounced");
    TickQuickPreviewSelection(s, now + 121);
    check(s.quickPreview.item().path.find(L"banana.txt") != std::wstring::npos,
          "open preview follows the newly selected file");
    ui::QuickPreviewItem internal = s.quickPreview.item();
    internal.path = original;
    internal.name = L"apricot.txt";
    s.quickPreview.Update(internal);
    TickQuickPreviewSelection(s, now + 300);
    check(s.quickPreview.item().path == original, "unchanged list selection preserves preview navigation");
    tab->ClearSelection();
    TickQuickPreviewSelection(s, now + 400);
    TickQuickPreviewSelection(s, now + 521);
    check(s.quickPreview.visible(), "empty selection keeps the preview accessible");
    s.quickPreview.Close();
    std::fclose(log);
    return failures ? 1 : 0;
}
