// ai_pack_test - the AI enhancement pack: the Pulse.AI.exe shell, its asset
// and catalog from pulse_pack_catalog (--key ai), installed by InstallPack
// from a fake server and verified like the content pack, then the app-side
// API (bundled host, no catalog, removal) with LOCALAPPDATA redirected to a
// scratch directory so the real packs folder is never touched.
#include "../app/ai_pack.h"
#include "../app/pack_installer.h"
#include "../common/preview_packs.h"
#include "../common/utf8_file.h"
#include <cstdio>
#include <cwchar>
#include <map>
#include <string>
#include <vector>

using namespace pulse;
using namespace pulse::app;

namespace {
int g_failed = 0, g_passed = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    ok ? ++g_passed : ++g_failed;
}

std::vector<uint8_t> ReadAll(const std::wstring& p) {
    std::vector<uint8_t> out;
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"rb") || !f) return out;
    uint8_t buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return out;
}

bool WriteAll(const std::wstring& p, const std::vector<uint8_t>& data) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"wb") || !f) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

std::wstring ModulePath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

std::wstring ModuleDirectory() {
    const std::wstring path = ModulePath();
    return path.substr(0, path.find_last_of(L"\\/"));
}

DWORD Run(std::wstring command) {
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &process)) return GetLastError() | 0x80000000u;
    WaitForSingleObject(process.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code;
}

// Copied away from the build directory (no Pulse.AI.exe beside it) and run
// with --unbundled: this test build carries no catalog, so the app sees the
// pack as unavailable and finds no host.
int RunUnbundled() {
    const bool ok = QueryAiPackState() == AiPackState::Unavailable && LocateAiHost().empty() &&
                    !AiPackInstallable(QueryAiPackState());
    return ok ? 0 : 3;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && !wcscmp(argv[1], L"--unbundled")) return RunUnbundled();

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring root = std::wstring(tmp) + L"pulse_ai_pack_test_" + std::to_wstring(GetCurrentProcessId());
    RemovePackTree(root);
    const std::wstring src = root + L"\\src", out = root + L"\\out", local = root + L"\\local";
    const std::wstring packs_root = local + L"\\Pulse\\packs";
    packs::CreateDirectoryChain(src);
    packs::CreateDirectoryChain(packs_root);
    // Everything below that reads PacksRoot() lands in the scratch directory.
    SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
    Check(packs::PacksRoot() == packs_root, "LOCALAPPDATA redirect isolates the packs root");

    // --- the Pulse.AI.exe shell built beside this test.
    const std::wstring host = ModuleDirectory() + L"\\Pulse.AI.exe";
    Check(Run(L"\"" + host + L"\" --version") == 0, "Pulse.AI.exe --version exits 0");
    Check(Run(L"\"" + host + L"\" --mcp") == ERROR_CALL_NOT_IMPLEMENTED, "Pulse.AI.exe --mcp: not implemented yet");
    Check(Run(L"\"" + host + L"\"") == ERROR_INVALID_PARAMETER, "Pulse.AI.exe without a mode is refused");

    // --- in the build directory the sibling host counts as bundled.
    Check(QueryAiPackState() == AiPackState::Bundled && AiPackReady(QueryAiPackState()),
          "sibling Pulse.AI.exe: Bundled");
    Check(_wcsicmp(LocateAiHost().c_str(), host.c_str()) == 0, "bundled host is located beside the app");
    Check(AiPackRelease().file_count == 0 && !wcscmp(AiPackRelease().key, L"ai"), "test build: key ai, no catalog");

    // --- without a sibling host and without a catalog: Unavailable.
    const std::wstring away = root + L"\\away", copy = away + L"\\ai_pack_test_copy.exe";
    packs::CreateDirectoryChain(away);
    Check(CopyFileW(ModulePath().c_str(), copy.c_str(), FALSE) && Run(L"\"" + copy + L"\" --unbundled") == 0,
          "no sibling host and no catalog: Unavailable, nothing located");

    // --- pulse_pack_catalog on a stand-in host: asset, catalog, manifest.
    std::vector<uint8_t> plain((900u << 10) + 7);
    uint32_t x = 4242;
    for (size_t i = 0; i < plain.size(); ++i) {
        x = x * 1664525u + 1013904223u;
        plain[i] = (i / 4096) % 2 ? static_cast<uint8_t>(x >> 24) : static_cast<uint8_t>(i / 4096);
    }
    WriteAll(src + L"\\Pulse.AI.exe", plain);
    const std::wstring base_url = L"https://packs.example.invalid/v9.8.7/";
    const std::wstring header = out + L"\\ai_pack_catalog.h";
    Check(Run(L"\"" + ModuleDirectory() + L"\\pulse_pack_catalog.exe\" --key ai --symbol Ai --version 9.8.7 "
              L"--channel windows --base-url " + base_url + L" --out-dir \"" + out + L"\" --header \"" + header +
              L"\" \"" + src + L"\\Pulse.AI.exe\"") == 0,
          "pulse_pack_catalog packs Pulse.AI.exe");
    const std::wstring asset = L"ai-windows-Pulse.AI.exe.lzms";
    const auto packed = ReadAll(out + L"\\" + asset);
    std::string catalog, manifest;
    {
        auto h = ReadAll(header), m = ReadAll(out + L"\\ai-pack-assets.json");
        catalog.assign(h.begin(), h.end());
        manifest.assign(m.begin(), m.end());
    }
    Check(!packed.empty() && packed.size() < plain.size(), "LZMS asset ai-windows-Pulse.AI.exe.lzms is smaller");
    Check(catalog.find("kAiPackRelease{L\"ai\", L\"9.8.7\"") != std::string::npos &&
          catalog.find("kAiPackFiles[]") != std::string::npos, "catalog defines kAiPackRelease for key ai");
    Check(manifest.find("\"key\": \"ai\"") != std::string::npos &&
          manifest.find("\"asset\": \"ai-windows-Pulse.AI.exe.lzms\"") != std::string::npos,
          "manifest ai-pack-assets.json lists the asset");

    // Pin the stand-in like the generated catalog does (hashes read back from it).
    auto quoted_after = [&](const std::string& marker, size_t from) {
        const size_t at = catalog.find(marker, from);
        if (at == std::string::npos) return std::string{};
        const size_t open = catalog.find("L\"", at + marker.size());
        const size_t close = open == std::string::npos ? open : catalog.find('"', open + 2);
        return close == std::string::npos ? std::string{} : catalog.substr(open + 2, close - open - 2);
    };
    const size_t row = catalog.find("{L\"Pulse.AI.exe\"");
    const std::string sha = quoted_after("ull, ", row);
    const std::string packed_sha = quoted_after("ull, ", catalog.find("ull, ", row) + 5);
    const std::wstring sha_w(sha.begin(), sha.end()), packed_sha_w(packed_sha.begin(), packed_sha.end());
    Check(sha.size() == 64 && packed_sha.size() == 64, "catalog row carries both SHA-256 values");
    const PackFile file{L"Pulse.AI.exe", plain.size(), sha_w.c_str(), packed.size(), packed_sha_w.c_str(),
                        asset.c_str()};
    const PackRelease release{L"ai", L"9.8.7", base_url.c_str(), &file, 1};
    Check(ContentPackDownloadBytes(release) == packed.size(), "download size is the asset");

    // --- install from a fake server, then the launch-time verification.
    std::map<std::wstring, std::vector<uint8_t>> server{{base_url + asset, packed}};
    const UpdateResponseReader reader = [&](std::wstring_view url, uint64_t maximum, const std::atomic<bool>&,
                                            const std::function<bool(const void*, DWORD)>& consume, UpdateError& category,
                                            DWORD& error) {
        auto it = server.find(std::wstring(url));
        if (it == server.end()) { category = UpdateError::HttpStatus; error = 404; return false; }
        if (it->second.size() > maximum) { category = UpdateError::ResponseTooLarge; error = ERROR_FILE_TOO_LARGE; return false; }
        return consume(it->second.data(), static_cast<DWORD>(it->second.size()));
    };
    Check(CheckContentPack(release, packs_root).state == AiPackState::Missing, "nothing installed: Missing");
    PackInstallProgress progress;
    DWORD error = 0;
    Check(InstallPack(release, packs_root, progress, error, reader) == PackInstallOutcome::Installed,
          "the asset installs through InstallPack");
    std::vector<HANDLE> held;
    const ContentPackCheck ready = CheckContentPack(release, packs_root, &held);
    Check(ready.state == AiPackState::Ready && held.size() == 1 && ready.directory == packs_root + L"\\ai\\9.8.7",
          "installed pack verifies as Ready under packs\\ai\\9.8.7 and is held");

    // --- removal: refused while the file is held (as while it runs), then complete.
    Check(!RemoveAiPack(error) && error == ERROR_SHARING_VIOLATION, "removal fails while Pulse.AI.exe is held");
    for (HANDLE h : held) CloseHandle(h);
    held.clear();
    // The failed attempt may have taken installed.json already; reinstall for a clean start.
    PackInstallProgress again;
    InstallPack(release, packs_root, again, error, reader);
    Check(CheckContentPack(release, packs_root).state == AiPackState::Ready, "reinstall verifies again");
    const PackRelease newer{L"ai", L"9.8.8", base_url.c_str(), &file, 1};
    Check(CheckContentPack(newer, packs_root).state == AiPackState::Outdated, "pack of another version: Outdated");
    Check(RemoveAiPack(error) && GetFileAttributesW((packs_root + L"\\ai").c_str()) == INVALID_FILE_ATTRIBUTES,
          "RemoveAiPack deletes packs\\ai");
    Check(RemoveAiPack(error) && error == ERROR_SUCCESS, "removing an absent pack succeeds");
    Check(CheckContentPack(release, packs_root).state == AiPackState::Missing, "after removal: Missing");

    RemovePackTree(root);
    std::printf("ai_pack_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
