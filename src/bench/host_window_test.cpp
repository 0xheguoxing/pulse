// host_window_test.cpp - Multi-window M2: the hidden host window owns the
// single-instance endpoint. A second process forwards an open request through
// the real SingleInstanceCoordinator to the host; the host stays hidden, out of
// the taskbar, survives WM_CLOSE, and (unlike a message-only window) receives
// broadcasts such as TaskbarCreated.
#include "../app/host_window.h"
#include "../app/single_instance_coordinator.h"

#include <cstdio>
#include <string>

namespace {

using Coordinator = pulse::app::SingleInstanceCoordinator;
using pulse::app::HostWindow;

int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

void Pump(DWORD ms) {
    const auto deadline = GetTickCount64() + ms;
    do {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(2);
    } while (GetTickCount64() < deadline);
}

HANDLE Spawn(const std::wstring& arguments) {
    wchar_t exe[32768]{};
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    std::wstring command = L"\"" + std::wstring(exe) + L"\" " + arguments;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &startup, &process))
        return nullptr;
    CloseHandle(process.hThread);
    return process.hProcess;
}

// Pumps the host while the child forwards; true when the child exits with 0.
bool WaitChild(HANDLE process) {
    if (!process) return false;
    const auto deadline = GetTickCount64() + 10000;
    while (WaitForSingleObject(process, 0) == WAIT_TIMEOUT && GetTickCount64() < deadline) Pump(5);
    DWORD code = 1;
    GetExitCodeProcess(process, &code);
    if (code == STILL_ACTIVE) {
        TerminateProcess(process, 2);
        WaitForSingleObject(process, 2000);
    }
    CloseHandle(process);
    return code == 0;
}

UINT g_broadcast = 0;
int g_message_only_broadcasts = 0;
LRESULT CALLBACK MessageOnlyProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == g_broadcast) ++g_message_only_broadcasts;
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 4 && std::wstring_view(argv[1]) == L"--sender") {
        Coordinator sender;
        if (sender.Acquire(argv[2]) != Coordinator::AcquireResult::Existing) return 2;
        return sender.ForwardOpenPath(argv[3], 5000) ? 0 : 3;
    }

    const std::wstring endpoint = L"PulseHostWindowTest-" + std::to_wstring(GetCurrentProcessId());
    Coordinator primary;
    Check(primary.Acquire(endpoint) == Coordinator::AcquireResult::Primary, "test process owns a private endpoint");

    int opens = 0, consumed = 0, broadcasts = 0;
    std::wstring opened;
    g_broadcast = RegisterWindowMessageW(L"PulseHostWindowTest.Broadcast");
    HostWindow host;
    const bool created = host.Create(GetModuleHandleW(nullptr),
        [&](HWND hwnd, UINT message, WPARAM, LPARAM lparam) -> std::optional<LRESULT> {
            if (message == g_broadcast) ++broadcasts;
            if (message == WM_APP + 7) { ++consumed; return 42; }
            if (message != WM_COPYDATA) return std::nullopt;
            if (!primary.OwnsEndpoint(hwnd)) return FALSE;
            Coordinator::OpenRequest request;
            if (!Coordinator::DecodeOpenRequest(reinterpret_cast<COPYDATASTRUCT*>(lparam), request)) return FALSE;
            const auto accepted = primary.AcceptOpenRequest(request, GetTickCount64());
            if (accepted == Coordinator::OpenAcceptance::Invalid) return FALSE;
            if (accepted == Coordinator::OpenAcceptance::New) { ++opens; opened = request.path; }
            return TRUE;
        });
    Check(created && host.Hwnd() != nullptr, "host window created");
    const HWND hwnd = host.Hwnd();
    Check(!host.Create(GetModuleHandleW(nullptr), {}), "a second Create on the same host is refused");

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    Check(!IsWindowVisible(hwnd) && (style & WS_VISIBLE) == 0, "host is never visible");
    Check((ex_style & WS_EX_TOOLWINDOW) != 0 && (ex_style & WS_EX_APPWINDOW) == 0,
          "host stays out of the taskbar and Alt+Tab");
    Check(GetAncestor(hwnd, GA_PARENT) == GetDesktopWindow(), "host is a top-level window, not HWND_MESSAGE");

    Check(SendMessageW(hwnd, WM_APP + 7, 0, 0) == 42 && consumed == 1, "handler result is returned");
    Check(SendMessageW(hwnd, WM_APP + 8, 0, 0) == 0, "unhandled messages fall through to DefWindowProc");
    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    Check(IsWindow(hwnd) != FALSE, "WM_CLOSE does not destroy the host");

    // Broadcast reach: the host gets it, a message-only window does not.
    WNDCLASSW cls{};
    cls.lpfnWndProc = MessageOnlyProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"PulseHostWindowTestMessageOnly";
    RegisterClassW(&cls);
    const HWND message_only = CreateWindowW(cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                            nullptr, cls.hInstance, nullptr);
    PostMessageW(HWND_BROADCAST, g_broadcast, 0, 0);
    Pump(300);
    Check(broadcasts == 1, "host receives HWND_BROADCAST messages (TaskbarCreated)");
    Check(message_only && g_message_only_broadcasts == 0,
          "control: a message-only window misses the same broadcast");
    if (message_only) DestroyWindow(message_only);

    Check(primary.PublishEndpoint(hwnd), "endpoint published on the host");
    Check(Coordinator::FindPrimaryWindow(endpoint) == hwnd, "second launches resolve the host window");
    Check(primary.OwnsEndpoint(hwnd), "host owns the endpoint");

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(temp), temp);
    std::wstring folder = temp;
    while (folder.size() > 3 && folder.back() == L'\\') folder.pop_back();  // "x\" would escape the quote
    Check(WaitChild(Spawn(L"--sender " + endpoint + L" \"" + folder + L"\"")),
          "second process forwards an open request and gets acceptance");
    std::wstring expected;
    Coordinator::NormalizeLaunchPath(folder, expected);
    Check(opens == 1 && opened == expected, "host handler received the forwarded path once");

    host.Destroy();
    Check(host.Hwnd() == nullptr && !IsWindow(hwnd), "Destroy removes the host window");
    Check(Coordinator::FindPrimaryWindow(endpoint) == nullptr, "a destroyed host no longer resolves as the endpoint");
    Check(!WaitChild(Spawn(L"--sender " + endpoint + L" \"" + folder + L"\"")) && opens == 1,
          "forwarding fails after the host is gone (no stray delivery)");

    std::printf("host_window_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
