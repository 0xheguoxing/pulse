#ifdef PULSE_WITH_SELFTEST
#include "folder_sizes.h"
#include "folder_sizes_ui.h"
#include "entry_sort.h"
#include "app_state.h"
#include "../ui/ui_renderer_internal.h"
#include "../common/windows_compat.h"
#include "../common/text_format.h"
#include <filesystem>
#include <fstream>
#include <functional>

namespace pulse::ui {
struct FolderSizesUiTest {
    static bool Run(const std::filesystem::path& root, std::ofstream& log) {
        HWND hwnd = CreateWindowExW(0, L"STATIC", L"Folder sizes", WS_POPUP,
            0, 0, 920, 550, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        bool ok = hwnd != nullptr;
        {
            Compositor compositor;
            if (!hwnd || !compositor.Init(hwnd)) { if (hwnd) DestroyWindow(hwnd); return false; }
            MainRenderer renderer;
            renderer.SetCompositor(&compositor);
            for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) for (bool dark : {false, true}) for (int width : {920, 440}) for (auto mode : {ViewMode::Details, ViewMode::Tiles, ViewMode::MediumIcons, ViewMode::LargeIcons, ViewMode::ExtraLargeIcons, ViewMode::Content}) {
                compositor.Resize(static_cast<UINT>(width * scale), static_cast<UINT>(550 * scale));
                compositor.RecreateTextFormats(scale);
                renderer.SetScale(scale);
                const auto theme = MakeTheme(dark, HexColor(0x0078D4));
                renderer.UpdateBrushes(theme);
                renderer.text_background_ = theme.bg;
                renderer.icon_cache_.SetDeviceContext(compositor.Dc());
                renderer.painter_.BeginFrame(theme, false);
                WindowViewModel vm;
                vm.dark = dark;
                PaneViewModel pane;
                pane.path = L"C:\\资料"; pane.header_text = L"资料";
                pane.view_mode = mode;
                pane.selected_index = 1; pane.selected_count = 1;
                const wchar_t* names[] = {L"项目资料", L"设计素材", L"照片归档", L"工作文档", L"视频素材",
                    L"下载整理", L"共享资料", L"历史备份", L"空文件夹"};
                const wchar_t* sizes[] = {L"12.8 GB · 索引估算", L"4.62 GB", L"86.3 GB", L"328 MB", L"24.6 GB · 更新中",
                    L"计算中…", L"点击统计", L"8.4 GB · 部分统计", L"0 B"};
                for (int i = 0; i < 9; ++i) {
                    ListEntryView e;
                    e.name = names[i]; e.is_dir = true; e.path = pane.path + L"\\" + names[i];
                    pane.entries.push_back(std::move(e)); pane.folder_size_labels[i] = sizes[i];
                }
                pane.folder_size_actions.insert(6);
                pane.folder_size_actions.insert(0);
                ViewLayout geometry(mode, {0, 0, static_cast<float>(width)*scale, 550*scale}, 9, 0, 0, scale);
                const auto name_bounds = geometry.NameRect(0);
                const auto size_bounds = geometry.FolderSizeRect(0);
                // e922dbc: icon grids keep a sized folder's name to one line; the
                // size takes the second line's slot.
                const bool icon_grid = mode == ViewMode::ExtraLargeIcons || mode == ViewMode::LargeIcons ||
                                       mode == ViewMode::MediumIcons;
                const float name_floor = icon_grid ? name_bounds.top + kIconGridNameLineDip * scale : name_bounds.bottom;
                const float min_height = (icon_grid ? kIconGridNameLineDip : 21.0f) * scale;
                const bool fits = mode == ViewMode::Details || size_bounds.top + 0.01f >= name_floor &&
                    size_bounds.bottom <= geometry.ItemRect(0).bottom + 0.01f &&
                    size_bounds.bottom-size_bounds.top + 0.01f >= min_height;
                log << (fits ? "[PASS] " : "[FAIL] ") << "capacity line fits below name within cell\n";
                ok &= fits;
                auto* dc = compositor.Dc();
                dc->BeginDraw(); dc->Clear(theme.bg);
                const auto bounds = D2D1::RectF(8 * scale, 8 * scale, (width - 8.0f) * scale, 542 * scale);
                renderer.DrawSinglePane(vm, pane, bounds,
                    0, true, false, theme);
                const bool drawn = SUCCEEDED(dc->EndDraw());
                const auto shot = root / (std::wstring(ViewModeName(mode)) + L"-" + (width == 440 ? std::wstring(L"narrow-") : std::wstring()) +
                    std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark.png" : L"-light.png"));
                const bool saved = drawn && compositor.SaveSnapshot(shot.c_str());
                log << (saved ? "[PASS] " : "[FAIL] ") << "size states render mode=" << ViewModeIndex(mode) << " scale=" << scale << " dark=" << dark << std::endl;
                ok &= saved;
                if (width == 920) {
                    // Exercise production hit-testing with a real pane slot, away from chrome.
                    const auto hit_bounds = D2D1::RectF(240 * scale, 120 * scale, 900 * scale, 540 * scale);
                    PaneSlotView slot; slot.rect = hit_bounds; slot.pane = pane; slot.focused = true;
                    vm.pane_slots = {slot};
                    const auto list = renderer.PaneListRect(pane, hit_bounds);
                    ViewLayout positions(mode, list, pane.EntryCount(), 0, 0, scale);
                    const int action = mode == ViewMode::Tiles ? 6 : 0;
                    const auto name = positions.NameRect(action);
                    const auto columns = renderer.DetailsColumns(list, pane);
                    const auto cell = positions.ItemRect(action);
                    const float hit_x = mode == ViewMode::Details ? columns.Left(MainRenderer::ColumnKind::Size) + 4 * scale : name.left + 4 * scale;
                    const float hit_y = mode == ViewMode::Details ? (cell.top + cell.bottom) / 2 : name.top + 32 * scale;
                    const auto hit = renderer.HitTest(vm, D2D1::RectF(0, 0, 920 * scale, 550 * scale),
                        hit_x, hit_y);
                    const bool clickable = hit.region == HitTestResult::RowFolderSize && hit.index == action;
                    log << (clickable ? "[PASS] " : "[FAIL] ") << "manual size label hit-tests independently of folder open\n";
                    ok &= clickable;
                }
            }
        }
        DestroyWindow(hwnd);
        return ok;
    }
};
}
namespace pulse::app {
bool RunFolderSizeIndexClientTest(const std::filesystem::path& fixture, std::ofstream& log);
bool RunFolderSizesTest() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return false;
    compat::EnableDpiAwareness();
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const auto root = std::filesystem::current_path() / L"bench_data" / L"folder-sizes";
    const auto fixture = root / (L"fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(fixture / L"nested" / L"child");
    std::filesystem::create_directory(fixture / L"empty");
    std::ofstream log(root / L"results.log");
    bool ok = true;
    auto check = [&](bool passed, const char* label) {
        log << (passed ? "[PASS] " : "[FAIL] ") << label << std::endl; ok &= passed;
    };
    auto file = [&](const std::filesystem::path& p, size_t bytes) {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << std::string(bytes, 'x');
    };
    auto wait = [&](const std::function<bool()>& predicate) {
        const auto deadline = GetTickCount64() + 6000;
        while (GetTickCount64() < deadline) { if (predicate()) return true; Sleep(10); }
        return predicate();
    };
    file(fixture / L"nested" / L"a.bin", 1234);
    file(fixture / L"nested" / L"child" / L"b.bin", 5678);
    const auto nested = (fixture / L"nested").wstring();
    const auto empty = (fixture / L"empty").wstring();
    const auto missing = (fixture / L"missing").wstring();
    const auto cache = (fixture / L"cache.json").wstring();
    {
        FolderSizes service;
        service.SetIndexEnabled(false);
        service.SetCachePath([cache] { return cache; });
        service.Sync({{nested}, {empty}, {missing}, {L"\\\\not-a-server\\share", false}}, {});
        check(service.Get(L"\\\\not-a-server\\share").state == FolderSizeState::Manual,
              "network folder remains manual without issuing a scan");
        check(wait([&] { return service.Get(nested).state == FolderSizeState::Ready; }) && service.Get(nested).bytes == 6912,
              "nested logical file sizes are summed exactly");
        check(wait([&] { return service.Get(empty).state == FolderSizeState::Ready; }) &&
            service.Get(empty).has_value && service.Get(empty).bytes == 0 && format::ByteSize(0) == L"0 B",
              "empty directory is a known zero, not unknown");
        check(wait([&] { return service.Get(missing).state == FolderSizeState::Unavailable; }) && !service.Get(missing).has_value,
              "unreadable or missing root never becomes a false zero");
        file(fixture / L"nested" / L"child" / L"b.bin", 1000);
        service.Invalidate((fixture / L"nested" / L"child" / L"b.bin").wstring());
        const auto stale = service.Get(nested);
        check(stale.has_value && stale.bytes == 6912 && stale.state == FolderSizeState::Cached &&
            stale.source == FolderSizeSource::Scan && !stale.verified,
              "invalidation retains the published scan as unverified cache, separate from work progress");
        check(wait([&] { const auto v = service.Get(nested); return v.state == FolderSizeState::Ready && v.bytes == 2234; }),
              "changed descendant refreshes its parent total");
        for (int i = 0; i < 30; ++i) service.Sync({{i % 2 ? nested : empty}}, {});
        service.Sync({{empty}}, {});
        check(wait([&] { return service.Get(empty).state == FolderSizeState::Ready; }) && service.Get(empty).bytes == 0,
              "rapid scope changes cannot publish a previous directory total into the new one");
        service.Stop();
    }
    {
        FolderSizes loaded;
        loaded.SetIndexEnabled(false);
        loaded.SetCachePath([cache] { return cache; });
        loaded.Sync({{nested, false}}, {});
        check(wait([&] { return loaded.Get(nested).has_value; }) && loaded.Get(nested).bytes == 2234 &&
            loaded.Get(nested).state == FolderSizeState::Cached,
              "disk cache survives restart and is explicitly stale before validation");
        loaded.Calculate(nested);
        check(wait([&] { return loaded.Get(nested).state == FolderSizeState::Ready; }),
              "manual calculate starts work and replaces cached status");
        loaded.Stop();
    }
    const auto offline = fixture / L"nested" / L"offline";
    std::filesystem::create_directory(offline); file(offline / L"skipped.bin", 9000);
    if (SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE)) {
        FolderSizes partial; partial.SetIndexEnabled(false); partial.Sync({{nested}}, {});
        check(wait([&] { return partial.Get(nested).state == FolderSizeState::Partial; }) && partial.Get(nested).bytes == 2234,
              "offline subtree is skipped without discarding readable bytes or reporting a complete total");
        partial.Stop(); SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_DIRECTORY);
    } else log << "[SKIP] filesystem cannot create offline-directory fixture" << std::endl;
    {
        FolderSizes watched;
        watched.SetIndexEnabled(false);
        watched.Sync({{nested}}, {nested});
        check(wait([&] { return watched.Get(nested).state == FolderSizeState::Ready; }),
              "recursive watcher and initial scan settle together");
        file(fixture / L"nested" / L"child" / L"b.bin", 3333);
        check(wait([&] { const auto v = watched.Get(nested); return v.state == FolderSizeState::Ready && v.bytes == 13567; }),
              "real descendant write triggers automatic recalculation without UI refresh");
        watched.Stop();
    }
    {
        auto state = std::make_unique<AppState>();
        state->isolatedTest = true;
        state->appPrefs.persist = false;
        state->settings.BindUi(state->appPrefs, state->ctxMenuPrefs, state->index, state->networkIndex, {});
        ui::WindowViewModel vm;
        ui::PaneSlotView slot;
        slot.rect = D2D1::RectF(240, 120, 900, 550); slot.focused = true;
        slot.pane.path = fixture.wstring(); slot.pane.is_file_system = true;
        slot.pane.view_mode = ui::ViewMode::Tiles;
        // 91379bc: an empty folder shows 空/Empty instead of a size and skips the
        // size work, so the wiring is checked on a folder with content.
        const auto wired = fixture / L"wired";
        std::filesystem::create_directories(wired);
        file(wired / L"w.bin", 2048);
        const std::wstring wired_label = format::ByteSize(2048);
        ui::ListEntryView e; e.name = L"wired"; e.path = wired.wstring(); e.is_dir = true;
        slot.pane.entries.push_back(e);
        vm.pane_slots.push_back(slot);
        FillFolderSizes(*state, vm);
        check(wait([&] { FillFolderSizes(*state, vm); return vm.pane_slots[0].pane.folder_size_labels[0] == wired_label; }),
              "real view-model wiring requests and displays the last visible row, including a single-item folder");
        for (auto mode : {ui::ViewMode::MediumIcons, ui::ViewMode::LargeIcons, ui::ViewMode::ExtraLargeIcons, ui::ViewMode::Content}) {
            vm.pane_slots[0].pane.view_mode = mode;
            FillFolderSizes(*state, vm);
            check(vm.pane_slots[0].pane.folder_size_labels[0] == wired_label, "grid/content view receives actual folder total");
        }
        vm.pane_slots[0].pane.view_mode = ui::ViewMode::Details;
        FillFolderSizes(*state, vm);
        check(vm.pane_slots[0].pane.folder_size_labels[0] == wired_label, "details view receives folder total");
        vm.pane_slots[0].pane.is_file_system = false;
        vm.pane_slots[0].pane.is_search = true;
        vm.pane_slots[0].pane.path = L"pulse:search:test";
        FillFolderSizes(*state, vm);
        check(vm.pane_slots[0].pane.folder_size_labels[0] == wired_label, "search results use each folder's real path");
        auto& search_pane = vm.pane_slots[0].pane;
        search_pane.content_results = std::make_shared<index::ContentResultStore>(nullptr, 0);
        index::ContentHit hit;
        hit.path = nested + L"\\file.txt"; hit.name = L"file.txt";
        check(search_pane.content_results->Append({hit}), "content result fixture contains a paged hit");
        search_pane.snapshot = std::make_shared<const std::vector<fs::DirEntry>>();
        FillFolderSizes(*state, vm);
        check(search_pane.folder_size_labels.empty(), "paged content hits never index the empty directory snapshot");
        search_pane.snapshot.reset();
        search_pane.entries.clear();
        FillFolderSizes(*state, vm);
        check(search_pane.folder_size_labels.empty(), "paged content hits never index the empty entry vector");
        search_pane.content_results.reset();
        search_pane.entries.push_back(e);
        vm.pane_slots[0].pane.is_recycle = true;
        FillFolderSizes(*state, vm);
        check(vm.pane_slots[0].pane.folder_size_labels.empty(), "recycle results do not request folder sizes");
        // 91379bc: an empty folder says so, muted, instead of a size.
        auto& empty_pane = vm.pane_slots[0].pane;
        empty_pane.is_recycle = false; empty_pane.is_search = false; empty_pane.is_file_system = true;
        empty_pane.path = fixture.wstring(); empty_pane.view_mode = ui::ViewMode::Tiles;
        empty_pane.entries.clear();
        ui::ListEntryView empty_entry; empty_entry.name = L"empty"; empty_entry.path = empty; empty_entry.is_dir = true;
        empty_pane.entries.push_back(empty_entry);
        const std::wstring empty_label = pulse::l10n::Pick(L"空", L"Empty");
        check(wait([&] { FillFolderSizes(*state, vm);
                         return empty_pane.folder_size_labels[0] == empty_label && empty_pane.folder_size_muted.contains(0); }),
              "an empty folder says so instead of a size");
        state->folderSizes.Stop();
    }
    {
        // #58: a Size sort orders folders by their totals; unknown ones trail.
        auto entry = [](const wchar_t* name, bool dir, uint64_t size) {
            fs::DirEntry e; e.name = name; e.is_dir = dir; e.size = size; return e;
        };
        auto names = [](const std::vector<fs::DirEntry>& rows) {
            std::wstring out;
            for (const auto& e : rows) out += e.name + L",";
            return out;
        };
        const FolderSizeLookup sizes{{L"big", 9000}, {L"small", 10}, {L"mid", 500}};
        const auto saved_mode = CurrentFolderSortMode();
        SetFolderSortMode(FolderSortMode::FoldersFirst);
        std::vector<fs::DirEntry> rows{entry(L"Mid", true, 0), entry(L"zfile.bin", false, 700),
            entry(L"unknown", true, 0), entry(L"BIG", true, 0), entry(L"afile.bin", false, 5),
            entry(L"small", true, 0)};
        SortEntriesBySize(rows, ui::SortDirection::Asc, sizes);
        check(names(rows) == L"small,Mid,BIG,unknown,afile.bin,zfile.bin,",
              "size sort: folders ascend by their totals and an unknown folder trails");
        SortEntriesBySize(rows, ui::SortDirection::Desc, sizes);
        check(names(rows) == L"BIG,Mid,small,unknown,zfile.bin,afile.bin,",
              "size sort: descending order keeps the unknown folder last");
        SetFolderSortMode(FolderSortMode::Mixed);
        SortEntriesBySize(rows, ui::SortDirection::Asc, sizes);
        check(names(rows) == L"afile.bin,small,Mid,zfile.bin,BIG,unknown,",
              "size sort: mixed order interleaves folder totals with file sizes");
        const auto before = names(rows);
        bool threw = false;
        try { SortEntriesBySize(rows, ui::SortDirection::Desc, sizes, [] { throw 1; }); } catch (int) { threw = true; }
        check(threw && names(rows) == before, "size sort: an abandoned sort leaves the rows untouched");
        SetFolderSortMode(saved_mode);
        check(FolderSizeSignature({}) == 0 &&
            FolderSizeSignature(sizes) != FolderSizeSignature({{L"big", 9001}, {L"small", 10}, {L"mid", 500}}),
              "size sort: the signature notices a changed total");

        const auto sorted = fixture / L"sorted";
        std::filesystem::create_directories(sorted / L"Big");
        std::filesystem::create_directories(sorted / L"small");
        file(sorted / L"Big" / L"x.bin", 3000);
        file(sorted / L"small" / L"y.bin", 10);
        auto state = std::make_unique<AppState>();
        state->isolatedTest = true;
        state->appPrefs.persist = false;
        state->settings.BindUi(state->appPrefs, state->ctxMenuPrefs, state->index, state->networkIndex, {});
        ui::WindowViewModel vm;
        ui::PaneSlotView slot;
        slot.rect = D2D1::RectF(240, 120, 900, 550); slot.focused = true;
        slot.pane.path = sorted.wstring(); slot.pane.is_file_system = true;
        slot.pane.view_mode = ui::ViewMode::List;  // draws no folder sizes
        slot.pane.sort_column = ui::SortColumn::Size;
        auto listing = std::make_shared<std::vector<fs::DirEntry>>();
        listing->push_back(entry(L"Big", true, 0));
        listing->push_back(entry(L"small", true, 0));
        slot.pane.snapshot = listing;
        vm.pane_slots.push_back(slot);
        const auto known = [&] {
            FillFolderSizes(*state, vm);
            const auto children = state->folderSizes.KnownChildren(sorted.wstring() + L"\\");
            return children.size() == 2 && children.contains(L"big") && children.at(L"big") == 3000 &&
                children.contains(L"small") && children.at(L"small") == 10;
        };
        check(wait(known), "size sort: every folder is sized, shown or not, and known by its child name");
        check(vm.pane_slots[0].pane.folder_size_labels.empty(), "size sort: a view without size labels still draws none");
        state->folderSizes.Stop();
    }
    check(RunFolderSizeIndexClientTest(fixture, log), "folder size index IPC integration");
    check(ui::FolderSizesUiTest::Run(root, log), "actual tile renderer shows all size states");
    std::filesystem::remove_all(fixture);
    CoUninitialize();
    return ok;
}
}
#endif
