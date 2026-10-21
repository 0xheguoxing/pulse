#include "app_catalog.h"

#include "../index/filename_pinyin.h"

#include <knownfolders.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cwctype>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace pulse::app {
namespace {

std::wstring Fold(std::wstring_view text) {
    std::wstring out(text);
    if (!out.empty()) CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
    return out;
}

bool Separator(wchar_t c) {
    return c == L' ' || c == L'-' || c == L'_' || c == L'.' || c == L'(' || c == L')' ||
        c == L'&' || c == L'+' || c == L'/' || c == L'\\' || c == L'\u3000';
}

bool WordPrefix(const std::wstring& folded_name, const std::wstring& query) {
    for (size_t at = folded_name.find(query); at != std::wstring::npos;
         at = folded_name.find(query, at + 1))
        if (at > 0 && Separator(folded_name[at - 1])) return true;
    return false;
}

// First letter of every word, plus the capitals inside one ("PowerToys" -> pt).
std::wstring Initials(std::wstring_view name) {
    std::wstring out;
    for (size_t i = 0; i < name.size(); ++i) {
        const wchar_t c = name[i];
        if (Separator(c)) continue;
        const bool word_start = i == 0 || Separator(name[i - 1]);
        const bool camel = i > 0 && std::iswupper(c) && std::iswlower(name[i - 1]);
        if (word_start || camel) out.push_back(c);
    }
    return Fold(out);
}

std::vector<std::wstring> Words(const std::wstring& folded) {
    std::vector<std::wstring> out;
    size_t begin = 0;
    while (begin < folded.size()) {
        while (begin < folded.size() && std::iswspace(folded[begin])) ++begin;
        size_t end = begin;
        while (end < folded.size() && !std::iswspace(folded[end])) ++end;
        if (end > begin) out.push_back(folded.substr(begin, end - begin));
        begin = end;
    }
    return out;
}

// 0 = the whole name ... 5 = a substring; -1 = no match.
int Score(const AppEntry& app, const std::wstring& folded_name, const std::wstring& query,
          bool pinyin) {
    if (folded_name == query) return 0;
    if (folded_name.starts_with(query)) return 1;
    if (WordPrefix(folded_name, query)) return 2;
    if (query.size() >= 2 && Initials(app.name).starts_with(query)) return 3;
    if (pinyin && index::PinyinEligible(query) && index::HasPinyinCharacters(app.name)) {
        if (const auto match = index::FindPinyinMatch(app.name, query)) {
            if (match.start == 0) return match.kind == index::PinyinMatchKind::Full ? 2 : 3;
            return 4;
        }
    }
    if (folded_name.find(query) != std::wstring::npos) return 5;
    return -1;
}

bool HasNoiseExtension(std::wstring_view path) {
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring_view::npos) return false;
    const std::wstring ext = Fold(path.substr(dot));
    static constexpr std::wstring_view kNoise[] = {L".url", L".htm", L".html", L".chm", L".txt",
                                                   L".pdf", L".rtf", L".md",  L".hlp", L".log",
                                                   L".ini", L".website"};
    return std::find(std::begin(kNoise), std::end(kNoise), ext) != std::end(kNoise);
}

} // namespace

bool IsAppNoise(const AppEntry& app) {
    const std::wstring name = Fold(app.name);
    if (name.find(L"uninstall") != std::wstring::npos || name.find(L"卸载") != std::wstring::npos ||
        name.find(L"卸載") != std::wstring::npos || name.find(L"解除安裝") != std::wstring::npos)
        return true;
    const std::wstring path = Fold(app.path);
    if (path.find(L"http://") != std::wstring::npos || path.find(L"https://") != std::wstring::npos)
        return true;
    return HasNoiseExtension(app.path);
}

std::vector<size_t> MatchApps(const std::vector<AppEntry>& apps, std::wstring_view query,
                              bool pinyin, size_t limit) {
    std::vector<size_t> out;
    const std::wstring folded = Fold(query);
    const auto words = Words(folded);
    if (words.empty() || limit == 0) return out;
    std::wstring whole;
    for (const auto& word : words) whole += (whole.empty() ? L"" : L" ") + word;

    struct Hit {
        int score;
        size_t index;
    };
    std::vector<Hit> hits;
    for (size_t i = 0; i < apps.size(); ++i) {
        const std::wstring name = Fold(apps[i].name);
        int score = Score(apps[i], name, whole, pinyin);
        if (score < 0 && words.size() > 1) {
            bool all = true;
            for (const auto& word : words)
                all = all && Score(apps[i], name, word, pinyin) >= 0;
            if (all) score = 5;
        }
        if (score >= 0) hits.push_back({score, i});
    }
    std::sort(hits.begin(), hits.end(), [&](const Hit& a, const Hit& b) {
        if (a.score != b.score) return a.score < b.score;
        const auto& an = apps[a.index].name;
        const auto& bn = apps[b.index].name;
        if (an.size() != bn.size()) return an.size() < bn.size();
        return CompareStringOrdinal(an.c_str(), static_cast<int>(an.size()), bn.c_str(),
                                    static_cast<int>(bn.size()), TRUE) == CSTR_LESS_THAN;
    });
    for (size_t i = 0; i < hits.size() && out.size() < limit; ++i) out.push_back(hits[i].index);
    return out;
}

std::vector<AppEntry> LoadApps() {
    std::vector<AppEntry> out;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellItem* folder = nullptr;
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr,
                                       IID_PPV_ARGS(&folder))) && folder) {
        IEnumShellItems* items = nullptr;
        if (SUCCEEDED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&items))) && items) {
            std::unordered_set<std::wstring> names;
            IShellItem* item = nullptr;
            while (out.size() < 4000 && items->Next(1, &item, nullptr) == S_OK && item) {
                PWSTR name = nullptr;
                PWSTR id = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name && *name &&
                    SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &id)) && id && *id) {
                    AppEntry entry{name, std::wstring(kAppsFolderPrefix) + id};
                    if (!IsAppNoise(entry) && names.insert(Fold(entry.name)).second)
                        out.push_back(std::move(entry));
                }
                CoTaskMemFree(name);
                CoTaskMemFree(id);
                item->Release();
                item = nullptr;
            }
            items->Release();
        }
        folder->Release();
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return out;
}

struct AppCatalog::State {
    std::mutex mutex;
    std::shared_ptr<const std::vector<AppEntry>> apps = std::make_shared<const std::vector<AppEntry>>();
    bool loading = false;
    uint64_t loaded_at = 0;  // GetTickCount64; 0 = never
};

AppCatalog::AppCatalog() : state_(std::make_shared<State>()) {}
AppCatalog::~AppCatalog() = default;

void AppCatalog::Refresh(HWND notify, UINT message, uint64_t max_age_ms) {
    {
        std::lock_guard lock(state_->mutex);
        if (state_->loading) return;
        if (state_->loaded_at && GetTickCount64() - state_->loaded_at < max_age_ms) return;
        state_->loading = true;
    }
    try {
        std::thread([state = state_, notify, message] {
            auto apps = std::make_shared<const std::vector<AppEntry>>(LoadApps());
            {
                std::lock_guard lock(state->mutex);
                state->apps = std::move(apps);
                state->loading = false;
                state->loaded_at = GetTickCount64();
            }
            if (notify && IsWindow(notify)) PostMessageW(notify, message, 0, 0);
        }).detach();
    } catch (...) {
        std::lock_guard lock(state_->mutex);
        state_->loading = false;
    }
}

std::shared_ptr<const std::vector<AppEntry>> AppCatalog::Apps() const {
    std::lock_guard lock(state_->mutex);
    return state_->apps;
}

} // namespace pulse::app
