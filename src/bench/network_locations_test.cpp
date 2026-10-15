#include "../app/network_locations.h"
#include <shlobj.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
using pulse::app::NetworkDiscoveryState;
using pulse::app::NetworkLocation;
using pulse::app::ReadNetworkLocations;
using pulse::app::SummarizeNetworkDiscoveryProfiles;

struct Fixture {
    fs::path root;
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
};

bool LocalLink(const fs::path& file_path, const fs::path& target) {
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> file;
    return SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&link))) && SUCCEEDED(link->SetPath(target.c_str())) &&
        SUCCEEDED(link.As(&file)) && SUCCEEDED(file->Save(file_path.c_str(), TRUE));
}

bool VirtualLink(const fs::path& file_path) {
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> file;
    PIDLIST_ABSOLUTE id = nullptr;
    if (FAILED(SHGetKnownFolderIDList(FOLDERID_ComputerFolder, KF_FLAG_DONT_VERIFY, nullptr, &id))) return false;
    const bool saved = SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&link))) && SUCCEEDED(link->SetIDList(id)) && SUCCEEDED(link.As(&file)) &&
        SUCCEEDED(file->Save(file_path.c_str(), TRUE));
    CoTaskMemFree(id);
    return saved;
}

// Standard Shell Link Header + EnvironmentVariableDataBlock. The raw target is
// stored directly so neither fixture creation nor loading needs a network lookup.
bool UncLink(const fs::path& file_path, bool arguments = false) {
    const std::string unc = "\\\\pulse-offline.invalid\\share";
    const std::wstring wide_unc(unc.begin(), unc.end());
    std::vector<unsigned char> bytes(76, 0);
    auto put = [&](size_t offset, DWORD value) { std::memcpy(bytes.data() + offset, &value, sizeof(value)); };
    put(0, 76);
    const GUID clsid = CLSID_ShellLink;
    std::memcpy(bytes.data() + 4, &clsid, sizeof(clsid));
    put(20, 0x280 | (arguments ? 0x20 : 0)); // HasExpString | IsUnicode | optional HasArguments
    put(24, FILE_ATTRIBUTE_DIRECTORY);
    put(60, SW_SHOWNORMAL);
    if (arguments) {
        const WORD length = 4;
        const wchar_t text[] = L"/arg";
        const auto old_size = bytes.size();
        bytes.resize(old_size + sizeof(length) + length * sizeof(wchar_t));
        std::memcpy(bytes.data() + old_size, &length, sizeof(length));
        std::memcpy(bytes.data() + old_size + sizeof(length), text, length * sizeof(wchar_t));
    }
    const size_t block = bytes.size();
    bytes.resize(block + 0x314, 0);
    put(block, 0x314);
    put(block + 4, 0xA0000001);
    std::memcpy(bytes.data() + block + 8, unc.c_str(), unc.size() + 1);
    std::memcpy(bytes.data() + block + 268, wide_unc.c_str(), (wide_unc.size() + 1) * sizeof(wchar_t));
    bytes.resize(bytes.size() + sizeof(DWORD), 0); // TerminalBlock
    std::ofstream output(file_path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return output.good();
}
const NetworkLocation* Named(const std::vector<NetworkLocation>& items, const wchar_t* name) {
    const auto found = std::find_if(items.begin(), items.end(), [&](const auto& item) { return item.name == name; });
    return found == items.end() ? nullptr : &*found;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    bool ok = true;
    auto check = [&](bool pass, const char* label) {
        std::cout << (pass ? "[PASS] " : "[FAIL] ") << label << '\n';
        ok &= pass;
    };
    using Discovery = NetworkDiscoveryState;
    check(SummarizeNetworkDiscoveryProfiles({}) == Discovery::Unknown,
        "no active network profiles cannot prove discovery is off");
    check(SummarizeNetworkDiscoveryProfiles({Discovery::Disabled, Discovery::Disabled}) == Discovery::Disabled,
        "discovery is off only when every active profile is confirmed off");
    check(SummarizeNetworkDiscoveryProfiles({Discovery::Disabled, Discovery::Unknown}) == Discovery::Unknown,
        "an unreadable active profile suppresses the off banner");
    check(SummarizeNetworkDiscoveryProfiles({Discovery::Enabled, Discovery::Enabled}) == Discovery::Enabled,
        "all active profiles enabled is reported as enabled");
    check(SummarizeNetworkDiscoveryProfiles({Discovery::Enabled, Discovery::Disabled}) == Discovery::Unknown,
        "mixed active profile states are not reported as conclusively on or off");
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) { std::cout << "[FAIL] COM setup\n"; return 1; }
    if (argc != 1) {
        if (argc != 3 || std::wstring(argv[1]) != L"--fixture-dir") {
            std::cout << "Usage: pulse_network_locations_test [--fixture-dir <new absolute directory>]\n";
            CoUninitialize();
            return 1;
        }
        const fs::path root(argv[2]);
        const auto path = root.wstring();
        std::error_code error;
        if (!root.is_absolute() || path.starts_with(L"\\\\") || path.size() < 3 ||
            GetDriveTypeW(path.substr(0, 3).c_str()) == DRIVE_REMOTE || !fs::create_directory(root, error)) {
            std::cout << "[FAIL] fixture directory must be a new absolute local directory; existing files are never replaced\n";
            CoUninitialize();
            return 1;
        }
        const bool created = fs::create_directory(root / L"Team archive", error) &&
            UncLink(root / L"Offline share.lnk") &&
            UncLink(root / L"Team archive" / L"target.lnk") &&
            VirtualLink(root / L"This PC.lnk") && LocalLink(root / L"Local shortcut.lnk", root);
        std::ofstream portal(root / L"Project portal.url");
        portal << "[InternetShortcut]\nURL=https://example.invalid/\n";
        check(created && portal.good(), "create persistent screenshot fixtures without accessing targets");
        CoUninitialize();
        return ok ? 0 : 1;
    }
    {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        Fixture fixture{fs::path(temp) / (L"pulse-network-locations-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()))};
        const auto root = fixture.root / L"NetHood";
        fs::create_directories(root / L"Wrapped share");
        fs::create_directories(fixture.root / L"Local target");
        check(UncLink(root / L"Offline.lnk") && UncLink(root / L"Wrapped share" / L"target.lnk") &&
            UncLink(root / L"Arguments.lnk", true) &&
            LocalLink(root / L"Local.lnk", fixture.root / L"Local target"), "create real shortcut fixtures");
        std::ofstream(root / L"Portal.url") << "[InternetShortcut]\nURL=https://example.invalid/\n";
        std::ofstream(root / L"Broken.lnk") << "not a shell link";
        std::ofstream(root / L"desktop.ini") << "[.ShellClassInfo]\n";
        fs::create_directories(root / L"Ignored folder");
        auto scan = ReadNetworkLocations(root.wstring());
        check(scan.discovery == Discovery::Unknown, "fixture scans do not read host firewall settings");
        check(!scan.error && !scan.cancelled && scan.items.size() == 5, "enumerate supported shortcuts and skip malformed/irrelevant files");
        const auto* offline = Named(scan.items, L"Offline");
        check(offline && !offline->shell_link && offline->path == L"\\\\pulse-offline.invalid\\share",
            "offline UNC is read as stored, without target access");
        const auto* wrapped = Named(scan.items, L"Wrapped share");
        check(wrapped && !wrapped->shell_link && wrapped->path == L"\\\\pulse-offline.invalid\\share",
            "NetHood target.lnk uses wrapper display name");
        const auto* local = Named(scan.items, L"Local");
        check(local && local->shell_link && local->path == (root / L"Local.lnk").wstring(), "local target delegates original shortcut to shell");
        const auto* arguments = Named(scan.items, L"Arguments");
        check(arguments && arguments->shell_link && arguments->path == (root / L"Arguments.lnk").wstring(),
            "shortcut arguments are preserved by shell delegation");
        const auto* url = Named(scan.items, L"Portal");
        check(url && url->shell_link && url->path == (root / L"Portal.url").wstring(), "URL shortcut delegates original local file");

        check(VirtualLink(root / L"Computer.lnk"), "create virtual PIDL shortcut");
        auto virtual_scan = ReadNetworkLocations(root.wstring());
        const auto* computer = Named(virtual_scan.items, L"Computer");
        check(!virtual_scan.error && computer && computer->shell_link &&
            computer->path == (root / L"Computer.lnk").wstring(), "virtual PIDL delegates original shortcut");
        fs::remove(root / L"Computer.lnk");

        const std::vector<NetworkLocation> pinned{{L"My share", L"//PULSE-OFFLINE.INVALID/share/", false}};
        auto merged = pulse::app::MergeNetworkLocations(pinned, scan.items);
        check(merged.size() == 4 && merged.front().name == L"My share" && merged.front().path == pinned.front().path,
            "manual name/path win over duplicate system targets");
        merged = pulse::app::MergeNetworkLocations(pinned, {{L"Extended", L"\\\\?\\UNC\\pulse-offline.invalid\\share\\", false}});
        check(merged.size() == 1 && pinned.front().name == L"My share", "extended UNC, case, and separator deduplication");
        scan = ReadNetworkLocations(root.wstring(), [] { return true; });
        check(scan.cancelled && scan.items.empty() && !scan.error, "immediate cancellation publishes nothing");
        size_t calls = 0;
        scan = ReadNetworkLocations(root.wstring(), [&] { return ++calls >= 6; });
        check(scan.cancelled && scan.items.empty() && !scan.error, "mid-scan cancellation discards partial results");
        fs::remove(root / L"Offline.lnk");
        scan = ReadNetworkLocations(root.wstring());
        check(!scan.error && !Named(scan.items, L"Offline") && scan.items.size() == 4, "rescan removes deleted shortcuts");
        scan = ReadNetworkLocations((fixture.root / L"Missing").wstring());
        check(!scan.error && scan.items.empty(), "absent local folder is empty");
        scan = ReadNetworkLocations((root / L"Broken.lnk").wstring());
        check(scan.error != 0 && scan.items.empty(), "invalid root reports an error");
        scan = ReadNetworkLocations(L"\\\\pulse-offline.invalid\\share");
        check(scan.error == ERROR_BAD_PATHNAME && scan.items.empty(), "UNC scan roots are rejected before filesystem access");

        const auto extended_root = L"\\\\?\\" + root.wstring();
        scan = ReadNetworkLocations(extended_root);
        check(!scan.error && scan.items.size() == 4, "extended local root is supported");
        const fs::path long_root(L"\\\\?\\" + fixture.root.wstring() + L"\\" + std::wstring(100, L'a') +
            L"\\" + std::wstring(100, L'b') + L"\\" + std::wstring(80, L'c'));
        fs::create_directories(long_root);
        check(UncLink(long_root / L"Long path.lnk"), "create long local shortcut path");
        scan = ReadNetworkLocations(long_root.wstring().substr(4));
        check(!scan.error && scan.items.size() == 1 && !scan.items.front().shell_link,
            "long local scan paths retain stored offline UNC target");

        fs::create_directories(fixture.root / L"Outside");
        UncLink(fixture.root / L"Outside" / L"target.lnk");
        const auto junction = root / L"Reparse folder";
        if (CreateSymbolicLinkW(junction.c_str(), (fixture.root / L"Outside").c_str(),
            SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2)) {
            scan = ReadNetworkLocations(root.wstring());
            check(!scan.error && !Named(scan.items, L"Reparse folder"), "reparse directory is not followed");
            scan = ReadNetworkLocations(junction.wstring());
            check(scan.error == ERROR_BAD_PATHNAME && scan.items.empty(), "reparse root is not followed");
            fs::remove(junction);
        } else {
            std::cout << "[SKIP] reparse fixture creation unavailable (Win32 " << GetLastError() << ")\n";
        }
        const auto cap_root = fixture.root / L"Entry cap";
        fs::create_directory(cap_root);
        for (size_t i = 0; i < 4097; ++i) std::ofstream(cap_root / (std::to_wstring(i) + L".txt"));
        scan = ReadNetworkLocations(cap_root.wstring());
        check(scan.error == ERROR_BUFFER_OVERFLOW && scan.items.empty(), "entry limit is explicit and publishes no partial list");
    }
    CoUninitialize();
    return ok ? 0 : 1;
}
