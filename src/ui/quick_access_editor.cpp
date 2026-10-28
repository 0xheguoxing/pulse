// quick_access_editor.cpp — the 编辑快速访问 panel (see the header).
//
// Same surface as ColorPickerPopup: the card and its shadow are drawn with
// Direct2D into an offscreen bitmap, read back, and shown with
// UpdateLayeredWindow; a modal message loop runs until the panel closes.
//
// Card (DIP): width 360; title row 32 with 恢复默认 on the right; hint 22;
// rows 36 (at most 9 before the list scrolls); divider; footer 32 with
// 添加文件夹… on the left and 完成 on the right.
// Row: grip dots, check box (pins: checked and disabled, they always show),
// 20-DIP icon, label, dimmed folder path, remove button for pins.
#include "quick_access_editor.h"

#include "../common/windows_compat.h"
#include "fluent_color_svg.h"
#include "fluent_components.h"
#include "fluent_menu.h"
#include "typography.h"
#include "ui_compositor.h"

#include <windowsx.h>
#include <wincodec.h>
#include <d2d1effects.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace pulse::ui {

void ResetQuickAccessEditorRows(std::vector<QuickAccessEditorRow>& rows) {
    std::stable_sort(rows.begin(), rows.end(),
        [](const QuickAccessEditorRow& a, const QuickAccessEditorRow& b) {
            if (a.builtin != b.builtin) return a.builtin;
            return a.builtin && a.default_index < b.default_index;
        });
    for (auto& row : rows) {
        if (row.builtin) row.visible = row.default_visible;
    }
}

bool MoveQuickAccessEditorRow(std::vector<QuickAccessEditorRow>& rows, size_t from, size_t to) {
    if (from >= rows.size()) return false;
    to = (std::min)(to, rows.size() - 1);
    if (from == to) return false;
    QuickAccessEditorRow moved = std::move(rows[from]);
    rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(from));
    rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
    return true;
}

namespace {

// CLSID_D2D1Shadow (d2d1.lib does not export the effect CLSIDs).
constexpr GUID kShadowEffectClsid = { 0xC67EA361, 0x1863, 0x4e69,
    { 0x89, 0xDB, 0x69, 0x5D, 0x3E, 0x9D, 0x5B, 0x53 } };

constexpr wchar_t kClassName[] = L"PulseQuickAccessEditor";

constexpr float kCardW = 360.0f;
constexpr float kPadX = 16.0f;
constexpr float kPadTop = 10.0f;
constexpr float kTitleH = 34.0f;
constexpr float kHintH = 22.0f;
constexpr float kListGap = 6.0f;
constexpr float kListInset = 8.0f;   // rows reach closer to the card edge than text
constexpr float kRowH = 36.0f;
constexpr size_t kMaxVisibleRows = 9;
constexpr float kFooterGap = 10.0f;
constexpr float kFooterH = 32.0f;
constexpr float kPadBottom = 14.0f;
constexpr float kDragStartDip = 4.0f;
constexpr int kShadow = FluentMenu::kShadowMargin;

enum class Part { None, Reset, Add, Done, Row, Check, Remove };

struct Hit {
    Part part = Part::None;
    int row = -1;
    bool operator==(const Hit&) const = default;
};

// Per-key text measurements; the scale does not change while the panel is open.
struct RowText {
    float label_w = 0.0f;
    std::wstring detail;   // the path, shortened from the left to fit
    float detail_w = 0.0f;
};

struct State {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    Compositor* compositor = nullptr;
    float scale = 1.0f;
    bool dark = true;
    Theme theme{};
    fluent::Painter painter;
    const QuickAccessEditorText* text = nullptr;
    std::vector<QuickAccessEditorRow>* rows = nullptr;
    QuickAccessEditorResult result = QuickAccessEditorResult::Cancelled;
    bool done = false;

    int base_x = 0;
    int base_y = 0;
    int present_dy = 0;
    int win_w = 0;
    int win_h = 0;
    float card_h = 0.0f;   // DIP

    // Window pixels, shadow margin included.
    D2D1_RECT_F r_card{};
    D2D1_RECT_F r_title{};
    D2D1_RECT_F r_reset{};
    D2D1_RECT_F r_hint{};
    D2D1_RECT_F r_list{};
    D2D1_RECT_F r_divider{};
    D2D1_RECT_F r_add{};
    D2D1_RECT_F r_done{};
    float scroll = 0.0f;   // pixels

    Hit hot;
    Hit press;
    float press_y = 0.0f;
    bool dragging = false;
    int drag_row = -1;
    int focus_row = -1;
    bool keyboard_focus = false;
    bool tracking_leave = false;

    std::unordered_map<std::wstring, RowText> text_cache;
    // Color artwork by resource id; a null document marks one that failed.
    std::unordered_map<int, ComPtr<ID2D1SvgDocument>> artwork;

    HDC mem_dc = nullptr;
    HBITMAP dib = nullptr;
    void* bits = nullptr;
    int dib_w = 0;
    int dib_h = 0;
};

float Px(const State& s, float dip) { return dip * s.scale; }

bool Contains(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

size_t RowCount(const State& s) { return s.rows ? s.rows->size() : 0; }

float RowPx(const State& s) { return Px(s, kRowH); }

float MaxScroll(const State& s) {
    const float content = RowPx(s) * static_cast<float>(RowCount(s));
    return (std::max)(0.0f, content - (s.r_list.bottom - s.r_list.top));
}

void ClampScroll(State& s) { s.scroll = std::clamp(s.scroll, 0.0f, MaxScroll(s)); }

void Layout(State& s) {
    const size_t shown = (std::max)(size_t{1}, (std::min)(RowCount(s), kMaxVisibleRows));
    const float list_h = kRowH * static_cast<float>(shown);
    s.card_h = kPadTop + kTitleH + kHintH + kListGap + list_h + kFooterGap + 1.0f +
               kFooterGap + kFooterH + kPadBottom;
    const float m = static_cast<float>(kShadow);
    auto rect = [&](float l, float t, float r, float b) {
        return D2D1::RectF(m + Px(s, l), m + Px(s, t), m + Px(s, r), m + Px(s, b));
    };
    s.r_card = rect(0.0f, 0.0f, kCardW, s.card_h);
    float y = kPadTop;
    // 恢复默认 is a borderless button; its text lines up with the padding.
    const float reset_w = s.painter.MeasureButtonWidth(s.text ? s.text->reset : L"");
    const float reset_right = s.r_card.right - Px(s, kPadX - 8.0f);
    const float title_top = m + Px(s, y);
    const float title_bottom = m + Px(s, y + kTitleH);
    s.r_reset = D2D1::RectF(reset_right - reset_w, title_top + Px(s, 1.0f), reset_right,
                            title_bottom - Px(s, 1.0f));
    s.r_title = D2D1::RectF(s.r_card.left + Px(s, kPadX), title_top,
                            s.r_reset.left - Px(s, 8.0f), title_bottom);
    y += kTitleH;
    s.r_hint = rect(kPadX, y, kCardW - kPadX, y + kHintH);
    y += kHintH + kListGap;
    s.r_list = rect(kListInset, y, kCardW - kListInset, y + list_h);
    y += list_h + kFooterGap;
    s.r_divider = rect(kPadX, y, kCardW - kPadX, y + 1.0f);
    y += 1.0f + kFooterGap;
    const float footer_top = m + Px(s, y);
    const float footer_bottom = m + Px(s, y + kFooterH);
    const float done_w = (std::max)(Px(s, 88.0f),
        s.painter.MeasureButtonWidth(s.text ? s.text->done : L""));
    const float add_w =
        s.painter.MeasureButtonWidth(s.text ? s.text->add_folder : L"", L"\xE710");
    s.r_done = D2D1::RectF(s.r_card.right - Px(s, kPadX) - done_w, footer_top,
                           s.r_card.right - Px(s, kPadX), footer_bottom);
    s.r_add = D2D1::RectF(s.r_card.left + Px(s, kPadX), footer_top,
                          s.r_card.left + Px(s, kPadX) + add_w, footer_bottom);
    s.win_w = static_cast<int>(std::lround(Px(s, kCardW))) + kShadow * 2;
    s.win_h = static_cast<int>(std::lround(Px(s, s.card_h))) + kShadow * 2;
    ClampScroll(s);
}

D2D1_RECT_F RowRect(const State& s, size_t index) {
    const float top = s.r_list.top + RowPx(s) * static_cast<float>(index) - s.scroll;
    return D2D1::RectF(s.r_list.left, top, s.r_list.right, top + RowPx(s));
}

D2D1_RECT_F CheckRect(const State& s, const D2D1_RECT_F& row) {
    const float left = row.left + Px(s, 24.0f);
    return D2D1::RectF(left, row.top, left + Px(s, 18.0f), row.bottom);
}

D2D1_RECT_F RemoveRect(const State& s, const D2D1_RECT_F& row) {
    const float cy = (row.top + row.bottom) * 0.5f;
    return D2D1::RectF(row.right - Px(s, 34.0f), cy - Px(s, 14.0f),
                       row.right - Px(s, 6.0f), cy + Px(s, 14.0f));
}

Hit HitTest(const State& s, float x, float y) {
    if (Contains(s.r_reset, x, y)) return { Part::Reset, -1 };
    if (Contains(s.r_add, x, y)) return { Part::Add, -1 };
    if (Contains(s.r_done, x, y)) return { Part::Done, -1 };
    if (!Contains(s.r_list, x, y) || !s.rows) return {};
    const auto index = static_cast<size_t>((y - s.r_list.top + s.scroll) / RowPx(s));
    if (index >= RowCount(s)) return {};
    const D2D1_RECT_F row = RowRect(s, index);
    const auto& item = (*s.rows)[index];
    const int i = static_cast<int>(index);
    if (!item.builtin && Contains(RemoveRect(s, row), x, y)) return { Part::Remove, i };
    if (item.builtin && x < CheckRect(s, row).right + Px(s, 4.0f)) return { Part::Check, i };
    return { Part::Row, i };
}

float MeasureWidth(const State& s, std::wstring_view text, IDWriteTextFormat* format) {
    if (!s.compositor || !format || text.empty()) return 0.0f;
    return typography::MeasureLine(s.compositor, format, text);
}

// Keeps the end of a path (the folder names) and drops the start.
std::wstring FitTail(const State& s, const std::wstring& text, IDWriteTextFormat* format,
                     float max_w) {
    if (max_w <= 0.0f) return {};
    if (MeasureWidth(s, text, format) <= max_w) return text;
    size_t lo = 0;
    size_t hi = text.size();
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        const std::wstring candidate = L"\x2026" + text.substr(text.size() - mid);
        if (MeasureWidth(s, candidate, format) <= max_w) lo = mid;
        else hi = mid - 1;
    }
    return lo == 0 ? std::wstring{} : L"\x2026" + text.substr(text.size() - lo);
}

float TextLeft(const State& s, const D2D1_RECT_F& row) { return row.left + Px(s, 78.0f); }

float TextRight(const State& s, const D2D1_RECT_F& row, bool builtin) {
    return builtin ? row.right - Px(s, 10.0f) : RemoveRect(s, row).left - Px(s, 6.0f);
}

const RowText& TextFor(State& s, const QuickAccessEditorRow& item, const D2D1_RECT_F& row) {
    auto found = s.text_cache.find(item.key);
    if (found != s.text_cache.end()) return found->second;
    RowText measured;
    measured.label_w = MeasureWidth(s, item.label, s.compositor->TextFormat());
    if (!item.detail.empty()) {
        const float room = TextRight(s, row, item.builtin) - TextLeft(s, row) -
                           measured.label_w - Px(s, 10.0f);
        measured.detail = FitTail(s, item.detail, s.compositor->SmallFormat(), room);
        measured.detail_w = MeasureWidth(s, measured.detail, s.compositor->SmallFormat());
    }
    return s.text_cache.emplace(item.key, std::move(measured)).first->second;
}

// The color Fluent icon the sidebar draws for the same row (桌面, 下载, 图片…);
// false leaves the row to its tinted glyph (no artwork, high contrast, legacy).
bool DrawRowArtwork(State& s, ID2D1DeviceContext* dc, const std::wstring& glyph,
                    const D2D1_RECT_F& box, float opacity) {
    if (IsHighContrast() || compat::LegacyMode()) return false;
    const int id = FluentSvgIdForGlyph(glyph);
    if (id == 0) return false;
    ComPtr<ID2D1DeviceContext5> dc5;
    if (FAILED(dc->QueryInterface(IID_PPV_ARGS(&dc5))) || !dc5.get()) return false;
    auto it = s.artwork.find(id);
    if (it == s.artwork.end()) {
        ComPtr<ID2D1SvgDocument> document;
        LoadFluentSvg(dc5.get(), id, document);
        it = s.artwork.emplace(id, std::move(document)).first;
    }
    return it->second.get() && DrawFluentSvgFitted(dc5.get(), it->second.get(), box, opacity);
}

void DrawRow(State& s, ID2D1DeviceContext* dc, size_t index, const D2D1_RECT_F& row) {
    const auto& item = (*s.rows)[index];
    const int i = static_cast<int>(index);
    const bool dragged = s.dragging && s.drag_row == i;
    const bool hovered = !s.dragging && s.hot.row == i;
    const bool pressed = !s.dragging && s.press.row == i && hovered &&
                         (s.press.part == Part::Row || s.press.part == Part::Check);
    const D2D1_RECT_F pill = D2D1::RectF(row.left, row.top + Px(s, 2.0f), row.right,
                                         row.bottom - Px(s, 2.0f));
    const float radius = Px(s, 5.0f);
    if (dragged) {
        s.painter.FillRoundedRect(pill, radius, s.theme.fill_pressed);
        s.painter.StrokeRoundedRect(pill, radius, s.theme.accent, Px(s, 1.0f));
    } else if (pressed) {
        s.painter.FillRoundedRect(pill, radius, s.theme.fill_pressed);
    } else if (hovered) {
        s.painter.FillRoundedRect(pill, radius, s.theme.fill_hover);
    }
    if (s.keyboard_focus && s.focus_row == i) s.painter.DrawFocusRing(pill, radius);

    // Grip: two columns of three dots, clearer under the pointer.
    const float cy = (row.top + row.bottom) * 0.5f;
    const float dot = Px(s, 2.4f);
    D2D1_COLOR_F grip = s.theme.text_secondary;
    if (!hovered && !dragged) grip.a *= 0.55f;
    for (int column = 0; column < 2; ++column) {
        for (int line = -1; line <= 1; ++line) {
            const float gx = row.left + Px(s, 9.0f + 5.0f * static_cast<float>(column));
            const float gy = cy + Px(s, 5.0f * static_cast<float>(line));
            s.painter.FillRoundedRect(D2D1::RectF(gx - dot * 0.5f, gy - dot * 0.5f,
                                                  gx + dot * 0.5f, gy + dot * 0.5f),
                                      dot * 0.5f, grip);
        }
    }

    fluent::ControlState check;
    check.enabled = item.builtin;
    check.checked = item.builtin ? item.visible : true;
    check.hovered = hovered && item.builtin;
    check.pressed = pressed && item.builtin;
    s.painter.DrawCheckBox(CheckRect(s, row), {}, check);

    const bool dim = item.builtin && !item.visible;
    D2D1_COLOR_F icon = item.color;
    if (dim) icon.a *= 0.45f;
    const float ix = row.left + Px(s, 50.0f);
    const D2D1_RECT_F icon_box =
        D2D1::RectF(ix, cy - Px(s, 10.0f), ix + Px(s, 20.0f), cy + Px(s, 10.0f));
    if (!DrawRowArtwork(s, dc, item.glyph, icon_box, dim ? 0.45f : 1.0f))
        s.painter.DrawGlyph(item.glyph, icon_box, icon);

    const RowText& measured = TextFor(s, item, row);
    const float left = TextLeft(s, row);
    const float right = TextRight(s, row, item.builtin);
    const D2D1_COLOR_F background = s.theme.surface_flyout;
    dc->PushAxisAlignedClip(D2D1::RectF(left, row.top, right, row.bottom),
                            D2D1_ANTIALIAS_MODE_ALIASED);
    s.painter.DrawText(item.label,
                       D2D1::RectF(left, row.top, left + measured.label_w + Px(s, 4.0f), row.bottom),
                       s.compositor->TextFormat(), dim ? s.theme.text_secondary : s.theme.text,
                       fluent::HorizontalAlignment::Left, background);
    if (!measured.detail.empty()) {
        const float dx = left + measured.label_w + Px(s, 10.0f);
        s.painter.DrawText(measured.detail,
                           D2D1::RectF(dx, row.top, dx + measured.detail_w + Px(s, 4.0f), row.bottom),
                           s.compositor->SmallFormat(), s.theme.text_secondary,
                           fluent::HorizontalAlignment::Left, background);
    }
    dc->PopAxisAlignedClip();

    if (!item.builtin) {
        const D2D1_RECT_F button = RemoveRect(s, row);
        const bool hot = s.hot.part == Part::Remove && s.hot.row == i;
        if (hot) {
            const bool down = s.press.part == Part::Remove && s.press.row == i;
            s.painter.FillRoundedRect(button, Px(s, 4.5f),
                                      down ? s.theme.fill_pressed : s.theme.fill_hover);
        }
        const float bx = (button.left + button.right) * 0.5f;
        const float g = Px(s, 7.0f);
        s.painter.DrawGlyph(L"\xE8BB", D2D1::RectF(bx - g, cy - g, bx + g, cy + g),
                            hot ? s.theme.text : s.theme.text_secondary);
    }
}

void DrawPanelButton(State& s, const D2D1_RECT_F& bounds, std::wstring_view label,
                std::wstring_view glyph, fluent::ButtonKind kind, Part part, bool bordered) {
    fluent::ButtonSpec spec;
    spec.bounds = bounds;
    spec.text = label;
    spec.glyph = glyph;
    spec.kind = kind;
    spec.bordered = bordered;
    spec.state.hovered = s.hot.part == part;
    spec.state.pressed = s.hot.part == part && s.press.part == part;
    s.painter.DrawButton(spec);
}

bool Render(State& s) {
    ID2D1DeviceContext* dc = s.compositor ? s.compositor->Dc() : nullptr;
    if (!dc || !s.text || !s.rows || s.win_w <= 0 || s.win_h <= 0) return false;
    const D2D1_SIZE_U size{ static_cast<UINT32>(s.win_w), static_cast<UINT32>(s.win_h) };
    const auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> bmp;
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &bmp))) return false;

    ComPtr<ID2D1Image> old_target;
    dc->GetTarget(&old_target);
    dc->SetTarget(bmp.get());
    // Grayscale text AA: ClearType fringes survive the premultiplied readback.
    const D2D1_TEXT_ANTIALIAS_MODE old_text_aa = dc->GetTextAntialiasMode();
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));

    const D2D1_RECT_F card = s.r_card;
    const float radius = Px(s, s.theme.radius_flyout);
    ComPtr<ID2D1CommandList> card_list;
    if (SUCCEEDED(dc->CreateCommandList(&card_list))) {
        dc->SetTarget(card_list.get());
        ComPtr<ID2D1SolidColorBrush> black;
        dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1.0f), &black);
        dc->FillRoundedRectangle(D2D1::RoundedRect(card, radius, radius), black.get());
        card_list->Close();
        dc->SetTarget(bmp.get());
        ComPtr<ID2D1Effect> shadow;
        if (SUCCEEDED(dc->CreateEffect(kShadowEffectClsid, &shadow))) {
            shadow->SetInput(0, card_list.get());
            shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, 7.0f);
            shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0.0f, 0.0f, 0.0f, 0.36f));
            dc->DrawImage(shadow.get(), D2D1::Point2F(0.0f, 4.0f * s.scale));
        }
    }

    if (!s.painter.BeginFrame(s.theme, IsHighContrast())) {
        dc->EndDraw();
        dc->SetTextAntialiasMode(old_text_aa);
        dc->SetTarget(old_target.get());
        return false;
    }
    s.painter.DrawMenuSurface(card);

    const D2D1_COLOR_F background = s.theme.surface_flyout;
    s.painter.DrawText(s.text->title, s.r_title, s.compositor->TextFormat(), s.theme.text,
                       fluent::HorizontalAlignment::Left, background);
    DrawPanelButton(s, s.r_reset, s.text->reset, {}, fluent::ButtonKind::Transparent, Part::Reset,
               false);
    s.painter.DrawText(s.text->hint, s.r_hint, s.compositor->SmallFormat(),
                       s.theme.text_secondary, fluent::HorizontalAlignment::Left, background);

    dc->PushAxisAlignedClip(s.r_list, D2D1_ANTIALIAS_MODE_ALIASED);
    for (size_t i = 0; i < RowCount(s); ++i) {
        const D2D1_RECT_F row = RowRect(s, i);
        if (row.bottom <= s.r_list.top || row.top >= s.r_list.bottom) continue;
        DrawRow(s, dc, i, row);
    }
    dc->PopAxisAlignedClip();

    // A thin thumb tells there is more to scroll.
    if (const float max_scroll = MaxScroll(s); max_scroll > 0.0f) {
        const float track = s.r_list.bottom - s.r_list.top;
        const float content = track + max_scroll;
        const float thumb = (std::max)(Px(s, 24.0f), track * track / content);
        const float top = s.r_list.top + (track - thumb) * (s.scroll / max_scroll);
        D2D1_COLOR_F color = s.theme.text_secondary;
        color.a *= 0.5f;
        s.painter.FillRoundedRect(D2D1::RectF(s.r_list.right - Px(s, 3.0f), top,
                                              s.r_list.right - Px(s, 1.0f), top + thumb),
                                  Px(s, 1.0f), color);
    }

    s.painter.FillRoundedRect(s.r_divider, 0.0f, s.theme.stroke_divider);
    DrawPanelButton(s, s.r_add, s.text->add_folder, L"\xE710", fluent::ButtonKind::Standard,
               Part::Add, true);
    DrawPanelButton(s, s.r_done, s.text->done, {}, fluent::ButtonKind::Primary, Part::Done, true);

    HRESULT hr = dc->EndDraw();
    dc->SetTextAntialiasMode(old_text_aa);
    dc->SetTarget(old_target.get());
    if (FAILED(hr)) return false;

    const auto cpu_props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> cpu;
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, cpu_props, &cpu))) return false;
    if (FAILED(cpu->CopyFromBitmap(nullptr, bmp.get(), nullptr))) return false;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;

    if (!s.mem_dc) s.mem_dc = CreateCompatibleDC(nullptr);
    if (s.dib && (s.dib_w != s.win_w || s.dib_h != s.win_h)) {
        DeleteObject(s.dib);
        s.dib = nullptr;
        s.bits = nullptr;
    }
    if (!s.dib) {
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = s.win_w;
        bmi.bmiHeader.biHeight = -s.win_h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        s.dib = CreateDIBSection(s.mem_dc, &bmi, DIB_RGB_COLORS, &s.bits, nullptr, 0);
        if (!s.dib) {
            cpu->Unmap();
            return false;
        }
        s.dib_w = s.win_w;
        s.dib_h = s.win_h;
    }
    for (int row = 0; row < s.win_h; ++row) {
        std::memcpy(static_cast<uint8_t*>(s.bits) + static_cast<size_t>(row) * s.win_w * 4,
                    static_cast<const uint8_t*>(mapped.bits) +
                        static_cast<size_t>(row) * mapped.pitch,
                    static_cast<size_t>(s.win_w) * 4);
    }
    cpu->Unmap();
    return true;
}

void Present(State& s, BYTE alpha, int dy) {
    if (!s.dib || !s.bits) return;
    HGDIOBJ old = SelectObject(s.mem_dc, s.dib);
    POINT dst{ s.base_x, s.base_y + dy };
    POINT src{ 0, 0 };
    SIZE sz{ s.win_w, s.win_h };
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
    UpdateLayeredWindow(s.hwnd, nullptr, &dst, &sz, s.mem_dc, &src, 0, &blend, ULW_ALPHA);
    SelectObject(s.mem_dc, old);
    s.present_dy = dy;
}

void Repaint(State& s) {
    if (Render(s)) Present(s, 255, s.present_dy);
}

void Finish(State& s, QuickAccessEditorResult result) {
    s.result = result;
    s.done = true;
}

void EnsureVisible(State& s, int index) {
    if (index < 0) return;
    const float top = RowPx(s) * static_cast<float>(index);
    const float view = s.r_list.bottom - s.r_list.top;
    if (top < s.scroll) s.scroll = top;
    else if (top + RowPx(s) > s.scroll + view) s.scroll = top + RowPx(s) - view;
    ClampScroll(s);
}

void ToggleRow(State& s, int index) {
    if (index < 0 || static_cast<size_t>(index) >= RowCount(s)) return;
    auto& row = (*s.rows)[static_cast<size_t>(index)];
    if (row.builtin) row.visible = !row.visible;
}

void RemoveRow(State& s, int index) {
    if (index < 0 || static_cast<size_t>(index) >= RowCount(s)) return;
    if ((*s.rows)[static_cast<size_t>(index)].builtin) return;
    s.rows->erase(s.rows->begin() + index);
    const int count = static_cast<int>(RowCount(s));
    s.focus_row = count == 0 ? -1 : (std::min)(s.focus_row, count - 1);
    s.hot = {};
    // The list may shrink, and the window with it.
    Layout(s);
}

// While dragging, the row under the pointer swaps in live; past the list
// edges the list scrolls a little per move.
void UpdateDrag(State& s, float y) {
    const size_t count = RowCount(s);
    if (count == 0 || s.drag_row < 0) return;
    if (y < s.r_list.top) s.scroll -= RowPx(s) * 0.25f;
    else if (y > s.r_list.bottom) s.scroll += RowPx(s) * 0.25f;
    ClampScroll(s);
    const float clamped = std::clamp(y, s.r_list.top, s.r_list.bottom - 1.0f);
    int target = static_cast<int>(std::floor((clamped - s.r_list.top + s.scroll) / RowPx(s)));
    target = std::clamp(target, 0, static_cast<int>(count) - 1);
    if (target != s.drag_row &&
        MoveQuickAccessEditorRow(*s.rows, static_cast<size_t>(s.drag_row),
                                 static_cast<size_t>(target))) {
        s.drag_row = target;
        s.focus_row = target;
    }
}

void RefreshHot(State& s) {
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(s.hwnd, &pt);
    s.hot = HitTest(s, static_cast<float>(pt.x), static_cast<float>(pt.y));
}

bool HandleKey(State& s, WPARAM key) {
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const int count = static_cast<int>(RowCount(s));
    switch (key) {
    case VK_ESCAPE:
        Finish(s, QuickAccessEditorResult::Cancelled);
        return true;
    case VK_RETURN:
        Finish(s, QuickAccessEditorResult::Applied);
        return true;
    case VK_UP:
    case VK_DOWN: {
        if (count == 0) return true;
        s.keyboard_focus = true;
        const int step = key == VK_UP ? -1 : 1;
        if (s.focus_row < 0) {
            s.focus_row = 0;
        } else if (alt) {
            const int to = s.focus_row + step;
            if (to >= 0 && to < count &&
                MoveQuickAccessEditorRow(*s.rows, static_cast<size_t>(s.focus_row),
                                         static_cast<size_t>(to)))
                s.focus_row = to;
        } else {
            s.focus_row = std::clamp(s.focus_row + step, 0, count - 1);
        }
        EnsureVisible(s, s.focus_row);
        Repaint(s);
        return true;
    }
    case VK_SPACE:
        s.keyboard_focus = true;
        ToggleRow(s, s.focus_row);
        Repaint(s);
        return true;
    case VK_DELETE:
        s.keyboard_focus = true;
        RemoveRow(s, s.focus_row);
        Repaint(s);
        return true;
    default:
        return false;
    }
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    State* s = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        s = static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        s->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
    }
    if (!s) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN: {
        const float x = static_cast<float>(GET_X_LPARAM(lp));
        const float y = static_cast<float>(GET_Y_LPARAM(lp));
        SetFocus(hwnd);
        const Hit hit = HitTest(*s, x, y);
        if (hit.part == Part::None) return 0;
        s->press = hit;
        s->press_y = y;
        s->dragging = false;
        s->drag_row = -1;
        if (hit.row >= 0) {
            s->focus_row = hit.row;
            s->keyboard_focus = false;
        }
        SetCapture(hwnd);
        Repaint(*s);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const float x = static_cast<float>(GET_X_LPARAM(lp));
        const float y = static_cast<float>(GET_Y_LPARAM(lp));
        if (s->press.row >= 0 &&
            (s->press.part == Part::Row || s->press.part == Part::Check)) {
            if (!s->dragging && std::fabs(y - s->press_y) >= Px(*s, kDragStartDip)) {
                s->dragging = true;
                s->drag_row = s->press.row;
            }
            if (s->dragging) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                UpdateDrag(*s, y);
                Repaint(*s);
                return 0;
            }
        }
        const Hit hit = HitTest(*s, x, y);
        if (!(hit == s->hot)) {
            s->hot = hit;
            Repaint(*s);
        }
        if (!s->tracking_leave) {
            s->tracking_leave = true;
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        s->tracking_leave = false;
        if (!(s->hot == Hit{})) {
            s->hot = {};
            Repaint(*s);
        }
        return 0;
    case WM_LBUTTONUP: {
        const float x = static_cast<float>(GET_X_LPARAM(lp));
        const float y = static_cast<float>(GET_Y_LPARAM(lp));
        const Hit hit = HitTest(*s, x, y);
        // Snapshot before ReleaseCapture: WM_CAPTURECHANGED clears the state.
        const Hit press = s->press;
        const bool was_drag = s->dragging;
        s->press = {};
        s->dragging = false;
        s->drag_row = -1;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (was_drag) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            s->hot = hit;
            Repaint(*s);
            return 0;
        }
        const auto row_part = [](Part part) { return part == Part::Row || part == Part::Check; };
        if (press.row >= 0 && press.row == hit.row && row_part(press.part) &&
            row_part(hit.part)) {
            ToggleRow(*s, hit.row);
        } else if (press.part == Part::Remove && press == hit) {
            RemoveRow(*s, hit.row);
        } else if (press.part == Part::Reset && hit.part == Part::Reset) {
            ResetQuickAccessEditorRows(*s->rows);
            s->scroll = 0.0f;
        } else if (press.part == Part::Add && hit.part == Part::Add) {
            Finish(*s, QuickAccessEditorResult::AddFolder);
            return 0;
        } else if (press.part == Part::Done && hit.part == Part::Done) {
            Finish(*s, QuickAccessEditorResult::Applied);
            return 0;
        }
        s->hot = HitTest(*s, x, y);
        Repaint(*s);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const float notches = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
        s->scroll -= notches * RowPx(*s) * 2.0f;
        ClampScroll(*s);
        RefreshHot(*s);
        Repaint(*s);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (HandleKey(*s, wp)) return 0;
        break;
    case WM_CAPTURECHANGED:
        s->press = {};
        s->dragging = false;
        s->drag_row = -1;
        return 0;
    case WM_CLOSE:
        Finish(*s, QuickAccessEditorResult::Applied);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void DestroySurface(State& s) {
    if (s.dib) DeleteObject(s.dib);
    if (s.mem_dc) DeleteDC(s.mem_dc);
    s.dib = nullptr;
    s.mem_dc = nullptr;
    s.bits = nullptr;
}

bool SavePng(const wchar_t* path, int w, int h, void* bits) {
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic)))) {
        return false;
    }
    ComPtr<IWICStream> stream;
    wic->CreateStream(&stream);
    if (!stream.get() || FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) {
        return false;
    }
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) return false;
    if (FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr))) return false;
    if (FAILED(frame->Initialize(nullptr))) return false;
    if (FAILED(frame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h)))) return false;
    // The staging DIB is premultiplied BGRA.
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    if (FAILED(frame->SetPixelFormat(&format))) return false;
    if (FAILED(frame->WritePixels(static_cast<UINT>(h), static_cast<UINT>(w * 4),
                                  static_cast<UINT>(w * 4 * h), static_cast<BYTE*>(bits)))) {
        return false;
    }
    if (FAILED(frame->Commit())) return false;
    return SUCCEEDED(encoder->Commit());
}

bool RegisterPanelClass() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    registered = true;
    return true;
}

} // namespace

QuickAccessEditorResult QuickAccessEditorPopup::Edit(HWND owner, Compositor* compositor,
                                                     float scale, POINT screen_pt, bool dark,
                                                     D2D1_COLOR_F accent, const QuickAccessEditorText& text,
                                                     std::vector<QuickAccessEditorRow>& rows) {
    if (!compositor || !compositor->Dc() || !RegisterPanelClass())
        return QuickAccessEditorResult::Cancelled;
    const std::vector<QuickAccessEditorRow> original = rows;

    State s;
    s.owner = owner;
    s.compositor = compositor;
    s.scale = (std::max)(0.5f, scale);
    s.dark = dark;
    s.theme = IsHighContrast() ? MakeHighContrastTheme() : MakeTheme(dark, accent);
    s.text = &text;
    s.rows = &rows;
    s.painter.SetCompositor(compositor);
    s.painter.SetScale(s.scale);
    Layout(s);

    s.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, kClassName, L"", WS_POPUP,
                             0, 0, s.win_w, s.win_h, owner, nullptr,
                             GetModuleHandleW(nullptr), &s);
    if (!s.hwnd) return QuickAccessEditorResult::Cancelled;

    // Anchor at the menu point; the shadow margin sits outside the card.
    HMONITOR monitor = MonitorFromPoint(screen_pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(monitor, &mi);
    s.base_x = std::clamp(screen_pt.x - kShadow, mi.rcWork.left,
                          (std::max)(mi.rcWork.left, mi.rcWork.right - s.win_w));
    s.base_y = std::clamp(screen_pt.y - kShadow, mi.rcWork.top,
                          (std::max)(mi.rcWork.top, mi.rcWork.bottom - s.win_h));

    // Fade in while sliding down by the menu travel, 170 ms OutCubic.
    if (Render(s)) {
        ShowWindow(s.hwnd, SW_SHOW);
        SetForegroundWindow(s.hwnd);
        const auto t0 = std::chrono::steady_clock::now();
        constexpr fluent::MotionSpec slide{ 170.0f, fluent::EasingCurve::OutCubic };
        for (;;) {
            const float elapsed = static_cast<float>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count());
            if (elapsed >= slide.duration_ms) break;
            const float p = fluent::EvaluateMotion(slide, elapsed);
            const int dy = -static_cast<int>(
                std::lround((1.0f - p) * fluent::motion::MenuTravelDip * s.scale));
            const BYTE alpha = static_cast<BYTE>(
                std::clamp(elapsed / 120.0f, 0.0f, 1.0f) * 255.0f);
            Present(s, alpha, dy);
            Sleep(10);
        }
        Present(s, 255, 0);
    } else {
        ShowWindow(s.hwnd, SW_SHOW);
        SetForegroundWindow(s.hwnd);
    }
    SetFocus(s.hwnd);

    MSG msg{};
    while (!s.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        const bool click = msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN ||
                           msg.message == WM_MBUTTONDOWN || msg.message == WM_NCLBUTTONDOWN ||
                           msg.message == WM_NCRBUTTONDOWN || msg.message == WM_NCMBUTTONDOWN;
        // A click elsewhere keeps the edits (light dismiss); only Esc discards.
        if (click && msg.hwnd != s.hwnd) Finish(s, QuickAccessEditorResult::Applied);
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    const QuickAccessEditorResult result = s.result;
    if (result == QuickAccessEditorResult::Cancelled) rows = original;
    DestroyWindow(s.hwnd);
    DestroySurface(s);
    s.painter.SetCompositor(nullptr);
    return result;
}

bool QuickAccessEditorPopup::SaveDebugSnapshot(Compositor* compositor, const wchar_t* png_path,
                                               bool dark, D2D1_COLOR_F accent, float scale,
                                               const QuickAccessEditorText& text,
                                               const std::vector<QuickAccessEditorRow>& rows) {
    if (!compositor || !compositor->Dc() || !png_path) return false;
    std::vector<QuickAccessEditorRow> copy = rows;
    State s;
    s.compositor = compositor;
    s.scale = (std::max)(0.5f, scale);
    s.dark = dark;
    s.theme = MakeTheme(dark, accent);
    s.text = &text;
    s.rows = &copy;
    s.hot = { Part::Row, copy.size() > 1 ? 1 : -1 };
    s.painter.SetCompositor(compositor);
    s.painter.SetScale(s.scale);
    Layout(s);
    const bool ok = Render(s) && SavePng(png_path, s.win_w, s.win_h, s.bits);
    DestroySurface(s);
    s.painter.SetCompositor(nullptr);
    return ok;
}

} // namespace pulse::ui
