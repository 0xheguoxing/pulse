// ctx_menu_controller_test.cpp - ContextMenuController bookkeeping: the
// per-menu timing record (ctx_menu_timing.h).
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
