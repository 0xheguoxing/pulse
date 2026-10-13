// ctx_menu_controller_test.cpp - ContextMenuController bookkeeping: the
// per-menu timing record (ctx_menu_timing.h), static-verb re-reads and rows
// painted from the cache while the live answer arrives in parts.
#include "../app/context_menu_controller.h"
#include "../app/ctx_menu_timing.h"

#include <windows.h>
#include <cstdio>
#include <string>

namespace pulse::app { std::wstring GetPulseDataDir() { return {}; } }

namespace {

int failures = 0;

void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}

pulse::ops::ShellMenuItem Item(uint32_t id, const wchar_t* text) {
    pulse::ops::ShellMenuItem item;
    item.id = id;
    item.text = text;
    return item;
}

pulse::ops::ShellMenuItem Row(uint32_t id, const wchar_t* clsid, const wchar_t* verb,
                              const wchar_t* text, bool parent = false, bool child = false) {
    pulse::ops::ShellMenuItem item;
    item.id = id;
    item.clsid = clsid;
    item.verb = verb;
    item.text = text;
    item.has_children = parent;
    item.child = child;
    return item;
}

const pulse::ops::ShellMenuItem* RowNamed(const std::vector<pulse::ops::ShellMenuItem>& items,
                                          const wchar_t* text) {
    for (const auto& item : items)
        if (item.text == text) return &item;
    return nullptr;
}

bool Predicted(uint32_t id) {
    return id >= pulse::app::ContextMenuController::kPredictedIdFirst &&
           id <= pulse::app::ContextMenuController::kPredictedIdLast;
}

bool Contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

int wmain() {
    using namespace pulse;
    app::ContextMenuPrefs prefs;
    prefs.persist = false;

    auto start = [&](app::ContextMenuController& controller) {
        app::ContextMenuController::ShellOperations operations;
        operations.query = [](std::vector<std::wstring>, HWND, bool, bool,
                              std::vector<std::wstring>) { return 7u; };
        controller.SetShellOperations(std::move(operations));
        controller.StartQuery(prefs, nullptr, { L"C:\\fixture\\a.dwg" }, false, L".dwg",
                              false, {}, {});
    };

    {
        // Menu opened before the host answered; the answer grows it once.
        app::ContextMenuController controller;
        start(controller);
        controller.OpenMenu({});
        controller.BeginTiming(12, 5, false, 1000);
        controller.NoteDisplayUpdate(5, 1040);   // ids patched, same rows
        controller.CompleteComQuery(7, { Item(1, L"A"), Item(2, L"B") }, 1180, false);
        controller.NoteDisplayUpdate(7, 1180);   // rows added on screen
        controller.CloseMenu();
        app::CtxMenuTiming timing;
        Check(controller.TakeTiming(true, timing), "a begun record is taken once");
        Check(timing.key == L".dwg" && !timing.background, "record names the menu's type");
        Check(timing.input_to_open_ms == 12 && timing.rows_open == 5 && !timing.seeded,
              "record keeps the open-time facts");
        Check(!timing.live_at_open && timing.live_after_open_ms == 180,
              "final answer after open is timed from the open");
        Check(timing.refreshes == 2 && timing.grew == 1 && timing.rows_final == 7,
              "only an update that changes the row count counts as growth");
        Check(timing.shell_command, "record keeps whether an Explorer row ran");
        Check(timing.trace == "40:5 180:7", "trace lists every rebuild as ms-since-open:rows");
        app::CtxMenuTiming again;
        Check(!controller.TakeTiming(false, again), "a record is not taken twice");
    }
    {
        // Answer in before the popup: nothing may grow.
        app::ContextMenuController controller;
        start(controller);
        controller.CompleteComQuery(7, { Item(1, L"A") }, 900, false);
        controller.OpenMenu({});
        controller.BeginTiming(3, 6, true, 1000);
        controller.CloseMenu();
        app::CtxMenuTiming timing;
        Check(controller.TakeTiming(false, timing) && timing.live_at_open &&
                  timing.live_after_open_ms == -1 && timing.grew == 0 && timing.rows_final == 6,
              "an answer that beat the popup is recorded as live at open");
    }
    {
        // Partial answers never stamp the final arrival.
        app::ContextMenuController controller;
        start(controller);
        controller.OpenMenu({});
        controller.BeginTiming(0, 4, false, 1000);
        controller.CompleteComQuery(7, { Item(1, L"A") }, 1080, true);
        controller.CloseMenu();
        app::CtxMenuTiming timing;
        Check(controller.TakeTiming(false, timing) && timing.live_after_open_ms == -1,
              "a partial snapshot is not the final answer");
    }
    {
        // Re-reading a cached type: throttled, and a re-read that finds the same
        // verbs must not touch an open menu.
        app::ContextMenuController controller;
        start(controller);
        const app::StaticVerb edit{ L"edit", L"Edit", L"", L"", {}, 0 };
        const app::StaticVerb print{ L"print", L"Print", L"", L"", {}, 0 };
        Check(!controller.RequestStaticRevalidate(L".dwg", 0),
              "an uncached type is fetched, not re-read");
        Check(controller.RequestStaticPrefetch(L".dwg"), "first fetch is requested");
        Check(!controller.RequestStaticRevalidate(L".dwg", 0), "no re-read while a read runs");
        Check(controller.CompleteStaticVerbs(L".dwg", { edit }, controller.cache_generation()),
              "first rows change the menu");
        Check(!controller.RequestStaticPrefetch(L".dwg"), "a cached type is not fetched again");
        Check(controller.RequestStaticRevalidate(L".dwg", 10000), "a cached type is re-read");
        Check(!controller.CompleteStaticVerbs(L".dwg", { edit }, controller.cache_generation()),
              "a re-read with the same verbs changes nothing");
        Check(!controller.RequestStaticRevalidate(
                  L".dwg", 10000 + app::ContextMenuController::kStaticRevalidateMs - 1),
              "re-reads are throttled");
        Check(controller.RequestStaticRevalidate(
                  L".dwg", 10000 + app::ContextMenuController::kStaticRevalidateMs),
              "the next re-read is allowed after the interval");
        Check(controller.CompleteStaticVerbs(L".dwg", { edit, print },
                                             controller.cache_generation()) &&
                  controller.HasCachedStaticVerbs(L".dwg"),
              "a re-read that finds a new verb updates the menu and the cache");
        const uint32_t before = controller.cache_generation();
        Check(controller.RequestStaticRevalidate(L".dwg", 100000), "re-read before a flush");
        controller.InvalidateCaches();
        Check(!controller.CompleteStaticVerbs(L".dwg", { edit }, before) &&
                  !controller.HasCachedStaticVerbs(L".dwg"),
              "a re-read from before a registry flush is dropped");
        Check(!controller.RequestStaticRevalidate(L".dwg", 100001),
              "after a flush the type is fetched afresh, not re-read");
    }
    {
        // Painted from the cache, then the live answer in parts.
        app::ContextMenuController controller;
        uint32_t token = 10;
        uint32_t invoked = 0;
        app::ContextMenuController::ShellOperations operations;
        operations.query = [&](std::vector<std::wstring>, HWND, bool, bool,
                               std::vector<std::wstring>) { return ++token; };
        operations.invoke = [&](uint32_t, uint32_t id, std::wstring, std::wstring) {
            invoked = id;
        };
        controller.SetShellOperations(std::move(operations));
        const std::vector<ops::ShellMenuItem> complete = {
            Row(3, L"{A}", L"", L"Zip", true), Row(4, L"{A}", L"", L"to a.zip", false, true),
            Row(5, L"{A}", L"", L"to a.7z", false, true), Row(9, L"{B}", L"scan", L"Scan"),
            Row(12, L"{C}", L"", L"Send") };
        controller.StartQuery(prefs, nullptr, { L"C:\\f\\a.dwg" }, false, L".dwg", false, {}, {});
        controller.CompleteComQuery(token, complete, 100, false);
        controller.StartQuery(prefs, nullptr, { L"C:\\f\\b.dwg" }, false, L".dwg", false, {}, {});
        Check(controller.com_predicted() && controller.com_items().size() == complete.size(),
              "the next query of the type starts from the last complete answer");
        Check(!controller.SeedComItemsFromCache(), "seeding again at popup time changes nothing");
        bool synthetic = true;
        for (size_t i = 0; i < controller.com_items().size(); ++i) {
            const uint32_t id = controller.com_items()[i].id;
            synthetic &= Predicted(id) && (i == 0 || id != controller.com_items()[i - 1].id);
        }
        Check(synthetic, "painted rows carry their own ids, never the last menu's host ids");

        controller.CompleteComQuery(token, { Row(31, L"{B}", L"scan", L"Scan"),
                                             Row(40, L"{D}", L"", L"Late handler") }, 150, true);
        const auto& first = controller.com_items();
        Check(first.size() == complete.size() && RowNamed(first, L"Scan") &&
                  RowNamed(first, L"Scan")->id == 31 && Predicted(RowNamed(first, L"Send")->id),
              "a partial answer patches the rows it names and keeps the others");
        Check(!RowNamed(first, L"Late handler"), "rows the cache lacks wait for the full answer");

        controller.CompleteComQuery(token, { Row(20, L"{A}", L"", L"Zip", true),
                                             Row(21, L"{A}", L"", L"to b.zip", false, true),
                                             Row(31, L"{B}", L"scan", L"Scan") }, 200, true);
        Check(controller.com_items().size() == complete.size() &&
                  RowNamed(controller.com_items(), L"Zip")->id == 20,
              "a later partial keeps the row count too");

        const uint32_t send = RowNamed(controller.com_items(), L"Send")->id;
        const std::vector<ops::ShellMenuItem> final_rows = {
            Row(20, L"{A}", L"", L"Zip", true), Row(21, L"{A}", L"", L"to b.zip", false, true),
            Row(22, L"{A}", L"", L"to b.7z", false, true), Row(31, L"{B}", L"scan", L"Scan"),
            Row(50, L"{C}", L"", L"Send") };
        controller.ExecuteShellCommand(
            static_cast<int>(app::CmdShellComBase + send),
            [&] { controller.CompleteComQuery(token, final_rows, 300, false); }, {});
        Check(invoked == 50, "a painted row clicked before the answer runs the live command");

        controller.StartQuery(prefs, nullptr, { L"C:\\f\\c.dwg" }, false, L".dwg", false, {}, {});
        controller.CompleteComQuery(token, final_rows, 400, false);
        Check(controller.com_items().size() == final_rows.size() &&
                  RowNamed(controller.com_items(), L"to b.7z")->id == 22 &&
                  !Predicted(RowNamed(controller.com_items(), L"Send")->id),
              "the full answer replaces the painted rows");

        controller.StartQuery(prefs, nullptr, { L"C:\\f\\d.zip" }, false, L".zip", false, {}, {});
        Check(!controller.com_predicted() && !controller.SeedComItemsFromCache(),
              "an unseen type has nothing to paint");
        controller.CompleteComQuery(token, { Row(7, L"{B}", L"scan", L"Scan") }, 500, true);
        Check(controller.com_items().size() == 1 && controller.com_items()[0].id == 7,
              "without cached rows a partial answer is shown as it is");
    }
    {
        app::CtxMenuTiming timing;
        timing.key = L".d\"w\\g\x4E2D";
        timing.rows_open = 9;
        timing.grew = 2;
        timing.trace = "12:9 80:7";
        const std::string line = app::FormatCtxMenuTiming(timing, 42);
        Check(!line.empty() && line.back() == '\n' && line.find('\n') == line.size() - 1,
              "one record is one line");
        Check(Contains(line, "\"event\":\"ctx_menu\"") && Contains(line, "\"tick_ms\":42") &&
                  Contains(line, "\"rows_open\":9") && Contains(line, "\"grew\":2") &&
                  Contains(line, "\"trace\":\"12:9 80:7\""),
              "record carries the counters");
        Check(Contains(line, "\"key\":\".d\\u0022w\\u005cg\\u4e2d\""),
              "quotes, backslashes and non-ASCII in the key stay valid JSON");
    }
    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
