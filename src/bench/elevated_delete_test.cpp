#include "../ops/elevated_delete.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <wrl/client.h>
#include "../common/path_utils.h"
#include <shellapi.h>
#include <sherrors.h>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
using Microsoft::WRL::ComPtr;
HRESULT FindRecycledFixture(const std::filesystem::path& original, ComPtr<IShellItem>& found) {
    const PROPERTYKEY deleted_from{FMTID_Displaced, PID_DISPLACED_FROM};
    ComPtr<IShellItem> bin;
    HRESULT hr = SHGetKnownFolderItem(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&bin));
    if (FAILED(hr)) return hr;
    ComPtr<IEnumShellItems> items;
    hr = bin->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&items));
    if (FAILED(hr)) return hr;
    found.Reset();
    ComPtr<IShellFolder2> bin_folder;
    bin->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&bin_folder));
    const auto marker = original.parent_path().filename().wstring();
    for (;;) {
        ComPtr<IShellItem> item;
        if (items->Next(1, &item, nullptr) != S_OK) break;
        ComPtr<IShellItem2> properties;
        if (FAILED(item.As(&properties))) continue;
        PWSTR parent = nullptr, name = nullptr;
        const HRESULT parent_hr = properties->GetString(deleted_from, &parent);
        const HRESULT name_hr = item->GetDisplayName(SIGDN_NORMALDISPLAY, &name);
        std::wstring location = parent ? parent : L"";
        std::wstring display = name ? name : L"";
        CoTaskMemFree(parent); CoTaskMemFree(name);
        VARIANT folder_value{};
        PIDLIST_ABSOLUTE pidl = nullptr;
        HRESULT details_hr = E_NOINTERFACE;
        if (bin_folder && SUCCEEDED(SHGetIDListFromObject(item.Get(), &pidl))) {
            details_hr = bin_folder->GetDetailsEx(ILFindLastID(pidl), &deleted_from, &folder_value);
            CoTaskMemFree(pidl);
        }
        const std::wstring folder_location = SUCCEEDED(details_hr) && folder_value.vt == VT_BSTR && folder_value.bstrVal ? folder_value.bstrVal : L"";
        VariantClear(&folder_value);
        auto matches = [&](const std::wstring& value) {
            return pulse::path::EqualInsensitive(value, original.wstring()) ||
                (pulse::path::EqualInsensitive(value, original.parent_path().wstring()) &&
                 pulse::path::EqualInsensitive(display, original.filename().wstring()));
        };
        const bool match = matches(location) || matches(folder_location) || pulse::path::EqualInsensitive(display, original.wstring());
        if (!match && (location.find(marker) != std::wstring::npos || folder_location.find(marker) != std::wstring::npos || display.find(marker) != std::wstring::npos))
            std::wcerr << L"fixture-only bin match: item_property_hr=0x" << std::hex << static_cast<unsigned long>(parent_hr)
                << L" display_hr=0x" << static_cast<unsigned long>(name_hr) << L" folder_property_hr=0x" << static_cast<unsigned long>(details_hr)
                << std::dec << L" location=" << location << L" folder_location=" << folder_location << L" display=" << display << L" match=" << match << L"\n";
        if (!match) continue;
        if (found) return HRESULT_FROM_WIN32(ERROR_DUP_NAME);
        found = item;
    }
    return found ? S_OK : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}
int RestoreExistingFixture(const std::filesystem::path& original) {
    if (!original.parent_path().filename().wstring().starts_with(L"pulse_recycle_fixture_{") ||
        original.filename() != L"unique_roundtrip_item" || std::filesystem::exists(original)) return 2;
    ComPtr<IShellItem> item;
    HRESULT hr = FindRecycledFixture(original, item);
    if (SUCCEEDED(hr) && !std::filesystem::exists(original)) {
        ComPtr<IShellItem> parent;
        hr = SHCreateItemFromParsingName(original.parent_path().c_str(), nullptr, IID_PPV_ARGS(&parent));
        ComPtr<IFileOperation> operation;
        if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&operation));
        if (SUCCEEDED(hr)) hr = operation->SetOperationFlags(FOF_NO_UI | FOFX_EARLYFAILURE | FOFX_NOCOPYHOOKS);
        if (SUCCEEDED(hr)) hr = operation->MoveItem(item.Get(), parent.Get(), original.filename().c_str(), nullptr);
        if (SUCCEEDED(hr)) hr = operation->PerformOperations();
        BOOL aborted = FALSE;
        if (SUCCEEDED(hr) && (FAILED(operation->GetAnyOperationsAborted(&aborted)) || aborted)) hr = E_FAIL;
    }
    std::wcerr << L"restore exact fixture hr=0x" << std::hex << static_cast<unsigned long>(hr) << std::dec << L" exists=" << std::filesystem::exists(original) << L"\n";
    if (SUCCEEDED(hr) && std::filesystem::exists(original)) {
        std::filesystem::remove_all(original.parent_path());
        return 0;
    }
    return 1;
}
int RecycleRoundTrip(bool directory = false) {
    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp)) return 2;
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return 2;
    wchar_t unique[40]{}; StringFromGUID2(guid, unique, 40);
    const auto root = std::filesystem::path(temp) / (L"pulse_recycle_fixture_" + std::wstring(unique));
    const auto original = root / L"unique_roundtrip_item";
    std::filesystem::create_directories(root);
    if (directory) std::filesystem::create_directory(original);
    const auto content_path = directory ? original / L"child.txt" : original;
    { std::ofstream file(content_path); file << "Pulse isolated recycle roundtrip"; }
    std::atomic<bool> cancel{false};
    const auto result = pulse::ops::DeleteWithCurrentToken({original.wstring()}, false, nullptr, cancel);
    ComPtr<IShellItem> recycled;
    HRESULT restore = FindRecycledFixture(original, recycled);
    const bool deleted = SUCCEEDED(result.hr) && result.sources.size() == 1 &&
        !std::filesystem::exists(original) && SUCCEEDED(restore);
    if (recycled && !std::filesystem::exists(original)) {
        ComPtr<IShellItem> target;
        restore = SHCreateItemFromParsingName(root.c_str(), nullptr, IID_PPV_ARGS(&target));
        ComPtr<IFileOperation> operation;
        if (SUCCEEDED(restore)) restore = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&operation));
        if (SUCCEEDED(restore)) restore = operation->SetOperationFlags(FOF_NO_UI | FOFX_EARLYFAILURE | FOFX_NOCOPYHOOKS);
        if (SUCCEEDED(restore)) restore = operation->MoveItem(recycled.Get(), target.Get(), original.filename().c_str(), nullptr);
        if (SUCCEEDED(restore)) restore = operation->PerformOperations();
        BOOL aborted = FALSE;
        if (SUCCEEDED(restore) && (FAILED(operation->GetAnyOperationsAborted(&aborted)) || aborted)) restore = E_FAIL;
    }
    std::string content;
    { std::ifstream file(content_path); content.assign(std::istreambuf_iterator<char>(file), {}); }
    ComPtr<IShellItem> remaining;
    const HRESULT remaining_hr = FindRecycledFixture(original, remaining);
    const bool restored = SUCCEEDED(restore) && content == "Pulse isolated recycle roundtrip" &&
        remaining_hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    std::cout << (deleted ? "[PASS] " : "[FAIL] ") << (directory ? "folder" : "file") << " recycle creates exact isolated bin entry without permanent deletion\n";
    std::cout << (restored ? "[PASS] " : "[FAIL] ") << (directory ? "folder" : "file") << " restore preserves content and removes only fixture bin entry\n";
    if (!deleted || !restored) {
        std::wcerr << L"delete hr=0x" << std::hex << static_cast<unsigned long>(result.hr)
                   << L" restore hr=0x" << static_cast<unsigned long>(restore) << std::dec
                   << L" fixture=" << original.wstring() << L"\n";
    }
    // Never clear the bin. Only remove this unique directory after the fixture
    // was restored or never left it; an unsuccessful restore stays recoverable.
    if (restored || std::filesystem::exists(original)) std::filesystem::remove_all(root);
    return deleted && restored ? 0 : 1;
}
}
int wmain(int argc, wchar_t** argv) {
    using namespace pulse::ops;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    if (argc == 3 && std::wstring(argv[1]) == L"--restore-fixture") {
        const int result = RestoreExistingFixture(argv[2]); CoUninitialize(); return result;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--recycle-roundtrip") {
        const int file_result = RecycleRoundTrip();
        const int folder_result = RecycleRoundTrip(true);
        CoUninitialize(); return file_result || folder_result ? 1 : 0;
    }
    int failed = 0;
    auto check = [&](bool ok, const char* label) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; if (!ok) ++failed; };
    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp)) { CoUninitialize(); return 2; }
    const auto root = std::filesystem::path(temp) / (L"pulse_delete_test_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    std::atomic<bool> cancel{false};
    try {
        std::filesystem::create_directories(root / L"folder");
        { std::ofstream file(root / L"file.txt"); file << "isolated fixture"; }
        { std::ofstream file(root / L"folder" / L"child.txt"); file << "isolated child"; }
        cancel = true;
        auto result = DeleteWithCurrentToken({(root / L"file.txt").wstring()}, true, nullptr, cancel);
        check(result.cancelled && !result.mutated && result.sources.empty() && std::filesystem::exists(root / L"file.txt"), "pre-cancel preserves fixture");
        cancel = false;
        result = DeleteWithCurrentToken({L"\\\\localhost\\nonexistent_pulse_recycle_test_share\\file.txt"}, false, nullptr, cancel);
        check(FAILED(result.hr) && !result.mutated && !result.cancelled && result.sources.empty(), "UNC recycle rejected before any delete attempt");
        result = DeleteWithCurrentToken({(root / L"file.txt").wstring(), (root / L"missing.txt").wstring()}, true, nullptr, cancel);
        if (result.sources.size() != 1) std::cerr << "delete hr=" << std::hex << static_cast<unsigned long>(result.hr) << std::dec << " successes=" << result.sources.size() << " mutated=" << result.mutated << '\n';
        check(FAILED(result.hr) && !result.cancelled && result.mutated && result.sources.size() == 1 && !std::filesystem::exists(root / L"file.txt"), "partial deletion retains only completed source and original error");
        result = DeleteWithCurrentToken({(root / L"folder").wstring()}, true, nullptr, cancel);
        const bool folder_deleted = SUCCEEDED(result.hr) && result.sources.size() == 1 && result.destinations.empty() && !std::filesystem::exists(root / L"folder");
        if (!folder_deleted) std::cerr << "folder delete hr=0x" << std::hex << static_cast<unsigned long>(result.hr) << std::dec << " successes=" << result.sources.size() << " mutated=" << result.mutated << " cancelled=" << result.cancelled << " folder_exists=" << std::filesystem::exists(root / L"folder") << '\n';
        check(folder_deleted, "permanent folder deletion reports completed root");
        // The bin's limits are judged before anything is touched.
        {
            using pulse::ops::RecycleBinLimits;
            using pulse::ops::CheckRecycleFits;
            std::filesystem::create_directories(root / L"big" / L"sub");
            { std::ofstream file(root / L"large.bin", std::ios::binary); file << std::string(2u << 20, 'x'); }
            { std::ofstream file(root / L"small.bin", std::ios::binary); file << std::string(100u << 10, 'x'); }
            for (int i = 0; i < 3; ++i) {
                std::ofstream file(root / L"big" / L"sub" / (L"part" + std::to_wstring(i)), std::ios::binary);
                file << std::string(512u << 10, 'x');
            }
            RecycleBinLimits tiny;
            tiny.max_bytes = 1u << 20;
            tiny.used_bytes = 100ull << 30;
            check(CheckRecycleFits((root / L"large.bin").wstring(), tiny, cancel) == pulse::ops::kRecycleBinTooSmall,
                  "a file larger than the bin is refused");
            check(CheckRecycleFits((root / L"small.bin").wstring(), tiny, cancel) == S_OK, "a file that fits passes");
            check(CheckRecycleFits((root / L"big").wstring(), tiny, cancel) == pulse::ops::kRecycleBinTooSmall,
                  "a folder larger than the bin is refused (measured through subfolders)");
            check(CheckRecycleFits((root / L"big").wstring(), tiny, cancel, false) == S_OK,
                  "folders are not measured on the cheap check");
            RecycleBinLimits roomy = tiny;
            roomy.max_bytes = roomy.used_bytes;
            check(CheckRecycleFits((root / L"large.bin").wstring(), roomy, cancel) == S_OK,
                  "a bin as large as the used space takes anything");
            RecycleBinLimits off;
            off.disabled = true;
            check(CheckRecycleFits((root / L"small.bin").wstring(), off, cancel) == COPYENGINE_E_RECYCLE_BIN_NOT_FOUND,
                  "a bin that is turned off refuses");
            check(std::filesystem::exists(root / L"large.bin") && std::filesystem::exists(root / L"big" / L"sub" / L"part2"),
                  "the checks touch nothing");
            DWORD volume_error = 0;
            const RecycleBinLimits real = pulse::ops::ReadRecycleBinLimits(root.wstring(), &volume_error);
            std::cout << "temp volume bin: disabled=" << real.disabled << " max_mb=" << (real.max_bytes >> 20)
                      << " used_mb=" << (real.used_bytes >> 20) << " error=" << volume_error << '\n';
            check(volume_error == 0 && (real.disabled || real.max_bytes > 0), "the temp volume's bin limits are read");
        }
        std::filesystem::remove_all(root);
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n'; ++failed;
        std::error_code ignored; std::filesystem::remove_all(root, ignored);
    }
    CoUninitialize(); return failed ? 1 : 0;
}
