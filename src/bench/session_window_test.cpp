// session_window_test.cpp - Multi-window M0 baseline: the single-window session
// (window rect, maximized, layout tabs, tab groups, active tab) round-trips
// through SessionToJson / ParseSessionJson, and a frozen version-7 file keeps
// parsing to the same window. Session v8 (several windows) must keep these
// passing: old files migrate to one window, legacy fields mirror the focused one.
#include "../app/session.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace pulse::app;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

bool SameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

SessionSnapshot SampleSession() {
    SessionSnapshot s;
    s.window_rect = {120, 80, 1720, 1040};
    s.maximized = true;
    s.details_panel = true;
    s.details_panel_width = 360;

    LayoutTabSnapshot docs;
    docs.title = L"文档";
    docs.pinned = true;
    docs.group = 1;
    docs.layout = 0;
    PaneFolderSnapshot docs_pane;
    docs_pane.path = L"C:\\Users\\Example\\Documents";
    docs.panes.push_back(docs_pane);

    LayoutTabSnapshot work;
    work.title = L"";
    work.group = 1;
    work.layout = 1;
    work.focused = 1;
    work.split_ratios = {0.4f};
    PaneFolderSnapshot left, right;
    left.path = L"D:\\项目\\设计稿";
    left.view = pulse::ui::ViewMode::Details;
    right.path = L"\\\\nas\\share\\交付";
    work.panes = {left, right};

    s.layout_tabs = {docs, work};
    GroupSessionSnapshot group;
    group.id = 1;
    group.name = L"工作";
    group.color_rgb = 0x4CC2FF;
    s.tab_groups = {group};
    s.active_layout_tab = 1;
    return s;
}

void ExpectSampleWindow(const SessionSnapshot& got, const char* label) {
    std::string prefix = std::string(label) + ": ";
    auto check = [&](bool ok, const char* what) { Check(ok, (prefix + what).c_str()); };
    check(SameRect(got.window_rect, RECT{120, 80, 1720, 1040}), "window rect");
    check(got.maximized, "maximized");
    check(got.details_panel && got.details_panel_width == 360, "details panel state");
    check(got.layout_tabs.size() == 2, "two layout tabs");
    if (got.layout_tabs.size() != 2) return;
    const auto& docs = got.layout_tabs[0];
    const auto& work = got.layout_tabs[1];
    check(docs.title == L"文档" && docs.pinned && docs.group == 1, "first tab title, pin, group");
    check(docs.panes.size() == 1 && docs.panes[0].path == L"C:\\Users\\Example\\Documents",
          "first tab pane path");
    check(work.layout == 1 && work.focused == 1 && work.group == 1, "second tab layout, focus, group");
    check(work.panes.size() == 2 && work.panes[0].path == L"D:\\项目\\设计稿" &&
              work.panes[1].path == L"\\\\nas\\share\\交付",
          "second tab Unicode and UNC pane paths");
    check(work.split_ratios.size() == 1 && work.split_ratios[0] > 0.39f && work.split_ratios[0] < 0.41f,
          "split ratio");
    check(got.tab_groups.size() == 1 && got.tab_groups[0].id == 1 && got.tab_groups[0].name == L"工作" &&
              got.tab_groups[0].color_rgb == 0x4CC2FF,
          "tab group");
    check(got.active_layout_tab == 1, "active layout tab");
}

// ASCII-only C++ wide literal of `text` (one L"..." chunk per line), used to
// regenerate session_window_v7_fixture.inc with PULSE_SESSION_DUMP=1.
std::string ToWideLiteral(const std::wstring& text) {
    std::string out = "L\"";
    size_t column = 0;
    for (wchar_t ch : text) {
        char buf[12];
        if (ch == L'\\' || ch == L'"') std::snprintf(buf, sizeof(buf), "\\%c", static_cast<char>(ch));
        else if (ch == L'\n') std::snprintf(buf, sizeof(buf), "\\n");
        else if (ch >= 0x20 && ch < 0x7F) std::snprintf(buf, sizeof(buf), "%c", static_cast<char>(ch));
        else if (ch < 0xA0) std::snprintf(buf, sizeof(buf), "\\%03o", static_cast<unsigned>(ch));
        else if (ch >= 0xD800 && ch <= 0xDFFF)  // no \u for surrogates; end the chunk after \x
            std::snprintf(buf, sizeof(buf), "\\x%04X\" L\"", static_cast<unsigned>(ch));
        else std::snprintf(buf, sizeof(buf), "\\u%04X", static_cast<unsigned>(ch));
        out += buf;
        column += std::strlen(buf);
        if (ch == L'\n' || column > 90) { out += "\"\n    L\""; column = 0; }
    }
    out += "\"\n";
    return out;
}

// Written by SessionToJson at main 11a62b2 (session version 7) for
// SampleSession(); regenerate only together with a deliberate format change.
const wchar_t kFrozenV7[] =
#include "session_window_v7_fixture.inc"
    ;

}  // namespace

int main() {
    const SessionSnapshot sample = SampleSession();
    const std::wstring json = SessionToJson(sample);
    wchar_t dump[4]{};
    // PULSE_SESSION_DUMP=1: the writer's output (session_window_v7_fixture.inc);
    // =2: that output parsed and written again.
    if (GetEnvironmentVariableW(L"PULSE_SESSION_DUMP", dump, ARRAYSIZE(dump)) == 1) {
        SessionSnapshot reparsed;
        if (dump[0] == L'2') ParseSessionJson(json, reparsed);
        std::fputs(ToWideLiteral(dump[0] == L'2' ? SessionToJson(reparsed) : json).c_str(), stdout);
        return 0;
    }

    Check(json.find(L"\"version\":7") != std::wstring::npos, "writer emits session version 7");
    SessionSnapshot parsed;
    Check(ParseSessionJson(json, parsed), "round-trip JSON parses");
    ExpectSampleWindow(parsed, "round-trip");

    SessionSnapshot frozen;
    Check(ParseSessionJson(kFrozenV7, frozen), "frozen v7 file parses");
    ExpectSampleWindow(frozen, "frozen v7");
    // The parser normalizes defaults (an empty sidebarOrder becomes the default
    // order), so compare the rewritten forms rather than the raw writer output.
    Check(SessionToJson(frozen) == SessionToJson(parsed),
          "frozen v7 file and the current writer parse to the same state");

    SessionSnapshot empty;
    Check(ParseSessionJson(SessionToJson(SessionSnapshot{}), empty) && empty.layout_tabs.empty() &&
              !empty.maximized,
          "empty session round-trips as an empty window");

    std::printf("session_window_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
