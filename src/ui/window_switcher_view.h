#pragma once
// window_switcher_view.h - The Ctrl+` window switcher (multi-window M7d)
// drawn over the window it was opened in: a dimmed backdrop and one card per
// Pulse window, most recent first, with the window's color dot and name, its
// active folder, tab count and first tabs. The selected card is highlighted.
// The app (app/window_switcher_ui.h) owns the selection and forwards input
// while it is open; this class only lays out, draws and hit-tests.
#include <d2d1_1.h>
#include <dwrite.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pulse::ui {

struct Theme;

struct WindowSwitcherCard {
    std::wstring name;                 // window name (M7c)
    uint32_t rgb = 0x60A5FA;           // window color
    std::wstring folder;               // active tab's folder
    std::wstring tab_count;            // "3 个标签页"
    std::vector<std::wstring> tabs;    // first few tab titles
    int active_tab = -1;               // index into tabs; drawn stronger
};

struct WindowSwitcherData {
    std::vector<WindowSwitcherCard> cards;
    int selected = 0;
    std::wstring hint;
};

// Shared with the window overview (window_overview_view.h).
float MeasureTextWidth(IDWriteFactory* dwrite, IDWriteTextFormat* f, const std::wstring& s);
// `path` cut to `width` from the front (".../Pulse/src" with Windows
// separators), keeping whole folder names while it can.
std::wstring FitPathTail(IDWriteFactory* dwrite, IDWriteTextFormat* f, const std::wstring& path, float width);

class WindowSwitcherView {
public:
    ~WindowSwitcherView();
    // Shows (or refreshes) the switcher; client = window client rect (px).
    void Show(WindowSwitcherData data, const D2D1_RECT_F& client, float scale);
    void Hide() noexcept { open_ = false; }
    bool IsOpen() const noexcept { return open_; }
    // Card under the point (index into data.cards), or -1.
    int HitCard(float x, float y) const noexcept;
    bool Contains(float x, float y) const noexcept;
    void Draw(ID2D1DeviceContext* dc, IDWriteFactory* dwrite, IDWriteTextFormat* base_format, const Theme& theme);

private:
    void Layout();
    void EnsureFormats(IDWriteFactory* dwrite, IDWriteTextFormat* base_format);
    void ReleaseFormats();

    WindowSwitcherData data_;
    D2D1_RECT_F client_{};
    D2D1_RECT_F panel_{};
    std::vector<D2D1_RECT_F> card_rects_;  // per card; empty rect = scrolled out
    float scale_ = 1.0f;
    bool open_ = false;
    IDWriteTextFormat* f_name_ = nullptr;
    IDWriteTextFormat* f_small_ = nullptr;
    IDWriteTextFormat* f_hint_ = nullptr;
    float format_scale_ = 0.0f;
};

}  // namespace pulse::ui
