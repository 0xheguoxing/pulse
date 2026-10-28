#include "app_wall_hint.h"
#include "app_internal.h"
#include "../fs/fs_enum.h"
#include "../ui/view_layout.h"
#include "../ui/wall_aspects.h"
#include <cwctype>
#include <string>
#include <unordered_set>

namespace pulse {
namespace {

constexpr ULONGLONG kShowMs = 9000;
constexpr ULONGLONG kFadeMs = 250;     // the renderer fades out in 200 ms
constexpr double kPhotoShare = 0.7;
constexpr size_t kMinPictures = 6;     // a couple of images is not a photo folder

struct WallHint {
    const app::Tab* tab = nullptr;     // the card's tab; null = no card
    std::wstring folder;
    ULONGLONG since = 0;
    ULONGLONG until = 0;               // hide time; 0 = showing
    // Last listing counted, so a folder is counted once per snapshot.
    const app::Tab* counted_tab = nullptr;
    const void* counted_snapshot = nullptr;
    std::unordered_set<std::wstring> offered;  // folders, lower case
};

WallHint& Hint() {
    static WallHint hint;
    return hint;
}

std::wstring FolderKey(const std::wstring& path) {
    std::wstring key = path;
    for (auto& ch : key) ch = static_cast<wchar_t>(std::towlower(ch));
    while (key.size() > 3 && (key.back() == L'\\' || key.back() == L'/')) key.pop_back();
    return key;
}

void Hide(AppState& s) {
    WallHint& h = Hint();
    if (!h.tab || h.until) return;
    h.until = GetTickCount64();
    if (s.hwnd) {
        KillTimer(s.hwnd, kTimerWallHint);
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
}

// Offscreen shots stay as they were unless a shot asks for the card.
bool ShotAllows(const AppState& s) {
    if (!s.shot.active) return true;
    wchar_t flag[4]{};
    return GetEnvironmentVariableW(L"PULSE_SHOT_WALL_HINT", flag, 4) > 0 && flag[0] == L'1';
}

bool GridView(const app::Tab& tab) {
    return !tab.wall && ui::IsZoomIconMode(tab.view_mode);
}

}  // namespace

void UpdateWallHint(AppState& s) {
    WallHint& h = Hint();
    const ULONGLONG now = GetTickCount64();
    if (h.tab && h.until && now >= h.until + kFadeMs) h.tab = nullptr;
    app::Tab* tab = ActiveTab(s);
    if (h.tab && !h.until &&
        (tab != h.tab || tab->current_path != h.folder || !GridView(*tab) || !s.appPrefs.photo_wall_hint))
        Hide(s);
    if (!s.appPrefs.photo_wall_hint || !ShotAllows(s) || !tab || tab->loading || !tab->snapshot ||
        !GridView(*tab) || tab->current_path.empty() || fs::IsVirtualPath(tab->current_path))
        return;
    if (tab == h.counted_tab && tab->snapshot.get() == h.counted_snapshot) return;
    h.counted_tab = tab;
    h.counted_snapshot = tab->snapshot.get();
    std::wstring key = FolderKey(tab->current_path);
    if (h.offered.contains(key)) return;
    const auto& entries = *tab->snapshot;
    size_t pictures = 0;
    for (const auto& e : entries)
        if (ui::IsWallPicture(e.name, e.is_dir)) ++pictures;
    if (pictures < kMinPictures || static_cast<double>(pictures) < entries.size() * kPhotoShare) return;
    h.offered.insert(std::move(key));
    h.tab = tab;
    h.folder = tab->current_path;
    h.since = now;
    h.until = 0;
    if (s.hwnd) {
        SetTimer(s.hwnd, kTimerWallHint, static_cast<UINT>(kShowMs), nullptr);
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
}

void FillWallHint(const app::Tab& tab, ui::PaneViewModel& out) {
    const WallHint& h = Hint();
    if (h.tab != &tab || tab.current_path != h.folder) return;
    out.wall_hint_since = h.since;
    out.wall_hint_until = h.until;
}

void WallHintTimer(AppState& s) {
    if (s.hwnd) KillTimer(s.hwnd, kTimerWallHint);
    Hide(s);
}

void WallHintSwitch(AppState& s) {
    Hide(s);
    ShowIconWall(s, true);
}

void WallHintNever(AppState& s) {
    Hide(s);
    s.appPrefs.photo_wall_hint = false;
    if (!s.isolatedTest) s.appPrefs.Save();
}

}  // namespace pulse
