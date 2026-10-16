// content_host_main.cpp - Pulse.Content.exe, the content search agent.
//
// Ships in the content enhancement pack (%LOCALAPPDATA%\Pulse\packs\content)
// next to Pulse.Document.exe, so installs that never search file contents
// carry neither. Only pulse.exe starts it, as the signed-in user, through
// ContentSearchClient; the PulseIndex service never does. pulse.exe checks
// size and SHA-256 of both files against the catalog compiled into it before
// every launch, which also pins this binary to the app's exact version.
#include "content_agent.h"
#include "../common/crash_reporter.h"
#include <windows.h>
#include <shellapi.h>
#include <cwchar>
#include <string>
#include <vector>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    if (argv) {
        args.reserve(static_cast<size_t>(argc));
        for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
        LocalFree(argv);
    }
    const std::wstring a1 = args.size() >= 2 ? args[1] : L"";
    pulse::crash::Initialize({pulse::crash::ProcessRole::ContentAgent, false, {}});

    using namespace pulse::index;
    const auto parent = [&] { return static_cast<DWORD>(wcstoul(args[3].c_str(), nullptr, 10)); };
    if (a1 == L"--content-instant-agent" && args.size() == 4)
        return RunPersistentContentAgent(args[2], parent(), ContentAgentMode::Instant);
    if (a1 == L"--content-index-agent" && args.size() == 4)
        return RunPersistentContentAgent(args[2], parent());
    if (a1 == L"--content-index-observer" && args.size() == 4)
        return RunPersistentContentAgent(args[2], parent(), true);
    if (a1 == L"--content-agent" && args.size() == 3) return RunContentAgent(args[2]);
    return ERROR_INVALID_PARAMETER;
}
