// app_hover_preview.cpp - The hover preview card (see app_hover_preview.h).
#include "app_hover_preview.h"
#include "app_internal.h"
#include "../common/localization.h"
#include "../common/preview_extensions.h"
#include "../ui/video_preview.h"
#include <windowsx.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <string>
#include <unordered_map>

namespace pulse {
namespace {

bool HoverPreviewOn(const AppState& s) {
    return s.appPrefs.preview_trigger == app::kPreviewTriggerHover && !s.safeMode;
}

// The shell's name for the file type ("JPEG 图像"), by extension: a registry
// lookup (SHGFI_USEFILEATTRIBUTES never opens the file), remembered per
// extension.
std::wstring TypeName(const std::wstring& name) {
    thread_local std::unordered_map<std::wstring, std::wstring> cache;
    const size_t dot = name.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos || dot == 0 ? std::wstring() : name.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    if (const auto it = cache.find(ext); it != cache.end()) return it->second;
    std::wstring type;
    SHFILEINFOW info{};
    const std::wstring probe = L"file" + ext;
    if (SHGetFileInfoW(probe.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                       SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES))
        type = info.szTypeName;
    if (type.empty()) {
        std::wstring upper = ext.size() > 1 ? ext.substr(1) : std::wstring();
        for (auto& c : upper) c = static_cast<wchar_t>(towupper(c));
        type = upper.empty() ? std::wstring(l10n::Pick(L"文件", L"File"))
                             : upper + l10n::Pick(L" 文件", L" file");
    }
    if (cache.size() > 256) cache.clear();
    cache.emplace(ext, type);
    return type;
}

// Nothing else owns the pointer: no drag, marquee, resize or capture.
bool PointerFree(const AppState& s) {
    return !GetCapture() && !s.dragPending && !s.marqueePending && !s.marqueeActive &&
        !s.tabDragging && !s.scrollbarDragging && !s.columnResizing && !s.stripResizing &&
        !s.stripHScrolling && !s.splitterDragging && !s.detailsPanelResizing;
}

bool CanShow(AppState& s) {
    return s.hwnd && HoverPreviewOn(s) && GetForegroundWindow() == s.hwnd && !IsIconic(s.hwnd) &&
        !s.shot.active && !s.menushot && !s.quickPreview.visible() && !s.context_menu.menu_open() &&
        s.renameIndex < 0 && PointerFree(s) &&
        s.hoverRegion == static_cast<int>(ui::HitTestResult::Row);
}

app::Tab* TabAtSlot(AppState& s, int slot) {
    app::Pane* pane = slot >= 0 ? PaneAtSlot(s, slot) : s.pane;
    return pane ? pane->ActiveTab() : nullptr;
}

std::wstring LowerExtension(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos || dot == 0 ? std::wstring() : name.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    return ext;
}

// Plays (silently) in the card: video Media Foundation or the FFmpeg pack
// can open; audio-only files keep their cover art or icon.
bool CardVideo(const std::wstring& path) {
    return ui::VideoPreview::Supports(path) && !ui::VideoPreview::IsAudio(path);
}

// Takes the preview host or a player a noticeable moment: these show the
// dwell line on the row and a loading sweep in the card.
bool SlowToDecode(const fs::DirEntry& entry, const std::wstring& path) {
    if (entry.size >= 48ull * 1024 * 1024) return true;
    const std::wstring ext = LowerExtension(entry.name);
    if (preview::IsVideoExtension(ext) || CardVideo(path) || preview::IsPdfRasterExtension(ext) ||
        preview::IsArchiveExtension(ext) || preview::IsPsdExtension(ext) ||
        preview::IsFamilyExtension(preview::PreviewFamily::Raw, ext))
        return true;
    return preview::IsOneOf(ext, {L".docx", L".pptx", L".xlsx", L".xlsm", L".epub", L".ipynb", L".dwg",
                                  L".heic", L".heif", L".hif", L".avif"});
}

// Repaints for the card's animations (dwell line, loading sweep, video
// progress); 0 stops them.
void SetFrameTimer(AppState& s, UINT ms) {
    auto& card = s.hoverPreview;
    if (!s.hwnd || ms == card.frame_ms) return;
    card.frame_ms = ms;
    if (ms) SetTimer(s.hwnd, kTimerHoverPreviewFrame, ms, nullptr);
    else KillTimer(s.hwnd, kTimerHoverPreviewFrame);
}

void ClearDwell(AppState& s) {
    auto& card = s.hoverPreview;
    if (card.dwell_row < 0) return;
    card.dwell_row = card.dwell_pane = -1;
    card.dwell_start = 0;
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Starts the dwell for the hovered row: the chosen delay, or a short one
// when the pointer just left a card (browsing a column of files).
void Arm(AppState& s) {
    auto& card = s.hoverPreview;
    if (!s.hwnd) return;
    KillTimer(s.hwnd, kTimerHoverPreview);
    ClearDwell(s);
    if (!HoverPreviewOn(s) || s.hoverRow < 0 || card.wait_move) return;
    if (s.hoverRow == card.quiet_row && s.hoverPaneIndex == card.quiet_pane) return;
    card.quiet_row = card.quiet_pane = -1;
    const ULONGLONG now = GetTickCount64();
    const bool browsing = card.hidden_at != 0 && now - card.hidden_at < 450;
    const int delay = browsing ? 60 : app::NormalizeHoverPreviewDelay(s.appPrefs.hover_preview_delay_ms);
    SetTimer(s.hwnd, kTimerHoverPreview, static_cast<UINT>(delay), nullptr);
    if (browsing) return;
    app::Tab* tab = TabAtSlot(s, s.hoverPaneIndex);
    if (!tab || !tab->snapshot || tab->loading || s.hoverRow >= static_cast<int>(tab->EntryCount())) return;
    const fs::DirEntry& entry = tab->EntryAt(static_cast<size_t>(s.hoverRow));
    if (entry.is_dir || entry.change_record_only || !SlowToDecode(entry, EntryFullPath(*tab, s.hoverRow)))
        return;
    card.dwell_row = s.hoverRow;
    card.dwell_pane = s.hoverPaneIndex;
    card.dwell_start = now;
    card.dwell_ms = static_cast<UINT>(delay);
    SetFrameTimer(s, 16);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void Show(AppState& s) {
    auto& card = s.hoverPreview;
    if (s.hoverRow < 0 || !CanShow(s)) return;
    app::Tab* tab = TabAtSlot(s, s.hoverPaneIndex);
    if (!tab || !tab->snapshot || tab->loading || s.hoverRow >= static_cast<int>(tab->EntryCount()))
        return;
    const fs::DirEntry& entry = tab->EntryAt(static_cast<size_t>(s.hoverRow));
    // Folders open on click: a card on every folder on the way would only
    // get in the way.
    if (entry.is_dir || entry.change_record_only) return;
    ui::QuickPreviewItem item;
    if (!QuickPreviewItemForRow(s, *tab, s.hoverRow, item)) return;
    card.visible = true;
    card.row = s.hoverRow;
    card.pane = s.hoverPaneIndex;
    card.path = std::move(item.path);
    card.name = std::move(item.name);
    card.attrs = item.attrs;
    card.size = item.size;
    card.modified = item.modified;
    card.type_text = TypeName(card.name);
    card.point = s.hoverPoint;
    card.hidden_at = 0;
    card.video = CardVideo(card.path);
    card.slow = SlowToDecode(entry, card.path);
    card.shown_at = GetTickCount64();
    ClearDwell(s);
    SetFrameTimer(s, 33);  // the first frames settle the loading sweep / video
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// A click, key or wheel: the card goes and stays away until the pointer
// moves again (after a click, until it reaches another row).
void Quiet(AppState& s, bool this_row) {
    auto& card = s.hoverPreview;
    HideHoverPreview(s);
    card.hidden_at = 0;
    if (this_row) {
        card.quiet_row = s.hoverRow;
        card.quiet_pane = s.hoverPaneIndex;
    }
    card.wait_move = true;
    POINT point{};
    if (s.hwnd && GetCursorPos(&point) && ScreenToClient(s.hwnd, &point)) card.quiet_point = point;
    else card.quiet_point = s.hoverPoint;
}

} // namespace

void HideHoverPreview(AppState& s) {
    auto& card = s.hoverPreview;
    if (s.hwnd) KillTimer(s.hwnd, kTimerHoverPreview);
    ClearDwell(s);
    if (!card.visible) return;
    card.visible = false;
    card.hidden_at = GetTickCount64();
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

void NoteHoverPreviewRow(AppState& s) {
    HideHoverPreview(s);
    Arm(s);
}

bool HoverPreviewMessage(AppState& s, HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (hwnd != s.hwnd) return false;
    auto& card = s.hoverPreview;
    switch (msg) {
    case WM_TIMER:
        if (wparam == kTimerHoverPreviewFrame) {
            if (!card.visible && card.dwell_row < 0) SetFrameTimer(s, 0);
            else InvalidateRect(hwnd, nullptr, FALSE);
            return true;
        }
        if (wparam != kTimerHoverPreview) return false;
        KillTimer(hwnd, kTimerHoverPreview);
        Show(s);
        return true;
    case WM_MOUSEMOVE:
        if (card.wait_move) {
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (std::abs(point.x - card.quiet_point.x) + std::abs(point.y - card.quiet_point.y) > 2) {
                card.wait_move = false;
                if (!card.visible) Arm(s);
            }
        }
        return false;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wparam == VK_SHIFT || wparam == VK_CONTROL || wparam == VK_MENU) return false;
        // Space with a card up opens it in quick look (app_input.cpp).
        if (msg == WM_KEYDOWN && wparam == VK_SPACE && card.visible) return false;
        Quiet(s, false);
        return false;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK:
        Quiet(s, true);
        return false;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        Quiet(s, false);
        return false;
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) HideHoverPreview(s);
        return false;
    case WM_KILLFOCUS:
    case WM_ENTERSIZEMOVE:
    case WM_SIZE:
        HideHoverPreview(s);
        return false;
    default:
        return false;
    }
}

bool OpenHoverPreviewInQuickLook(AppState& s) {
    auto& card = s.hoverPreview;
    if (!card.visible) return false;
    const int row = card.row;
    const int slot = card.pane;
    HideHoverPreview(s);
    card.hidden_at = 0;
    // No card over this row again when quick look closes.
    card.quiet_row = row;
    card.quiet_pane = slot;
    app::Pane* pane = slot >= 0 ? PaneAtSlot(s, slot) : s.pane;
    app::Tab* tab = pane ? pane->ActiveTab() : nullptr;
    if (!tab || !tab->snapshot || row < 0 || row >= static_cast<int>(tab->EntryCount())) return false;
    if (pane != s.pane) FocusPane(s, pane);
    if (s.quickPreview.visible()) s.quickPreview.Close();
    tab->SelectOnly(row);
    EnsureRowVisible(s, *tab, row);
    ToggleQuickPreview(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

bool StageHoverDwellShot(AppState& s, int nth_file) {
    if (!StageHoverPreviewShot(s, nth_file)) return false;
    auto& card = s.hoverPreview;
    card.visible = false;
    card.dwell_row = card.row;
    card.dwell_pane = card.pane;
    card.dwell_ms = 500;
    card.dwell_start = GetTickCount64() - 300;  // 60% through the dwell
    return true;
}

bool StageHoverPreviewShot(AppState& s, int nth_file) {
    app::Tab* tab = s.pane ? s.pane->ActiveTab() : nullptr;
    if (!tab || !tab->snapshot) return false;
    ui::PaneViewModel pane;
    app::FillPaneViewModel(pane, *s.pane, &s.places);
    const D2D1_RECT_F bounds = FocusedPaneRect(s);
    const int count = static_cast<int>(tab->EntryCount());
    int seen = 0;
    for (int row = 0; row < count; ++row) {
        const fs::DirEntry& entry = tab->EntryAt(static_cast<size_t>(row));
        if (entry.is_dir || entry.change_record_only) continue;
        if (seen++ < nth_file) continue;
        const int index = pane.ViewIndex(row);
        if (index < 0) return false;
        const D2D1_RECT_F cell = s.renderer.ItemRectInPane(pane, bounds, index);
        ui::QuickPreviewItem item;
        if (!QuickPreviewItemForRow(s, *tab, row, item)) return false;
        auto& card = s.hoverPreview;
        s.hoverRegion = static_cast<int>(ui::HitTestResult::Row);
        s.hoverRow = row;
        s.hoverPaneIndex = -1; // the focused pane (vm.pane)
        s.hoverPoint = POINT{static_cast<LONG>(cell.left + 120.0f),
                             static_cast<LONG>((cell.top + cell.bottom) * 0.5f)};
        card.visible = true;
        card.row = row;
        card.pane = s.hoverPaneIndex;
        card.path = std::move(item.path);
        card.name = std::move(item.name);
        card.attrs = item.attrs;
        card.size = item.size;
        card.modified = item.modified;
        card.type_text = TypeName(card.name);
        card.point = s.hoverPoint;
        card.video = CardVideo(card.path);
        card.slow = SlowToDecode(entry, card.path);
        card.shown_at = GetTickCount64() - 1000;  // settled: a video opens at once
        return true;
    }
    return false;
}

void FillHoverPreviewView(AppState& s, const ui::WindowViewModel& vm, ui::HoverPreviewView& view) {
    view = {};
    auto& card = s.hoverPreview;
    // The row's cell in this frame's layout; false when it is gone or
    // scrolled out of the list.
    const auto cell_of = [&](int slot, int row, D2D1_RECT_F& cell) {
        const ui::PaneViewModel* pane = &vm.pane;
        D2D1_RECT_F bounds = FocusedPaneRect(s);
        if (slot >= 0 && slot < static_cast<int>(vm.pane_slots.size())) {
            pane = &vm.pane_slots[static_cast<size_t>(slot)].pane;
            bounds = vm.pane_slots[static_cast<size_t>(slot)].rect;
        }
        const int index = pane->ViewIndex(row);
        if (index < 0) return false;
        cell = s.renderer.ItemRectInPane(*pane, bounds, index);
        const D2D1_RECT_F list = s.renderer.PaneListRect(*pane, bounds);
        const float cy = (cell.top + cell.bottom) * 0.5f;
        return cy >= list.top && cy <= list.bottom;
    };
    if (!card.visible) {
        if (card.dwell_row < 0) return;
        if (card.dwell_row != s.hoverRow || card.dwell_pane != s.hoverPaneIndex || !HoverPreviewOn(s) ||
            !card.dwell_ms) {
            card.dwell_row = card.dwell_pane = -1;
            return;
        }
        D2D1_RECT_F cell{};
        if (!cell_of(card.dwell_pane, card.dwell_row, cell)) return;
        view.dwell = true;
        view.dwell_row = cell;
        view.dwell_progress = (std::min)(1.0f,
            static_cast<float>(GetTickCount64() - card.dwell_start) / static_cast<float>(card.dwell_ms));
        return;
    }
    const auto drop = [&] {
        card.visible = false;
        card.hidden_at = 0;
    };
    const bool staged = s.shot.active; // --shot-hover-preview: no foreground, no pointer
    if (!HoverPreviewOn(s) || card.row != s.hoverRow || card.pane != s.hoverPaneIndex ||
        s.quickPreview.visible() || s.renameIndex >= 0 || s.context_menu.menu_open() ||
        (!staged && (GetForegroundWindow() != s.hwnd || !PointerFree(s)))) {
        drop();
        return;
    }
    app::Tab* tab = TabAtSlot(s, card.pane);
    if (!tab || !tab->snapshot || card.row >= static_cast<int>(tab->EntryCount()) ||
        tab->EntryAt(static_cast<size_t>(card.row)).name != card.name) {
        drop();
        return;
    }
    D2D1_RECT_F cell{};
    if (!cell_of(card.pane, card.row, cell)) {
        drop();
        return;
    }
    view.visible = true;
    view.anchor = cell;
    view.pointer = D2D1::Point2F(static_cast<float>(card.point.x), static_cast<float>(card.point.y));
    view.path = card.path;
    view.name = card.name;
    view.type_text = card.type_text;
    view.attrs = card.attrs;
    view.size = card.size;
    view.modified = card.modified;
    view.video = card.video;
    view.slow = card.slow;
    view.owner = s.hwnd;
    view.shown_ms = card.shown_at;
}

void WriteHoverVideoProbe(AppState& s, const std::wstring& shot_output) {
    std::wstring path = shot_output;
    if (const size_t dot = path.find_last_of(L'.'); dot != std::wstring::npos) path.resize(dot);
    path += L".video.txt";
    const auto video = s.renderer.HoverVideoState();
    char text[512];
    const int n = snprintf(text, sizeof(text),
        "card_video=%d\nready=%d\nplaying=%d\nmuted=%d\nffmpeg=%d\nposition_ms=%lld\nduration_ms=%lld\n"
        "size=%ux%u\nerror=0x%08lX\nvisible=%d dwell_row=%d dwell_pane=%d hover_row=%d hover_pane=%d frame_ms=%u\n",
        s.hoverPreview.video ? 1 : 0, video.ready ? 1 : 0, video.playing ? 1 : 0, video.muted ? 1 : 0,
        video.ffmpeg ? 1 : 0, static_cast<long long>(video.position / 10000),
        static_cast<long long>(video.duration / 10000), video.width, video.height,
        static_cast<unsigned long>(video.error), s.hoverPreview.visible ? 1 : 0, s.hoverPreview.dwell_row,
        s.hoverPreview.dwell_pane, s.hoverRow, s.hoverPaneIndex, s.renderer.HoverPreviewFrameMs());
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    if (n > 0) WriteFile(file, text, static_cast<DWORD>(n), &written, nullptr);
    CloseHandle(file);
}

void AfterHoverPreviewDraw(AppState& s) {
    auto& card = s.hoverPreview;
    UINT ms = s.renderer.HoverPreviewFrameMs();
    // A card whose video is still opening keeps polling until it plays.
    if (!ms && card.visible && card.video && GetTickCount64() - card.shown_at < 1000) ms = 33;
    SetFrameTimer(s, ms);
}

} // namespace pulse
