#include "app_internal.h"
#include "app_navigation.h"
#include "app_input.h"
#include "folder_sizes_ui.h"
#include "folder_size_store.h"
#include "../ui/view_layout.h"
#include "../common/text_format.h"
#include "../ui/ui_renderer.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <windowsx.h>

void Render(pulse::AppState&);
bool WaitForShotReady(pulse::AppState&);

// Explicit isolated shot hook only. It exercises real navigation, view models,
// hit testing and size-cell clicks, without touching files in the viewed tree.
int RunFolderSizeExperienceProbe(pulse::AppState& s, const wchar_t* output) {
    using namespace pulse;
    FILE* log = nullptr;
    if (_wfopen_s(&log, output, L"w") || !log) return 2;
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool ok, const char* label) {
        ++checks; if (!ok) ++failures;
        std::fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label); std::fflush(log);
    };
    const auto pump = [&](DWORD ms) {
        const auto until = GetTickCount64() + ms;
        do {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            ProcessPendingResults(s); Render(s); Sleep(10);
        } while (GetTickCount64() < until);
    };
    const auto ready = [&] {
        s.shot.start = std::chrono::steady_clock::now();
        return WaitForShotReady(s);
    };
    const auto labels = [&] {
        std::map<std::wstring, std::wstring> values;
        const auto vm = BuildVm(s);
        const auto* tab = ActiveTab(s);
        if (tab) for (const auto& [row, text] : vm.pane.folder_size_labels) {
            const auto path = EntryFullPath(*tab, row);
            if (s.folderSizes.Get(path).has_value) values[path] = text;
        }
        return values;
    };
    wchar_t sample_dir[32768]{};
    if (s.isolatedTest && s.shot.active &&
        GetEnvironmentVariableW(L"PULSE_TEST_FOLDER_SIZE_SAMPLE_DIR", sample_dir, ARRAYSIZE(sample_dir))) {
        NavigateTo(s, sample_dir);
        check(ready(), "isolated known-size fixture loads");
        pump(1500);
        const auto current = labels();
        const std::pair<const wchar_t*, uint64_t> expected[] = {
            {L"Completed", 65536}, {L"Empty", 0}, {L"Nested", 3145728}
        };
        for (const auto& [name, bytes] : expected) {
            const auto path = std::wstring(sample_dir) + L"\\" + name;
            const auto value = s.folderSizes.Get(path);
            check(value.has_value && value.bytes == bytes && !value.partial && value.verified,
                  "known file metadata produces a complete monitored total, including empty folders");
            const auto label = std::find_if(current.begin(), current.end(), [&](const auto& item) {
                return app::folder_size::Key(item.first) == app::folder_size::Key(path);
            });
            check(label != current.end() && label->second == format::ByteSize(bytes),
                  "real UI displays a readable complete total without estimates or cache suffixes");
        }
        Render(s);
        check(s.compositor.SaveSnapshot(s.shot.output.c_str()), "known-size presentation screenshot saved");
        std::fprintf(log, "[SUMMARY] checks=%u failures=%u\n", checks, failures);
        std::fclose(log);
        return failures ? 1 : 0;
    }
    wchar_t buffer[32768]{};
    GetEnvironmentVariableW(L"ProgramFiles(x86)", buffer, ARRAYSIZE(buffer));
    const std::wstring programs = buffer;
    if (programs.empty() || !s.isolatedTest || !s.shot.active) { std::fclose(log); return 3; }
    NavigateTo(s, programs);
    check(ready(), "real Program Files listing loads in isolated application");
    pump(4000);
    const auto baseline_labels = labels();
    bool compact_labels = true;
    for (const auto& [path, label] : baseline_labels) {
        const auto value = s.folderSizes.Get(path);
        compact_labels &= label.find(L'\u2248') == std::wstring::npos &&
            label.find(L'\u00b7') == std::wstring::npos &&
            (!value.partial || label.starts_with(L"\u2265 "));
    }
    check(!baseline_labels.empty() && compact_labels,
          "real published labels are compact numbers and preserve incomplete lower bounds");
    std::fprintf(log, "[INFO] initially published visible program-folder labels=%zu\n", baseline_labels.size());
    const auto before = s.folderSizes.ReadStats();
    check(s.compositor.SaveSnapshot((s.shot.output + L".programs.png").c_str()), "program directory screenshot saved");
    double slowest = 0;
    std::vector<double> navigation_ms, rendered_ms;
    std::map<std::wstring,std::wstring> root_labels;
    for (unsigned i = 0; i < 20; ++i) {
        GoUp(s);
        if (!ready()) { check(false, "Up navigation completed"); break; }
        pump(25);
        const auto current_root = labels();
        if (i==0) {
            root_labels=current_root;
            bool measured=false;
            for (const auto& [path,label] : root_labels)
                measured |= app::folder_size::Key(path)==app::folder_size::Key(programs);
            check(measured,"real measured Program Files value is present in the C drive UI (non-vacuous cache check)");
        }
        bool root_stable=true;
        for (const auto& [path,label] : root_labels) {
            const auto found=current_root.find(path);
            root_stable &= found!=current_root.end() && found->second==label;
        }
        check(root_stable,"actual C drive return retains previously published numeric labels");
        const auto start = std::chrono::steady_clock::now();
        GoBack(s);
        const bool loaded = ready();
        navigation_ms.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
        pump(25);
        rendered_ms.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
        slowest = (std::max)(slowest, rendered_ms.back());
        const auto current = labels();
        bool stable = loaded;
        for (const auto& [path, label] : baseline_labels) {
            const auto found = current.find(path);
            stable &= found != current.end() && found->second == label;
        }
        check(stable, "actual Up/Back keeps published visible size labels stable");
    }
    const auto after = s.folderSizes.ReadStats();
    check(after.jobs_started == before.jobs_started && after.entries_scanned == before.entries_scanned,
          "actual system-directory navigation starts no recursive scans");
    std::fprintf(log, "[METRIC] hot_back_including_pump_max_ms=%.3f jobs_delta=%llu entries_delta=%llu index_queries=%llu watches=%llu\n",
                 slowest, after.jobs_started-before.jobs_started, after.entries_scanned-before.entries_scanned,
                 after.index_queries, after.active_watches);
    if (!navigation_ms.empty()) {
        std::sort(navigation_ms.begin(), navigation_ms.end());
        std::sort(rendered_ms.begin(), rendered_ms.end());
        const auto p95 = (navigation_ms.size() * 95 + 99) / 100 - 1;
        std::fprintf(log, "[METRIC] navigation_ready_p50_ms=%.3f p95_ms=%.3f max_ms=%.3f rendered_p95_ms=%.3f\n",
            navigation_ms[navigation_ms.size()/2], navigation_ms[p95], navigation_ms.back(), rendered_ms[p95]);
    }
    GoUp(s); check(ready(), "C drive listing loads for size-cell interaction"); pump(500);
    check(s.compositor.SaveSnapshot((s.shot.output + L".root.png").c_str()), "C drive stable overview screenshot saved");
    auto* tab = ActiveTab(s);
    int row = -1;
    if (tab) for (size_t i=0; i<tab->EntryCount(); ++i)
        if (app::folder_size::Key(EntryFullPath(*tab, static_cast<int>(i))) == app::folder_size::Key(programs)) row=static_cast<int>(i);
    check(row >= 0, "Program Files row exists in real C drive view");
    bool clicked = false;
    if (row >= 0) {
        EnsureRowVisible(s, *tab, row); Render(s);
        const auto vm = BuildVm(s);
        for (size_t slot_index=0; slot_index<vm.pane_slots.size() && !clicked; ++slot_index) {
            const auto& slot = vm.pane_slots[slot_index];
            if (!slot.focused) continue;
            const auto list = s.renderer.PaneListRect(slot.pane, slot.rect);
            ui::ViewLayout layout(slot.pane.view_mode, list, slot.pane.EntryCount(), slot.pane.scroll_x, slot.pane.scroll_y,
                                  s.scale, s.renderer.ListRowHeightDip(slot.pane, list), slot.pane.Groups());
            const auto [first,last]=layout.VisibleRange();
            for (int view=first; view<=last && !clicked; ++view) {
                if (slot.pane.SourceIndex(view)!=row) continue;
                const auto rect=layout.ItemRect(view);
                const int y=static_cast<int>((rect.top+rect.bottom)*0.5f);
                for (float x=list.right-24*s.scale; x>list.left && x>list.right-260*s.scale; x-=4*s.scale) {
                    const auto hit=s.renderer.HitTest(vm,{0,0,float(s.compositor.Width()),float(s.compositor.Height())},x,float(y));
                    if (hit.region!=ui::HitTestResult::RowFolderSize || hit.index!=row) continue;
                    const LPARAM point=MAKELPARAM(static_cast<int>(x),y);
                    const auto prior=s.folderSizes.Get(programs);
                    SendMessageW(s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
                    SendMessageW(s.hwnd, WM_LBUTTONUP, 0, point);
                    const auto work=s.folderSizes.GetWork(programs);
                    check(work.manual && work.Running(), "real size-cell click starts explicit full calculation");
                    check(!prior.has_value || s.folderSizes.Get(programs).bytes==prior.bytes,
                          "real click retains the published number");
                    s.hoverRegion=static_cast<int>(ui::HitTestResult::RowFolderSize);
                    s.hoverControlIndex=row; s.hoverPaneIndex=static_cast<int>(slot_index);
                    s.hoverPoint={static_cast<LONG>(x),y};
                    s.tooltipText=DescribeFolderSize(s,programs); Render(s);
                    const auto progress_vm = BuildVm(s);
                    check(!progress_vm.tooltip_text.empty() && progress_vm.pane.folder_size_running.contains(row),
                          "running work has an independent indicator and explanatory tooltip");
                    check(s.compositor.SaveSnapshot(s.shot.output.c_str()), "size progress tooltip screenshot saved");
                    if (s.folderSizes.GetWork(programs).Running()) {
                        SendMessageW(s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
                        SendMessageW(s.hwnd, WM_LBUTTONUP, 0, point);
                        check(s.folderSizes.GetWork(programs).activity==app::FolderSizeActivity::Cancelled,
                              "second real size-cell click cancels work without clearing value");
                    } else std::fprintf(log,"[INFO] full scan completed before second click; cancel UI not exercised\n");
                    clicked=true; break;
                }
            }
        }
    }
    check(clicked,"size action hit-tested inside actual rendered size column");
    s.folderSizes.Cancel(programs);
    std::fprintf(log,"[SUMMARY] checks=%u failures=%u\n",checks,failures);
    std::fclose(log);
    return failures ? 1 : 0;
}
