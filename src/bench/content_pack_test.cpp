// content_pack_test - the content enhancement pack end to end: assets and
// catalog from pulse_pack_catalog, installed by InstallPack from a fake
// server, then the launch-time verification pulse.exe runs before every
// Pulse.Content.exe start (version, size, SHA-256, files held open).
#include "../app/content_pack.h"
#include "../app/pack_installer.h"
#include "../common/preview_packs.h"
#include "../common/utf8_file.h"
#include <bcrypt.h>
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

std::wstring Sha256Hex(const std::vector<uint8_t>& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    uint8_t digest[32]{};
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    BCryptHashData(hash, const_cast<uint8_t*>(data.data()), static_cast<ULONG>(data.size()), 0);
    BCryptFinishHash(hash, digest, 32, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    std::wstring hex;
    for (uint8_t b : digest) { wchar_t t[3]; swprintf(t, 3, L"%02x", b); hex += t; }
    return hex;
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

std::string Narrow(const std::wstring& w) {
    std::string out;
    for (wchar_t c : w) out += static_cast<char>(c);  // ASCII names, hashes and URLs only
    return out;
}

std::wstring ModuleDirectory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    return dir.substr(0, dir.find_last_of(L"\\/"));
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
} // namespace

int main() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring root = std::wstring(tmp) + L"pulse_content_pack_test_" + std::to_wstring(GetCurrentProcessId());
    RemovePackTree(root);
    const std::wstring src = root + L"\\src", out = root + L"\\out", packs_root = root + L"\\packs";
    packs::CreateDirectoryChain(src);
    packs::CreateDirectoryChain(packs_root);

    // Two stand-in executables: noise and runs, so LZMS has work to do.
    const wchar_t* names[] = {L"Pulse.Content.exe", L"Pulse.Document.exe"};
    std::vector<std::vector<uint8_t>> plain(2);
    uint32_t x = 777;
    for (size_t f = 0; f < 2; ++f) {
        plain[f].resize((f + 1) * (700u << 10) + 13);
        for (size_t i = 0; i < plain[f].size(); ++i) {
            x = x * 1664525u + 1013904223u;
            plain[f][i] = (i / 2048) % 3 ? static_cast<uint8_t>(x >> 24) : static_cast<uint8_t>(i / 2048 + f);
        }
        WriteAll(src + L"\\" + names[f], plain[f]);
    }

    // --- pulse_pack_catalog: assets, catalog header, manifest.
    const std::wstring base_url = L"https://packs.example.invalid/v9.8.7/";
    const std::wstring header = out + L"\\catalog.h";
    const DWORD code = Run(L"\"" + ModuleDirectory() + L"\\pulse_pack_catalog.exe\" --key content --symbol Content "
                           L"--version 9.8.7 --channel windows --base-url " + base_url + L" --out-dir \"" + out +
                           L"\" --header \"" + header + L"\" \"" + src + L"\\Pulse.Content.exe\" \"" + src +
                           L"\\Pulse.Document.exe\"");
    Check(code == 0, "pulse_pack_catalog packs both files");
    std::string catalog, manifest;
    {
        auto h = ReadAll(header), m = ReadAll(out + L"\\content-pack-assets.json");
        catalog.assign(h.begin(), h.end());
        manifest.assign(m.begin(), m.end());
    }
    std::vector<std::wstring> sha(2), packed_sha(2), assets(2);
    std::vector<std::vector<uint8_t>> packed(2);
    std::map<std::wstring, std::vector<uint8_t>> server;
    std::vector<PackFile> files(2);
    bool assets_ok = true, catalog_ok = catalog.find("kContentPackRelease{L\"content\", L\"9.8.7\", L\"" + Narrow(base_url)) != std::string::npos;
    for (size_t f = 0; f < 2; ++f) {
        assets[f] = std::wstring(L"content-windows-") + names[f] + L".lzms";
        packed[f] = ReadAll(out + L"\\" + assets[f]);
        assets_ok &= !packed[f].empty() && packed[f].size() < plain[f].size();
        sha[f] = Sha256Hex(plain[f]);
        packed_sha[f] = Sha256Hex(packed[f]);
        const std::string row = "{L\"" + Narrow(names[f]) + "\", " + std::to_string(plain[f].size()) + "ull, L\"" +
                                Narrow(sha[f]) + "\", " + std::to_string(packed[f].size()) + "ull, L\"" +
                                Narrow(packed_sha[f]) + "\", L\"" + Narrow(assets[f]) + "\"}";
        catalog_ok &= catalog.find(row) != std::string::npos;
        assets_ok &= manifest.find("\"asset\": \"" + Narrow(assets[f]) + "\"") != std::string::npos &&
                     manifest.find(Narrow(packed_sha[f])) != std::string::npos;
        files[f] = {names[f], plain[f].size(), sha[f].c_str(), packed[f].size(), packed_sha[f].c_str(), assets[f].c_str()};
        server[base_url + assets[f]] = packed[f];
    }
    Check(assets_ok, "LZMS assets are smaller and listed with their hashes in the manifest");
    Check(catalog_ok, "catalog header pins size, SHA-256 and asset of each file");
    Check(manifest.find("\"channel\": \"windows\"") != std::string::npos && manifest.find("\"version\": \"9.8.7\"") != std::string::npos,
          "manifest records channel and version");
    Check(Run(L"\"" + ModuleDirectory() + L"\\pulse_pack_catalog.exe\" --key ../x --symbol S --version 1 --channel c "
              L"--base-url https://a/ --out-dir \"" + out + L"\" --header \"" + header + L"\" \"" + src + L"\\Pulse.Content.exe\"") == 2,
          "catalog tool refuses unsafe keys");

    const PackRelease release{L"content", L"9.8.7", base_url.c_str(), files.data(), files.size()};
    Check(ContentPackDownloadBytes(release) == packed[0].size() + packed[1].size(), "download size sums the assets");

    // --- before installing.
    Check(CheckContentPack(release, packs_root).state == ContentPackState::Missing, "nothing installed: Missing");
    const PackRelease empty{L"content", L"9.8.7", base_url.c_str(), nullptr, 0};
    Check(CheckContentPack(empty, packs_root).state == ContentPackState::Unavailable, "no catalog: Unavailable");

    // --- InstallPack from the fake server, then verification.
    const UpdateResponseReader reader = [&](std::wstring_view url, uint64_t maximum, const std::atomic<bool>&,
                                            const std::function<bool(const void*, DWORD)>& consume, UpdateError& category,
                                            DWORD& error) {
        auto it = server.find(std::wstring(url));
        if (it == server.end()) { category = UpdateError::HttpStatus; error = 404; return false; }
        if (it->second.size() > maximum) { category = UpdateError::ResponseTooLarge; error = ERROR_FILE_TOO_LARGE; return false; }
        return consume(it->second.data(), static_cast<DWORD>(it->second.size()));
    };
    PackInstallProgress progress;
    DWORD error = 0;
    Check(InstallPack(release, packs_root, progress, error, reader) == PackInstallOutcome::Installed,
          "the tool's assets install through InstallPack");
    std::vector<HANDLE> held;
    const ContentPackCheck ready = CheckContentPack(release, packs_root, &held);
    Check(ready.state == ContentPackState::Ready && held.size() == 2 &&
          ready.directory == packs_root + L"\\content\\9.8.7", "installed pack verifies as Ready and is held");
    const std::wstring document = ready.directory + L"\\Pulse.Document.exe";
    HANDLE writer = CreateFileW(document.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    const DWORD write_error = GetLastError();
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    Check(writer == INVALID_HANDLE_VALUE && write_error == ERROR_SHARING_VIOLATION, "held files cannot be rewritten");
    Check(!DeleteFileW(document.c_str()), "held files cannot be deleted");
    HANDLE reader_handle = CreateFileW(document.c_str(), GENERIC_READ | GENERIC_EXECUTE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, 0, nullptr);
    Check(reader_handle != INVALID_HANDLE_VALUE, "held files can still be opened to run");
    if (reader_handle != INVALID_HANDLE_VALUE) CloseHandle(reader_handle);
    for (HANDLE h : held) CloseHandle(h);
    Check(CheckContentPack(release, packs_root).state == ContentPackState::Ready, "verification without holding");

    // --- another app version.
    const PackRelease newer{L"content", L"9.8.8", base_url.c_str(), files.data(), files.size()};
    Check(CheckContentPack(newer, packs_root).state == ContentPackState::Outdated, "pack of another version: Outdated");

    // --- tampering.
    auto bytes = ReadAll(document);
    bytes[bytes.size() / 2] ^= 0x5a;
    WriteAll(document, bytes);
    Check(CheckContentPack(release, packs_root).state == ContentPackState::Damaged, "changed byte: Damaged");
    bytes.push_back(0);
    WriteAll(document, bytes);
    Check(CheckContentPack(release, packs_root).state == ContentPackState::Damaged, "changed size: Damaged");
    DeleteFileW(document.c_str());
    Check(CheckContentPack(release, packs_root).state == ContentPackState::Damaged, "missing file: Damaged");

    // --- installed.json names a path outside the pack.
    WriteUtf8FileAtomic(packs_root + L"\\content\\installed.json", L"{\"version\":\"9.8.7\",\"dir\":\"..\"}\n");
    Check(CheckContentPack(release, packs_root).state == ContentPackState::Missing, "installed.json escaping the pack is ignored");

    // --- a repaired install verifies again.
    PackInstallProgress again;
    Check(InstallPack(release, packs_root, again, error, reader) == PackInstallOutcome::Installed &&
          CheckContentPack(release, packs_root).state == ContentPackState::Ready, "reinstall repairs the pack");

    RemovePackTree(root);
    std::printf("content_pack_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
