#include "../app/app_internal.h"
#include "../app/search_query.h"
#include "../app/session.h"
#include "../common/localization.h"
#include <iostream>
#include <filesystem>
#include <fstream>

namespace pulse::index {
struct ContentRefreshTestPeer {
    static void Scope(ContentSearchClient& client, bool enabled) {
        client.mode_ = ContentAgentMode::Instant;
        client.config_.roots = enabled ? std::vector<ContentIndexRoot>{{L"C:\\fixture"}} : std::vector<ContentIndexRoot>{};
    }
    static void Generation(ContentSearchClient& client, uint64_t generation) { client.generation_ = generation; }
    static void Status(ContentSearchClient& client, uint64_t revision) {
        client.status_.revision = revision;
        client.status_.indexed_files = 1;
        client.status_.error = ERROR_NOT_SUPPORTED;
        client.status_.errors = 1;
        client.config_.roots = {{L"C:\\fixture"}};
    }
};
}

int LiveLatency(const wchar_t* document) {
    using namespace pulse;
    const auto base = std::filesystem::absolute(std::filesystem::path(L"../bench_data") /
        (L"live-latency-" + std::to_wstring(GetCurrentProcessId())));
    const auto root = base / L"files"; const auto profile = base / L"profile";
    std::filesystem::create_directories(root); std::filesystem::create_directories(profile);
    SetEnvironmentVariableW(L"LOCALAPPDATA", profile.c_str());
    for (int i = 0; i < 4000; ++i) std::ofstream(root / (std::to_wstring(i) + L".txt")) << "Background indexed document " << i << std::string(2048, 'x');
    auto owned = std::make_unique<AppState>(); auto& s = *owned;
    s.appPrefs.persist = false;
    s.hwnd = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
    auto& tab = *ActiveTab(s); const std::wstring query = L"content:合同";
    tab.current_path = app::MakeSearchPath(query);
    s.contentSearch.Start(nullptr, 0);
    index::ContentIndexConfig config; config.roots = {{root.wstring()}};
    s.contentSearch.Configure(config);
    auto wait = [&](auto ready, DWORD timeout) {
        const auto end = GetTickCount64() + timeout;
        while (GetTickCount64() < end) {
            index::ContentSearchUpdate update;
            while (s.contentSearch.TakeUpdate(update)) ApplyContentSearchUpdate(s, std::move(update));
            TickAddressSearch(s, GetTickCount64());
            if (ready()) return true;
            Sleep(20);
        }
        return false;
    };
    if (!wait([&] { const auto status = s.contentSearch.GetStatus(); return status.indexed_files == 4000 && !status.indexing && !status.pending_files; }, 60000)) {
        std::cout << "[FAIL] initial 4000-file indexing timed out\n"; return 1;
    }
    RequestSearchPage(s, tab, query, true);
    if (!wait([&] { return !tab.search_content_active; }, 10000)) return 1;
    int failures = 0;
    const auto file = root / L"new.xlsx";
    for (int i = 0; i < 3; ++i) {
        auto start = GetTickCount64();
        const bool copied = CopyFileW(document, file.c_str(), FALSE) != FALSE;
        if (!copied) std::cout << "CopyFile error=" << GetLastError() << '\n';
        const bool appeared = copied && wait([&] { return !tab.search_content_active && tab.EntryCount() == 1; }, 5000);
        auto elapsed = GetTickCount64() - start;
        std::cout << (appeared && elapsed < 3000 ? "[PASS] " : "[FAIL] ") << "save -> visible Excel result: " << elapsed << " ms\n" << std::flush;
        if (!appeared || elapsed >= 3000) ++failures;
        start = GetTickCount64();
        const bool removed = DeleteFileW(file.c_str()) != FALSE;
        const bool disappeared = removed && wait([&] { return !tab.search_content_active && tab.EntryCount() == 0; }, 5000);
        elapsed = GetTickCount64() - start;
        std::cout << (disappeared && elapsed < 3000 ? "[PASS] " : "[FAIL] ") << "delete -> result removed: " << elapsed << " ms\n" << std::flush;
        if (!disappeared || elapsed >= 3000) ++failures;
    }
    s.contentSearch.Stop(); DestroyWindow(s.hwnd); s.hwnd = nullptr;
    return failures ? 1 : 0;
}

int StableRefresh(pulse::AppState& s) {
    using namespace pulse;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        if (!ok) ++failures;
    };
    const auto root = std::filesystem::absolute(std::filesystem::path(L"../bench_data") /
        (L"stable-refresh-" + std::to_wstring(GetCurrentProcessId())));
    std::filesystem::create_directories(root);
    std::ofstream(root / L"first.txt") << "budget";
    auto& tab = *ActiveTab(s);
    tab.current_path = app::MakeSearchPath(L"content:budget");
    tab.content_results.reset(); tab.search_preserve_selection.clear();
    index::ContentSearchClient client;
    client.Start(nullptr, 0, false);
    uint64_t generation = 300000;
    auto run = [&](const std::wstring& path, bool refresh) {
        index::ContentSearchRequest request;
        request.generation = ++generation; request.root = path;
        request.needle = L"budget"; request.indexed = false; request.paged_results = true;
        request.sort = index::ContentResultSort::Name;
        if (refresh) request.previous_results = tab.content_results;
        tab.pending_generation = generation; tab.search_content_active = true;
        const auto previous = tab.content_results;
        client.SearchAsync(std::move(request));
        bool done = false, interim = false; DWORD error = ERROR_TIMEOUT;
        const auto deadline = GetTickCount64() + 10000;
        while (!done && GetTickCount64() < deadline) {
            index::ContentSearchUpdate update;
            while (client.TakeUpdate(update)) {
                done = update.progress.done; error = update.progress.error;
                interim |= refresh && !done;
                ApplyContentSearchUpdate(s, std::move(update));
            }
            if (!done) {
                if (refresh && tab.content_results != previous) interim = true;
                Sleep(5);
            }
        }
        check(done && !interim, "background refresh publishes only a completed replacement");
        return error;
    };
    check(run(root.wstring(), false) == ERROR_SUCCESS && tab.EntryCount() == 1,
        "initial streamed content result appears");
    auto original = tab.content_results;
    index::ContentResultStore::Row row;
    const auto deadline = GetTickCount64() + 5000;
    while (!original->Get(0, row) && GetTickCount64() < deadline) Sleep(5);
    tab.SelectOnly(0); tab.scroll_y = 40;
    const auto selection = tab.selection_revision;
    tab.search_index_revision = 20;
    index::ContentRefreshTestPeer::Status(s.contentSearch, 21);
    s.contentStatusTick = 0; s.addressLiveDue = 0;
    TickAddressSearch(s, GetTickCount64());
    check(tab.content_results == original && tab.EntryCount() == 1 && !tab.loading && tab.IsSelected(0),
        "index revision starts refresh without clearing or disabling the displayed row");
    check(run(root.wstring(), true) == ERROR_SUCCESS && tab.content_results == original &&
        tab.selection_revision == selection && tab.scroll_y == 40,
        "unchanged results retain the same store, selection and scroll position");
    check(run((root / L"missing").wstring(), true) != ERROR_SUCCESS && tab.content_results == original && tab.EntryCount() == 1,
        "failed refresh keeps existing matches instead of publishing zero results");
    std::ofstream(root / L"second.txt") << "budget new";
    check(run(root.wstring(), true) == ERROR_SUCCESS && tab.content_results != original && tab.EntryCount() == 2,
        "new matching file replaces the completed result list");
    original = tab.content_results;
    std::ofstream(root / L"first.txt") << "budget revised contents";
    check(run(root.wstring(), true) == ERROR_SUCCESS && tab.content_results != original && tab.EntryCount() == 2,
        "changed metadata and snippet update even when the result count stays the same");
    std::filesystem::remove(root / L"first.txt"); std::filesystem::remove(root / L"second.txt");
    check(run(root.wstring(), true) == ERROR_SUCCESS && tab.EntryCount() == 0,
        "successful refresh removes genuinely deleted matches");
    client.Stop();
    std::filesystem::remove(root);
    return failures;
}

int SessionTransitions(pulse::AppState& s) {
    using namespace pulse;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        if (!ok) ++failures;
    };
    auto& tab = *ActiveTab(s);
    tab = app::Tab{};
    tab.current_path = app::MakeSearchPath(L"content:合同");
    index::ContentSearchRequest request;
    request.generation = 400001; request.paged_results = true;
    tab.pending_generation = request.generation; tab.search_content_active = true;
    index::ContentSearchSession initial(request, nullptr, 0);
    auto batch = [&](uint64_t generation, bool done, DWORD error, bool hits) {
        index::ContentSearchUpdate update;
        update.progress.generation = generation; update.progress.done = done; update.progress.error = error;
        if (hits) for (int i = 0; i < 9; ++i)
            update.hits.push_back({L"C:\\fixture\\" + std::to_wstring(i) + L".txt", L"合同.txt", L"合同"});
        return update;
    };
    auto deliver = [&](std::optional<index::ContentSearchUpdate> update) {
        if (update) ApplyContentSearchUpdate(s, std::move(*update));
    };
    auto progress = initial.Accept(batch(request.generation, false, 0, false));
    check(progress && !progress->results, "empty progress carries no replacement list");
    deliver(std::move(progress));
    deliver(initial.Accept(batch(request.generation, false, 0, true)));
    check(tab.EntryCount() == 9 && tab.search_content_active, "nine matches appear before completion");
    const auto original = tab.content_results;
    deliver(initial.Fail(ERROR_NOT_SUPPORTED));
    check(tab.EntryCount() == 9 && tab.content_results == original && !tab.search_content_active &&
        !tab.content_count_final, "error 50 after first results preserves all nine matches as partial");
    check(!initial.Accept(batch(request.generation, true, 0, false)), "completed request rejects duplicate final reply");
    for (DWORD error : {DWORD(ERROR_NOT_SUPPORTED), DWORD(ERROR_BROKEN_PIPE), DWORD(ERROR_CANCELLED)}) {
        request.previous_results = original; ++request.generation;
        tab.pending_generation = request.generation; tab.search_content_active = true;
        index::ContentSearchSession refresh(request, nullptr, 0);
        check(!refresh.Accept(batch(request.generation, false, 0, false)), "refresh progress leaves published list intact");
        deliver(refresh.Fail(error));
        check(tab.EntryCount() == 9 && tab.content_results == original,
            "failed or cancelled refresh cannot replace matches with an empty list");
    }
    ++request.generation; tab.pending_generation = request.generation; tab.search_content_active = true;
    index::ContentSearchSession refresh(request, nullptr, 0);
    check(!refresh.Accept(batch(request.generation - 1, true, 0, false)) && !refresh.Done(),
        "stale completion cannot finish the new request");
    ApplyContentSearchUpdate(s, batch(request.generation - 1, true, 0, false));
    check(tab.EntryCount() == 9 && tab.search_content_active, "UI rejects late completion from old query");
    deliver(refresh.Accept(batch(request.generation, true, 0, false)));
    check(tab.EntryCount() == 0 && tab.content_count_final, "successful empty refresh removes obsolete matches");
    ApplyContentSearchUpdate(s, batch(0, true, 0, true));
    check(tab.EntryCount() == 0, "generation zero cannot populate a completed search");
    tab.content_results.reset();
    tab.search_entries.reset();
    tab.pending_generation = ++request.generation;
    ApplyContentSearchUpdate(s, batch(request.generation, true, 0, true));
    RequestSearchPage(s, tab, L"content:合同", true);
    ApplyContentSearchUpdate(s, batch(tab.pending_generation, true, ERROR_NOT_SUPPORTED, false));
    check(tab.EntryCount() == 9 && !tab.search_retaining_results && !tab.loading,
        "legacy batch list also survives error 50 through the shared refresh entry point");
    return failures;
}

int AutomaticRefresh(pulse::AppState& s) {
    using namespace pulse;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        if (!ok) ++failures;
    };
    const auto logs = std::filesystem::absolute(std::filesystem::path(L"../bench_data") /
        (L"automatic-refresh-" + std::to_wstring(GetCurrentProcessId())));
    SetEnvironmentVariableW(L"PULSE_CONTENT_TIMING_DIR", logs.c_str());
    auto& tab = *ActiveTab(s);
    tab = app::Tab{};
    tab.current_path = app::MakeSearchPath(L"content:refresh-private-query");
    index::ContentRefreshTestPeer::Status(s.contentSearch, 1);
    RequestSearchPage(s, tab, L"content:refresh-private-query", true);
    const auto generation = tab.pending_generation;
    const auto session_id = tab.search_session_id;
    const auto next_request = s.nextIndexReq;
    index::ContentSearchRequest request;
    request.generation = generation; request.paged_results = true; request.task_scan = true;
    index::ContentSearchSession session(request, nullptr, 0);
    auto deliver = [&](bool done, bool delta, std::vector<index::ContentHit> hits) {
        index::ContentSearchUpdate update;
        update.progress.generation = generation; update.progress.done = done;
        update.progress.delta = delta; update.progress.live = true;
        update.hits = std::move(hits);
        if (auto accepted = session.Accept(std::move(update))) ApplyContentSearchUpdate(s, std::move(*accepted));
    };
    index::ContentHit first;
    first.path = L"C:\\fixture\\before.txt"; first.name = L"before.txt";
    deliver(false, false, {first});
    const auto original = tab.content_results;
    const auto selection = tab.selection_revision;
    for (auto reason : {RefreshReason::ShellNotification, RefreshReason::OperationCompleted,
                         RefreshReason::FileChange, RefreshReason::Background}) {
        RefreshActiveTab(s, reason);
        check(tab.pending_generation == generation && tab.search_live_generation == generation &&
            tab.search_session_id == session_id && s.nextIndexReq == next_request && tab.search_content_active,
            "automatic refresh during initial scan preserves its generation and subscription");
        check(tab.content_results == original && tab.EntryCount() == 1 && !tab.loading &&
            tab.selection_revision == selection, "automatic refresh preserves visible rows without a loading flash");
    }
    deliver(true, false, {});
    RefreshActiveTab(s, RefreshReason::OperationCompleted);
    check(!tab.search_content_active && !tab.pending_generation && tab.search_live_generation == generation &&
        s.nextIndexReq == next_request && tab.content_results == original,
        "automatic refresh after completion does not schedule another scan");
    auto renamed = first;
    renamed.path = L"C:\\fixture\\after.txt"; renamed.name = L"after.txt";
    first.removed = true;
    deliver(true, true, {first, renamed});
    index::ContentResultStore::Row row;
    const auto deadline = GetTickCount64() + 3000;
    while (!original->Get(0, row) && GetTickCount64() < deadline) Sleep(5);
    check(tab.content_results == original && tab.EntryCount() == 1 && row.entry.full_path == renamed.path &&
        tab.search_live_generation == generation, "retained subscription applies rename changes in the same store");
    renamed.removed = true;
    deliver(true, true, {renamed});
    check(tab.content_results == original && tab.EntryCount() == 0 && tab.search_live_generation == generation,
        "retained subscription removes deleted matches without restarting");
    renamed.removed = false;
    deliver(true, true, {renamed});
    RefreshActiveTab(s);
    check(tab.pending_generation > generation && tab.search_content_active &&
        tab.search_session_id == session_id && tab.content_results == original && tab.EntryCount() == 1,
        "explicit refresh starts a new generation while retaining published rows at dispatch");
    const auto explicit_generation = tab.pending_generation;
    RequestSearchPage(s, tab, L"content:changed-private-query", true);
    check(tab.pending_generation > explicit_generation, "query condition changes still start a new generation");
    for (const DWORD scan_error : {DWORD(ERROR_SUCCESS), DWORD(ERROR_NOT_SUPPORTED)}) {
        RequestSearchPage(s, tab, L"content:changed-private-query", true);
        const auto active_generation = tab.pending_generation;
        index::ContentSearchUpdate initial;
        initial.progress.generation = active_generation; initial.progress.done = initial.progress.live = true;
        initial.progress.error = scan_error; initial.progress.scanned_files = initial.progress.total_files = 77;
        initial.results = original;
        ApplyContentSearchUpdate(s, std::move(initial));
        const bool complete_count = tab.content_count_final;
        index::ContentSearchUpdate paused;
        paused.progress.generation = active_generation - 1;
        paused.progress.delta = paused.progress.done = true;
        paused.progress.subscription_error = ERROR_NOTIFY_ENUM_DIR;
        paused.progress.subscription_failure = index::ContentSubscriptionFailure::WatchOverflow;
        ApplyContentSearchUpdate(s, paused);
        check(tab.search_live_generation == active_generation && !tab.content_subscription_error,
            "stale subscription failure cannot pause the current search");
        paused.progress.generation = active_generation;
        ApplyContentSearchUpdate(s, std::move(paused));
        check(!tab.search_live_generation && !tab.pending_generation && !tab.search_content_active &&
            tab.content_subscription_error == ERROR_NOTIFY_ENUM_DIR && tab.content_scan_error == scan_error &&
            tab.content_count_final == complete_count && tab.content_scanned_files == 77 && tab.content_total_files == 77 &&
            tab.content_results == original && tab.EntryCount() == 1,
            "subscription pause preserves initial completion/error, counts and results while clearing live state");
        const auto before_notice = s.nextIndexReq;
        RefreshActiveTab(s, RefreshReason::OperationCompleted);
        check(s.nextIndexReq == before_notice && tab.content_subscription_error == ERROR_NOTIFY_ENUM_DIR,
            "automatic notification cannot silently rescan a paused subscription");
        RefreshActiveTab(s);
        check(tab.pending_generation > active_generation && !tab.content_subscription_error &&
            tab.content_subscription_failure == index::ContentSubscriptionFailure::None,
            "explicit refresh clears subscription pause and starts a new search");
    }
    std::wcout << L"Session diagnostics: " << logs.wstring() << std::endl;
    return failures;
}

int RestoreContentSearch(pulse::AppState& s) {
    using namespace pulse;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        if (!ok) ++failures;
    };
    s.isolatedTest = true;
    s.searchHistory.persist = false;
    s.places.persist = false;
    index::ContentRefreshTestPeer::Status(s.contentSearch, 1);
    app::AdvancedSearchSpec spec;
    spec.content = L"3d3s";
    const auto query = app::CompileSearchQuery(spec);
    const auto path = app::MakeSearchPath(query);
    auto& initial = *ActiveTab(s);
    initial.current_path = path;
    auto rows = std::make_shared<std::vector<fs::DirEntry>>(1);
    initial.SetSnapshot(rows);
    initial.search_content_active = true;
    initial.pending_generation = initial.search_live_generation = 12;
    s.addressLiveDue = 100;
    SuspendContentSearches(s);
    check(initial.search_content_stopped && initial.snapshot == rows && !s.addressLiveDue &&
        !initial.pending_generation && !initial.search_live_generation,
        "closing stops active content search and retains in-memory rows");

    const auto captured = app::CaptureLayoutTab(*s.window_tabs.Active());
    std::vector<app::LayoutTabSnapshot> decoded;
    check(app::ParseLayoutTabs(app::LayoutTabsToJson({captured}), decoded),
        "saved search session survives the real session JSON round trip");
    decoded[0].layout = 1; // Restoring a missing second pane also clones the search path.
    const auto before = s.nextIndexReq;
    s.pane = nullptr;
    app::RestoreWindowTabs(s.window_tabs, decoded, {}, 0,
        [&](app::Tab& tab, const std::wstring& saved) {
            StartLoadingPath(s, tab, saved, PathLoadReason::RestoreSession);
        });
    s.pane = s.window_tabs.Active()->FocusedPane();
    check(s.window_tabs.Active()->panes.size() == 2 && s.nextIndexReq == before,
        "session restore and split-pane cloning dispatch no search requests");
    for (auto& pane : s.window_tabs.Active()->panes) {
        const auto& tab = pane->view;
        check(tab.current_path == path && tab.search_content_stopped && !tab.loading &&
            !tab.pending_generation && !tab.search_live_generation && !tab.search_content_active &&
            tab.snapshot && tab.EntryCount() == 0 &&
            tab.banner_title == l10n::Get(l10n::StringId::ContentSearchStopped),
            "restored content pane keeps the query and presents a stopped empty view");
    }
    ui::WindowViewModel vm;
    FillAddressSearchView(s, vm);
    check(vm.address_search_content && vm.address_search_text == L"3d3s" && !s.addressLiveDue,
        "restoring the visible query does not queue live typing search");
    for (auto reason : {RefreshReason::Background, RefreshReason::ShellNotification,
                         RefreshReason::OperationCompleted, RefreshReason::FileChange}) {
        RefreshActiveTab(s, reason);
        check(s.nextIndexReq == before && ActiveTab(s)->search_content_stopped,
            "automatic refresh cannot restart a restored stopped search");
    }
    MaybePrefetchSearchPage(s);
    TickAddressSearch(s, GetTickCount64() + 1000);
    check(s.nextIndexReq == before && ActiveTab(s)->search_content_stopped,
        "status polling and paging leave a restored search stopped");
    RefreshActiveTab(s);
    check(s.nextIndexReq > before && ActiveTab(s)->search_content_active &&
        !ActiveTab(s)->search_content_stopped &&
        s.window_tabs.Active()->panes[1]->view.search_content_stopped == false,
        "explicit path refresh restarts only panes displaying that search");

    auto& tab = *ActiveTab(s);
    StartLoadingPath(s, tab, path, PathLoadReason::RestoreSession);
    const auto before_enter = s.nextIndexReq;
    s.hwndAddressEdit = CreateWindowExW(0, L"EDIT", L"3d3s", WS_CHILD,
        0, 0, 100, 24, s.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    s.addressSearching = s.addressSearchContent = true;
    s.addressSearchCurrent = false;
    s.addressSearchRoot.clear();
    SubmitAddressSearch(s, true);
    check(s.nextIndexReq == before_enter && tab.search_content_stopped,
        "unchanged live input does not resume a stopped query");
    SubmitAddressSearch(s, false);
    check(s.nextIndexReq > before_enter && tab.search_content_active && !tab.search_content_stopped,
        "pressing Enter with the unchanged query explicitly restarts search");
    StartLoadingPath(s, tab, path, PathLoadReason::RestoreSession);
    const auto before_edit = s.nextIndexReq;
    SetWindowTextW(s.hwndAddressEdit, L"changed-query");
    SubmitAddressSearch(s, true);
    check(s.nextIndexReq > before_edit && tab.search_content_active && !tab.search_content_stopped,
        "typing a changed query still starts a new search");
    DestroyWindow(s.hwndAddressEdit); s.hwndAddressEdit = nullptr;
    s.addressSearching = false;
    s.addressHistoryDue = 0;

    s.savedSearches.Add({L"restored content", app::SavedSearchMode::Content, L"C:\\fixture", L"3d3s"});
    const auto saved_path = L"pulse:saved-search:" + std::to_wstring(s.savedSearches.items().size() - 1);
    const auto before_saved = s.nextIndexReq;
    StartLoadingPath(s, tab, saved_path, PathLoadReason::RestoreSession);
    RefreshActiveTab(s, RefreshReason::FileChange);
    check(s.nextIndexReq == before_saved && tab.search_content_stopped && !tab.search_content_active,
        "saved content searches also restore without scanning");
    RefreshActiveTab(s);
    check(s.nextIndexReq > before_saved && tab.search_content_active && !tab.search_content_stopped,
        "explicit refresh can restart a saved content search");
    const auto before_name = s.nextIndexReq;
    StartLoadingPath(s, tab, app::MakeSearchPath(L"readme"), PathLoadReason::RestoreSession);
    check(s.nextIndexReq > before_name && !tab.search_content_stopped,
        "restored filename searches retain their existing loading behavior");
    return failures;
}

int ScopeRecovery(pulse::AppState& s) {
    using namespace pulse;
    s.isolatedTest = true;
    s.searchHistory.persist = false;
    s.places.persist = false;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        if (!ok) ++failures;
    };
    ULONGLONG tick = GetTickCount64();
    for (const auto& query : {std::wstring(L"path:C:\\fixture content:marker"), std::wstring(L"content:marker")}) {
        auto& tab = *ActiveTab(s);
        tab = app::Tab{};
        tab.current_path = app::MakeSearchPath(query);
        index::ContentRefreshTestPeer::Scope(s.contentSearch, false);
        RequestSearchPage(s, tab, query, true);
        check(!tab.search_content_active && !tab.banner_title.empty(), "disabled scope blocks content search");
        TickAddressSearch(s, tick += 1000);
        check(!tab.search_content_active, "disabled scope is not repeatedly searched");
        index::ContentRefreshTestPeer::Scope(s.contentSearch, true);
        TickAddressSearch(s, tick += 1000);
        check(tab.search_content_active && tab.pending_generation && tab.banner_title.empty(),
            "restored scope retries blocked search without an index revision");
        const auto generation = tab.pending_generation;
        TickAddressSearch(s, tick += 1000);
        check(tab.pending_generation == generation, "active search is not restarted on the next tick");
        MarkContentSearchStopped(tab);
        TickAddressSearch(s, tick += 1000);
        check(!tab.search_content_active, "explicitly stopped search stays stopped");
    }
    return failures ? 1 : 0;
}

int wmain(int argc, wchar_t** argv) {
    const bool scope_only = argc == 2 && std::wstring_view(argv[1]) == L"--scope-recovery";
    const bool automatic_only = argc == 2 && std::wstring_view(argv[1]) == L"--automatic-refresh";
    const bool restore_only = argc == 2 && std::wstring_view(argv[1]) == L"--restore-content";
    if (argc == 2 && !automatic_only && !restore_only && !scope_only) return LiveLatency(argv[1]);
    using namespace pulse;
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << std::endl;
        if (!ok) ++failures;
    };
    auto owned = std::make_unique<AppState>(); auto& s = *owned;
    s.appPrefs.persist = false;
    s.hwnd = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
    if (scope_only) {
        l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
        const int result = ScopeRecovery(s);
        DestroyWindow(s.hwnd); s.hwnd = nullptr;
        return result;
    }
    if (restore_only) {
        l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
        const int result = RestoreContentSearch(s);
        DestroyWindow(s.hwnd); s.hwnd = nullptr;
        return result ? 1 : 0;
    }
    if (automatic_only) {
        const int result = AutomaticRefresh(s);
        DestroyWindow(s.hwnd); s.hwnd = nullptr;
        return result ? 1 : 0;
    }
    auto& tab = *ActiveTab(s);
    const std::wstring query = L"content:合同";
    tab.current_path = app::MakeSearchPath(query);
    auto status = [&](uint64_t revision) { index::ContentRefreshTestPeer::Status(s.contentSearch, revision); };
    auto complete = [&](bool found) {
        index::ContentSearchUpdate update;
        update.progress.generation = tab.pending_generation; update.progress.done = true;
        if (found) {
            index::ContentHit hit; hit.path = L"C:\\fixture\\new.xlsx"; hit.name = L"new.xlsx";
            hit.snippet = L"合同"; update.hits.push_back(std::move(hit));
        }
        ApplyContentSearchUpdate(s, std::move(update));
    };
    ULONGLONG tick = 0;
    auto poll = [&] { tick += 1000; TickAddressSearch(s, tick); };
    status(10);
    RequestSearchPage(s, tab, query, true); complete(false);
    check(tab.EntryCount() == 0 && tab.search_index_revision == 10, "initial search is empty");
    poll(); const auto initial = s.nextIndexReq;
    check(!tab.search_content_active, "unchanged index does not repeat the query");
    const auto text = ContentIndexStatusText(s);
    status(11); poll();
    check(text == ContentIndexStatusText(s) && tab.search_content_active && s.nextIndexReq > initial,
        "new revision refreshes despite unchanged error text");
    status(12); const auto in_flight = s.nextIndexReq; poll();
    check(s.nextIndexReq == in_flight && tab.search_index_revision == 11,
        "new changes remain pending while the query is active");
    complete(true);
    check(tab.EntryCount() == 1 && tab.EntryAt(0).full_path == L"C:\\fixture\\new.xlsx",
        "completed query displays the newly matching Excel file");
    poll();
    check(tab.search_content_active && tab.search_index_revision == 12 && !tab.search_preserve_selection.empty(),
        "deferred update runs after completion and preserves selection");
    complete(true); status(13); s.addressLiveDue = tick + 10000; poll();
    check(!tab.search_content_active && tab.search_index_revision == 12, "typing defers refresh without consuming the change");
    s.addressLiveDue = 0; poll();
    check(tab.search_content_active && tab.search_index_revision == 13, "refresh resumes after typing ends");
    complete(true); status(14); tab.search_allow_scan = true; poll();
    check(!tab.search_content_active, "explicit scans are not silently rerun");
    tab.search_allow_scan = false; tab.search_content_empty = true; poll();
    check(!tab.search_content_active, "cleared search remains empty");
    tab.search_content_empty = false; s.contentStatusText.clear(); poll();
    check(tab.search_content_active && tab.search_index_revision == 14, "first status observation also refreshes stale results");
    complete(false);
    check(tab.EntryCount() == 0, "removed matches disappear after refresh");
    status(15); s.renameIndex = 0; poll();
    check(!tab.search_content_active && tab.search_index_revision == 14, "live index changes cannot move the file being renamed");
    s.renameIndex = -1; poll(); complete(false);
    check(tab.search_index_revision == 15, "pending index change refreshes when renaming ends");
    auto other = std::make_unique<app::Pane>(); auto& other_tab = *other->ActiveTab();
    other_tab.current_path = L"C:\\fixture";
    other_tab.search_content_active = true; other_tab.pending_generation = 200;
    s.window_tabs.Active()->panes.push_back(std::move(other));
    index::ContentRefreshTestPeer::Generation(s.contentSearch, 200);
    status(16); poll();
    check(!tab.search_content_active && tab.search_index_revision == 15, "another pane's current query is not cancelled by auto refresh");
    index::ContentRefreshTestPeer::Generation(s.contentSearch, 201); poll();
    check(tab.search_content_active && tab.search_index_revision == 16, "an obsolete query flag in another pane does not block updates");
    complete(false);
    failures += StableRefresh(s);
    failures += SessionTransitions(s);
    DestroyWindow(s.hwnd); s.hwnd = nullptr;
    return failures ? 1 : 0;
}
