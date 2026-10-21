#include "ai_launcher.h"
#include "ai_pack.h"
#include "../common/localization.h"
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app::ai {
namespace {

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const noexcept { return h_; }
    explicit operator bool() const noexcept { return h_ && h_ != INVALID_HANDLE_VALUE; }
    void reset(HANDLE h = nullptr) noexcept {
        if (*this) CloseHandle(h_);
        h_ = h;
    }

private:
    HANDLE h_ = nullptr;
};

bool Usable(HANDLE h) { return h && h != INVALID_HANDLE_VALUE; }

// One line for the client's log. The text is canonical Simplified Chinese
// (service_text.cpp has the other languages) in the system UI language.
void Report(HINSTANCE instance, HANDLE err, std::wstring_view zh, DWORD code = 0) {
    if (!Usable(err)) return;
    l10n::Initialize(instance, L"system");
    std::wstring line = L"Pulse: " + l10n::ServiceText(zh);
    if (code) line += L" (" + std::to_wstring(code) + L")";
    line += L"\n";
    const int n = WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return;
    std::string utf8(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), utf8.data(), n, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(err, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

const wchar_t* UnusableText(AiPackState state) {
    switch (state) {
        case AiPackState::Outdated: return L"AI 增强包需要更新。请在 Pulse 的“设置 › 增强包”中更新后重试。";
        case AiPackState::Damaged: return L"AI 增强包已损坏。请在 Pulse 的“设置 › 增强包”中修复后重试。";
        case AiPackState::Unavailable: return L"这个版本的 Pulse 没有可用的 AI 增强包。";
        default: return L"未安装 AI 增强包。请在 Pulse 的“设置 › 增强包”中安装后重试。";
    }
}

std::wstring ModulePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return {};
        if (n < path.size()) { path.resize(n); return path; }
        if (path.size() >= 32768) return {};
        path.resize(path.size() * 2);
    }
}

// An inheritable duplicate, so exactly these handles reach the child.
Handle Inheritable(HANDLE source) {
    HANDLE copy = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &copy, 0, TRUE, DUPLICATE_SAME_ACCESS))
        return Handle{};
    return Handle{copy};
}

Handle InheritableNul() {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    return Handle{CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr)};
}

std::wstring Quote(const std::wstring& arg) { return L"\"" + arg + L"\""; }

} // namespace

bool IsMcpStdioLaunch(int argc, wchar_t** argv) {
    return argc == 2 && argv && argv[1] && wcscmp(argv[1], L"--mcp-stdio") == 0;
}

int RunMcpStdioLauncher(HINSTANCE instance) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (!Usable(in) || !Usable(out)) return kLauncherNoStdio;

    const AiPackState state = QueryAiPackState();   // holds the verified file until exit
    const std::wstring host = AiPackReady(state) ? LocateAiHost() : std::wstring{};
    if (host.empty()) {
        Report(instance, err, UnusableText(state));
        return kLauncherPackUnusable;
    }
    const std::wstring pipe = AiToolPipeName();
    const std::wstring self = ModulePath();
    const size_t slash = host.find_last_of(L'\\');
    const std::wstring directory = slash == std::wstring::npos ? std::wstring{} : host.substr(0, slash);

    Handle child_in = Inheritable(in);
    Handle child_out = Inheritable(out);
    Handle child_err = Usable(err) ? Inheritable(err) : InheritableNul();
    if (!child_in || !child_out || !child_err || self.empty()) {
        Report(instance, err, L"无法启动 Pulse.AI.exe", GetLastError());
        return kLauncherStartFailed;
    }
    HANDLE inherit[] = {child_in.get(), child_out.get(), child_err.get()};
    SIZE_T list_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &list_size);
    std::vector<unsigned char> list_buffer(list_size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_buffer.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &list_size)) {
        Report(instance, err, L"无法启动 Pulse.AI.exe", GetLastError());
        return kLauncherStartFailed;
    }
    struct ListGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~ListGuard() { DeleteProcThreadAttributeList(list); }
    } list_guard{list};
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr)) {
        Report(instance, err, L"无法启动 Pulse.AI.exe", GetLastError());
        return kLauncherStartFailed;
    }

    // Kill-on-close: when the client kills this launcher, Pulse.AI.exe goes too.
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        Report(instance, err, L"无法启动 Pulse.AI.exe", GetLastError());
        return kLauncherStartFailed;
    }

    std::wstring command = Quote(host) + L" --mcp --server " + Quote(self);
    if (!pipe.empty()) command += L" --pipe " + Quote(pipe);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = child_in.get();
    startup.StartupInfo.hStdOutput = child_out.get();
    startup.StartupInfo.hStdError = child_err.get();
    startup.lpAttributeList = list;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(host.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                        directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &process)) {
        Report(instance, err, L"无法启动 Pulse.AI.exe", GetLastError());
        return kLauncherStartFailed;
    }
    Handle child{process.hProcess};
    Handle thread{process.hThread};
    if (!AssignProcessToJobObject(job.get(), child.get())) {
        const DWORD error = GetLastError();
        TerminateProcess(child.get(), kLauncherStartFailed);
        Report(instance, err, L"无法启动 Pulse.AI.exe", error);
        return kLauncherStartFailed;
    }
    ResumeThread(thread.get());
    thread.reset();
    // The child has its own copies now.
    child_in.reset();
    child_out.reset();
    child_err.reset();
    WaitForSingleObject(child.get(), INFINITE);
    DWORD exit_code = kLauncherStartFailed;
    GetExitCodeProcess(child.get(), &exit_code);
    return static_cast<int>(exit_code);
}

} // namespace pulse::app::ai
