#include "favorites_bar.h"
#include "app_commands.h"
#include "app_navigation.h"
#include "../common/display_path.h"
#include "../common/localization.h"
#include "keymap.h"
#include "../ui/fluent_menu.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <mutex>
#include <numeric>
#include <thread>
#include <unordered_map>

namespace pulse {
namespace {
constexpr int kFavoriteMenuBase = 36000;   // » and ★⌄ menus: kFavoriteMenuBase + favorite index
constexpr int kIconOnlyCommand = 36900, kHideCommand = 36901, kShowCommand = 36902, kStarHereCommand = 36903;
constexpr int kSubfolderBase = 37000;      // ⌄ menu: kSubfolderBase + subfolder index
constexpr size_t kSubfolderMax = 400;
constexpr wchar_t kFolderGlyph[] = L"\xE8B7";
constexpr wchar_t kFileGlyph[] = L"\xE8A5";

// A press on a favorite: a click on release, or a drag along the bar (the
// toolbar pins' model, MoveToolbarPinDrag): the favorite follows the pointer
// 1:1 and takes the slot whose centre is nearest; nothing is saved before the
// release. Below the row it fades; let go there and it leaves the list.
struct FavoriteDrag {
    const AppState* owner = nullptr;
    bool down = false, active = false, out = false;
    int from = -1;                 // its index in starred_items at the press
    int slot = -1;                 // where the bar draws it now
    POINT start{};
    float grab = 0.0f;             // pointer x - its left edge at the press
    float left = 0.0f, dy = 0.0f;
    std::vector<size_t> order;     // the bar's order while dragging (starred indices)
};
FavoriteDrag g_drag;

// The favorite last dragged off the bar, for the snack's undo.
struct RemovedFavorite {
    const AppState* owner = nullptr;
    app::StarredItem item;
    size_t index = 0;
};
RemovedFavorite g_removed;

// Offline dimming (§8 不可用): whether each favorite can be reached, asked on a
// worker thread (a dead share can take the whole network timeout) and cached
// for 30 s. The bar only ever reads the cache; an unchecked path counts as
// reachable.
class Reachability {
public:
    bool Offline(const std::wstring& path) {
        std::lock_guard<std::mutex> lock(m_);
        const auto it = state_.find(Key(path));
        return it != state_.end() && it->second.offline;
    }
    // Rechecks the stale ones in the background; repaints `hwnd` when an
    // answer changed. At most one worker at a time.
    void Refresh(const std::vector<std::wstring>& paths, HWND hwnd) {
        const ULONGLONG now = GetTickCount64();
        std::vector<std::wstring> due;
        {
            std::lock_guard<std::mutex> lock(m_);
            if (busy_ || now - polled_ < 2000) return;
            polled_ = now;
            for (const auto& path : paths) {
                const Entry& e = state_[Key(path)];
                if (!e.checked || now - e.checked >= 30000) due.push_back(path);
            }
            if (due.empty()) return;
            busy_ = true;
        }
        try {
            std::thread([this, due = std::move(due), hwnd] { Check(due, hwnd); }).detach();
        } catch (...) {
            std::lock_guard<std::mutex> lock(m_);
            busy_ = false;
        }
    }

private:
    struct Entry {
        bool offline = false;
        ULONGLONG checked = 0;
    };
    static std::wstring Key(const std::wstring& path) {
        std::wstring key = path;
        for (auto& c : key) c = static_cast<wchar_t>(towlower(c));
        return key;
    }
    void Check(const std::vector<std::wstring>& due, HWND hwnd) {
        bool changed = false;
        for (const auto& path : due) {
            const bool offline = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
            std::lock_guard<std::mutex> lock(m_);
            Entry& e = state_[Key(path)];
            changed = changed || e.offline != offline;
            e.offline = offline;
            e.checked = GetTickCount64();
        }
        {
            std::lock_guard<std::mutex> lock(m_);
            busy_ = false;
        }
        if (changed && IsWindow(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
    }
    std::mutex m_;
    std::unordered_map<std::wstring, Entry> state_;
    ULONGLONG polled_ = 0;
    bool busy_ = false;
};

// Never destroyed: a worker may still be asking a share at exit.
Reachability& Reach() {
    static Reachability* reach = new Reachability;
    return *reach;
}

bool DraggingIn(const AppState& s) {
    return g_drag.down && g_drag.owner == &s && g_drag.order.size() == s.places.starred_items.size();
}

// The starred item the bar shows at `slot`.
const app::StarredItem* FavoriteAt(const AppState& s, int slot) {
    if (slot < 0) return nullptr;
    size_t index = static_cast<size_t>(slot);
    if (DraggingIn(s)) {
        if (index >= g_drag.order.size()) return nullptr;
        index = g_drag.order[index];
    }
    if (index >= s.places.starred_items.size()) return nullptr;
    return &s.places.starred_items[index];
}

bool IsFolder(const app::StarredItem& item) { return item.kind != app::PlaceItemKind::File; }

// how: 0 here, 1 a new tab, 2 the other pane (a new tab without one).
void OpenFolder(AppState& s, const std::wstring& path, int how) {
    if (how == 2) {
        app::Pane* other = s.targetPane && s.targetPane != s.pane ? s.targetPane : nullptr;
        if (!other)
            for (app::Pane* pane : LiveLayout(s).VisiblePanes())
                if (pane && pane != s.pane) { other = pane; break; }
        if (other) {
            FocusPane(s, other);
            NavigateTo(s, path);
            return;
        }
        how = 1;
    }
    if (how == 1) OpenFolderTab(s, path);
    else NavigateTo(s, path);
}

void OpenFavorite(AppState& s, const app::StarredItem& item, int how) {
    if (item.kind == app::PlaceItemKind::File) {
        OpenPath(s, item.path);   // the default program
        return;
    }
    OpenFolder(s, item.path, how);
}

int HowFromKeys() {
    if (GetKeyState(VK_SHIFT) & 0x8000) return 2;
    if (GetKeyState(VK_CONTROL) & 0x8000) return 1;
    return 0;
}

void SaveAndRepaint(AppState& s) {
    if (!s.isolatedTest) s.appPrefs.Save();
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

std::wstring ChildOf(const std::wstring& dir, const std::wstring& name) {
    std::wstring path = dir;
    if (!path.empty() && path.back() != L'\\') path += L'\\';
    return path + name;
}

// Folders directly inside `dir`, sorted as Explorer does (digits as
// numbers); hidden and system ones are left out. False: it cannot be listed.
bool ListSubfolders(const std::wstring& dir, std::vector<std::wstring>& names) {
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW(ChildOf(dir, L"*").c_str(), FindExInfoBasic, &data,
                                   FindExSearchLimitToDirectories, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) ||
            std::wcscmp(data.cFileName, L".") == 0 || std::wcscmp(data.cFileName, L"..") == 0)
            continue;
        names.emplace_back(data.cFileName);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS,
                               a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()),
                               nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    return true;
}

// Lists on a worker thread: a share that has not answered in 800 ms counts
// as unreachable, so the UI never waits on the network for longer (the
// worker finishes on its own).
bool ListSubfoldersBounded(const std::wstring& dir, std::vector<std::wstring>& names) {
    struct Listing {
        std::mutex m;
        std::condition_variable cv;
        bool done = false, ok = false;
        std::vector<std::wstring> names;
    };
    auto job = std::make_shared<Listing>();
    try {
        std::thread([job, dir] {
            std::vector<std::wstring> found;
            const bool ok = ListSubfolders(dir, found);
            std::lock_guard<std::mutex> lock(job->m);
            job->ok = ok;
            job->names = std::move(found);
            job->done = true;
            job->cv.notify_all();
        }).detach();
    } catch (...) {
        return false;
    }
    std::unique_lock<std::mutex> lock(job->m);
    if (!job->cv.wait_for(lock, std::chrono::milliseconds(800), [&] { return job->done; })) return false;
    names = std::move(job->names);
    return job->ok;
}

ui::FluentMenuItem DisabledItem(l10n::StringId id) {
    ui::FluentMenuItem item;
    item.text = l10n::Get(id);
    item.enabled = false;
    return item;
}

// ⌄ on a favorite folder: its subfolders; one opens (Ctrl: a new tab,
// Shift: the other pane).
void ShowSubfolderMenu(AppState& s, const std::wstring& folder, POINT screen) {
    if (!EnsureMenu(s)) return;
    std::vector<std::wstring> names;
    const bool ok = ListSubfoldersBounded(folder, names);
    std::vector<ui::FluentMenuItem> items;
    for (size_t i = 0; i < names.size() && i < kSubfolderMax; ++i) {
        ui::FluentMenuItem item;
        item.command = kSubfolderBase + static_cast<int>(i);
        item.text = names[i];
        item.glyph = kFolderGlyph;
        items.push_back(std::move(item));
    }
    if (items.empty())
        items.push_back(DisabledItem(ok ? l10n::StringId::FavoritesBarNoSubfolders
                                        : l10n::StringId::FavoritesBarUnreachable));
    const int cmd = s.menu->TrackPopup(screen, std::move(items));
    const int i = cmd - kSubfolderBase;
    if (cmd < kSubfolderBase || i < 0 || static_cast<size_t>(i) >= names.size()) return;
    OpenFolder(s, ChildOf(folder, names[static_cast<size_t>(i)]), HowFromKeys());
}

// Right-click on the bar's empty part: icons only, hide.
void ShowFavoritesBarMenu(AppState& s, POINT screen) {
    if (!EnsureMenu(s)) return;
    std::vector<ui::FluentMenuItem> items;
    ui::FluentMenuItem icons;
    icons.command = kIconOnlyCommand;
    icons.text = l10n::Get(l10n::StringId::FavoritesBarIconOnly);
    icons.checked = s.appPrefs.favorites_bar_icon_only;
    items.push_back(std::move(icons));
    ui::FluentMenuItem hide;
    hide.command = kHideCommand;
    hide.text = l10n::Get(l10n::StringId::FavoritesBarHide);
    hide.glyph = L"\xED1A";   // hide
    hide.shortcut = app::ShownShortcut(L"Ctrl+Shift+B");
    items.push_back(std::move(hide));
    const int cmd = s.menu->TrackPopup(screen, std::move(items));
    if (cmd == kIconOnlyCommand) {
        s.appPrefs.favorites_bar_icon_only = !s.appPrefs.favorites_bar_icon_only;
        SaveAndRepaint(s);
    } else if (cmd == kHideCommand) {
        ToggleFavoritesBar(s);
    }
}

ui::FluentMenuItem FavoriteMenuItem(const app::StarredItem& item, size_t index) {
    ui::FluentMenuItem out;
    out.command = kFavoriteMenuBase + static_cast<int>(index);
    out.text = app::TabTitle(item.path);
    out.glyph = item.kind == app::PlaceItemKind::File ? kFileGlyph : kFolderGlyph;
    return out;
}

void OpenFavoriteCommand(AppState& s, int cmd) {
    if (cmd < kFavoriteMenuBase) return;
    const size_t index = static_cast<size_t>(cmd - kFavoriteMenuBase);
    if (index < s.places.starred_items.size())
        OpenFavorite(s, app::StarredItem(s.places.starred_items[index]), HowFromKeys());
}

// »: the favorites without room on the bar, in list order.
void ShowFavoritesOverflow(AppState& s, int first, POINT screen) {
    if (!EnsureMenu(s)) return;
    const auto& list = s.places.starred_items;
    std::vector<ui::FluentMenuItem> items;
    for (size_t i = static_cast<size_t>(std::max(first, 0)); i < list.size(); ++i)
        items.push_back(FavoriteMenuItem(list[i], i));
    if (items.empty()) return;
    OpenFavoriteCommand(s, s.menu->TrackPopup(screen, std::move(items)));
}

// The address bar's ★⌄ (the bar hidden): star / unstar this folder, then the
// whole list, then a way to show the bar.
void ShowFavoritesDropdown(AppState& s, POINT screen) {
    if (!EnsureMenu(s)) return;
    const app::Tab* tab = ActiveTab(s);
    const std::wstring here = tab && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path)
        ? tab->current_path : std::wstring{};
    const bool starred = !here.empty() && s.places.IsStarred(here);
    std::vector<ui::FluentMenuItem> items;
    if (!here.empty()) {
        ui::FluentMenuItem star;
        star.command = kStarHereCommand;
        star.text = l10n::Get(starred ? l10n::StringId::FavoritesUnstarHere : l10n::StringId::FavoritesStarHere);
        star.glyph = starred ? L"\xE735" : L"\xE734";
        star.separator_after = true;
        items.push_back(std::move(star));
    }
    const auto& list = s.places.starred_items;
    for (size_t i = 0; i < list.size(); ++i) items.push_back(FavoriteMenuItem(list[i], i));
    if (list.empty()) items.push_back(DisabledItem(l10n::StringId::NoStarred));
    items.back().separator_after = true;
    ui::FluentMenuItem show;
    show.command = kShowCommand;
    show.text = l10n::Get(l10n::StringId::FavoritesBarShow);
    show.shortcut = app::ShownShortcut(L"Ctrl+Shift+B");
    items.push_back(std::move(show));
    const int cmd = s.menu->TrackPopup(screen, std::move(items));
    if (cmd == kStarHereCommand) ToggleStarred(s, here, app::PlaceItemKind::Folder);
    else if (cmd == kShowCommand) ToggleFavoritesBar(s);
    else OpenFavoriteCommand(s, cmd);
}

POINT BelowControl(const AppState& s, const ui::HitTestResult& hit) {
    POINT pt{static_cast<LONG>(std::lround(hit.control_bounds.left)),
             static_cast<LONG>(std::lround(hit.control_bounds.bottom))};
    ClientToScreen(s.hwnd, &pt);
    return pt;
}

void BeginDrag(AppState& s, const ui::HitTestResult& hit) {
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(s.hwnd, &pt);
    g_drag = FavoriteDrag{};
    g_drag.owner = &s;
    g_drag.down = true;
    g_drag.from = g_drag.slot = hit.index;
    g_drag.start = pt;
    g_drag.grab = static_cast<float>(pt.x) - hit.control_bounds.left;
    g_drag.order.resize(s.places.starred_items.size());
    std::iota(g_drag.order.begin(), g_drag.order.end(), size_t{0});
    SetCapture(s.hwnd);
}

// Off the list, with the snack's undo.
void RemoveFavorite(AppState& s, size_t index) {
    if (index >= s.places.starred_items.size()) return;
    g_removed = RemovedFavorite{&s, s.places.starred_items[index], index};
    const std::wstring path = g_removed.item.path;
    ToggleStarred(s, path);   // starred, so this removes it
    if (!s.hwnd) return;
    s.notification_toast.ShowSnack(
        s.hwnd, ui::SnackIcon::Undo, l10n::Get(l10n::StringId::FavoritesBarRemoved), app::TabTitle(path),
        {ui::SnackAction{l10n::Get(l10n::StringId::SnackUndo), WM_FAVORITE_UNDO}});
}
} // namespace

void FillFavoritesBar(AppState& s, ui::WindowViewModel& vm) {
    const bool on = s.appPrefs.favorites_bar && !s.renderer.Embedded();
    s.renderer.SetFavoritesBar(on);
    vm.favorites_bar_on = on;
    const app::Tab* tab = ActiveTab(s);
    if (!on) {
        // The address bar's ★⌄ shows whether this folder is starred.
        vm.favorites_here = tab && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path) &&
                            s.places.IsStarred(tab->current_path);
        return;
    }
    vm.favorites_bar_icon_only = s.appPrefs.favorites_bar_icon_only;
    vm.favorites_bar_hint = l10n::Get(l10n::StringId::FavoritesBarEmpty);
    vm.favorites_bar_more = l10n::Get(l10n::StringId::FavoritesBarMore);
    vm.favorites_bar_drop = s.dropFavorite;
    vm.favorites_bar_drop_add = s.dropFavoriteAdd;
    // Starred paths are normalized (\\?\C:\...); compare as display text.
    const std::wstring here = tab ? pulse::path::FriendlyPathText(tab->current_path) : std::wstring{};
    const size_t count = s.places.starred_items.size();
    vm.favorites_bar.reserve(count);
    {
        std::vector<std::wstring> paths;
        paths.reserve(count);
        for (const auto& entry : s.places.starred_items) paths.push_back(entry.path);
        Reach().Refresh(paths, s.hwnd);
    }
    for (size_t slot = 0; slot < count; ++slot) {
        const app::StarredItem& entry = *FavoriteAt(s, static_cast<int>(slot));
        ui::WindowViewModel::FavoritesBarItem item;
        item.path = entry.path;
        item.label = app::TabTitle(entry.path);
        item.folder = IsFolder(entry);
        item.current = item.folder && !here.empty() &&
                       _wcsicmp(here.c_str(), pulse::path::FriendlyPathText(entry.path).c_str()) == 0;
        item.offline = Reach().Offline(entry.path);
        vm.favorites_bar.push_back(std::move(item));
    }
    if (DraggingIn(s) && g_drag.active) {
        vm.favorites_bar_drag = g_drag.slot;
        vm.favorites_bar_drag_left = g_drag.left;
        vm.favorites_bar_drag_dy = g_drag.dy;
        vm.favorites_bar_drag_out = g_drag.out;
    }
}

void ToggleFavoritesBar(AppState& s) {
    s.appPrefs.favorites_bar = !s.appPrefs.favorites_bar;
    s.renderer.SetFavoritesBar(s.appPrefs.favorites_bar && !s.renderer.Embedded());
    SaveAndRepaint(s);
}

bool HandleFavoritesBarPress(AppState& s, const ui::HitTestResult& hit) {
    if (hit.region == ui::HitTestResult::AddressFavorites) {
        ShowFavoritesDropdown(s, BelowControl(s, hit));
        return true;
    }
    if (hit.region == ui::HitTestResult::FavoritesBarMore) {
        ShowFavoritesOverflow(s, hit.index, BelowControl(s, hit));
        return true;
    }
    if (hit.region == ui::HitTestResult::FavoritesBarChevron) {
        if (const app::StarredItem* item = FavoriteAt(s, hit.index)) {
            const std::wstring folder = item->path;
            ShowSubfolderMenu(s, folder, BelowControl(s, hit));
        }
        return true;
    }
    if (hit.region != ui::HitTestResult::FavoritesBarItem) return false;
    if (FavoriteAt(s, hit.index)) BeginDrag(s, hit);
    return true;   // the bar's empty part does nothing
}

bool FavoritesBarDragging() { return g_drag.down; }

bool MoveFavoritesBarDrag(AppState& s, POINT pt) {
    FavoriteDrag& d = g_drag;
    if (!d.down) return false;
    if (!DraggingIn(s)) {   // the list changed under the press
        EndFavoritesBarDrag(s, true);
        return false;
    }
    const float dx = static_cast<float>(pt.x - d.start.x), dy = static_cast<float>(pt.y - d.start.y);
    if (!d.active) {
        if (std::hypot(dx, dy) < 4.0f * s.scale) return false;
        d.active = true;
    }
    const ui::WindowViewModel vm = BuildVm(s);
    const ui::FavoritesBarLayout lay = s.renderer.FavoritesBarLayoutAt(vm, static_cast<float>(s.compositor.Width()));
    if (d.slot < 0 || static_cast<size_t>(d.slot) >= lay.items.size()) return true;
    const D2D1_RECT_F home = lay.items[static_cast<size_t>(d.slot)];
    const float w = home.right - home.left;
    d.left = static_cast<float>(pt.x) - d.grab;
    // Below the row it stays in sight (14 DIP down at most), faded once
    // letting go would take it off.
    d.dy = std::clamp(dy, -4.0f * s.scale, 14.0f * s.scale);
    d.out = dy > 34.0f * s.scale;
    // The slot whose centre is nearest its own; folders stay ahead of files.
    const bool folder = IsFolder(s.places.starred_items[d.order[static_cast<size_t>(d.slot)]]);
    int best = d.slot;
    float best_dist = 1e9f;
    for (size_t i = 0; i < lay.items.size() && i < d.order.size(); ++i) {
        const D2D1_RECT_F r = lay.items[i];
        if (r.right <= r.left || IsFolder(s.places.starred_items[d.order[i]]) != folder) continue;
        const float dist = std::fabs((r.left + r.right) * 0.5f - (d.left + w * 0.5f));
        if (dist < best_dist) { best_dist = dist; best = static_cast<int>(i); }
    }
    if (best != d.slot && !d.out) {
        const size_t moved = d.order[static_cast<size_t>(d.slot)];
        d.order.erase(d.order.begin() + d.slot);
        d.order.insert(d.order.begin() + best, moved);
        d.slot = best;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

void EndFavoritesBarDrag(AppState& s, bool cancel) {
    const FavoriteDrag d = g_drag;
    if (!d.down) return;
    g_drag = FavoriteDrag{};
    if (GetCapture() == s.hwnd) ReleaseCapture();
    InvalidateRect(s.hwnd, nullptr, FALSE);
    if (d.owner != &s || cancel || d.from < 0) return;
    const size_t from = static_cast<size_t>(d.from);
    if (from >= s.places.starred_items.size()) return;
    if (!d.active) {
        OpenFavorite(s, app::StarredItem(s.places.starred_items[from]), HowFromKeys());
        return;
    }
    if (d.order.size() != s.places.starred_items.size()) return;
    if (d.out) RemoveFavorite(s, from);
    else if (d.slot >= 0 && static_cast<size_t>(d.slot) != from) s.places.MoveStarredItem(from, static_cast<size_t>(d.slot));
}

void UndoFavoriteRemoval(AppState& s) {
    RemovedFavorite r = std::move(g_removed);
    g_removed = RemovedFavorite{};
    if (r.owner != &s || r.item.path.empty() || s.places.IsStarred(r.item.path)) return;
    ToggleStarred(s, r.item.path, r.item.kind);
    // It comes back at the end of its kind; take it back to its place.
    const auto& list = s.places.starred_items;
    for (size_t i = 0; i < list.size(); ++i) {
        if (_wcsicmp(list[i].path.c_str(), r.item.path.c_str()) != 0) continue;
        s.places.MoveStarredItem(i, std::min(r.index, list.size() - 1));
        break;
    }
    if (!r.item.badge.empty() || r.item.badge_rgb != app::kDefaultBadgeRgb)
        s.places.SetStarredBadge(r.item.path, r.item.badge, r.item.badge_rgb);
    RefreshStarredViews(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool HandleFavoritesBarContextMenu(AppState& s, const ui::HitTestResult& hit, POINT screen) {
    if (hit.region == ui::HitTestResult::AddressFavorites) {
        ShowFavoritesDropdown(s, screen);
        return true;
    }
    if (hit.region != ui::HitTestResult::FavoritesBarItem && hit.region != ui::HitTestResult::FavoritesBarMore &&
        hit.region != ui::HitTestResult::FavoritesBarChevron)
        return false;
    // A favorite: the same menu as its row in the sidebar's 收藏夹.
    if (const app::StarredItem* item = FavoriteAt(s, hit.index);
        item && hit.region != ui::HitTestResult::FavoritesBarMore) {
        ShowCuratedItemMenu(s, std::wstring(item->path), false, screen);
        return true;
    }
    ShowFavoritesBarMenu(s, screen);
    return true;
}

std::wstring FavoritesBarMiddleClickPath(const AppState& s, const ui::HitTestResult& hit) {
    if (hit.region != ui::HitTestResult::FavoritesBarItem && hit.region != ui::HitTestResult::FavoritesBarChevron)
        return {};
    const app::StarredItem* item = FavoriteAt(s, hit.index);
    return item && IsFolder(*item) ? item->path : std::wstring{};
}

std::wstring FavoritesBarDropFolder(const AppState& s, const ui::HitTestResult& hit, bool* offline) {
    const app::StarredItem* item = FavoriteAt(s, hit.index);
    const bool away = item && Reach().Offline(item->path);
    if (offline) *offline = away;
    return item && IsFolder(*item) && !away ? item->path : std::wstring{};
}

std::wstring FavoritesBarTooltip(const AppState& s, const ui::HitTestResult& hit) {
    const app::StarredItem* item = FavoriteAt(s, hit.index);
    if (!item) return {};
    std::wstring text = pulse::path::FriendlyPathText(item->path);
    if (Reach().Offline(item->path)) {
        text += L"\n";
        text += l10n::Get(l10n::StringId::FavoritesBarUnreachable);
    }
    return text;
}
} // namespace pulse
