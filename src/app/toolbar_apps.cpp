// toolbar_apps.cpp — See toolbar_apps.h for the contract.
#include "toolbar_apps.h"

#include "link_resolve.h"
#include "shell_verbs.h"

#include <objbase.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <functional>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace pulse::app {
namespace {

// Leaked on purpose: workers may still write while the process exits.
struct Caches {
    std::mutex mutex;
    std::unordered_map<std::wstring, ToolbarAppInfo> apps;                 // lower path
    std::unordered_map<std::wstring, std::vector<std::wstring>> programs;  // lower ext
    std::unordered_set<std::wstring> pending_apps, pending_exts;
    // The name 打开方式 shows for a program (IAssocHandler::GetUIName): the
    // package name of a Store-installed desktop exe (记事本), which its version
    // resource does not carry. Learned while the handlers are read anyway.
    std::unordered_map<std::wstring, std::wstring> shown_names;            // lower exe
};
Caches& Cache() {
    static Caches* caches = new Caches;
    return *caches;
}

void RememberShownName(const std::wstring& exe, const wchar_t* ui_name) {
    if (exe.empty() || !ui_name || !*ui_name) return;
    Caches& c = Cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.shown_names[ToolbarLower(ToolbarPlainPath(exe))] = ui_name;
}

// c.mutex held. A shortcut keeps its own (user-chosen) name.
void ApplyShownName(const Caches& c, ToolbarAppInfo& info) {
    if (info.exe.empty() || ToolbarFileExt(info.path) == L".lnk") return;
    if (const auto found = c.shown_names.find(ToolbarLower(ToolbarPlainPath(info.exe))); found != c.shown_names.end())
        info.name = found->second;
}

bool IsShortcut(const std::wstring& path) { return ToolbarFileExt(path) == L".lnk"; }

std::wstring Stem(const std::wstring& path) {
    std::wstring name = ToolbarFileName(path);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name.resize(dot);
    return name;
}

ToolbarAppInfo Placeholder(const std::wstring& path) {
    ToolbarAppInfo info;
    info.path = path;
    info.exe = IsShortcut(path) ? std::wstring() : ToolbarPlainPath(path);
    info.name = Stem(path);
    return info;
}

// The programs the 打开方式 submenu offers for the type, plus the default.
// ASSOC_FILTER_NONE would also list every app without SupportedTypes (the
// "look for another app" set), making Notepad or a browser "fit" .dwg.
std::vector<std::wstring> ReadPrograms(const std::wstring& ext) {
    std::vector<std::wstring> out;
    IEnumAssocHandlers* handlers = nullptr;
    if (SUCCEEDED(SHAssocEnumHandlers(ext.c_str(), ASSOC_FILTER_RECOMMENDED, &handlers)) && handlers) {
        IAssocHandler* handler = nullptr;
        ULONG fetched = 0;
        while (out.size() < 64 && handlers->Next(1, &handler, &fetched) == S_OK && handler) {
            LPWSTR name = nullptr, ui_name = nullptr;
            if (SUCCEEDED(handler->GetName(&name)) && name) {
                out.emplace_back(name);
                if (SUCCEEDED(handler->GetUIName(&ui_name)) && ui_name) {
                    RememberShownName(name, ui_name);
                    CoTaskMemFree(ui_name);
                }
                CoTaskMemFree(name);
            }
            handler->Release();
            handler = nullptr;
        }
        handlers->Release();
    }
    wchar_t exe[MAX_PATH * 2]{};
    DWORD cch = ARRAYSIZE(exe);
    if (SUCCEEDED(AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_EXECUTABLE, ext.c_str(),
                                    nullptr, exe, &cch)) && exe[0])
        out.emplace_back(exe);
    return out;
}

void RunOnWorker(const ToolbarAppWork& work, std::function<void()> job, HWND notify) {
    work([job = std::move(job), notify] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        job();
        if (SUCCEEDED(hr)) CoUninitialize();
        if (notify && IsWindow(notify)) InvalidateRect(notify, nullptr, FALSE);
    });
}

const std::vector<std::wstring>* CachedPrograms(Caches& c, const std::wstring& ext) {
    const auto found = c.programs.find(ext);
    return found == c.programs.end() ? nullptr : &found->second;
}

}  // namespace

ToolbarAppInfo DescribeToolbarAppNow(const std::wstring& path) {
    ToolbarAppInfo info = Placeholder(path);
    if (IsShortcut(path)) {
        // Start-menu shortcuts carry the name people know the program by.
        fs::DirEntry entry;
        if (ResolveLink(ToolbarPlainPath(path), entry) && !entry.link_target_is_dir)
            info.exe = ToolbarPlainPath(entry.link_target);
    } else {
        info.name = ProgramFriendlyName(info.exe);
        if (info.name.empty()) info.name = Stem(path);
    }
    info.icon = ProgramMenuIcon(info.exe.empty() ? ToolbarPlainPath(path) : info.exe);
    info.ready = true;
    Caches& c = Cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    ApplyShownName(c, info);
    return info;
}

ToolbarAppInfo ToolbarApp(const std::wstring& path, HWND notify, const ToolbarAppWork& work) {
    Caches& c = Cache();
    const std::wstring key = ToolbarLower(ToolbarPlainPath(path));
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        if (const auto found = c.apps.find(key); found != c.apps.end()) {
            ToolbarAppInfo info = found->second;
            ApplyShownName(c, info);   // the handlers may have been read since
            return info;
        }
        if (!c.pending_apps.insert(key).second) return Placeholder(path);
    }
    RunOnWorker(work, [path, key] {
        ToolbarAppInfo info = DescribeToolbarAppNow(path);
        Caches& cache = Cache();
        std::lock_guard<std::mutex> lock(cache.mutex);
        cache.pending_apps.erase(key);
        cache.apps[key] = std::move(info);
    }, notify);
    return Placeholder(path);
}

int ToolbarAppOpens(const std::wstring& exe, const std::wstring& ext, HWND notify,
                    const ToolbarAppWork& work) {
    if (exe.empty()) return -1;
    if (ext.empty() || ext == L".lnk") return 0;
    Caches& c = Cache();
    const std::wstring key = ToolbarLower(ext);
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        if (const auto* programs = CachedPrograms(c, key)) return ProgramListHas(*programs, exe) ? 1 : 0;
        if (!c.pending_exts.insert(key).second) return -1;
    }
    RunOnWorker(work, [key] {
        std::vector<std::wstring> programs = ReadPrograms(key);
        Caches& cache = Cache();
        std::lock_guard<std::mutex> lock(cache.mutex);
        cache.pending_exts.erase(key);
        cache.programs[key] = std::move(programs);
    }, notify);
    return -1;
}

bool ToolbarAppOpensNow(const std::wstring& exe, const std::wstring& ext) {
    if (exe.empty() || ext.empty() || ext == L".lnk") return false;
    Caches& c = Cache();
    const std::wstring key = ToolbarLower(ext);
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        if (const auto* programs = CachedPrograms(c, key)) return ProgramListHas(*programs, exe);
    }
    std::vector<std::wstring> programs = ReadPrograms(key);
    const bool opens = ProgramListHas(programs, exe);
    std::lock_guard<std::mutex> lock(c.mutex);
    c.programs[key] = std::move(programs);
    return opens;
}

std::vector<SuggestedToolbarApp> SuggestToolbarApps(const std::vector<std::wstring>& exts,
                                                    const std::vector<std::wstring>& exclude,
                                                    size_t cap) {
    struct Candidate {
        SuggestedToolbarApp app;
        std::wstring icon_ref;
        int score = 0;
        size_t first = 0;
    };
    std::map<std::wstring, Candidate> found;   // lower exe path
    size_t order = 0;
    for (const auto& ext : exts) {
        if (ext.empty() || ext == L".lnk") continue;
        IEnumAssocHandlers* handlers = nullptr;
        if (FAILED(SHAssocEnumHandlers(ext.c_str(), ASSOC_FILTER_RECOMMENDED, &handlers)) || !handlers)
            continue;
        IAssocHandler* handler = nullptr;
        ULONG fetched = 0;
        while (handlers->Next(1, &handler, &fetched) == S_OK && handler) {
            LPWSTR name = nullptr, ui_name = nullptr, icon = nullptr;
            int icon_index = 0;
            if (SUCCEEDED(handler->GetName(&name)) && name) {
                const std::wstring exe = name;
                const DWORD attrs = GetFileAttributesW(exe.c_str());
                // Desktop programs only: Store handlers have no exe to start
                // with arguments.
                if (ToolbarFileExt(exe) == L".exe" && attrs != INVALID_FILE_ATTRIBUTES &&
                    !(attrs & FILE_ATTRIBUTE_DIRECTORY) && !ProgramListHas(exclude, exe)) {
                    Candidate& c = found[ToolbarLower(exe)];
                    if (c.app.exe.empty()) {
                        c.app.exe = exe;
                        c.first = order++;
                        if (SUCCEEDED(handler->GetUIName(&ui_name)) && ui_name) {
                            c.app.name = ui_name;
                            RememberShownName(exe, ui_name);
                        }
                        if (SUCCEEDED(handler->GetIconLocation(&icon, &icon_index)) && icon && *icon)
                            c.icon_ref = std::wstring(icon) + L"," + std::to_wstring(icon_index);
                    }
                    ++c.score;
                }
            }
            if (name) CoTaskMemFree(name);
            if (ui_name) CoTaskMemFree(ui_name);
            if (icon) CoTaskMemFree(icon);
            handler->Release();
            handler = nullptr;
        }
        handlers->Release();
    }
    std::vector<Candidate> list;
    for (auto& [key, c] : found) list.push_back(std::move(c));
    std::stable_sort(list.begin(), list.end(), [](const Candidate& a, const Candidate& b) {
        return a.score != b.score ? a.score > b.score : a.first < b.first;
    });
    std::vector<SuggestedToolbarApp> out;
    for (auto& c : list) {
        if (out.size() >= cap) break;
        if (c.app.name.empty()) c.app.name = ProgramFriendlyName(c.app.exe);
        c.app.icon = ProgramMenuIcon(c.icon_ref.empty() ? c.app.exe : c.icon_ref);
        if (!c.app.icon) c.app.icon = ProgramMenuIcon(c.app.exe);
        out.push_back(std::move(c.app));
    }
    return out;
}

}  // namespace pulse::app
