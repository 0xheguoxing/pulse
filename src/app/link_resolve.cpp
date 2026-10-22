// link_resolve.cpp — See link_resolve.h. Worker threads live for the whole
// session, so COM is initialized once per thread via a thread_local guard.
#include "link_resolve.h"
#include <shobjidl.h>
#include <wrl/client.h>
#include <cwctype>
#include <winioctl.h>
#include <array>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace pulse::app {
namespace {

struct ThreadCom {
    ThreadCom() { hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                               COINIT_DISABLE_OLE1DDE); }
    ~ThreadCom() { if (SUCCEEDED(hr)) CoUninitialize(); }
    HRESULT hr = E_FAIL;
};

bool HasLnkSuffix(const std::wstring& name) {
    if (name.size() < 4) return false;
    const std::wstring tail = name.substr(name.size() - 4);
    return _wcsicmp(tail.c_str(), L".lnk") == 0;
}

bool HasUrlSuffix(const std::wstring& name) {
    return name.size() >= 4 && _wcsicmp(name.c_str() + name.size() - 4, L".url") == 0;
}

std::wstring ReparseDestination(const std::wstring& path) {
    const HANDLE handle = CreateFileW(fs::NormalizePath(path).c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {};
    std::array<BYTE, MAXIMUM_REPARSE_DATA_BUFFER_SIZE> data{};
    DWORD bytes = 0;
    const bool read = DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, nullptr, 0,
        data.data(), static_cast<DWORD>(data.size()), &bytes, nullptr) != FALSE;
    CloseHandle(handle);
    if (!read || bytes < 16) return {};
    DWORD tag = 0;
    WORD length = 0, offset = 0, target_length = 0;
    memcpy(&tag, data.data(), sizeof(tag));
    memcpy(&length, data.data() + 4, sizeof(length));
    // Use the substitute name, not the optional human label. Read one hop only.
    memcpy(&offset, data.data() + 8, sizeof(offset));
    memcpy(&target_length, data.data() + 10, sizeof(target_length));
    const size_t path_offset = tag == IO_REPARSE_TAG_SYMLINK ? 20 : 16;
    if ((tag != IO_REPARSE_TAG_SYMLINK && tag != IO_REPARSE_TAG_MOUNT_POINT) ||
        static_cast<size_t>(length) + 8 > bytes ||
        path_offset + offset + target_length > static_cast<size_t>(length) + 8 ||
        (offset % sizeof(wchar_t)) || (target_length % sizeof(wchar_t)) || !target_length)
        return {};
    std::wstring target(target_length / sizeof(wchar_t), L'\0');
    memcpy(target.data(), data.data() + path_offset + offset, target_length);
    if (target.find(L'\0') != std::wstring::npos) return {};
    if (target.starts_with(L"\\??\\UNC\\")) return L"\\\\" + target.substr(8);
    if (target.starts_with(L"\\??\\")) return target.substr(4);
    return target;
}

std::wstring UrlDestination(const std::wstring& path) {
    std::array<wchar_t, 32768> url{};
    const DWORD count = GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"",
        url.data(), static_cast<DWORD>(url.size()), fs::NormalizePath(path).c_str());
    if (!count || count >= url.size() - 1) return {};
    std::wstring target(url.data(), count);
    // A malformed profile value is not a destination. Never launch or fetch it.
    if (target.find(L':') == std::wstring::npos ||
        target.find_first_of(L"\r\n\t") != std::wstring::npos) return {};
    return target;
}

} // namespace

bool ResolveLink(const std::wstring& lnk_path, fs::DirEntry& e) {
    e.link_destination = std::wstring();
    e.link_target = std::wstring();
    e.link_target_size = 0;
    e.link_target_mtime = FILETIME{};
    e.link_target_is_dir = false;
    thread_local ThreadCom com;
    if (FAILED(com.hr)) return false;

    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link)))) return false;
    ComPtr<IPersistFile> file;
    if (FAILED(link.As(&file))) return false;
    // STGM_READ: IPersistFile::Load parses the shortcut; no target tracking.
    if (FAILED(file->Load(lnk_path.c_str(), STGM_READ))) return false;
    wchar_t raw[32768]{};
    WIN32_FIND_DATAW fd{};
    if (FAILED(link->GetPath(raw, static_cast<int>(std::size(raw)), &fd,
                             SLGP_RAWPATH)) || raw[0] == L'\0') return false;

    const std::wstring target = fs::NormalizePath(raw);
    e.link_destination = raw;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &data))
        return false;

    e.link_target = target;
    e.link_target_is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    e.link_target_size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) |
                         data.nFileSizeLow;
    e.link_target_mtime = data.ftLastWriteTime;
    return true;
}

void ResolveLinksInPlace(const std::wstring& parent_path,
                         std::vector<fs::DirEntry>& entries,
                         const std::function<bool()>& cancel) {
    bool any = false;
    for (const auto& e : entries) {
        if (fs::ClassifyLink(e.attrs, e.reparse_tag) != fs::LinkKind::None ||
            (!e.is_dir && (HasLnkSuffix(e.name) || HasUrlSuffix(e.name)))) { any = true; break; }
    }
    if (!any) return;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (cancel && cancel()) return;
        fs::DirEntry& e = entries[i];
        const bool reparse = fs::ClassifyLink(e.attrs, e.reparse_tag) != fs::LinkKind::None;
        if (!reparse && (e.is_dir || (!HasLnkSuffix(e.name) && !HasUrlSuffix(e.name)))) continue;
        e.link_destination = std::wstring();
        if (e.cloud_recall || e.change_record_only) continue;
        std::wstring full = e.full_path;
        if (full.empty()) {
            if (parent_path.empty() || fs::IsVirtualPath(parent_path)) continue;
            full = parent_path;
            if (full.back() != L'\\') full += L'\\';
            full += e.name;
        }
        if (reparse) e.link_destination = ReparseDestination(full);
        else if (HasUrlSuffix(e.name)) e.link_destination = UrlDestination(full);
        else ResolveLink(full, e);
    }
}

} // namespace pulse::app
