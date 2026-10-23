#ifdef PULSE_WITH_SELFTEST
#include "../app/app_internal.h"
#include "../app/app_input.h"
#include "../ui/ui_renderer.h"
#include <windowsx.h>
#include <fstream>
#include <filesystem>
#include <cmath>

int RunLayoutRegressionProbe(pulse::AppState& s, const wchar_t* output) {
    using namespace pulse;
    wchar_t data[32768]{};
    const auto length = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", data, ARRAYSIZE(data));
    if (!s.isolatedTest || !length || length >= ARRAYSIZE(data)) return 2;
    std::ofstream log{std::filesystem::path(output)};
    if (!log) return 2;
    int failures = 0;
    BYTE original_keys[256]{}, test_keys[256]{};
    if (!GetKeyboardState(original_keys) || !SetKeyboardState(test_keys)) return 2;
    struct RestoreKeys {
        BYTE* keys;
        ~RestoreKeys() { SetKeyboardState(keys); }
    } restore_keys{original_keys};
    auto check = [&](bool ok, const char* label) {
        log << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        failures += !ok;
    };
    s.appPrefs.persist = false;
    s.places.persist = false;
    const std::wstring private_folder = std::wstring(data) + L"\\layout-fixture";
    const auto artifacts = std::filesystem::path(data) / L"layout-render";
    std::error_code artifact_error;
    std::filesystem::create_directories(artifacts, artifact_error);
    check(!artifact_error, "private layout screenshot directory is available");
    auto entries = std::make_shared<std::vector<fs::DirEntry>>(30);
    for (int i = 0; i < 30; ++i) {
        auto& e = (*entries)[static_cast<size_t>(i)];
        e.name = L"synthetic-report-" + std::to_wstring(i) + L".txt";
        e.full_path = private_folder + L"\\" + e.name;
        e.attrs = FILE_ATTRIBUTE_NORMAL;
    }
    auto setup = [&](int panes, const std::wstring& path) {
        s.pane = nullptr;
        Root(s).reset(); Panes(s).clear();
        std::vector<app::Pane*> used;
        for (int i = 0; i < panes; ++i) {
            auto pane = std::make_unique<app::Pane>();
            pane->view.current_path = path;
            pane->view.SetSnapshot(entries);
            pane->view.loading = false;
            pane->view.view_mode = ui::ViewMode::Details;
            pane->focused = i == 0;
            used.push_back(pane.get()); Panes(s).push_back(std::move(pane));
        }
        LayoutOf(s) = panes == 1 ? app::LayoutPreset::Single : app::LayoutPreset::TwoVertical;
        Root(s) = app::MakePresetTree(LayoutOf(s), used);
        LiveLayout(s).focused_index = 0;
        s.pane = used.front();
    };
    auto point = [](float x, float y) { return MAKELPARAM(static_cast<short>(std::lround(x)), static_cast<short>(std::lround(y))); };
    for (float scale : {1.0f, 1.5f, 2.0f}) for (int panes : {1, 2})
    for (int width : {1000, 1680}) for (uint32_t mask : {ui::kDetailsColumnsDefault, ui::kDetailsColumnModified | ui::kDetailsColumnSize})
    for (const auto& path :
        std::vector<std::wstring>{private_folder, L"pulse:search:report", L"pulse:saved-search:fixture", L"pulse:recycle"}) {
        setup(panes, path);
        log << "[CASE] scale=" << scale << " panes=" << panes << " width=" << width << " mask=" << mask
            << " kind=" << (path == private_folder ? "folder" : path == L"pulse:recycle" ? "recycle" :
                           path == L"pulse:search:report" ? "search" : "saved-search") << std::endl;
        s.renderer.SetDetailsColumns(mask);
        s.scale = scale; s.renderer.SetScale(scale); s.compositor.RecreateTextFormats(scale);
        SetWindowPos(s.hwnd, nullptr, 0, 0, static_cast<int>(width * scale), static_cast<int>(740 * scale),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        s.compositor.Resize(static_cast<UINT>(width * scale), static_cast<UINT>(740 * scale));
        auto vm = BuildVm(s);
        const D2D1_RECT_F window{0, 0, static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height())};
        for (int pane_index = 0; pane_index < panes; ++pane_index) {
            const auto& slot = vm.pane_slots[static_cast<size_t>(pane_index)];
            auto* tab = PaneAtSlot(s, pane_index)->ActiveTab();
            const auto body = s.renderer.PaneBodyBounds(slot.pane, slot.rect);
            const auto columns = s.renderer.DetailsColumns(body, slot.pane);
            if (columns.count < 2) continue;
            const float x = columns.DividerX(0);
            const auto list = s.renderer.PaneListRect(slot.pane, slot.rect);
            const float y = list.top - 12 * scale;
            const auto hit = s.renderer.HitTest(vm, window, std::round(x), std::round(y));
            check(hit.region == ui::HitTestResult::ColumnDivider && hit.pane_index == pane_index,
                  "real renderer identifies intended divider and pane");
            if (hit.region != ui::HitTestResult::ColumnDivider) continue;
            test_keys[VK_LBUTTON] = 0x80; SetKeyboardState(test_keys);
            HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point(x, y));
            const auto details = tab->details_column_dividers;
            const auto search = tab->search_column_dividers;
            HandleMouseMove(&s, s.hwnd, WM_MOUSEMOVE, MK_LBUTTON, point(x + 30 * scale, y));
            const bool changed = slot.pane.is_search ? tab->search_column_dividers != search : tab->details_column_dividers != details;
            check(changed, "real mouse drag changes the displayed layout storage");
            check(slot.pane.is_search ? tab->details_column_dividers == details : tab->search_column_dividers == search,
                  "real mouse drag leaves unrelated column storage unchanged");
            bool hidden_unchanged = true;
            using K = ui::MainRenderer::ColumnKind;
            for (const auto kind : {K::Date, K::Type, K::Size, K::Created, K::Accessed}) {
                const int stored = ui::MainRenderer::ManualColumnSlot(kind, slot.pane.is_search);
                if (stored < 0 || columns.Has(kind)) continue;
                const auto index = static_cast<size_t>(stored);
                hidden_unchanged = hidden_unchanged && (slot.pane.is_search
                    ? tab->search_column_dividers[index] == search[index]
                    : tab->details_column_dividers[index] == details[index]);
            }
            check(hidden_unchanged, "drag does not alter widths of hidden optional columns");
            test_keys[VK_LBUTTON] = 0; SetKeyboardState(test_keys);
            HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, point(x + 30 * scale, y));
            auto moved_vm = BuildVm(s);
            const auto& moved_slot = moved_vm.pane_slots[static_cast<size_t>(pane_index)];
            const auto moved_columns = s.renderer.DetailsColumns(s.renderer.PaneBodyBounds(moved_slot.pane, moved_slot.rect), moved_slot.pane);
            check(std::abs(moved_columns.DividerX(0) - columns.DividerX(0)) > 1,
                  "visible divider follows real drag");
            if (panes == 1 && width == 1680 && scale <= 1.5f && mask == ui::kDetailsColumnsDefault &&
                (path == L"pulse:recycle" || path == L"pulse:saved-search:fixture")) {
                for (bool dark : {false, true}) {
                    const auto theme = ui::MakeTheme(dark, s.accentColor);
                    auto* dc = s.compositor.Dc(); dc->BeginDraw(); dc->Clear(theme.bg);
                    auto themed_vm = moved_vm;
                    themed_vm.dark = dark;
                    s.renderer.Render(themed_vm, window, theme);
                    check(SUCCEEDED(dc->EndDraw()), "render actual virtual-page column drag result");
                    const auto shot = artifacts / (std::wstring(path == L"pulse:recycle" ? L"recycle-" : L"saved-search-") +
                        std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark.png" : L"-light.png"));
                    check(s.compositor.SaveSnapshot(shot.c_str()), "save actual virtual-page drag screenshot");
                }
            }
            const auto fitted_details = tab->details_column_dividers;
            const auto fitted_search = tab->search_column_dividers;
            HandleLButtonDblClk(&s, s.hwnd, WM_LBUTTONDBLCLK, 0, point(moved_columns.DividerX(0), y));
            check(slot.pane.is_search ? tab->search_column_dividers != fitted_search : tab->details_column_dividers != fitted_details,
                  "real double click resets visible layout widths for automatic fit");
            check(slot.pane.is_search ? tab->details_column_dividers == fitted_details : tab->search_column_dividers == fitted_search,
                  "double click preserves unrelated layout storage");
        }
    }
    setup(1, private_folder);
    s.scale = 1; s.renderer.SetScale(1); s.compositor.RecreateTextFormats(1);
    s.compositor.Resize(1180, 735);
    for (const auto mode : {ui::ViewMode::ExtraLargeIcons, ui::ViewMode::List}) {
        auto* tab = ActiveTab(s); tab->view_mode = mode; tab->SelectOnly(0);
        const auto vm = BuildVm(s); const auto& slot = vm.pane_slots.front();
        const auto list = s.renderer.PaneListRect(slot.pane, slot.rect);
        ui::ViewLayout geometry(mode, list, entries->size(), 0, 0, 1);
        const auto metrics = geometry.Metrics();
        // Icon grids: the gap after the first column (rows are centred, so the
        // right remainder may be narrower than the scrollbar).
        const float x = mode == ui::ViewMode::List ? list.left + 30 :
            list.left + metrics.origin_x + metrics.cell_width + 3;
        const float y = mode == ui::ViewMode::List ? list.top + metrics.rows_per_column * metrics.cell_height + 2 : list.top + 20;
        const D2D1_RECT_F window{0, 0, 1180, 735};
        const auto hit = s.renderer.HitTest(vm, window, x, y);
        check(x < list.right - 14 && y < list.bottom && geometry.HitTest(x, y) < 0 && hit.index < 0,
              "main renderer treats real grid remainder as empty space");
        HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, 0, point(x, y));
        HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, point(x, y));
        check(tab->SelectedCount() == 0, "real blank-space mouse click clears selection instead of selecting adjacent item");
    }
    return failures ? 1 : 0;
}
#endif
