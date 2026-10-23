// app_catalog.cpp - Start menu programs from the shell's Apps folder.
#include "app_catalog.h"

#include <knownfolders.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace pulse::app {
namespace {
// SHParseDisplayName rejects "::{4234D49B-...}\<id>" but takes the shell: form.
constexpr wchar_t kAppsFolder[] = L"shell:AppsFolder\\";

std::wstring Lower(std::wstring text) {
    if (!text.empty()) CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}
bool EndsWith(const std::wstring& text, const wchar_t* tail) {
    const size_t n = wcslen(tail);
    return text.size() >= n && text.compare(text.size() - n, n, tail) == 0;
}
}

std::wstring AppPath(const std::wstring& id) { return kAppsFolder + id; }

bool IsAppPath(const std::wstring& path) {
    const size_t n = std::size(kAppsFolder) - 1;
    return path.size() > n && CompareStringOrdinal(path.c_str(), static_cast<int>(n), kAppsFolder, static_cast<int>(n), TRUE) == CSTR_EQUAL;
}

bool KeepApp(const std::wstring& name, const std::wstring& id) {
    if (name.empty() || id.empty()) return false;
    const std::wstring n = Lower(name), i = Lower(id);
    for (const wchar_t* word : {L"uninstall", L"\x5378\x8F7D", L"\x5378\x8F09", L"readme", L"read me"})  // 卸载 卸載
        if (n.find(word) != std::wstring::npos) return false;
    for (const wchar_t* ext : {L".url", L".chm", L".hlp", L".htm", L".html", L".pdf", L".txt", L".rtf", L".md", L".website"})
        if (EndsWith(i, ext)) return false;
    return !(i.rfind(L"http://", 0) == 0 || i.rfind(L"https://", 0) == 0);
}

std::vector<AppEntry> EnumerateApps() {
    std::vector<AppEntry> apps;
    ComPtr<IShellItem> folder;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&folder)))) return apps;
    ComPtr<IEnumShellItems> items;
    if (FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&items)))) return apps;
    for (;;) {
        ComPtr<IShellItem> item;
        ULONG fetched = 0;
        if (items->Next(1, &item, &fetched) != S_OK || !fetched) break;
        PWSTR name = nullptr, id = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) &&
            SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &id)) && KeepApp(name, id)) {
            const std::wstring shown = name;
            // One row per name: the same program can be listed per user and
            // for all users.
            const bool seen = std::any_of(apps.begin(), apps.end(), [&](const AppEntry& a) {
                return CompareStringOrdinal(a.name.c_str(), -1, shown.c_str(), -1, TRUE) == CSTR_EQUAL;
            });
            if (!seen) apps.push_back({shown, AppPath(id)});
        }
        if (name) CoTaskMemFree(name);
        if (id) CoTaskMemFree(id);
        if (apps.size() >= 2000) break;
    }
    std::sort(apps.begin(), apps.end(), [](const AppEntry& a, const AppEntry& b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE, a.name.c_str(), -1, b.name.c_str(), -1,
                               nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    return apps;
}

AppCatalog::AppCatalog() : state_(std::make_shared<State>()) {}

void AppCatalog::RefreshIfStale(HWND notify, UINT message, uint64_t max_age_ms) {
    {
        std::lock_guard lock(state_->mu);
        if (state_->loading) return;
        if (state_->loaded && GetTickCount64() - state_->loaded_at < max_age_ms) return;
        state_->loading = true;
    }
    // The worker owns a share of the state, so the popup can go away first.
    std::thread([state = state_, notify, message] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        auto apps = EnumerateApps();
        if (SUCCEEDED(com)) CoUninitialize();
        {
            std::lock_guard lock(state->mu);
            if (!apps.empty() || !state->loaded) state->apps = std::move(apps);
            state->loaded = true;
            state->loaded_at = GetTickCount64();
            state->loading = false;
        }
        if (notify && IsWindow(notify)) PostMessageW(notify, message, 0, 0);
    }).detach();
}

std::vector<AppEntry> AppCatalog::Snapshot() const {
    std::lock_guard lock(state_->mu);
    return state_->apps;
}
}
