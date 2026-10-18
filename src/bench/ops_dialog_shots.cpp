// Renders the progress card (pulse_ops_dialogs.html §1–§4) in each state to
// PNG and checks the 2.5 s auto-close. Usage: pulse_ops_dialog_shots [out-dir]
#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "../ui/file_operation_dialog.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

namespace pulse::ui {
struct FileOperationWindowTestAccess {
    static HWND Handle(FileOperationWindow& window) { return window.hwnd_; }
    static void Size(FileOperationWindow& window, UINT dpi, bool detailed) {
        window.detailed_ = detailed;
        RECT rect{0, 0, MulDiv(468, dpi, 96), MulDiv(detailed ? 452 : 348, dpi, 96)};
        SendMessageW(window.hwnd_, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&rect));
    }
    static void Graph(FileOperationWindow& window, float upto, double base, bool by_progress = true) {
        window.graph_.clear();
        window.graph_by_progress_ = by_progress;
        const int count = by_progress ? static_cast<int>(upto * 200.0f) : 90;
        for (int i = 0; i <= count; ++i) {
            const float x = by_progress ? upto * static_cast<float>(i) / static_cast<float>(count) : 0.0f;
            const double t = static_cast<double>(i);
            double v = base * (0.82 + 0.12 * std::sin(t * 0.21) + 0.06 * std::sin(t * 0.77));
            if (i < 8) v *= 0.25 + 0.75 * t / 8.0;  // warm-up ramp
            window.graph_.push_back({ x, v });
        }
    }
    static void Elapsed(FileOperationWindow& window, ULONGLONG ms, bool ended) {
        const ULONGLONG now = GetTickCount64();
        window.task_start_tick_ = now - ms;
        window.task_end_tick_ = ended ? now : 0;
    }
    static void Countdown(FileOperationWindow& window, ULONGLONG ago, bool hover) {
        window.countdown_base_ = GetTickCount64() - ago;
        window.countdown_hover_ = hover;
    }
    static bool Pending(FileOperationWindow& window) { return window.AutoClosePending(); }
    static bool Closing(FileOperationWindow& window) { return window.closing_; }
    static void Tick(FileOperationWindow& window, ULONGLONG now) { window.TickAutoClose(now); }
    static ULONGLONG Base(FileOperationWindow& window) { return window.countdown_base_; }
    static void Place(FileOperationWindow& window, int x, int y) {
        window.positioned_ = true;
        SetWindowPos(window.hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    static bool Snapshot(FileOperationWindow& window, const std::filesystem::path& path) {
        window.backdrop_enabled_ = false;
        window.Render();
        window.Render();
        return window.compositor_.SaveSnapshot(path.c_str());
    }
};
}

namespace {
int failures = 0;
void Check(bool pass, const char* label) {
    std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", label);
    if (!pass) ++failures;
}

pulse::ops::OpStatus Running(pulse::ops::OpType type, float percent) {
    using namespace pulse;
    ops::OpStatus st;
    st.task_id = 11;
    st.active = true;
    st.type = type;
    st.phase = ops::OpPhase::Running;
    st.percent = percent;
    st.source_label = L"Downloads";
    st.destination_label = L"D:\\Backup\\Projects 2026";
    st.current_item = L"raw\\2026-10-17\\DSC_04187.NEF";
    st.total_items = 2003;
    st.completed_items = static_cast<uint64_t>(2003 * percent / 100.0f);
    st.total_bytes = 18ull * 1024 * 1024 * 1024;
    st.transferred_bytes = static_cast<uint64_t>(st.total_bytes * percent / 100.0);
    st.bytes_per_second = 212.0 * 1024 * 1024;
    st.peak_bytes_per_second = 248.0 * 1024 * 1024;
    st.speed_basis = ops::OpSpeedBasis::Bytes;
    st.eta_seconds = 34;
    return st;
}
}

int wmain(int argc, wchar_t** argv) {
    using namespace pulse;
    using Access = ui::FileOperationWindowTestAccess;
    compat::EnableDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const wchar_t* locale = argc > 2 ? argv[2] : L"zh-CN";
    l10n::Initialize(GetModuleHandleW(nullptr), locale);
    std::filesystem::path output;
    if (argc > 1) { output = argv[1]; std::filesystem::create_directories(output); }

    int dismissed = 0;
    ui::FileOperationCallbacks callbacks;
    callbacks.dismiss = [&] { ++dismissed; };
    ui::FileOperationWindow window;
    Check(window.Create(nullptr, std::move(callbacks)), "create hidden operation window");
    const double mb = 1024.0 * 1024.0;

    auto shot = [&](const std::wstring& name) {
        if (output.empty()) return;
        Check(Access::Snapshot(window, output / (name + L".png")), "save screenshot");
    };

    struct Scale { UINT dpi; const wchar_t* tag; };
    const Scale scales[] = { {96, L"100"}, {144, L"150"}, {192, L"200"} };
    for (const bool dark : {true, false}) {
        window.SetTheme(dark, ui::HexColor(0x6474C7));
        const std::wstring theme = dark ? L"dark" : L"light";
        for (const auto& scale : scales) {
            const bool full = scale.dpi == 96;
            const std::wstring sfx = L"-" + theme + L"-" + scale.tag;
            Access::Size(window, scale.dpi, false);

            auto st = Running(ops::OpType::Copy, 63.0f);
            window.Update(st);
            Access::Graph(window, 0.63f, 212.0 * mb);
            Access::Elapsed(window, 61000, false);
            shot(L"01-running" + sfx);
            if (!full) continue;

            st.pause_requested = true;
            window.Update(st);
            shot(L"02-pausing" + sfx);
            st.phase = ops::OpPhase::Paused;
            window.Update(st);
            shot(L"03-paused" + sfx);
            st.pause_requested = false;
            st.phase = ops::OpPhase::WaitingForConflict;
            window.Update(st);
            shot(L"04-waiting" + sfx);

            auto done = Running(ops::OpType::Copy, 100.0f);
            done.active = false;
            done.phase = ops::OpPhase::Completed;
            window.Update(done);
            Access::Graph(window, 1.0f, 212.0 * mb);
            Access::Elapsed(window, 88000, true);
            Access::Countdown(window, 1100, false);
            shot(L"05-done-countdown" + sfx);
            Access::Countdown(window, 0, true);
            shot(L"06-done-hover" + sfx);
            Access::Countdown(window, 0, false);
            window.Hide();

            auto fail = Running(ops::OpType::Move, 41.0f);
            fail.task_id = 12;
            fail.active = false;
            fail.phase = ops::OpPhase::Failed;
            fail.last_error = L"Access is denied.";
            window.Update(fail);
            Access::Graph(window, 0.41f, 150.0 * mb);
            Access::Elapsed(window, 23000, true);
            shot(L"07-failed" + sfx);

            auto del = Running(ops::OpType::RecycleDelete, 37.0f);
            del.task_id = 13;
            del.destination_label.clear();
            del.speed_basis = ops::OpSpeedBasis::Items;
            del.items_per_second = 1480.0;
            del.peak_items_per_second = 2210.0;
            window.Update(del);
            Access::Graph(window, 0.37f, 1480.0);
            Access::Elapsed(window, 4200, false);
            shot(L"08-delete" + sfx);

            auto move = Running(ops::OpType::Move, 78.0f);
            move.task_id = 14;
            Access::Size(window, scale.dpi, true);
            window.Update(move);
            Access::Graph(window, 0.78f, 180.0 * mb);
            Access::Elapsed(window, 140000, false);
            shot(L"09-move-details" + sfx);
            Access::Size(window, scale.dpi, false);

            auto auth = Running(ops::OpType::Copy, 12.0f);
            auth.task_id = 15;
            auth.authorization = ops::AuthorizationState::ActionRequired;
            auth.can_skip_authorization = true;
            window.Update(auth);
            shot(L"10-authorization" + sfx);

            auto scan = Running(ops::OpType::Copy, -1.0f);
            scan.task_id = 16;
            scan.phase = ops::OpPhase::Scanning;
            scan.total_items = 0;
            scan.speed_basis = ops::OpSpeedBasis::None;
            window.Update(scan);
            shot(L"11-scanning" + sfx);
        }
    }

    // Auto-close: a success seen while visible counts down 2.5 s, then zooms
    // out and dismisses; hovering holds it; failures never auto-close.
    {
        window.SetTheme(true, ui::HexColor(0x6474C7));
        Access::Size(window, 96, false);
        Access::Place(window, -4000, -4000);  // keep the pointer off the card
        auto st = Running(ops::OpType::Copy, 50.0f);
        st.task_id = 21;
        window.Update(st);
        window.Show(false);
        Check(!Access::Pending(window), "no countdown while running");
        auto done = st;
        done.active = false;
        done.phase = ops::OpPhase::Completed;
        done.percent = 100.0f;
        window.Update(done);
        Check(Access::Pending(window), "countdown starts on a visible success");
        const ULONGLONG base = Access::Base(window);
        Access::Tick(window, base + 1200);
        Check(window.IsVisible() && !Access::Closing(window), "still open at 1.2 s");
        Access::Tick(window, base + 2600);
        Check(Access::Closing(window), "exit animation starts after 2.5 s");
        Access::Tick(window, base + 2600 + 250);
        Check(!window.IsVisible() && dismissed == 1, "closed and dismissed after the zoom");

        auto fail = st;
        fail.task_id = 22;
        window.Update(fail);
        window.Show(false);
        fail.active = false;
        fail.phase = ops::OpPhase::Failed;
        fail.last_error = L"Access is denied.";
        window.Update(fail);
        Check(!Access::Pending(window), "failure does not auto-close");
        window.Hide();

        auto late = done;
        late.task_id = 23;
        window.Update(late);
        window.Show(false);
        Check(!Access::Pending(window), "summary opened after the finish stays open");
        window.Hide();
    }

    std::printf("%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
    window.Destroy();
    CoUninitialize();
    return failures == 0 ? 0 : 1;
}
