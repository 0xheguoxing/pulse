#include "app_input.h"
#include "../ui/ui_renderer.h"
#include <algorithm>
#include <cstdio>
#include <vector>
#include <tlhelp32.h>
#include <map>

void Render(pulse::AppState& state);
namespace pulse::app::hang { std::wstring ThreadStackForProbe(DWORD tid); }

namespace {
std::map<DWORD, uint64_t> ThreadCpuTimes() {
    std::map<DWORD, uint64_t> times;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return times;
    THREADENTRY32 entry{sizeof(entry)};
    for (BOOL found = Thread32First(snapshot, &entry); found; found = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId()) continue;
        const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (!thread) continue;
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetThreadTimes(thread, &created, &exited, &kernel, &user))
            times[entry.th32ThreadID] = ((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
                ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime);
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    return times;
}
}

// Explicit isolated --shot runs only. Measures the production rendering path.
int RunFolderThumbnailProbe(pulse::AppState& state, const wchar_t* output) {
    FILE* log = nullptr;
    if (_wfopen_s(&log, output, L"w") || !log) return 2;
    const bool baseline = GetEnvironmentVariableW(L"PULSE_TEST_FOLDER_BASELINE", nullptr, 0) > 0;
    state.renderer.SetFolderThumbnailsEnabled(!baseline);
    const auto pump = [&] {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    };
    const auto start = GetTickCount64();
    const auto cpu_time = [] {
        FILETIME created{}, exited{}, kernel{}, user{};
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        return ((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
            ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime);
    };
    const auto cpu_start = cpu_time();
    const auto ui_cpu_time = [] {
        FILETIME created{}, exited{}, kernel{}, user{};
        GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
        return ((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
            ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime);
    };
    IO_COUNTERS io_start{};
    GetProcessIoCounters(GetCurrentProcess(), &io_start);
    uint64_t first_bitmap = 0, settled_at = 0, first_size = 0, first_scan = 0;
    wchar_t scroll_text[32]{};
    const bool preview_trace = GetEnvironmentVariableW(L"PULSE_TEST_PREVIEW_SCROLL", scroll_text, ARRAYSIZE(scroll_text)) > 0;
    const float trace_scroll = preview_trace ? static_cast<float>(wcstod(scroll_text, nullptr)) : 0;
    bool trace_scrolled = false;
    size_t peak_indexed = 0;
    size_t last_resolved = 0;
    while (GetTickCount64() - start < (preview_trace ? 25000u : 8000u)) {
        pump();
        if (preview_trace && !trace_scrolled && GetTickCount64() - start >= 1500) {
            if (auto* active = pulse::ActiveTab(state)) {
                active->scroll_y = trace_scroll * state.scale;
                pulse::ClampScroll(state);
                trace_scrolled = true;
            }
        }
        Render(state);
        const auto current = state.renderer.FolderThumbnailDebugStats();
        const auto elapsed = GetTickCount64() - start;
        if (current.bitmaps && !first_bitmap) first_bitmap = elapsed;
        if (current.cached != last_resolved) {
            std::fprintf(log, "[LOAD] elapsed=%llu cached=%zu bitmaps=%zu pending=%zu\n",
                elapsed, current.cached, current.bitmaps, current.pending);
            last_resolved = current.cached;
        }
        if (current.visible && !current.pending && current.cached >= current.visible && !settled_at)
            settled_at = elapsed;
        {
            size_t indexed = 0;
            if (const auto* active = pulse::ActiveTab(state); active && active->snapshot) {
                for (const auto& entry : *active->snapshot) {
                    if (!entry.is_dir) continue;
                    const auto path = entry.full_path.empty() ? active->current_path + L"\\" + entry.name : entry.full_path;
                    const auto value = state.folderSizes.Get(path);
                    if (value.has_value && !first_size) first_size = elapsed;
                    if (value.source == pulse::app::FolderSizeSource::Scan &&
                        value.state == pulse::app::FolderSizeState::Ready && !first_scan) first_scan = elapsed;
                    indexed += value.source == pulse::app::FolderSizeSource::Index;
                }
            }
            peak_indexed = std::max(peak_indexed, indexed);
        }
        Sleep(15);
    }
    Render(state);
    const auto stats = state.renderer.FolderThumbnailDebugStats();
    IO_COUNTERS io_end{};
    GetProcessIoCounters(GetCurrentProcess(), &io_end);
    std::fprintf(log, "[LOAD-TIME] first_bitmap=%llu settled=%llu first_size=%llu ms after listing ready; cpu=%.1f ms main_read=%llu bytes\n",
        first_bitmap, settled_at, first_size, static_cast<double>(cpu_time() - cpu_start) / 10000.0,
        io_end.ReadTransferCount - io_start.ReadTransferCount);
    size_t size_known = 0, size_partial = 0, size_indexed = 0, size_scanned = 0, size_verified = 0;
    if (const auto* active = pulse::ActiveTab(state); active && active->snapshot) {
        for (const auto& entry : *active->snapshot) {
            if (!entry.is_dir) continue;
            const auto path = entry.full_path.empty() ? active->current_path + L"\\" + entry.name : entry.full_path;
            const auto value = state.folderSizes.Get(path);
            size_known += value.has_value; size_partial += value.partial;
            size_indexed += value.state == pulse::app::FolderSizeState::Indexed;
            size_scanned += value.source == pulse::app::FolderSizeSource::Scan;
            size_verified += value.verified;
        }
    }
    std::fprintf(log, "[SIZES] known=%zu partial=%zu indexed=%zu (listing folders, only visible rows requested)\n",
        size_known, size_partial, size_indexed);
    const auto size_stats = state.folderSizes.ReadStats();
    std::fprintf(log, "[SIZE-VERIFY] first_scan=%llu ms scanned=%zu verified=%zu peak_indexed=%zu entries=%llu subtree_hits=%llu jobs=%llu completed=%llu cancelled=%llu index_queries=%llu watch_gaps=%llu cache_items=%llu\n",
        first_scan, size_scanned, size_verified, peak_indexed, size_stats.entries_scanned, size_stats.subtree_hits,
        size_stats.jobs_started, size_stats.jobs_completed, size_stats.jobs_cancelled, size_stats.index_queries,
        size_stats.watch_gaps, size_stats.cache_items);
    std::fprintf(log, "[STATE] visible=%zu queued=%zu pending=%zu cached=%zu bitmaps=%zu failed=%zu blocked=%zu age=%llu running=%d context=%d error=%ls\n",
        stats.visible, stats.queued, stats.pending, stats.cached, stats.bitmaps, stats.failed, stats.blocked,
        stats.oldest_visible_ms, stats.running, stats.has_context, stats.error.c_str());
    const bool snapshot = state.compositor.SaveSnapshot(state.shot.output.c_str());
    std::fprintf(log, "[%s] production folder grid snapshot\n", snapshot ? "PASS" : "FAIL");
    const auto idle_start = GetTickCount64();
    const auto idle_cpu = cpu_time();
    const auto idle_ui = ui_cpu_time();
    const auto idle_threads = ThreadCpuTimes();
    while (GetTickCount64() - idle_start < 2000) { pump(); Sleep(15); }
    std::fprintf(log, "[IDLE] process_cpu=%.1f ui_thread_cpu=%.1f ms over %llu ms; ordinary message loop without forced paints\n",
        static_cast<double>(cpu_time() - idle_cpu) / 10000.0,
        static_cast<double>(ui_cpu_time() - idle_ui) / 10000.0, GetTickCount64() - idle_start);
    for (const auto& [id, cpu] : ThreadCpuTimes()) {
        const auto previous = idle_threads.find(id);
        if (previous == idle_threads.end() || cpu <= previous->second) continue;
        const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
        using Description = HRESULT(WINAPI*)(HANDLE, PWSTR*);
        const auto describe = reinterpret_cast<Description>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
        PWSTR name = nullptr;
        if (thread && describe) describe(thread, &name);
        std::fprintf(log, "[IDLE-THREAD] id=%lu cpu=%.1f ms name=%ls\n", id,
            static_cast<double>(cpu - previous->second) / 10000.0, name ? name : L"");
        if (name) LocalFree(name);
        if (thread) CloseHandle(thread);
        if (cpu - previous->second >= 1000000 && id != GetCurrentThreadId()) {
            const auto stack = pulse::app::hang::ThreadStackForProbe(id);
            std::fprintf(log, "[IDLE-STACK] thread=%lu\n%ls", id, stack.c_str());
        }
    }
    if (GetEnvironmentVariableW(L"PULSE_TEST_FOLDER_STOP_SIZES", nullptr, 0) > 0) {
        state.folderSizes.Stop();
        const auto stopped_start = GetTickCount64();
        const auto stopped_cpu = cpu_time(), stopped_ui = ui_cpu_time();
        while (GetTickCount64() - stopped_start < 2000) { pump(); Sleep(15); }
        std::fprintf(log, "[IDLE-SIZES-STOPPED] process_cpu=%.1f ui_thread_cpu=%.1f ms over %llu ms\n",
            static_cast<double>(cpu_time() - stopped_cpu) / 10000.0,
            static_cast<double>(ui_cpu_time() - stopped_ui) / 10000.0, GetTickCount64() - stopped_start);
        std::fclose(log);
        return snapshot ? 0 : 1;
    }
    auto* tab = pulse::ActiveTab(state);
    for (bool scrolling : {false, true}) {
        std::vector<double> times;
        for (int frame = 0; frame < 90; ++frame) {
            if (scrolling && tab) {
                tab->scroll_y = static_cast<float>((frame % 12) * 75) * state.scale;
                pulse::ClampScroll(state);
            }
            pump();
            Render(state);
            times.push_back(state.timing.draw_ms);
        }
        std::sort(times.begin(), times.end());
        std::fprintf(log, "[TIME] %s draw p50=%.3f ms p95=%.3f ms max=%.3f ms (90 frames, excludes Present)\n",
            scrolling ? "scrolling" : "warm", times[45], times[85], times.back());
    }
    std::fclose(log);
    return snapshot && (baseline || stats.bitmaps > 0) ? 0 : 1;
}
