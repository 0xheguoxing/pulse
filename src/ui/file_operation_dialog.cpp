#include "../common/windows_compat.h"
#include "file_operation_dialog.h"
#include "../ops/operation_presentation.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "typography.h"
#include "window_helpers.h"

#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <imm.h>
#include <wincodec.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <map>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace pulse::ui {
namespace {

// Wells and footer bands: faint shading over Mica or solid, see-through white
// cards over acrylic (对话框亚克力 ④).
D2D1_COLOR_F OpWell(HWND hwnd, bool backdrop, bool dark, bool hovered = false) {
    if (backdrop && IsAcrylicBackdrop(hwnd)) {
        D2D1_COLOR_F glass = GlassCard(dark);
        if (hovered) glass.a = dark ? 0.085f : 0.82f;
        return glass;
    }
    if (dark) return HexColor(0xFFFFFF, hovered ? 0.085f : 0.045f);
    return HexColor(0x000000, hovered ? 0.07f : 0.035f);
}

D2D1_COLOR_F OpFooter(HWND hwnd, bool backdrop, bool dark) {
    if (backdrop && IsAcrylicBackdrop(hwnd)) return GlassSheet(dark);
    return dark ? HexColor(0x000000, 0.16f) : HexColor(0x000000, 0.03f);
}

constexpr wchar_t kTransferClass[] = L"PulseFileOperationWindow";
constexpr wchar_t kConflictClass[] = L"PulseFileConflictWindow";
constexpr wchar_t kDeleteClass[] = L"PulsePermanentDeleteWindow";
constexpr wchar_t kLockedClass[] = L"PulseLockedItemWindow";
constexpr UINT_PTR kRenderTimer = 1;
// Hides the card when its exit zoom ends, whatever the render timer does.
constexpr UINT_PTR kCloseTimer = 6;
// Progress card, pulse_ops_dialogs.html §1 (DIPs). The footer stays anchored
// to the bottom edge; everything above it is laid out from the top.
constexpr float kDlgW = 468.0f;
constexpr float kTitleH = 40.0f;      // drag strip; the header tile overlaps it
constexpr float kCaptionH = 36.0f;    // minimize / close buttons
constexpr float kPadX = 18.0f;
constexpr float kFooterH = 56.0f;
constexpr float kCollapsedH = 348.0f;
constexpr float kDetailsBlockH = 104.0f;
constexpr float kDetailedH = kCollapsedH + kDetailsBlockH;
constexpr float kGraphTop = 118.0f;
constexpr float kGraphH = 72.0f;
constexpr float kItemTop = 198.0f;
constexpr float kStatsTop = 226.0f;
constexpr float kStatsH = 44.0f;
constexpr float kDetailsTop = 280.0f;
// Failed items (partial failure) are listed where the stats were; the stats
// and details move down below the list.
constexpr float kFailRowH = 36.0f;
constexpr size_t kFailMaxRows = 4;
constexpr float kFailMoreH = 22.0f;
constexpr float kFailGap = 10.0f;
constexpr int kHitOpen = 8;
constexpr int kHitFailRow = 20;   // + visible row
constexpr size_t kTimeGraphPoints = ops::OpSpeedGraph::kMaxTimePoints;
constexpr float kPi = 3.14159265f;

using pulse::ui::ScaleDip;

struct TransferChrome {
    D2D1_RECT_F minimize{};
    D2D1_RECT_F close{};
    D2D1_RECT_F details{};
    D2D1_RECT_F pause{};
    D2D1_RECT_F cancel{};
    D2D1_RECT_F retry{};
    D2D1_RECT_F skip{};
    D2D1_RECT_F open{};
};

struct TransferLabels {
    std::wstring details;
    std::wstring pause;
    const wchar_t* pause_glyph = L"\xF8AE";
    std::wstring cancel;
    std::wstring retry;
    std::wstring skip;
    bool show_details = true;
    bool show_retry = false;
    bool show_skip = false;
    bool actions_enabled = true;   // false while a retry is in flight
    std::wstring retry_reserve;    // widest retry label, keeps the layout fixed
    bool show_pause = true;
    // Partial failure: 打开目标文件夹, and the cancel slot becomes 重试失败项.
    bool show_open = false;
    std::wstring open;
    bool cancel_primary = false;
    bool cancel_retries = false;
};

// A pause shows from the click on: the worker reports Paused only at its next
// check, which a buffered write held by the cache throttle can delay.
bool ShowsPaused(const ops::OpStatus& status) {
    if (status.phase == ops::OpPhase::Paused) return true;
    return status.active && status.pause_requested &&
           (status.phase == ops::OpPhase::Running || status.phase == ops::OpPhase::Scanning);
}

// Asked for, not yet confirmed by the worker: the chip reads 正在暂停….
bool PauseInFlight(const ops::OpStatus& status) {
    return status.phase != ops::OpPhase::Paused && ShowsPaused(status);
}

TransferLabels MakeTransferLabels(const ops::OpStatus& status, bool detailed,
                                  const TransferFailureView& partial = {}) {
    TransferLabels labels;
    labels.details = detailed ? l10n::Get(l10n::StringId::OpSummary)
                              : l10n::Get(l10n::StringId::OpDetails);
    const bool paused = ShowsPaused(status);
    labels.pause = paused ? l10n::Get(l10n::StringId::OpResume) : l10n::Get(l10n::StringId::OpPause);
    labels.pause_glyph = paused ? L"\xEDDA" : L"\xF8AE";  // solid, as the HTML (F5B0 is the toolbar split icon)
    const bool emptying = status.type == ops::OpType::EmptyRecycle;
    const bool authorization = status.authorization != ops::AuthorizationState::None;
    labels.show_details = !authorization;
    labels.show_retry = status.active && status.authorization == ops::AuthorizationState::ActionRequired
        && status.phase != ops::OpPhase::Cancelling;
    labels.show_skip = labels.show_retry && status.can_skip_authorization;
    labels.actions_enabled = !status.authorization_retrying;
    labels.retry = l10n::Get(l10n::StringId::OpRetryAuthorization);
    const std::wstring retrying = l10n::Get(l10n::StringId::Retrying).c_str();
    labels.retry_reserve = retrying.size() > labels.retry.size() ? retrying : labels.retry;
    if (!labels.actions_enabled) labels.retry = retrying;
    labels.skip = l10n::Get(l10n::StringId::OpSkipAuthorization);
    labels.show_pause = !authorization && !emptying && status.active && status.can_pause;
    labels.cancel = status.active && !emptying
        ? l10n::Get(l10n::StringId::Cancel) : l10n::Get(l10n::StringId::Close);
    if (partial.on && !status.active) {
        labels.show_open = partial.can_open;
        labels.open = l10n::Get(l10n::StringId::OpOpenDestination);
        labels.cancel_retries = partial.can_retry;
        labels.cancel_primary = partial.can_retry;
        if (partial.can_retry) labels.cancel = l10n::Get(l10n::StringId::OpRetryFailedItems);
    }
    return labels;
}

TransferChrome MakeTransferChrome(float scale, float width, float height,
                                  const fluent::Painter& painter, const TransferLabels& labels) {
    TransferChrome chrome;
    const float title = ScaleDip(scale, kCaptionH);
    const float caption = ScaleDip(scale, 40.0f);
    chrome.close = D2D1::RectF(width - caption, 0, width, title);
    chrome.minimize = D2D1::RectF(width - caption * 2.0f, 0, width - caption, title);
    const float footer_h = ScaleDip(scale, kFooterH);
    const float footer_top = height - footer_h;
    const float pad = ScaleDip(scale, kPadX);
    const float gap = ScaleDip(scale, 8.0f);
    const float btn_h = painter.MeasureButtonHeight();
    const float y0 = footer_top + (footer_h - btn_h) * 0.5f;
    const float y1 = y0 + btn_h;

    const float cancel_w = painter.MeasureButtonWidth(labels.cancel);
    chrome.cancel = D2D1::RectF(width - pad - cancel_w, y0, width - pad, y1);
    if (labels.show_pause) {
        const float pause_w = (std::max)(
            painter.MeasureButtonWidth(l10n::Get(l10n::StringId::OpPause), L"\xF8AE"),
            painter.MeasureButtonWidth(l10n::Get(l10n::StringId::OpResume), L"\xEDDA"));
        chrome.pause = D2D1::RectF(chrome.cancel.left - gap - pause_w, y0,
                                   chrome.cancel.left - gap, y1);
    }
    float next_right = chrome.cancel.left - gap;
    if (labels.show_open) {
        const float open_w = painter.MeasureButtonWidth(labels.open);
        chrome.open = D2D1::RectF(next_right - open_w, y0, next_right, y1);
        next_right = chrome.open.left - gap;
    }
    if (labels.show_skip) {
        const float skip_w = painter.MeasureButtonWidth(labels.skip);
        chrome.skip = D2D1::RectF(next_right - skip_w, y0, next_right, y1);
        next_right = chrome.skip.left - gap;
    }
    if (labels.show_retry) {
        const float retry_w = (std::max)(painter.MeasureButtonWidth(labels.retry, L"\xEA18"),
                                         painter.MeasureButtonWidth(labels.retry_reserve, L"\xEA18"));
        chrome.retry = D2D1::RectF(next_right - retry_w, y0, next_right, y1);
    }
    if (labels.show_details) chrome.details = painter.FitButtonBounds(D2D1::RectF(pad, y0, pad, y1),
                                             labels.details, L"\xE70D");
    return chrome;
}

void DrawEllipsizedText(Compositor& compositor, IDWriteTextFormat* format,
                        const D2D1_RECT_F& bounds, const std::wstring& text,
                        const D2D1_COLOR_F& color, bool wrap = false) {
    auto* factory = compositor.DwriteFactory();
    auto* dc = compositor.Dc();
    if (!factory || !dc || !format || text.empty()) return;
    const float width = bounds.right - bounds.left;
    const float height = bounds.bottom - bounds.top;
    if (width <= 0.0f || height <= 0.0f) return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                         format, width, height, &layout))) return;
    layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(format, &ellipsis))) {
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        layout->SetTrimming(&trimming, ellipsis.get());
    }
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(color, &brush))) return;
    compositor.DrawTextLayout(D2D1::Point2F(bounds.left, bounds.top), layout.get(), brush.get(),
                              D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float TextWidth(IDWriteFactory2* factory, IDWriteTextFormat* format, const std::wstring& text) {
    if (!factory || !format || text.empty()) return 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                         format, 100000.0f, 1000.0f, &layout))) return 0.0f;
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return 0.0f;
    return metrics.widthIncludingTrailingWhitespace;
}

ComPtr<ID2D1Factory> FactoryOf(ID2D1DeviceContext* dc) {
    ComPtr<ID2D1Factory> factory;
    if (dc) dc->GetFactory(&factory);
    return factory;
}

ComPtr<ID2D1StrokeStyle> RoundStroke(ID2D1Factory* factory) {
    ComPtr<ID2D1StrokeStyle> style;
    if (factory) {
        factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND), nullptr, 0, &style);
    }
    return style;
}

ComPtr<ID2D1LinearGradientBrush> LinearBrush(ID2D1DeviceContext* dc, D2D1_POINT_2F from,
                                             D2D1_POINT_2F to, D2D1_COLOR_F start,
                                             D2D1_COLOR_F end) {
    ComPtr<ID2D1LinearGradientBrush> brush;
    if (!dc) return brush;
    const D2D1_GRADIENT_STOP stops[2] = {{0.0f, start}, {1.0f, end}};
    ComPtr<ID2D1GradientStopCollection> collection;
    if (FAILED(dc->CreateGradientStopCollection(stops, 2, &collection))) return brush;
    dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(from, to),
                                  collection.get(), &brush);
    return brush;
}

// The 40-DIP header tile: a diagonal gradient per operation (copy indigo,
// move amber, delete red, restore teal; green once done).
void FillGradientTile(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, float radius,
                      D2D1_COLOR_F top, D2D1_COLOR_F bottom) {
    auto brush = LinearBrush(dc, D2D1::Point2F(rect.left, rect.top),
                             D2D1::Point2F(rect.right, rect.bottom), top, bottom);
    if (!brush.get()) return;
    dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.get());
    // Top-edge highlight that lifts the tile off the card.
    const D2D1_RECT_F gloss = D2D1::RectF(rect.left, rect.top, rect.right,
                                          rect.top + (rect.bottom - rect.top) * 0.5f);
    auto shine = LinearBrush(dc, D2D1::Point2F(rect.left, rect.top),
                             D2D1::Point2F(rect.left, gloss.bottom),
                             HexColor(0xFFFFFF, 0.22f), HexColor(0xFFFFFF, 0.0f));
    if (shine.get())
        dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), shine.get());
}

// Clockwise arc from `start_deg` (0 = 12 o'clock) spanning `span_deg`.
void StrokeArc(ID2D1DeviceContext* dc, D2D1_POINT_2F center, float radius, float start_deg,
               float span_deg, D2D1_COLOR_F color, float stroke) {
    if (!dc || span_deg <= 0.0f || radius <= 0.0f) return;
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(color, &brush))) return;
    if (span_deg >= 359.5f) {
        dc->DrawEllipse(D2D1::Ellipse(center, radius, radius), brush.get(), stroke);
        return;
    }
    auto factory = FactoryOf(dc);
    if (!factory.get()) return;
    const float a0 = (start_deg - 90.0f) * kPi / 180.0f;
    const float a1 = (start_deg + span_deg - 90.0f) * kPi / 180.0f;
    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(factory->CreatePathGeometry(&path))) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(&sink))) return;
    sink->BeginFigure(D2D1::Point2F(center.x + radius * std::cos(a0), center.y + radius * std::sin(a0)),
                      D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(
        D2D1::Point2F(center.x + radius * std::cos(a1), center.y + radius * std::sin(a1)),
        D2D1::SizeF(radius, radius), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
        span_deg > 180.0f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return;
    auto style = RoundStroke(factory.get());
    dc->DrawGeometry(path.get(), brush.get(), stroke, style.get());
}

// Smoothed speed area: midpoint-x cubic segments, a soft vertical gradient
// under the curve and a 1.6-DIP line on top (the HTML's svg path).
void DrawSpeedArea(ID2D1DeviceContext* dc, const D2D1_RECT_F& plot,
                   const std::vector<D2D1_POINT_2F>& points, D2D1_COLOR_F tone,
                   float fill_alpha, float scale) {
    if (!dc || points.size() < 2) return;
    auto factory = FactoryOf(dc);
    if (!factory.get()) return;
    auto add_curve = [&](ID2D1GeometrySink* sink) {
        for (size_t i = 1; i < points.size(); ++i) {
            const float mx = (points[i - 1].x + points[i].x) * 0.5f;
            sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(mx, points[i - 1].y),
                                                D2D1::Point2F(mx, points[i].y), points[i]));
        }
    };
    ComPtr<ID2D1PathGeometry> area;
    if (SUCCEEDED(factory->CreatePathGeometry(&area))) {
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(area->Open(&sink))) {
            sink->BeginFigure(D2D1::Point2F(points.front().x, plot.bottom), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(points.front());
            add_curve(sink.get());
            sink->AddLine(D2D1::Point2F(points.back().x, plot.bottom));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            if (SUCCEEDED(sink->Close())) {
                auto fill = LinearBrush(dc, D2D1::Point2F(0, plot.top), D2D1::Point2F(0, plot.bottom),
                                        WithAlpha(tone, 0.42f * fill_alpha),
                                        WithAlpha(tone, 0.02f * fill_alpha));
                if (fill.get()) dc->FillGeometry(area.get(), fill.get());
            }
        }
    }
    ComPtr<ID2D1PathGeometry> line;
    if (SUCCEEDED(factory->CreatePathGeometry(&line))) {
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(line->Open(&sink))) {
            sink->BeginFigure(points.front(), D2D1_FIGURE_BEGIN_HOLLOW);
            add_curve(sink.get());
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            if (SUCCEEDED(sink->Close())) {
                ComPtr<ID2D1SolidColorBrush> brush;
                if (SUCCEEDED(dc->CreateSolidColorBrush(WithAlpha(tone, 0.95f * fill_alpha + 0.05f), &brush))) {
                    auto style = RoundStroke(factory.get());
                    dc->DrawGeometry(line.get(), brush.get(), 1.6f * scale, style.get());
                }
            }
        }
    }
}

D2D1_RECT_F Rect(float scale, float x, float y, float width, float height) {
    return pulse::ui::DipRect(scale, x, y, width, height);
}

std::wstring LeafName(const std::wstring& path) {
    std::wstring_view view = path;
    while (view.size() > 1 && (view.back() == L'\\' || view.back() == L'/'))
        view.remove_suffix(1);
    const size_t slash = view.find_last_of(L"\\/");
    return slash == std::wstring_view::npos ? std::wstring(view)
                                             : std::wstring(view.substr(slash + 1));
}

std::wstring ParentName(const std::wstring& path) {
    std::wstring copy = path;
    while (copy.size() > 1 && (copy.back() == L'\\' || copy.back() == L'/')) copy.pop_back();
    const size_t slash = copy.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return copy;
    return LeafName(copy.substr(0, slash));
}

// Speed comes in one of two units: bytes for copy/move/empty-recycle, items for
// delete/restore. Formatting in one place keeps the current readout and the peak
// label in step with whichever unit the task actually tracks.
std::wstring FormatSpeedText(ops::OpSpeedBasis basis, double value) {
    if (basis != ops::OpSpeedBasis::Items)
        return pulse::format::ByteSize(static_cast<uint64_t>(value)) + L"/s";
    // Item rates are small: deleting a folder tree of many files reports one item per
    // several seconds, so rounding to whole items would print a flat "0 items/s" while
    // the graph visibly moves. Keep decimals below 10 and trim the trailing zeros.
    if (value >= 10.0)
        return pulse::format::GroupedInt(static_cast<uint64_t>(value + 0.5)) + L" " +
            l10n::Get(l10n::StringId::OpItemsPerSecond);
    wchar_t text[32]{};
    if (value >= 1.0) swprintf_s(text, L"%.1f", value);
    else if (value >= 0.01) swprintf_s(text, L"%.2f", value);
    else wcscpy_s(text, L"0");
    std::wstring number = text;
    if (number.find(L'.') != std::wstring::npos) {
        while (!number.empty() && number.back() == L'0') number.pop_back();
        if (!number.empty() && number.back() == L'.') number.pop_back();
    }
    return number + L" " + l10n::Get(l10n::StringId::OpItemsPerSecond);
}

std::wstring FormatDuration(uint64_t seconds) {
    if (seconds == 0) return l10n::Get(l10n::StringId::OpEstimating).c_str();
    if (seconds < 60) return std::to_wstring(seconds) + l10n::Get(l10n::StringId::OpSeconds).c_str();
    const uint64_t minutes = seconds / 60;
    if (minutes < 60) return std::to_wstring(minutes) + l10n::Get(l10n::StringId::OpMinutes).c_str()
        + std::to_wstring(seconds % 60) + l10n::Get(l10n::StringId::OpSeconds).c_str();
    return std::to_wstring(minutes / 60) + l10n::Get(l10n::StringId::OpHours).c_str()
        + std::to_wstring(minutes % 60) + l10n::Get(l10n::StringId::OpMinutesEnd).c_str();
}

// 已用时间 counts from zero, so 0 s is a real value rather than "estimating".
std::wstring FormatElapsed(uint64_t seconds) {
    if (seconds < 60) return std::to_wstring(seconds) + l10n::Get(l10n::StringId::OpSeconds).c_str();
    return FormatDuration(seconds);
}

// File conflict, pulse_ops_dialogs.html §3: source and destination side by
// side (size, time, 更大 / 较新), three choices that preview their result on
// hover (replace greys the destination, skip greys the source, keep both
// shows the new name), R / S / K, and 对其余 N 个冲突执行相同操作 as a switch.
// Thumbnail colours by extension (images, video, audio, archives,
// documents, binaries); folders get the folder amber.
void ThumbColors(const std::wstring& name, bool directory, D2D1_COLOR_F& top,
                        D2D1_COLOR_F& bottom, std::wstring& label) {
    top = HexColor(0x5B6170);
    bottom = HexColor(0x3B404B);
    label.clear();
    if (directory) { top = HexColor(0xF5C451); bottom = HexColor(0xD99A1E); return; }
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= name.size()) return;
    std::wstring ext = name.substr(dot + 1);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    auto any = [&](std::initializer_list<const wchar_t*> list) {
        for (const auto* e : list) if (ext == e) return true;
        return false;
    };
    label = ext.substr(0, 4);
    for (auto& c : label) c = static_cast<wchar_t>(towupper(c));
    if (any({L"jpg", L"jpeg", L"png", L"gif", L"bmp", L"webp", L"heic", L"tif", L"tiff", L"nef", L"cr2", L"cr3", L"arw", L"dng", L"raw", L"svg", L"ico"})) {
        top = HexColor(0x4CC2C9); bottom = HexColor(0x2A8F96);
    } else if (any({L"mp4", L"mkv", L"mov", L"avi", L"wmv", L"webm", L"m4v", L"ts"})) {
        top = HexColor(0xF07A73); bottom = HexColor(0xC4433D);
    } else if (any({L"mp3", L"flac", L"wav", L"m4a", L"aac", L"ogg", L"opus", L"wma"})) {
        top = HexColor(0xA78BFA); bottom = HexColor(0x7C5CE6);
    } else if (any({L"zip", L"7z", L"rar", L"tar", L"gz", L"xz", L"bz2", L"cab", L"iso"})) {
        top = HexColor(0xF0B041); bottom = HexColor(0xC98410);
    } else if (any({L"pdf", L"doc", L"docx", L"txt", L"md", L"rtf", L"xls", L"xlsx", L"csv", L"ppt", L"pptx", L"odt"})) {
        top = HexColor(0x7D8CF0); bottom = HexColor(0x5260C0);
    } else if (any({L"exe", L"dll", L"bin", L"sys", L"msi", L"json", L"xml", L"cpp", L"h", L"js", L"py", L"cs"})) {
        top = HexColor(0x8E6CE8); bottom = HexColor(0x5A3FB8);
    }
}

class ConflictWindow {
public:
    ConflictDialogResult Show(HWND owner, const ops::ConflictItemInfo& conflict,
                              bool dark, D2D1_COLOR_F accent) {
        owner_ = owner;
        conflict_ = conflict;
        conflict_.source = path::FriendlyPathText(conflict_.source);
        conflict_.destination = path::FriendlyPathText(conflict_.destination);
        source_dir_ = ParentPath(conflict_.source);
        destination_dir_ = ParentPath(conflict_.destination);
        dark_ = dark;
        accent_ = accent;
        scale_ = static_cast<float>(pulse::compat::WindowDpi(owner)) / 96.0f;

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kConflictClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        if (!GetClassInfoExW(wc.hInstance, kConflictClass, &wc)) RegisterClassExW(&wc);

        const int width = static_cast<int>(kConflictW * scale_);
        const int height = static_cast<int>(HeightDip() * scale_);
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kConflictClass, l10n::Get(l10n::StringId::OpConflictTitle).c_str(),
            WS_POPUP | WS_THICKFRAME | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, width, height, owner, nullptr,
            wc.hInstance, this);
        if (!hwnd_) return result_;
        pulse::ui::CenterOwnedWindow(hwnd_, owner_, width, height);
        EnableWindow(owner_, FALSE);
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        // Without keyboard focus Windows sends letters as WM_SYSKEYDOWN, so
        // R / S / K, Tab and Enter would only work with Alt held.
        SetFocus(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (IsWindow(hwnd_)) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
        hwnd_ = nullptr;
        EnableWindow(owner_, TRUE);
        return result_;
    }

private:
    static constexpr float kConflictW = 560.0f;
    static constexpr float kCompareTop = 100.0f;
    static constexpr float kCompareH = 112.0f;
    static constexpr float kCardW = 242.0f;
    static constexpr float kActionsTop = 226.0f;
    static constexpr float kActionH = 52.0f;
    static constexpr float kFooterH = 56.0f;
    static constexpr UINT_PTR kAnimTimer = 2;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<ConflictWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<ConflictWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        return self ? self->Handle(message, wparam, lparam)
                    : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    static std::wstring ParentPath(const std::wstring& path) {
        std::wstring copy = path;
        while (copy.size() > 3 && (copy.back() == L'\\' || copy.back() == L'/')) copy.pop_back();
        const size_t slash = copy.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return copy;
        return copy.substr(0, slash == 2 && copy[1] == L':' ? 3 : slash);
    }

    bool ShowsApplyAll() const { return conflict_.remaining > 1; }
    float HeightDip() const { return ShowsApplyAll() ? 506.0f : 470.0f; }
    size_t Ordinal() const { return (std::max)(conflict_.ordinal, static_cast<size_t>(1)); }
    size_t Total() const {
        return Ordinal() - 1 + (std::max)(conflict_.remaining, static_cast<size_t>(1));
    }
    // 较新 only when the shown times differ: within a minute both read the same.
    static bool NewerThan(const FILETIME& a, const FILETIME& b) {
        const ULONGLONG ta = (static_cast<ULONGLONG>(a.dwHighDateTime) << 32) | a.dwLowDateTime;
        const ULONGLONG tb = (static_cast<ULONGLONG>(b.dwHighDateTime) << 32) | b.dwLowDateTime;
        return ta != 0 && tb != 0 && ta > tb + 60ull * 10000000ull;
    }
    bool SourceNewer() const { return NewerThan(conflict_.source_modified, conflict_.destination_modified); }
    bool DestinationNewer() const { return NewerThan(conflict_.destination_modified, conflict_.source_modified); }
    bool Files() const { return !conflict_.source_is_directory && !conflict_.destination_is_directory; }

    D2D1_RECT_F CloseRect() const { return Rect(scale_, kConflictW - 46, 0, 46, 40); }
    D2D1_RECT_F CardRect(int side) const {
        return Rect(scale_, side == 0 ? 20.0f : kConflictW - 20.0f - kCardW, kCompareTop, kCardW, kCompareH);
    }
    D2D1_RECT_F PathRect(int side) const {
        const float x = side == 0 ? 20.0f : kConflictW - 20.0f - kCardW;
        return Rect(scale_, x + 12, kCompareTop + 88, kCardW - 24, 16);
    }
    D2D1_RECT_F ActionRect(int index) const {
        return Rect(scale_, 20, kActionsTop + index * (kActionH + 6.0f), kConflictW - 40, kActionH);
    }
    D2D1_RECT_F ApplyRect() const { return Rect(scale_, 20, 404, kConflictW - 40, 30); }
    D2D1_RECT_F CancelRect() const {
        const float h = HeightDip();
        return Rect(scale_, kConflictW - 20 - 98, h - kFooterH + 11, 98, 34);
    }

    int Hit(float x, float y) const {
        if (pulse::ui::ContainsRect(CloseRect(), x, y)) return 7;
        for (int i = 0; i < 3; ++i) if (pulse::ui::ContainsRect(ActionRect(i), x, y)) return i + 1;
        if (ShowsApplyAll() && pulse::ui::ContainsRect(ApplyRect(), x, y)) return 4;
        if (pulse::ui::ContainsRect(CancelRect(), x, y)) return 6;
        return 0;
    }

    void Complete(ops::ConflictChoice choice) {
        result_.choice = choice;
        result_.apply_to_all = apply_all_;
        done_ = true;
        if (hwnd_) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
    }

    // Preview of the hovered (or focused) choice: 1 replace, 2 skip, 3 keep.
    int Preview() const {
        if (hover_ >= 1 && hover_ <= 3) return hover_;
        if (keyboard_focus_ && focus_ <= 2) return focus_ + 1;
        return 0;
    }

    void Animate() {
        const ULONGLONG now = GetTickCount64();
        const float dt = anim_tick_ == 0 ? 16.0f : static_cast<float>((std::min)(now - anim_tick_, 100ull));
        anim_tick_ = now;
        const float k = 1.0f - std::exp(-dt / 70.0f);
        const int preview = Preview();
        bool moving = false;
        for (int i = 1; i <= 3; ++i) {
            const float target = preview == i ? 1.0f : 0.0f;
            weight_[i] += (target - weight_[i]) * k;
            if (std::fabs(target - weight_[i]) < 0.01f) weight_[i] = target;
            else moving = true;
        }
        if (!moving) { KillTimer(hwnd_, kAnimTimer); anim_tick_ = 0; }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void Kick() {
        anim_tick_ = 0;
        SetTimer(hwnd_, kAnimTimer, 15, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void CreateFormats() {
        auto* factory = compositor_.DwriteFactory();
        auto make = [&](ComPtr<IDWriteTextFormat>& out, typography::FontRole role, float size,
                        DWRITE_FONT_WEIGHT weight) {
            out.reset();
            if (!factory) return;
            typography::TextFormatSpec spec;
            spec.role = role;
            spec.size = size * scale_;
            spec.weight = weight;
            if (FAILED(typography::CreateTextFormat(factory, spec, &out)) || !out.get()) return;
            out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        };
        make(head_format_, typography::FontRole::Text, 15.5f, DWRITE_FONT_WEIGHT_BOLD);
        make(name_format_, typography::FontRole::Text, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        make(meta_format_, typography::FontRole::Text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL);
        make(meta_bold_format_, typography::FontRole::Text, 12.0f, DWRITE_FONT_WEIGHT_BOLD);
        make(label_format_, typography::FontRole::Text, 11.0f, DWRITE_FONT_WEIGHT_BOLD);
        make(tiny_format_, typography::FontRole::Text, 10.5f, DWRITE_FONT_WEIGHT_BOLD);
        make(ext_format_, typography::FontRole::Text, 8.5f, DWRITE_FONT_WEIGHT_HEAVY);
        make(glyph_format_, typography::FontRole::Icon, 14.0f, DWRITE_FONT_WEIGHT_NORMAL);
        make(big_glyph_format_, typography::FontRole::Icon, 18.0f, DWRITE_FONT_WEIGHT_NORMAL);
        if (glyph_format_.get()) glyph_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        if (big_glyph_format_.get()) big_glyph_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    void DrawCompareCard(const Theme& theme, int side) {
        auto* dc = compositor_.Dc();
        const bool source = side == 0;
        const float s = scale_;
        const float x = source ? 20.0f : kConflictW - 20.0f - kCardW;
        const float y = kCompareTop;
        // Replace greys the destination and nudges the source toward it;
        // skip greys the source; keep both rings both.
        const float dim = source ? weight_[2] : weight_[1];
        const float shift = source ? 6.0f * weight_[1] : 0.0f;
        const float cx = x + shift;
        const D2D1_COLOR_F green = dark_ ? HexColor(0x3CCF8E) : HexColor(0x16A36A);
        const D2D1_COLOR_F sky = dark_ ? HexColor(0x38BDF8) : HexColor(0x0284C7);
        const D2D1_COLOR_F violet = dark_ ? HexColor(0xA78BFA) : HexColor(0x7C5CE6);
        const D2D1_COLOR_F red = dark_ ? HexColor(0xF0625B) : HexColor(0xD93D36);
        const D2D1_COLOR_F well = OpWell(hwnd_, backdrop_enabled_, dark_);
        const D2D1_COLOR_F line = dark_ ? HexColor(0xFFFFFF, 0.07f) : HexColor(0x000000, 0.07f);
        const auto card = Rect(s, cx, y, kCardW, kCompareH);
        painter_.FillRoundedRect(card, ScaleDip(s, 11.0f), well);
        painter_.StrokeRoundedRect(card, ScaleDip(s, 11.0f), line);
        const float ring = source ? (std::max)(weight_[1], weight_[3]) : (std::max)(weight_[2], weight_[3]);
        if (ring > 0.01f) {
            D2D1_COLOR_F color = weight_[3] > 0.01f ? violet : source ? green : sky;
            painter_.StrokeRoundedRect(card, ScaleDip(s, 11.0f), WithAlpha(color, ring), 1.5f * s);
        }

        const std::wstring name = LeafName(source ? conflict_.source : conflict_.destination);
        const bool directory = source ? conflict_.source_is_directory : conflict_.destination_is_directory;
        const float alpha = 1.0f - 0.55f * dim;
        auto fade = [&](D2D1_COLOR_F c) { c.a *= alpha; return c; };

        painter_.DrawText(l10n::Get(source ? l10n::StringId::OpSource : l10n::StringId::OpExisting),
                          Rect(s, cx + 12, y + 8, kCardW - 24, 16), label_format_.get(),
                          fade(WithAlpha(theme.text, 0.45f)));

        // Thumbnail: gradient tile with a folded corner and the extension.
        D2D1_COLOR_F top{}, bottom{};
        std::wstring ext;
        ThumbColors(name, directory, top, bottom, ext);
        if (dim > 0.01f) {
            const float grey = (top.r + top.g + top.b) / 3.0f;
            const float grey2 = (bottom.r + bottom.g + bottom.b) / 3.0f;
            top = D2D1::ColorF(top.r + (grey - top.r) * dim, top.g + (grey - top.g) * dim, top.b + (grey - top.b) * dim, alpha);
            bottom = D2D1::ColorF(bottom.r + (grey2 - bottom.r) * dim, bottom.g + (grey2 - bottom.g) * dim,
                                  bottom.b + (grey2 - bottom.b) * dim, alpha);
        }
        const auto thumb = Rect(s, cx + 12, y + 28, 40, 50);
        FillGradientTile(dc, thumb, ScaleDip(s, 7.0f), top, bottom);
        if (directory) {
            painter_.DrawGlyphWithFormat(L"\xE8B7", thumb, fade(HexColor(0xFFFFFF, 0.92f)), big_glyph_format_.get());
        } else {
            // Folded corner.
            auto factory = FactoryOf(dc);
            ComPtr<ID2D1PathGeometry> corner;
            if (factory.get() && SUCCEEDED(factory->CreatePathGeometry(&corner))) {
                ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(corner->Open(&sink))) {
                    const float c = ScaleDip(s, 10.0f);
                    sink->BeginFigure(D2D1::Point2F(thumb.right - c, thumb.top), D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine(D2D1::Point2F(thumb.right, thumb.top + c));
                    sink->AddLine(D2D1::Point2F(thumb.right - c, thumb.top + c));
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    if (SUCCEEDED(sink->Close())) {
                        ComPtr<ID2D1SolidColorBrush> brush;
                        if (SUCCEEDED(dc->CreateSolidColorBrush(HexColor(0xFFFFFF, 0.30f * alpha), &brush)))
                            dc->FillGeometry(corner.get(), brush.get());
                    }
                }
            }
            if (!ext.empty()) {
                const float w = TextWidth(compositor_.DwriteFactory(), ext_format_.get(), ext) / s + 7.0f;
                const auto tag = Rect(s, cx + 16, y + 63, (std::min)(w, 32.0f), 12);
                painter_.FillRoundedRect(tag, ScaleDip(s, 3.0f), HexColor(0x000000, 0.25f * alpha));
                painter_.DrawText(ext, tag, ext_format_.get(), fade(HexColor(0xFFFFFF)),
                                  fluent::HorizontalAlignment::Center);
            }
        }

        const float tx = cx + 62;
        const float tw = kCardW - 74;
        DrawEllipsizedText(compositor_, name_format_.get(), Rect(s, tx, y + 27, tw, 20), name, fade(theme.text));
        auto meta_line = [&](float row_y, const std::wstring& value, bool bold, const std::wstring& badge) {
            IDWriteTextFormat* format = bold ? meta_bold_format_.get() : meta_format_.get();
            const float w = (std::min)(tw, TextWidth(compositor_.DwriteFactory(), format, value) / s);
            painter_.DrawText(value, Rect(s, tx, row_y, w + 2, 17), format,
                              fade(bold ? theme.text : theme.text_secondary));
            if (!badge.empty()) {
                const float bw = TextWidth(compositor_.DwriteFactory(), tiny_format_.get(), badge) / s + 12.0f;
                const auto chip = Rect(s, tx + w + 6, row_y, bw, 17);
                painter_.FillRoundedRect(chip, ScaleDip(s, 5.0f), WithAlpha(green, (dark_ ? 0.16f : 0.12f) * alpha));
                painter_.DrawText(badge, chip, tiny_format_.get(), fade(green), fluent::HorizontalAlignment::Center);
            }
        };
        const uint64_t size = source ? conflict_.source_size : conflict_.destination_size;
        const uint64_t other = source ? conflict_.destination_size : conflict_.source_size;
        const bool larger = Files() && size > other;
        const bool newer = source ? SourceNewer() : DestinationNewer();
        meta_line(y + 49, directory ? l10n::Get(l10n::StringId::OpFolderKind) : pulse::format::ByteSize(size),
                  !directory, larger ? l10n::Get(l10n::StringId::OpLarger) : std::wstring());
        meta_line(y + 67, pulse::format::LocalFileTime(source ? conflict_.source_modified : conflict_.destination_modified,
                                                       l10n::Get(l10n::StringId::OpUnknown).c_str()),
                  false, newer ? l10n::Get(l10n::StringId::OpNewer) : std::wstring());

        // Path, or for keep-both the name the copy will get.
        const std::wstring& dir = source ? source_dir_ : destination_dir_;
        const D2D1_COLOR_F faint = WithAlpha(theme.text, dark_ ? 0.42f : 0.48f);
        const float ren = source ? weight_[3] : 0.0f;
        if (ren > 0.01f && !conflict_.keep_both_name.empty()) {
            DrawEllipsizedText(compositor_, meta_bold_format_.get(), Rect(s, cx + 12, y + 87, kCardW - 24, 17),
                               l10n::Get(l10n::StringId::OpSaveAsLead) + conflict_.keep_both_name +
                                   l10n::Get(l10n::StringId::OpQuoteClose),
                               WithAlpha(violet, ren));
            if (ren < 0.99f)
                DrawEllipsizedText(compositor_, label_format_.get(), Rect(s, cx + 12, y + 88, kCardW - 24, 16),
                                   dir, WithAlpha(faint, faint.a * (1.0f - ren)));
        } else {
            DrawEllipsizedText(compositor_, meta_format_.get(), Rect(s, cx + 12, y + 88, kCardW - 24, 16),
                               dir, fade(faint));
        }

        // Result tag in the corner: 将被覆盖 on the destination, 不复制 on the source.
        const float tag_alpha = source ? weight_[2] : weight_[1];
        if (tag_alpha > 0.01f) {
            const std::wstring text = source
                ? l10n::Get(conflict_.type == ops::OpType::Move ? l10n::StringId::OpNotMoved : l10n::StringId::OpNotCopied)
                : l10n::Get(l10n::StringId::OpWillBeReplaced);
            const float w = TextWidth(compositor_.DwriteFactory(), tiny_format_.get(), text) / s + 14.0f;
            const float lift = 3.0f * (1.0f - tag_alpha);
            const auto tag = Rect(s, cx + kCardW - 10 - w, y + 9 - lift, w, 18);
            painter_.FillRoundedRect(tag, ScaleDip(s, 5.0f),
                source ? WithAlpha(dark_ ? HexColor(0x3A3A3E) : HexColor(0xE2E2E6), tag_alpha)
                       : WithAlpha(red, tag_alpha));
            painter_.DrawText(text, tag, tiny_format_.get(),
                              WithAlpha(source ? theme.text : HexColor(0xFFFFFF), tag_alpha),
                              fluent::HorizontalAlignment::Center);
        }
    }

    void DrawAction(const Theme& theme, int index, const std::wstring& title, const std::wstring& subtitle,
                    const wchar_t* glyph, D2D1_COLOR_F tone, const wchar_t* key, const std::wstring& recommend) {
        const float s = scale_;
        const auto bounds = ActionRect(index);
        const bool hovered = hover_ == index + 1;
        const bool pressed = pressed_ == index + 1;
        const bool focused = keyboard_focus_ && focus_ == index;
        const D2D1_COLOR_F well = OpWell(hwnd_, backdrop_enabled_, dark_, hovered);
        D2D1_RECT_F box = bounds;
        if (pressed) {
            const float inset = ScaleDip(s, 1.5f);
            box = D2D1::RectF(box.left + inset, box.top + inset * 0.5f, box.right - inset, box.bottom - inset * 0.5f);
        }
        painter_.FillRoundedRect(box, ScaleDip(s, 10.0f), well);
        painter_.StrokeRoundedRect(box, ScaleDip(s, 10.0f),
            focused ? (dark_ ? HexColor(0x8F9CF0) : theme.accent)
                    : (dark_ ? HexColor(0xFFFFFF, 0.07f) : HexColor(0x000000, 0.07f)),
            focused ? 1.5f * s : 1.0f);
        const float y = kActionsTop + index * (kActionH + 6.0f);
        const auto icon = Rect(s, 32, y + 11, 30, 30);
        painter_.FillRoundedRect(icon, ScaleDip(s, 9.0f), WithAlpha(tone, dark_ ? 0.16f : 0.13f));
        painter_.DrawGlyphWithFormat(glyph, icon, tone, glyph_format_.get());
        const float title_w = TextWidth(compositor_.DwriteFactory(), name_format_.get(), title) / s;
        painter_.DrawText(title, Rect(s, 74, y + 8, title_w + 2, 20), name_format_.get(), theme.text);
        if (!recommend.empty()) {
            const D2D1_COLOR_F green = dark_ ? HexColor(0x3CCF8E) : HexColor(0x16A36A);
            const float w = TextWidth(compositor_.DwriteFactory(), tiny_format_.get(), recommend) / s + 14.0f;
            const auto chip = Rect(s, 74 + title_w + 8, y + 9, w, 18);
            painter_.FillRoundedRect(chip, ScaleDip(s, 9.0f), WithAlpha(green, dark_ ? 0.16f : 0.12f));
            painter_.DrawText(recommend, chip, tiny_format_.get(), green, fluent::HorizontalAlignment::Center);
        }
        DrawEllipsizedText(compositor_, meta_format_.get(), Rect(s, 74, y + 28, kConflictW - 40 - 54 - 56, 17),
                           subtitle, theme.text_secondary);
        painter_.DrawBadge({ Rect(s, kConflictW - 20 - 12 - 28, y + 15, 28, 22), key, fluent::BadgeKind::Keycap });
    }

    void Render() {
        if (compositor_.NeedsRecovery()) {
            if (!compositor_.Recover()) return;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            CreateFormats();
        }
        if (!compositor_.Dc()) return;
        if (!head_format_.get()) CreateFormats();
        const float s = scale_;
        const bool high_contrast = IsHighContrast();
        const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        pulse::ui::BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_enabled_, s);
        const D2D1_COLOR_F violet = dark_ ? HexColor(0xA78BFA) : HexColor(0x7C5CE6);
        const D2D1_COLOR_F green = dark_ ? HexColor(0x3CCF8E) : HexColor(0x16A36A);
        const D2D1_COLOR_F sky = dark_ ? HexColor(0x38BDF8) : HexColor(0x0284C7);
        const D2D1_COLOR_F faint = WithAlpha(theme.text, dark_ ? 0.42f : 0.48f);
        const bool moving = conflict_.type == ops::OpType::Move;

        // Title strip.
        painter_.DrawText(l10n::Get(l10n::StringId::OpConflictTitle), Rect(s, 16, 0, 300, 40),
                          compositor_.SmallFormat(), theme.text_secondary);
        fluent::ControlState close_state{};
        close_state.hovered = hover_ == 7;
        close_state.pressed = pressed_ == 7;
        painter_.DrawTitleBarButton(CloseRect(), fluent::TitleBarButtonRole::Close, {}, close_state);

        // Header: tile, headline, where it happened, n / m.
        const auto tile = Rect(s, 20, 46, 38, 38);
        painter_.FillRoundedRect(tile, ScaleDip(s, 11.0f), WithAlpha(violet, dark_ ? 0.16f : 0.13f));
        painter_.DrawGlyphWithFormat(L"\xE8AB", tile, violet, big_glyph_format_.get());
        float head_right = kConflictW - 20.0f;
        if (Total() > 1) {
            const std::wstring count = l10n::Get(l10n::StringId::OpConflictCountLead) +
                pulse::format::GroupedInt(Ordinal()) + l10n::Get(l10n::StringId::OpConflictCountSep) +
                pulse::format::GroupedInt(Total()) + l10n::Get(l10n::StringId::OpConflictCountEnd);
            const float w = TextWidth(compositor_.DwriteFactory(), tiny_format_.get(), count) / s + 18.0f;
            const auto chip = Rect(s, kConflictW - 20 - w, 54, w, 22);
            painter_.FillRoundedRect(chip, ScaleDip(s, 11.0f), WithAlpha(violet, dark_ ? 0.16f : 0.12f));
            painter_.DrawText(count, chip, tiny_format_.get(), violet, fluent::HorizontalAlignment::Center);
            head_right = kConflictW - 20 - w - 10;
        }
        DrawEllipsizedText(compositor_, head_format_.get(), Rect(s, 70, 44, head_right - 70, 24),
                           l10n::Get(l10n::StringId::OpConflictLead) + LeafName(conflict_.destination) +
                               l10n::Get(l10n::StringId::OpQuoteClose), theme.text);
        const std::wstring where = l10n::Get(moving ? l10n::StringId::OpConflictWhileMove
                                                    : l10n::StringId::OpConflictWhileCopy) +
            LeafName(source_dir_) + L" \x2192 " + LeafName(destination_dir_) +
            l10n::Get(l10n::StringId::OpConflictWhileEnd);
        DrawEllipsizedText(compositor_, meta_format_.get(), Rect(s, 70, 67, head_right - 70, 18), where,
                           theme.text_secondary);

        // Side-by-side comparison with the vs marker between.
        DrawCompareCard(theme, 0);
        DrawCompareCard(theme, 1);
        const auto vs = Rect(s, kConflictW * 0.5f - 13, kCompareTop + kCompareH * 0.5f - 13, 26, 26);
        painter_.FillRoundedRect(vs, ScaleDip(s, 13.0f), dark_ ? HexColor(0xFFFFFF, 0.08f) : HexColor(0x000000, 0.06f));
        painter_.DrawGlyphWithFormat(L"\xE8AB", vs, faint, glyph_format_.get());

        // Choices.
        const bool files = Files();
        std::wstring replace_sub = l10n::Get(l10n::StringId::OpReplaceDesc);
        if (files)
            replace_sub = l10n::Get(l10n::StringId::OpReplaceUseLead) + pulse::format::ByteSize(conflict_.source_size) +
                l10n::Get(l10n::StringId::OpReplaceUseMid) + pulse::format::ByteSize(conflict_.destination_size) +
                l10n::Get(l10n::StringId::OpReplaceUseEnd);
        const std::wstring keep_sub = conflict_.keep_both_name.empty()
            ? l10n::Get(l10n::StringId::OpKeepBothDesc)
            : l10n::Get(l10n::StringId::OpKeepNameLead) + conflict_.keep_both_name + l10n::Get(l10n::StringId::OpQuoteClose);
        DrawAction(theme, 0, l10n::Get(l10n::StringId::OpReplaceFile), replace_sub, L"\xE8EE", green, L"R",
                   files && SourceNewer() ? l10n::Get(l10n::StringId::OpRecommendNewer) : std::wstring());
        DrawAction(theme, 1, l10n::Get(l10n::StringId::OpSkipFile), l10n::Get(l10n::StringId::OpSkipDesc),
                   L"\xE893", sky, L"S", {});
        DrawAction(theme, 2, l10n::Get(l10n::StringId::OpKeepBoth), keep_sub, L"\xE8C8", violet, L"K", {});

        if (ShowsApplyAll()) {
            fluent::ControlState toggle{};
            toggle.checked = apply_all_;
            toggle.hovered = hover_ == 4;
            toggle.focused = keyboard_focus_ && focus_ == 3;
            toggle.keyboard_focus = toggle.focused;
            const std::wstring label = l10n::Get(l10n::StringId::OpApplyRestLead) +
                pulse::format::GroupedInt(conflict_.remaining - 1) + l10n::Get(l10n::StringId::OpApplyRestEnd);
            painter_.DrawSwitch(ApplyRect(), label, toggle);
        }

        // Footer.
        const float footer_top = (HeightDip() - kFooterH) * s;
        RECT client{};
        GetClientRect(hwnd_, &client);
        const float width = static_cast<float>(client.right);
        const float height = static_cast<float>(client.bottom);
        painter_.FillRoundedRect(D2D1::RectF(0, footer_top, width, height), 0,
                                 OpFooter(hwnd_, backdrop_enabled_, dark_));
        painter_.FillRoundedRect(D2D1::RectF(0, footer_top, width, footer_top + 1.0f), 0, theme.stroke_divider);
        painter_.DrawText(l10n::Get(l10n::StringId::OpHoverPreviewHint),
                          Rect(s, 20, HeightDip() - kFooterH, 300, kFooterH), meta_format_.get(), faint);
        fluent::ControlState cancel_state{};
        cancel_state.hovered = hover_ == 6;
        cancel_state.pressed = pressed_ == 6;
        cancel_state.focused = keyboard_focus_ && focus_ == (ShowsApplyAll() ? 4 : 3);
        painter_.DrawButton({ CancelRect(), l10n::Get(l10n::StringId::Cancel), {}, fluent::ButtonKind::Standard,
                              cancel_state });
        // Paths are cut to their line; hovering one shows it whole (#55).
        SetPathTip(1, PathRect(0));
        SetPathTip(2, PathRect(1));
        pulse::ui::EndSurface(compositor_);
    }

    void Choose(int index) {
        Complete(index == 0 ? ops::ConflictChoice::Replace
                 : index == 1 ? ops::ConflictChoice::Skip
                              : ops::ConflictChoice::KeepBoth);
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            scale_ = static_cast<float>(pulse::compat::WindowDpi(hwnd_)) / 96.0f;
            backdrop_enabled_ = pulse::ui::ApplyBackdrop(hwnd_, dark_);
            if (!compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            CreateFormats();
            CreatePathTips();
            // R / S / K are shortcuts here, not text: keep a Chinese IME from
            // swallowing them as composition (they would only work with Alt).
            ImmAssociateContextEx(hwnd_, nullptr, 0);
            return 0;
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST:
            return pulse::ui::BorderlessHitTest(hwnd_, lparam, 40 * scale_, CloseRect());
        case WM_SIZE:
            if (compositor_.Dc()) compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOVE:
            compositor_.UpdateTextRenderingParams(
                MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
            return 0;
        case WM_DPICHANGED: {
            scale_ = HIWORD(wparam) / 96.0f;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetScale(scale_);
            CreateFormats();
            const auto* suggested = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_TIMER:
            if (wparam == kAnimTimer) { Animate(); return 0; }
            break;
        case WM_MOUSEMOVE: {
            const int next = Hit(static_cast<float>(GET_X_LPARAM(lparam)),
                                 static_cast<float>(GET_Y_LPARAM(lparam)));
            if (next != hover_) { hover_ = next; Kick(); }
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_ = 0;
            Kick();
            return 0;
        case WM_LBUTTONDOWN:
            pressed_ = Hit(static_cast<float>(GET_X_LPARAM(lparam)),
                           static_cast<float>(GET_Y_LPARAM(lparam)));
            keyboard_focus_ = false;
            SetCapture(hwnd_);
            Kick(); // a focus preview fades once the mouse takes over
            return 0;
        case WM_LBUTTONUP: {
            const int hit = Hit(static_cast<float>(GET_X_LPARAM(lparam)),
                                static_cast<float>(GET_Y_LPARAM(lparam)));
            const int pressed = pressed_;
            pressed_ = 0;
            ReleaseCapture();
            if (hit == pressed) {
                if (hit >= 1 && hit <= 3) { focus_ = hit - 1; Choose(hit - 1); return 0; }
                if (hit == 4) apply_all_ = !apply_all_;
                else if (hit == 6 || hit == 7) { Complete(ops::ConflictChoice::Cancel); return 0; }
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN: {
            if (wparam == VK_PROCESSKEY) {
                const UINT vk = ImmGetVirtualKey(hwnd_);
                if (vk && vk != VK_PROCESSKEY) wparam = vk;
            }
            if (wparam == 'R') { Choose(0); return 0; }
            if (wparam == 'S') { Choose(1); return 0; }
            if (wparam == 'K') { Choose(2); return 0; }
            if (wparam == VK_ESCAPE) { Complete(ops::ConflictChoice::Cancel); return 0; }
            const int count = ShowsApplyAll() ? 5 : 4;
            if (wparam == VK_TAB || wparam == VK_DOWN || wparam == VK_UP) {
                const bool back = wparam == VK_UP || (wparam == VK_TAB && GetKeyState(VK_SHIFT) < 0);
                if (keyboard_focus_) focus_ = (focus_ + (back ? count - 1 : 1)) % count;
                keyboard_focus_ = true;
                Kick();
                return 0;
            }
            if (wparam == VK_RETURN || wparam == VK_SPACE) {
                if (focus_ <= 2) { Choose(focus_); return 0; }
                if (focus_ == 3 && ShowsApplyAll()) apply_all_ = !apply_all_;
                else { Complete(ops::ConflictChoice::Cancel); return 0; }
                keyboard_focus_ = true;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            break;
        }
        case WM_ACTIVATE:
            if (LOWORD(wparam) != WA_INACTIVE) SetFocus(hwnd_);
            return 0;
        case WM_SYSKEYDOWN:
            // No focus: plain keys arrive here with the Alt bit clear.
            if (!(lparam & (1 << 29)) && wparam != VK_F10) return Handle(WM_KEYDOWN, wparam, lparam);
            if (wparam == 'R') { Choose(0); return 0; }
            if (wparam == 'S') { Choose(1); return 0; }
            if (wparam == 'K') { Choose(2); return 0; }
            break;
        case WM_CLOSE:
            Complete(ops::ConflictChoice::Cancel);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd_, &paint);
            Render();
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            KillTimer(hwnd_, kAnimTimer);
            compositor_.Shutdown();
            done_ = true;
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    void CreatePathTips() {
        INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_BAR_CLASSES };
        InitCommonControlsEx(&controls);
        tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!tooltip_) return;
        SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, static_cast<LPARAM>(560 * scale_));
        for (UINT id : { 1u, 2u }) {
            TOOLINFOW info{ sizeof(info) };
            info.uFlags = TTF_SUBCLASS;
            info.hwnd = hwnd_;
            info.uId = id;
            info.lpszText = const_cast<wchar_t*>(
                (id == 1 ? conflict_.source : conflict_.destination).c_str());
            SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
        }
    }

    void SetPathTip(UINT id, const D2D1_RECT_F& box) {
        if (!tooltip_) return;
        TOOLINFOW info{ sizeof(info) };
        info.hwnd = hwnd_;
        info.uId = id;
        info.rect = { static_cast<LONG>(std::floor(box.left)), static_cast<LONG>(std::floor(box.top)),
                      static_cast<LONG>(std::ceil(box.right)), static_cast<LONG>(std::ceil(box.bottom)) };
        SendMessageW(tooltip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&info));
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HWND tooltip_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_{ &compositor_ };
    ops::ConflictItemInfo conflict_;
    std::wstring source_dir_;
    std::wstring destination_dir_;
    ConflictDialogResult result_;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    float scale_ = 1.0f;
    bool dark_ = false;
    bool backdrop_enabled_ = false;
    bool apply_all_ = false;
    bool done_ = false;
    bool keyboard_focus_ = false;
    int hover_ = 0;
    int pressed_ = 0;
    int focus_ = 0;
    float weight_[4] = {};
    ULONGLONG anim_tick_ = 0;
    ComPtr<IDWriteTextFormat> head_format_;
    ComPtr<IDWriteTextFormat> name_format_;
    ComPtr<IDWriteTextFormat> meta_format_;
    ComPtr<IDWriteTextFormat> meta_bold_format_;
    ComPtr<IDWriteTextFormat> label_format_;
    ComPtr<IDWriteTextFormat> tiny_format_;
    ComPtr<IDWriteTextFormat> ext_format_;
    ComPtr<IDWriteTextFormat> glyph_format_;
    ComPtr<IDWriteTextFormat> big_glyph_format_;
};

// Permanent delete, pulse_ops_dialogs.html §6: the first items fanned out as
// thumbnails, the first three listed with sizes, a red "can't be undone"
// line, and 移到回收站 as the way out when the items have a Recycle Bin.
class PermanentDeleteWindow {
public:
    PermanentDeleteChoice Show(HWND owner, const PermanentDeleteInfo& info, bool dark, D2D1_COLOR_F accent) {
        owner_ = owner;
        info_ = info;
        dark_ = dark;
        accent_ = accent;
        scale_ = static_cast<float>(pulse::compat::WindowDpi(owner)) / 96.0f;
        Measure();

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kDeleteClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!GetClassInfoExW(wc.hInstance, kDeleteClass, &wc)) RegisterClassExW(&wc);

        const int width = static_cast<int>(kW * scale_);
        const int height = static_cast<int>(kH * scale_);
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kDeleteClass,
            l10n::Get(l10n::StringId::PermanentDelete).c_str(), WS_POPUP | WS_THICKFRAME | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, width, height, owner, nullptr, wc.hInstance, this);
        if (!hwnd_) return choice_;
        pulse::ui::CenterOwnedWindow(hwnd_, owner_, width, height);
        if (owner_) EnableWindow(owner_, FALSE);
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
        opened_ = GetTickCount64();
        SetTimer(hwnd_, kFanTimer, 15, nullptr);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (IsWindow(hwnd_)) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
        hwnd_ = nullptr;
        if (owner_) EnableWindow(owner_, TRUE);
        return choice_;
    }

private:
    static constexpr float kW = 460.0f;
    static constexpr float kH = 310.0f;
    static constexpr float kStackTop = 100.0f;
    static constexpr float kWarnTop = 198.0f;
    static constexpr float kFooterTop = 250.0f;
    static constexpr UINT_PTR kFanTimer = 3;
    static constexpr ULONGLONG kFanDelay = 120;
    static constexpr ULONGLONG kFanMs = 450;

    struct Item {
        std::wstring name;
        bool directory = false;
        bool sized = false;
        uint64_t bytes = 0;
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<PermanentDeleteWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<PermanentDeleteWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        return self ? self->Handle(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    // Names and sizes for the list; the total only when every item is a file
    // (a folder's size would need a walk) and the selection is small.
    void Measure() {
        files_ = folders_ = 0;
        total_ = 0;
        const bool stat_all = info_.paths.size() <= 256;
        for (size_t i = 0; i < info_.paths.size(); ++i) {
            if (!stat_all && i >= 3) break;
            Item item;
            item.name = LeafName(info_.paths[i]);
            if (item.name.empty()) item.name = info_.paths[i];
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (GetFileAttributesExW(info_.paths[i].c_str(), GetFileExInfoStandard, &data)) {
                item.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                if (!item.directory) {
                    item.bytes = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
                    item.sized = true;
                }
            }
            item.directory ? ++folders_ : ++files_;
            total_ += item.bytes;
            if (items_.size() < 3) items_.push_back(std::move(item));
        }
        totals_known_ = stat_all;
    }

    std::wstring Headline() const {
        if (info_.paths.size() == 1 && !items_.empty())
            return l10n::Get(l10n::StringId::PermDeleteOneLead) + items_.front().name +
                   l10n::Get(l10n::StringId::PermDeleteOneEnd);
        return l10n::Get(l10n::StringId::PermDeleteManyLead) + pulse::format::GroupedInt(info_.paths.size()) +
               l10n::Get(l10n::StringId::PermDeleteManyEnd);
    }

    std::wstring Summary() const {
        std::wstring text;
        if (info_.no_recycle_bin) {
            using Reason = PermanentDeleteInfo::NoBinReason;
            text = l10n::Get(info_.no_bin_reason == Reason::Disabled   ? l10n::StringId::RecycleBinOffForDrive
                             : info_.no_bin_reason == Reason::TooLarge ? l10n::StringId::RecycleBinTooSmallForItem
                                                                       : l10n::StringId::NetworkLocationsAndRemovableDrivesHaveNoRecycleBin);
            while (!text.empty() && text.back() == L' ') text.pop_back();
        }
        std::wstring counts;
        if (totals_known_) {
            if (folders_ == 0) {
                counts = l10n::Get(l10n::StringId::PermDeleteTotalLead) + pulse::format::ByteSize(total_);
            } else if (info_.paths.size() > 1) {
                counts = pulse::format::GroupedInt(folders_) + l10n::Get(l10n::StringId::Folders);
                if (files_) counts += L" \x00B7 " + pulse::format::GroupedInt(files_) + l10n::Get(l10n::StringId::PermDeleteFiles);
            }
        }
        if (!counts.empty()) text += (text.empty() ? L"" : L" ") + counts;
        return text;
    }

    D2D1_RECT_F CloseRect() const { return Rect(scale_, kW - 46, 0, 46, 40); }
    D2D1_RECT_F DeleteRect() const { return Rect(scale_, kW - 16 - 136, kFooterTop + 13, 136, 34); }
    D2D1_RECT_F CancelRect() const { return Rect(scale_, kW - 16 - 136 - 8 - 92, kFooterTop + 13, 92, 34); }
    D2D1_RECT_F BinRect() const { return Rect(scale_, 16, kFooterTop + 13, 112, 34); }

    // Focus order: [recycle,] cancel, delete. Delete starts focused (Enter).
    int FocusCount() const { return info_.offer_recycle ? 3 : 2; }
    int FocusTarget(int focus) const { return info_.offer_recycle ? focus : focus + 1; } // 0 bin, 1 cancel, 2 delete

    int Hit(float x, float y) const {
        if (pulse::ui::ContainsRect(CloseRect(), x, y)) return 4;
        if (pulse::ui::ContainsRect(DeleteRect(), x, y)) return 3;
        if (pulse::ui::ContainsRect(CancelRect(), x, y)) return 2;
        if (info_.offer_recycle && pulse::ui::ContainsRect(BinRect(), x, y)) return 1;
        return 0;
    }

    void Complete(PermanentDeleteChoice choice) {
        choice_ = choice;
        done_ = true;
        if (hwnd_) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
    }

    void Activate(int target) {
        if (target == 0) Complete(PermanentDeleteChoice::Recycle);
        else if (target == 2) Complete(PermanentDeleteChoice::Delete);
        else Complete(PermanentDeleteChoice::Cancel);
    }

    void CreateFormats() {
        auto* factory = compositor_.DwriteFactory();
        auto make = [&](ComPtr<IDWriteTextFormat>& out, typography::FontRole role, float size, DWRITE_FONT_WEIGHT weight) {
            out.reset();
            if (!factory) return;
            typography::TextFormatSpec spec;
            spec.role = role;
            spec.size = size * scale_;
            spec.weight = weight;
            if (FAILED(typography::CreateTextFormat(factory, spec, &out)) || !out.get()) return;
            out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        };
        make(head_format_, typography::FontRole::Text, 15.5f, DWRITE_FONT_WEIGHT_BOLD);
        make(sub_format_, typography::FontRole::Text, 12.5f, DWRITE_FONT_WEIGHT_NORMAL);
        make(row_format_, typography::FontRole::Text, 12.5f, DWRITE_FONT_WEIGHT_MEDIUM);
        make(meta_format_, typography::FontRole::Text, 11.5f, DWRITE_FONT_WEIGHT_NORMAL);
        make(warn_format_, typography::FontRole::Text, 12.3f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        make(warn_sub_format_, typography::FontRole::Text, 12.3f, DWRITE_FONT_WEIGHT_NORMAL);
        make(button_format_, typography::FontRole::Text, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        make(kbd_format_, typography::FontRole::Text, 10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        make(ext_format_, typography::FontRole::Text, 9.0f, DWRITE_FONT_WEIGHT_HEAVY);
        make(glyph_format_, typography::FontRole::Icon, 18.0f, DWRITE_FONT_WEIGHT_NORMAL);
        make(small_glyph_format_, typography::FontRole::Icon, 12.0f, DWRITE_FONT_WEIGHT_NORMAL);
        make(thumb_glyph_format_, typography::FontRole::Icon, 20.0f, DWRITE_FONT_WEIGHT_NORMAL);
        for (auto* f : { glyph_format_.get(), small_glyph_format_.get(), thumb_glyph_format_.get() })
            if (f) f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // Back-out easing, close to the spec's cubic-bezier(.2, 1.3, .3, 1).
    static float Spring(float t) {
        t = (std::clamp)(t, 0.0f, 1.0f);
        const float c1 = 1.70158f * 0.9f;
        const float c3 = c1 + 1.0f;
        const float u = t - 1.0f;
        return 1.0f + c3 * u * u * u + c1 * u * u;
    }

    float FanProgress() const {
        const ULONGLONG now = GetTickCount64();
        if (now < opened_ + kFanDelay) return 0.0f;
        return Spring(static_cast<float>(now - opened_ - kFanDelay) / static_cast<float>(kFanMs));
    }

    void DrawThumb(const D2D1_RECT_F& box, const Item& item) {
        auto* dc = compositor_.Dc();
        const float s = scale_;
        D2D1_COLOR_F top{}, bottom{};
        std::wstring ext;
        ThumbColors(item.name, item.directory, top, bottom, ext);
        FillGradientTile(dc, box, ScaleDip(s, 7.0f), top, bottom);
        StrokeRoundedRect(dc, box, ScaleDip(s, 7.0f), HexColor(0xFFFFFF, 0.08f), 1.0f);
        if (item.directory) {
            painter_.DrawGlyphWithFormat(L"\xE8B7", box, HexColor(0xFFFFFF, 0.92f), thumb_glyph_format_.get());
            return;
        }
        auto factory = FactoryOf(dc);
        ComPtr<ID2D1PathGeometry> corner;
        if (factory.get() && SUCCEEDED(factory->CreatePathGeometry(&corner))) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(corner->Open(&sink))) {
                const float c = ScaleDip(s, 11.0f);
                sink->BeginFigure(D2D1::Point2F(box.right - c, box.top), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(box.right, box.top + c));
                sink->AddLine(D2D1::Point2F(box.right - c, box.top + c));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                if (SUCCEEDED(sink->Close())) {
                    ComPtr<ID2D1SolidColorBrush> brush;
                    if (SUCCEEDED(dc->CreateSolidColorBrush(HexColor(0xFFFFFF, 0.28f), &brush)))
                        dc->FillGeometry(corner.get(), brush.get());
                }
            }
        }
        if (!ext.empty()) {
            const float w = TextWidth(compositor_.DwriteFactory(), ext_format_.get(), ext) + ScaleDip(s, 8.0f);
            const D2D1_RECT_F tag = D2D1::RectF(box.left + ScaleDip(s, 5.0f), box.bottom - ScaleDip(s, 18.0f),
                                                box.left + ScaleDip(s, 5.0f) + (std::min)(w, box.right - box.left - ScaleDip(s, 8.0f)),
                                                box.bottom - ScaleDip(s, 5.0f));
            painter_.FillRoundedRect(tag, ScaleDip(s, 3.0f), HexColor(0x000000, 0.25f));
            painter_.DrawText(ext, tag, ext_format_.get(), HexColor(0xFFFFFF), fluent::HorizontalAlignment::Center);
        }
    }

    static void StrokeRoundedRect(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, float radius, D2D1_COLOR_F color, float width) {
        ComPtr<ID2D1SolidColorBrush> brush;
        if (dc && SUCCEEDED(dc->CreateSolidColorBrush(color, &brush)))
            dc->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.get(), width);
    }

    void DrawFan() {
        auto* dc = compositor_.Dc();
        const float s = scale_;
        const size_t count = (std::min)(items_.size(), static_cast<size_t>(3));
        if (count == 0) return;
        const float p = FanProgress();
        // Back to front: the spec's three cards end at -14 / -2 / +12 degrees.
        static const float kAngle3[3] = { -14.0f, -2.0f, 12.0f };
        static const float kShift3[3] = { -14.0f, 0.0f, 14.0f };
        static const float kAngle2[2] = { -9.0f, 7.0f };
        static const float kShift2[2] = { -9.0f, 9.0f };
        D2D1_MATRIX_3X2_F base{};
        dc->GetTransform(&base);
        for (size_t i = 0; i < count; ++i) {
            const float angle = count == 3 ? kAngle3[i] : count == 2 ? kAngle2[i] : 0.0f;
            const float shift = count == 3 ? kShift3[i] : count == 2 ? kShift2[i] : 0.0f;
            const float lift = count == 3 && i != 1 ? 4.0f : 0.0f;
            const auto box = Rect(s, 44, kStackTop + 12, 48, 60);
            const D2D1_POINT_2F pivot = D2D1::Point2F((box.left + box.right) * 0.5f, box.bottom);
            const D2D1_MATRIX_3X2_F transform =
                D2D1::Matrix3x2F::Rotation(angle * p, pivot) *
                D2D1::Matrix3x2F::Translation(ScaleDip(s, shift * p), ScaleDip(s, lift * p));
            dc->SetTransform(transform * base);
            // Soft shadow under each card.
            const D2D1_RECT_F shadow = D2D1::RectF(box.left + ScaleDip(s, 2.0f), box.top + ScaleDip(s, 5.0f),
                                                   box.right - ScaleDip(s, 2.0f), box.bottom + ScaleDip(s, 3.0f));
            painter_.FillRoundedRect(shadow, ScaleDip(s, 8.0f), HexColor(0x000000, dark_ ? 0.30f : 0.14f));
            DrawThumb(box, items_[i]);
        }
        dc->SetTransform(base);
    }

    void DrawFooterButton(const D2D1_RECT_F& r, const std::wstring& text, const wchar_t* key, int kind,
                          bool hovered, bool pressed, bool focused, const Theme& theme) {
        // kind: 0 ghost, 1 standard, 2 danger.
        const float s = scale_;
        const D2D1_COLOR_F bad = dark_ ? HexColor(0xF0625B) : HexColor(0xD93D36);
        D2D1_COLOR_F fill = kind == 2 ? bad
            : kind == 1 ? (dark_ ? HexColor(0xFFFFFF, hovered ? 0.12f : 0.08f) : HexColor(0xFFFFFF, hovered ? 1.0f : 0.85f))
                        : (hovered ? (dark_ ? HexColor(0xFFFFFF, 0.07f) : HexColor(0x000000, 0.05f)) : HexColor(0x000000, 0.0f));
        if (kind == 2 && hovered) fill = D2D1::ColorF((std::min)(1.0f, fill.r * 1.08f), (std::min)(1.0f, fill.g * 1.08f),
                                                      (std::min)(1.0f, fill.b * 1.08f), 1.0f);
        D2D1_RECT_F box = r;
        if (pressed) box = D2D1::RectF(r.left + s, r.top + s, r.right - s, r.bottom - s);
        if (focused) {
            const float g = ScaleDip(s, 2.0f);
            StrokeRoundedRect(compositor_.Dc(), D2D1::RectF(box.left - g, box.top - g, box.right + g, box.bottom + g),
                              ScaleDip(s, 10.0f), kind == 2 ? WithAlpha(bad, 0.6f) : (dark_ ? HexColor(0x8F9CF0) : theme.accent),
                              ScaleDip(s, 2.0f));
        }
        painter_.FillRoundedRect(box, ScaleDip(s, 8.0f), fill);
        if (kind == 1)
            StrokeRoundedRect(compositor_.Dc(), box, ScaleDip(s, 8.0f),
                              dark_ ? HexColor(0xFFFFFF, 0.08f) : HexColor(0x000000, 0.10f), 1.0f);
        const D2D1_COLOR_F ink = kind == 2 ? HexColor(0xFFFFFF) : theme.text;
        const float text_w = TextWidth(compositor_.DwriteFactory(), button_format_.get(), text);
        float key_w = 0.0f;
        if (key) key_w = TextWidth(compositor_.DwriteFactory(), kbd_format_.get(), key) + ScaleDip(s, 10.0f);
        const float gap = key ? ScaleDip(s, 7.0f) : 0.0f;
        const float left = (box.left + box.right - text_w - gap - key_w) * 0.5f;
        painter_.DrawText(text, D2D1::RectF(left, box.top, left + text_w + 2.0f, box.bottom), button_format_.get(), ink);
        if (key) {
            const float cy = (box.top + box.bottom) * 0.5f;
            const D2D1_RECT_F chip = D2D1::RectF(left + text_w + gap, cy - ScaleDip(s, 8.5f),
                                                 left + text_w + gap + key_w, cy + ScaleDip(s, 8.5f));
            painter_.FillRoundedRect(chip, ScaleDip(s, 5.0f),
                kind == 2 ? HexColor(0xFFFFFF, 0.18f) : (dark_ ? HexColor(0xFFFFFF, 0.08f) : HexColor(0x000000, 0.06f)));
            painter_.DrawText(key, chip, kbd_format_.get(), kind == 2 ? HexColor(0xFFFFFF) : theme.text_secondary,
                              fluent::HorizontalAlignment::Center);
        }
    }

    void Render() {
        if (compositor_.NeedsRecovery()) {
            if (!compositor_.Recover()) return;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            CreateFormats();
        }
        if (!compositor_.Dc()) return;
        if (!head_format_.get()) CreateFormats();
        const float s = scale_;
        const bool high_contrast = IsHighContrast();
        const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        pulse::ui::BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_enabled_, s);
        const D2D1_COLOR_F bad = dark_ ? HexColor(0xF0625B) : HexColor(0xD93D36);
        const D2D1_COLOR_F faint = WithAlpha(theme.text, dark_ ? 0.42f : 0.48f);

        painter_.DrawText(l10n::Get(l10n::StringId::PermanentDelete), Rect(s, 16, 0, 300, 40),
                          compositor_.SmallFormat(), theme.text_secondary);
        fluent::ControlState close_state{};
        close_state.hovered = hover_ == 4;
        close_state.pressed = pressed_ == 4;
        painter_.DrawTitleBarButton(CloseRect(), fluent::TitleBarButtonRole::Close, {}, close_state);

        const auto tile = Rect(s, 20, 46, 38, 38);
        painter_.FillRoundedRect(tile, ScaleDip(s, 11.0f), WithAlpha(bad, dark_ ? 0.16f : 0.12f));
        painter_.DrawGlyphWithFormat(L"\xE74D", tile, bad, glyph_format_.get());
        DrawEllipsizedText(compositor_, head_format_.get(), Rect(s, 70, 44, kW - 90, 24), Headline(), theme.text);
        DrawEllipsizedText(compositor_, sub_format_.get(), Rect(s, 70, 67, kW - 90, 18), Summary(), theme.text_secondary);

        DrawFan();
        // The first three, then "and N more".
        const size_t rows = items_.size() + (info_.paths.size() > items_.size() ? 1 : 0);
        float y = kStackTop + (84.0f - rows * 21.0f) * 0.5f;
        for (const Item& item : items_) {
            painter_.DrawGlyphWithFormat(item.directory ? L"\xE8B7" : L"\xE8A5", Rect(s, 130, y, 14, 21), item.directory ? HexColor(0xE9B949) : theme.text_secondary, small_glyph_format_.get());
            const std::wstring size = item.directory ? l10n::Get(l10n::StringId::Folder)
                                    : item.sized ? pulse::format::ByteSize(item.bytes) : std::wstring();
            const float size_w = size.empty() ? 0.0f : TextWidth(compositor_.DwriteFactory(), meta_format_.get(), size) / s;
            DrawEllipsizedText(compositor_, row_format_.get(), Rect(s, 152, y, kW - 20 - 152 - size_w - 10, 21), item.name, theme.text);
            if (!size.empty())
                painter_.DrawText(size, Rect(s, kW - 20 - size_w - 1, y, size_w + 2, 21), meta_format_.get(), faint);
            y += 21.0f;
        }
        if (info_.paths.size() > items_.size()) {
            painter_.DrawText(l10n::Get(l10n::StringId::PermDeleteMoreLead) +
                                  pulse::format::GroupedInt(info_.paths.size() - items_.size()) +
                                  l10n::Get(l10n::StringId::PermDeleteMoreEnd),
                              Rect(s, 152, y, kW - 172, 21), sub_format_.get(), theme.text_secondary);
        }

        const auto warn = Rect(s, 20, kWarnTop, kW - 40, 36);
        painter_.FillRoundedRect(warn, ScaleDip(s, 9.0f), WithAlpha(bad, dark_ ? 0.14f : 0.10f));
        painter_.DrawGlyphWithFormat(L"\xE7BA", Rect(s, 30, kWarnTop, 18, 36), bad, small_glyph_format_.get());
        const std::wstring warn_text = l10n::Get(l10n::StringId::PermDeleteWarn);
        const float warn_w = TextWidth(compositor_.DwriteFactory(), warn_format_.get(), warn_text) / s;
        painter_.DrawText(warn_text, Rect(s, 54, kWarnTop, warn_w + 2, 36), warn_format_.get(), bad);
        if (info_.offer_recycle)
            DrawEllipsizedText(compositor_, warn_sub_format_.get(), Rect(s, 54 + warn_w + 2, kWarnTop, kW - 40 - 46 - warn_w, 36),
                               l10n::Get(l10n::StringId::PermDeleteWarnAlt), theme.text_secondary);

        RECT client{};
        GetClientRect(hwnd_, &client);
        const float footer = kFooterTop * s;
        painter_.FillRoundedRect(D2D1::RectF(0, footer, static_cast<float>(client.right), static_cast<float>(client.bottom)), 0,
                                 OpFooter(hwnd_, backdrop_enabled_, dark_));
        painter_.FillRoundedRect(D2D1::RectF(0, footer, static_cast<float>(client.right), footer + 1.0f), 0, theme.stroke_divider);
        const int focus_target = keyboard_focus_ || focus_ == FocusCount() - 1 ? FocusTarget(focus_) : -1;
        if (info_.offer_recycle)
            DrawFooterButton(BinRect(), l10n::Get(l10n::StringId::PermDeleteToBin), nullptr, 0, hover_ == 1, pressed_ == 1,
                             focus_target == 0, theme);
        DrawFooterButton(CancelRect(), l10n::Get(l10n::StringId::Cancel), L"Esc", 1, hover_ == 2, pressed_ == 2,
                         focus_target == 1, theme);
        DrawFooterButton(DeleteRect(), l10n::Get(l10n::StringId::PermanentDelete), L"Enter", 2, hover_ == 3, pressed_ == 3,
                         focus_target == 2, theme);
        pulse::ui::EndSurface(compositor_);
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            scale_ = static_cast<float>(pulse::compat::WindowDpi(hwnd_)) / 96.0f;
            backdrop_enabled_ = pulse::ui::ApplyBackdrop(hwnd_, dark_);
            if (!compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            CreateFormats();
            ImmAssociateContextEx(hwnd_, nullptr, 0);
            focus_ = FocusCount() - 1; // the delete button, so Enter deletes
            return 0;
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST:
            return pulse::ui::BorderlessHitTest(hwnd_, lparam, 40 * scale_, CloseRect());
        case WM_SIZE:
            if (compositor_.Dc()) compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOVE:
            compositor_.UpdateTextRenderingParams(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
            return 0;
        case WM_DPICHANGED: {
            scale_ = HIWORD(wparam) / 96.0f;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetScale(scale_);
            CreateFormats();
            const auto* suggested = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOOWNERZORDER);
            return 0;
        }
        case WM_TIMER:
            if (wparam == kFanTimer) {
                if (GetTickCount64() > opened_ + kFanDelay + kFanMs) KillTimer(hwnd_, kFanTimer);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_MOUSEMOVE: {
            const int next = Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            if (next != hover_) { hover_ = next; InvalidateRect(hwnd_, nullptr, FALSE); }
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_ = 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            pressed_ = Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            const int hit = Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            const int pressed = pressed_;
            pressed_ = 0;
            ReleaseCapture();
            if (hit == pressed && hit != 0) {
                if (hit == 1) Complete(PermanentDeleteChoice::Recycle);
                else if (hit == 3) Complete(PermanentDeleteChoice::Delete);
                else Complete(PermanentDeleteChoice::Cancel);
                return 0;
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wparam) != WA_INACTIVE) SetFocus(hwnd_);
            return 0;
        case WM_SYSKEYDOWN:
            if (!(lparam & (1 << 29)) && wparam != VK_F10) return Handle(WM_KEYDOWN, wparam, lparam);
            break;
        case WM_KEYDOWN:
            if (wparam == VK_PROCESSKEY) {
                const UINT vk = ImmGetVirtualKey(hwnd_);
                if (vk && vk != VK_PROCESSKEY) wparam = vk;
            }
            if (wparam == VK_ESCAPE) { Complete(PermanentDeleteChoice::Cancel); return 0; }
            if (wparam == VK_RETURN || wparam == VK_SPACE) { Activate(FocusTarget(focus_)); return 0; }
            if (wparam == VK_TAB || wparam == VK_LEFT || wparam == VK_RIGHT) {
                const bool back = wparam == VK_LEFT || (wparam == VK_TAB && GetKeyState(VK_SHIFT) < 0);
                focus_ = (focus_ + (back ? FocusCount() - 1 : 1)) % FocusCount();
                keyboard_focus_ = true;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_CLOSE:
            Complete(PermanentDeleteChoice::Cancel);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd_, &paint);
            Render();
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            KillTimer(hwnd_, kFanTimer);
            compositor_.Shutdown();
            done_ = true;
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_{ &compositor_ };
    PermanentDeleteInfo info_;
    std::vector<Item> items_;
    size_t files_ = 0;
    size_t folders_ = 0;
    uint64_t total_ = 0;
    bool totals_known_ = false;
    PermanentDeleteChoice choice_ = PermanentDeleteChoice::Cancel;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    float scale_ = 1.0f;
    bool dark_ = false;
    bool backdrop_enabled_ = false;
    bool done_ = false;
    bool keyboard_focus_ = false;
    int hover_ = 0;
    int pressed_ = 0;
    int focus_ = 0;
    ULONGLONG opened_ = 0;
    ComPtr<IDWriteTextFormat> head_format_;
    ComPtr<IDWriteTextFormat> sub_format_;
    ComPtr<IDWriteTextFormat> row_format_;
    ComPtr<IDWriteTextFormat> meta_format_;
    ComPtr<IDWriteTextFormat> warn_format_;
    ComPtr<IDWriteTextFormat> warn_sub_format_;
    ComPtr<IDWriteTextFormat> button_format_;
    ComPtr<IDWriteTextFormat> kbd_format_;
    ComPtr<IDWriteTextFormat> ext_format_;
    ComPtr<IDWriteTextFormat> glyph_format_;
    ComPtr<IDWriteTextFormat> small_glyph_format_;
    ComPtr<IDWriteTextFormat> thumb_glyph_format_;
};

// File in use, pulse_ops_dialogs.html §5: one row per program holding the
// item (icon, name, image · PID, 切换到程序). The dialog watches the programs
// and the file; once the file is free it says so and continues on its own,
// no retry click needed. 结束进程并重试 when every owner may be ended.
class LockedItemWindow {
public:
    LockedItemChoice Show(HWND owner, const LockedItemInfo& info, bool dark, D2D1_COLOR_F accent) {
        owner_ = owner;
        info_ = info;
        dark_ = dark;
        accent_ = accent;
        scale_ = static_cast<float>(pulse::compat::WindowDpi(owner)) / 96.0f;
        gone_.assign(info_.owners.size(), false);
        windows_.assign(info_.owners.size(), nullptr);
        icons_.assign(info_.owners.size(), nullptr);
        bitmaps_.resize(info_.owners.size());
        LoadIcons();
        FindOwnerWindows();
        focus_ = DefaultFocus();

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kLockedClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!GetClassInfoExW(wc.hInstance, kLockedClass, &wc)) RegisterClassExW(&wc);

        const int width = static_cast<int>(kW * scale_);
        const int height = static_cast<int>(HeightDip() * scale_);
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kLockedClass,
            l10n::Get(l10n::StringId::LockedItemTitle).c_str(), WS_POPUP | WS_THICKFRAME | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, width, height, owner, nullptr, wc.hInstance, this);
        if (hwnd_) {
            pulse::ui::CenterOwnedWindow(hwnd_, owner_, width, height);
            if (owner_) EnableWindow(owner_, FALSE);
            ShowWindow(hwnd_, SW_SHOW);
            SetForegroundWindow(hwnd_);
            SetFocus(hwnd_);
            StartWatcher();
            SetTimer(hwnd_, kPingTimer, 50, nullptr);
            MSG message{};
            while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (IsWindow(hwnd_)) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
            hwnd_ = nullptr;
            if (owner_) EnableWindow(owner_, TRUE);
        }
        StopWatcher();
        for (HICON icon : icons_) if (icon) DestroyIcon(icon);
        return choice_;
    }

private:
    static constexpr float kW = 500.0f;
    static constexpr float kProcsTop = 100.0f;
    static constexpr float kRowH = 50.0f;
    static constexpr float kMoreH = 30.0f;
    static constexpr size_t kMaxRows = 4;
    static constexpr UINT_PTR kPingTimer = 4;
    static constexpr UINT_PTR kContinueTimer = 5;
    static constexpr UINT kWatchMessage = WM_APP + 41;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<LockedItemWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<LockedItemWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        return self ? self->Handle(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    // ---- layout ---------------------------------------------------------
    size_t Rows() const { return (std::min)(info_.owners.size(), kMaxRows); }
    bool HasMore() const { return info_.owners.size() > kMaxRows; }
    float ProcsH() const { return (std::max)(Rows(), static_cast<size_t>(1)) * kRowH + (HasMore() ? kMoreH : 0.0f); }
    float WatchTop() const { return kProcsTop + ProcsH() + 12.0f; }
    float ContentBottom() const { return WatchTop() + 20.0f + (info_.can_end ? 22.0f : 0.0f); }
    float FooterTop() const { return ContentBottom() + 14.0f; }
    float HeightDip() const { return FooterTop() + 60.0f; }
    D2D1_RECT_F CloseRect() const { return Rect(scale_, kW - 46, 0, 46, 40); }
    float PrimaryW() const { return info_.can_end ? 150.0f : 96.0f; }
    D2D1_RECT_F PrimaryRect() const { return Rect(scale_, kW - 16 - PrimaryW(), FooterTop() + 13, PrimaryW(), 34); }
    D2D1_RECT_F CancelRect() const { return Rect(scale_, kW - 16 - PrimaryW() - 8 - 92, FooterTop() + 13, 92, 34); }
    D2D1_RECT_F SkipRect() const { return Rect(scale_, 16, FooterTop() + 13, 104, 34); }
    D2D1_RECT_F SwitchRect(size_t row) const {
        return Rect(scale_, kW - 20 - 12 - 92, kProcsTop + row * kRowH + 12, 92, 26);
    }

    // Focus order: [skip,] cancel, primary.
    int FocusCount() const { return info_.can_skip ? 3 : 2; }
    int FocusTarget(int focus) const { return info_.can_skip ? focus : focus + 1; } // 0 skip, 1 cancel, 2 primary
    // Ending processes loses unsaved work: Enter must not do that by default.
    int DefaultFocus() const { return info_.can_end ? FocusCount() - 2 : FocusCount() - 1; }

    int Hit(float x, float y) const {
        if (pulse::ui::ContainsRect(CloseRect(), x, y)) return 4;
        if (pulse::ui::ContainsRect(PrimaryRect(), x, y)) return 3;
        if (pulse::ui::ContainsRect(CancelRect(), x, y)) return 2;
        if (info_.can_skip && pulse::ui::ContainsRect(SkipRect(), x, y)) return 1;
        for (size_t i = 0; i < Rows(); ++i)
            if (windows_[i] && !gone_[i] && pulse::ui::ContainsRect(SwitchRect(i), x, y)) return 10 + static_cast<int>(i);
        return 0;
    }

    void Complete(LockedItemChoice choice) {
        choice_ = choice;
        done_ = true;
        if (hwnd_) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
    }

    void Activate(int target) {
        if (target == 0) Complete(LockedItemChoice::Skip);
        else if (target == 2) Complete(info_.can_end ? LockedItemChoice::EndAndRetry : LockedItemChoice::Retry);
        else Complete(LockedItemChoice::Cancel);
    }

    // ---- owners -----------------------------------------------------------
    struct FindWindowContext { DWORD pid; HWND found; };
    static BOOL CALLBACK FindOwnerWindow(HWND hwnd, LPARAM param) {
        auto* context = reinterpret_cast<FindWindowContext*>(param);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != context->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
        if (GetWindowTextLengthW(hwnd) == 0) return TRUE;
        context->found = hwnd;
        return FALSE;
    }

    void FindOwnerWindows() {
        for (size_t i = 0; i < info_.owners.size(); ++i) {
            FindWindowContext context{ info_.owners[i].pid, nullptr };
            if (!gone_[i] && context.pid) EnumWindows(FindOwnerWindow, reinterpret_cast<LPARAM>(&context));
            windows_[i] = context.found;
        }
    }

    void SwitchTo(size_t index) {
        HWND target = index < windows_.size() ? windows_[index] : nullptr;
        if (!target || !IsWindow(target)) return;
        if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
        AllowSetForegroundWindow(info_.owners[index].pid);
        SetForegroundWindow(target);
    }

    void LoadIcons() {
        const int size = static_cast<int>(std::lround(20.0f * scale_));
        for (size_t i = 0; i < info_.owners.size() && i < kMaxRows; ++i) {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, info_.owners[i].pid);
            if (!process) continue;
            wchar_t image[MAX_PATH * 2]{};
            DWORD length = ARRAYSIZE(image);
            if (QueryFullProcessImageNameW(process, 0, image, &length)) {
                HICON icon = nullptr;
                UINT id = 0;
                if (PrivateExtractIconsW(image, 0, size, size, &icon, &id, 1, 0) == 1 && icon) icons_[i] = icon;
            }
            CloseHandle(process);
        }
    }

    ID2D1Bitmap* IconBitmap(size_t index) {
        if (index >= icons_.size() || !icons_[index]) return nullptr;
        if (bitmaps_[index].get()) return bitmaps_[index].get();
        if (!wic_.get() && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                                   IID_PPV_ARGS(&wic_)))) {
            icons_[index] = nullptr;
            return nullptr;
        }
        ComPtr<IWICBitmap> source;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(wic_->CreateBitmapFromHICON(icons_[index], &source)) ||
            FAILED(wic_->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(source.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                         nullptr, 0.0, WICBitmapPaletteTypeMedianCut)) ||
            FAILED(compositor_.Dc()->CreateBitmapFromWicBitmap(converter.get(), nullptr, &bitmaps_[index]))) {
            return nullptr;
        }
        return bitmaps_[index].get();
    }

    // ---- watching ---------------------------------------------------------
    // A worker polls every 500 ms: has each owner exited (same instance), and
    // can the item be opened with no sharing at all (nobody holds it)? Two
    // free reads in a row count as released; folders rely on the owners.
    static bool OwnerGone(const ops::LockOwner& owner) {
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, owner.pid);
        if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
        bool gone = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
        if (!gone && (owner.start_time.dwLowDateTime | owner.start_time.dwHighDateTime)) {
            FILETIME created{}, exited{}, kernel{}, user{};
            if (GetProcessTimes(process, &created, &exited, &kernel, &user))
                gone = CompareFileTime(&created, &owner.start_time) != 0; // the PID was reused
        }
        CloseHandle(process);
        return gone;
    }

    static bool ItemFree(const std::wstring& path) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) return false;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        CloseHandle(file);
        return true;
    }

    void StartWatcher() {
        stop_ = false;
        const HWND target = hwnd_;
        const std::vector<ops::LockOwner> owners = info_.owners;
        const std::wstring path = info_.path;
        watcher_ = std::thread([this, target, owners, path] {
            int free_reads = 0;
            while (!stop_.load()) {
                uint64_t mask = 0;
                bool all_gone = !owners.empty();
                for (size_t i = 0; i < owners.size(); ++i) {
                    const bool gone = OwnerGone(owners[i]);
                    if (gone && i < 64) mask |= 1ull << i;
                    all_gone = all_gone && gone;
                }
                free_reads = !path.empty() && ItemFree(path) ? free_reads + 1 : 0;
                const bool released = all_gone || free_reads >= 2;
                watch_mask_.store(mask);
                PostMessageW(target, kWatchMessage, released ? 1 : 0, 0);
                for (int slice = 0; slice < 10 && !stop_.load(); ++slice) Sleep(50);
            }
        });
    }

    void StopWatcher() {
        stop_ = true;
        if (watcher_.joinable()) watcher_.join();
    }

    void OnWatch(bool released) {
        const uint64_t mask = watch_mask_.load();
        bool changed = false;
        for (size_t i = 0; i < gone_.size() && i < 64; ++i) {
            const bool gone = (mask >> i) & 1u;
            if (gone && !gone_[i]) { gone_[i] = true; changed = true; }
        }
        if (changed) FindOwnerWindows();
        if (released && !released_) {
            released_ = true;
            released_at_ = GetTickCount64();
            // Every row reads as closed once the file is free.
            for (size_t i = 0; i < gone_.size(); ++i) gone_[i] = true;
            SetTimer(hwnd_, kContinueTimer, 900, nullptr);
            changed = true;
        }
        if (changed) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // ---- drawing ----------------------------------------------------------
    void CreateFormats() {
        auto* factory = compositor_.DwriteFactory();
        auto make = [&](ComPtr<IDWriteTextFormat>& out, typography::FontRole role, float size, DWRITE_FONT_WEIGHT weight) {
            out.reset();
            if (!factory) return;
            typography::TextFormatSpec spec;
            spec.role = role;
            spec.size = size * scale_;
            spec.weight = weight;
            if (FAILED(typography::CreateTextFormat(factory, spec, &out)) || !out.get()) return;
            out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        };
        make(head_format_, typography::FontRole::Text, 15.5f, DWRITE_FONT_WEIGHT_BOLD);
        make(sub_format_, typography::FontRole::Text, 12.5f, DWRITE_FONT_WEIGHT_NORMAL);
        make(name_format_, typography::FontRole::Text, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        make(meta_format_, typography::FontRole::Text, 11.5f, DWRITE_FONT_WEIGHT_NORMAL);
        make(watch_format_, typography::FontRole::Text, 12.3f, DWRITE_FONT_WEIGHT_NORMAL);
        make(tag_format_, typography::FontRole::Text, 12.0f, DWRITE_FONT_WEIGHT_BOLD);
        make(small_button_format_, typography::FontRole::Text, 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        make(button_format_, typography::FontRole::Text, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        make(glyph_format_, typography::FontRole::Icon, 18.0f, DWRITE_FONT_WEIGHT_NORMAL);
        make(app_glyph_format_, typography::FontRole::Icon, 15.0f, DWRITE_FONT_WEIGHT_NORMAL);
        for (auto* f : { glyph_format_.get(), app_glyph_format_.get(), small_button_format_.get() })
            if (f) f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    static void Stroke(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, float radius, D2D1_COLOR_F color, float width) {
        ComPtr<ID2D1SolidColorBrush> brush;
        if (dc && SUCCEEDED(dc->CreateSolidColorBrush(color, &brush)))
            dc->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.get(), width);
    }

    void DrawFooterButton(const D2D1_RECT_F& r, const std::wstring& text, int kind, bool hovered, bool pressed,
                          bool focused, const Theme& theme) {
        // kind: 0 ghost, 1 standard, 2 danger, 3 primary.
        const float s = scale_;
        const D2D1_COLOR_F bad = dark_ ? HexColor(0xF0625B) : HexColor(0xD93D36);
        const D2D1_COLOR_F acc = dark_ ? HexColor(0x6474C7) : HexColor(0x5566C4);
        D2D1_COLOR_F fill = kind == 2 ? bad : kind == 3 ? acc
            : kind == 1 ? (dark_ ? HexColor(0xFFFFFF, hovered ? 0.12f : 0.08f) : HexColor(0xFFFFFF, hovered ? 1.0f : 0.85f))
                        : (hovered ? (dark_ ? HexColor(0xFFFFFF, 0.07f) : HexColor(0x000000, 0.05f)) : HexColor(0x000000, 0.0f));
        if (kind >= 2 && hovered)
            fill = D2D1::ColorF((std::min)(1.0f, fill.r * 1.08f), (std::min)(1.0f, fill.g * 1.08f), (std::min)(1.0f, fill.b * 1.08f), 1.0f);
        D2D1_RECT_F box = r;
        if (pressed) box = D2D1::RectF(r.left + s, r.top + s, r.right - s, r.bottom - s);
        if (focused) {
            const float g = ScaleDip(s, 2.0f);
            Stroke(compositor_.Dc(), D2D1::RectF(box.left - g, box.top - g, box.right + g, box.bottom + g), ScaleDip(s, 10.0f),
                   kind == 2 ? WithAlpha(bad, 0.6f) : (dark_ ? HexColor(0x8F9CF0) : theme.accent), ScaleDip(s, 2.0f));
        }
        painter_.FillRoundedRect(box, ScaleDip(s, 8.0f), fill);
        if (kind == 1)
            Stroke(compositor_.Dc(), box, ScaleDip(s, 8.0f), dark_ ? HexColor(0xFFFFFF, 0.08f) : HexColor(0x000000, 0.10f), 1.0f);
        painter_.DrawText(text, box, button_format_.get(), kind >= 2 ? HexColor(0xFFFFFF) : theme.text,
                          fluent::HorizontalAlignment::Center);
    }

    std::wstring Verb() const {
        switch (info_.type) {
        case ops::OpType::RealDelete: return l10n::Get(l10n::StringId::LockVerbDelete);
        case ops::OpType::Move: return l10n::Get(l10n::StringId::LockVerbMove);
        case ops::OpType::Copy: return l10n::Get(l10n::StringId::LockVerbCopy);
        case ops::OpType::Rename:
        case ops::OpType::BatchRename: return l10n::Get(l10n::StringId::LockVerbRename);
        default: return l10n::Get(l10n::StringId::LockVerbRecycle);
        }
    }

    void Render() {
        if (compositor_.NeedsRecovery()) {
            if (!compositor_.Recover()) return;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            CreateFormats();
            for (auto& bitmap : bitmaps_) bitmap.reset();
        }
        if (!compositor_.Dc()) return;
        if (!head_format_.get()) CreateFormats();
        auto* dc = compositor_.Dc();
        const float s = scale_;
        const bool high_contrast = IsHighContrast();
        const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        pulse::ui::BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_enabled_, s);
        const D2D1_COLOR_F warn = dark_ ? HexColor(0xF0B041) : HexColor(0xC98410);
        const D2D1_COLOR_F ok = dark_ ? HexColor(0x3CCF8E) : HexColor(0x16A36A);
        const D2D1_COLOR_F faint = WithAlpha(theme.text, dark_ ? 0.42f : 0.48f);
        const D2D1_COLOR_F line = dark_ ? HexColor(0xFFFFFF, 0.07f) : HexColor(0x000000, 0.07f);

        painter_.DrawText(l10n::Get(l10n::StringId::LockedItemTitle), Rect(s, 16, 0, 300, 40),
                          compositor_.SmallFormat(), theme.text_secondary);
        fluent::ControlState close_state{};
        close_state.hovered = hover_ == 4;
        close_state.pressed = pressed_ == 4;
        painter_.DrawTitleBarButton(CloseRect(), fluent::TitleBarButtonRole::Close, {}, close_state);

        const auto tile = Rect(s, 20, 46, 38, 38);
        painter_.FillRoundedRect(tile, ScaleDip(s, 11.0f), WithAlpha(warn, dark_ ? 0.16f : 0.13f));
        painter_.DrawGlyphWithFormat(L"\xE72E", tile, warn, glyph_format_.get());
        DrawEllipsizedText(compositor_, head_format_.get(), Rect(s, 70, 44, kW - 90, 24),
                           l10n::Get(l10n::StringId::LockHeadline), theme.text);
        DrawEllipsizedText(compositor_, sub_format_.get(), Rect(s, 70, 67, kW - 90, 18),
                           l10n::Get(l10n::StringId::LockSubLead) + LeafName(info_.path) +
                               l10n::Get(l10n::StringId::LockSubMid) + Verb() + l10n::Get(l10n::StringId::LockSubEnd),
                           theme.text_secondary);

        // Programs.
        const auto box = Rect(s, 20, kProcsTop, kW - 40, ProcsH());
        painter_.FillRoundedRect(box, ScaleDip(s, 11.0f), OpWell(hwnd_, backdrop_enabled_, dark_));
        for (size_t i = 0; i < Rows(); ++i) {
            const ops::LockOwner& owner = info_.owners[i];
            const float y = kProcsTop + i * kRowH;
            if (gone_[i]) {
                // Square inside the list, rounded where the row meets the list's corners.
                const bool first = i == 0, last = i + 1 == Rows() && Rows() == info_.owners.size();
                const auto row = Rect(s, 20, y, kW - 40, kRowH);
                const float grow = first == last ? 0.0f : ScaleDip(s, 12.0f);
                auto fill = row;
                if (first && !last) fill.bottom += grow;
                if (last && !first) fill.top -= grow;
                dc->PushAxisAlignedClip(row, D2D1_ANTIALIAS_MODE_ALIASED);
                painter_.FillRoundedRect(fill, first || last ? ScaleDip(s, 11.0f) : 0.0f,
                                         WithAlpha(ok, dark_ ? 0.12f : 0.10f));
                dc->PopAxisAlignedClip();
            }
            if (i > 0) painter_.FillRoundedRect(Rect(s, 20, y, kW - 40, 1.0f / s), 0, line);
            const auto icon = Rect(s, 32, y + 10, 30, 30);
            painter_.FillRoundedRect(icon, ScaleDip(s, 8.0f), dark_ ? HexColor(0xFFFFFF, 0.07f) : HexColor(0x000000, 0.05f));
            if (ID2D1Bitmap* bitmap = IconBitmap(i)) {
                const float half = ScaleDip(s, 10.0f);
                const float cx = (icon.left + icon.right) * 0.5f;
                const float cy = (icon.top + icon.bottom) * 0.5f;
                dc->DrawBitmap(bitmap, D2D1::RectF(cx - half, cy - half, cx + half, cy + half), gone_[i] ? 0.55f : 1.0f);
            } else {
                painter_.DrawGlyphWithFormat(L"\xECAA", icon, theme.text_secondary, app_glyph_format_.get());
            }
            const std::wstring name = !owner.app_name.empty() ? owner.app_name
                                    : !owner.image_name.empty() ? owner.image_name : L"PID " + std::to_wstring(owner.pid);
            const float right = gone_[i] ? kW - 20 - 12 - 80 : (windows_[i] ? kW - 20 - 12 - 92 - 8 : kW - 32);
            const float name_w = (std::min)(right - 74, TextWidth(compositor_.DwriteFactory(), name_format_.get(), name) / s + 2);
            DrawEllipsizedText(compositor_, name_format_.get(), Rect(s, 74, y + 8, name_w, 18), name,
                               gone_[i] ? theme.text_secondary : theme.text);
            if (gone_[i]) // struck through
                painter_.FillRoundedRect(Rect(s, 74, y + 17, name_w - 2, 1.2f), 0, ok);
            std::wstring meta = owner.image_name.empty() ? std::wstring() : owner.image_name + L" \x00B7 ";
            meta += L"PID " + std::to_wstring(owner.pid);
            DrawEllipsizedText(compositor_, meta_format_.get(), Rect(s, 74, y + 27, right - 74, 16), meta, faint);
            if (gone_[i]) {
                painter_.DrawText(l10n::Get(l10n::StringId::LockClosed), Rect(s, kW - 20 - 12 - 80, y, 80, kRowH),
                                  tag_format_.get(), ok, fluent::HorizontalAlignment::Right);
            } else if (windows_[i]) {
                const auto button = SwitchRect(i);
                const bool hovered = hover_ == 10 + static_cast<int>(i);
                painter_.FillRoundedRect(button, ScaleDip(s, 7.0f),
                    dark_ ? HexColor(0xFFFFFF, hovered ? 0.13f : 0.08f) : HexColor(0xFFFFFF, hovered ? 1.0f : 0.85f));
                Stroke(dc, button, ScaleDip(s, 7.0f), dark_ ? HexColor(0xFFFFFF, 0.08f) : HexColor(0x000000, 0.10f), 1.0f);
                painter_.DrawText(l10n::Get(l10n::StringId::LockSwitchTo), button, small_button_format_.get(), theme.text,
                                  fluent::HorizontalAlignment::Center);
            }
        }
        if (HasMore()) {
            const float y = kProcsTop + Rows() * kRowH;
            painter_.FillRoundedRect(Rect(s, 20, y, kW - 40, 1.0f / s), 0, line);
            painter_.DrawText(l10n::Get(l10n::StringId::LockMoreLead) +
                                  pulse::format::GroupedInt(info_.owners.size() - Rows()) +
                                  l10n::Get(l10n::StringId::LockMoreEnd),
                              Rect(s, 74, y, kW - 110, kMoreH), sub_format_.get(), theme.text_secondary);
        }

        // Watching: a pinging dot, amber until the file is free, then green.
        const float wy = WatchTop();
        const D2D1_COLOR_F dot = released_ ? ok : warn;
        const D2D1_POINT_2F center = D2D1::Point2F(ScaleDip(s, 24.0f), ScaleDip(s, wy + 10.0f));
        {
            ComPtr<ID2D1SolidColorBrush> brush;
            if (SUCCEEDED(dc->CreateSolidColorBrush(dot, &brush))) {
                dc->FillEllipse(D2D1::Ellipse(center, ScaleDip(s, 4.0f), ScaleDip(s, 4.0f)), brush.get());
                const float phase = static_cast<float>(GetTickCount64() % 1600) / 1600.0f;
                const float radius = ScaleDip(s, 8.0f * (0.5f + 1.1f * phase)); // CSS ping: scale .5 → 1.6 of 16 px
                brush->SetColor(WithAlpha(dot, 0.8f * (1.0f - phase)));
                dc->DrawEllipse(D2D1::Ellipse(center, radius, radius), brush.get(), ScaleDip(s, 2.0f) * (1.0f - 0.5f * phase));
            }
        }
        painter_.DrawText(l10n::Get(released_ ? l10n::StringId::LockReleased : l10n::StringId::LockWatch),
                          Rect(s, 37, wy, kW - 57, 20), watch_format_.get(), released_ ? ok : theme.text_secondary);
        if (info_.can_end)
            DrawEllipsizedText(compositor_, meta_format_.get(), Rect(s, 37, wy + 22, kW - 57, 18),
                               l10n::Get(l10n::StringId::LockedItemEndHint), faint);

        RECT client{};
        GetClientRect(hwnd_, &client);
        const float footer = FooterTop() * s;
        painter_.FillRoundedRect(D2D1::RectF(0, footer, static_cast<float>(client.right), static_cast<float>(client.bottom)), 0,
                                 OpFooter(hwnd_, backdrop_enabled_, dark_));
        painter_.FillRoundedRect(D2D1::RectF(0, footer, static_cast<float>(client.right), footer + 1.0f), 0, theme.stroke_divider);
        const int focus_target = FocusTarget(focus_);
        if (info_.can_skip)
            DrawFooterButton(SkipRect(), l10n::Get(l10n::StringId::LockSkip), 0, hover_ == 1, pressed_ == 1,
                             focus_target == 0, theme);
        DrawFooterButton(CancelRect(), l10n::Get(l10n::StringId::Cancel), 1, hover_ == 2, pressed_ == 2,
                         focus_target == 1, theme);
        DrawFooterButton(PrimaryRect(),
                         l10n::Get(info_.can_end ? l10n::StringId::LockedItemEndRetry : l10n::StringId::LockedItemRetry),
                         info_.can_end ? 2 : 3, hover_ == 3, pressed_ == 3, focus_target == 2, theme);
        pulse::ui::EndSurface(compositor_);
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            scale_ = static_cast<float>(pulse::compat::WindowDpi(hwnd_)) / 96.0f;
            backdrop_enabled_ = pulse::ui::ApplyBackdrop(hwnd_, dark_);
            if (!compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            CreateFormats();
            ImmAssociateContextEx(hwnd_, nullptr, 0);
            return 0;
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST:
            return pulse::ui::BorderlessHitTest(hwnd_, lparam, 40 * scale_, CloseRect());
        case WM_SIZE:
            if (compositor_.Dc()) compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOVE:
            compositor_.UpdateTextRenderingParams(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
            return 0;
        case WM_DPICHANGED: {
            scale_ = HIWORD(wparam) / 96.0f;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetScale(scale_);
            CreateFormats();
            const auto* suggested = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOOWNERZORDER);
            return 0;
        }
        case kWatchMessage:
            OnWatch(wparam != 0);
            return 0;
        case WM_TIMER:
            if (wparam == kPingTimer) {
                // Only the dot's 20 px strip animates.
                RECT strip{ 0, static_cast<LONG>((WatchTop() - 6) * scale_), static_cast<LONG>(44 * scale_),
                            static_cast<LONG>((WatchTop() + 26) * scale_) };
                InvalidateRect(hwnd_, &strip, FALSE);
                return 0;
            }
            if (wparam == kContinueTimer) {
                KillTimer(hwnd_, kContinueTimer);
                Complete(LockedItemChoice::Retry);
                return 0;
            }
            break;
        case WM_MOUSEMOVE: {
            const int next = Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            if (next != hover_) { hover_ = next; InvalidateRect(hwnd_, nullptr, FALSE); }
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_ = 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            pressed_ = Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            const int hit = Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            const int pressed = pressed_;
            pressed_ = 0;
            ReleaseCapture();
            if (hit == pressed && hit != 0) {
                if (hit >= 10) { SwitchTo(static_cast<size_t>(hit - 10)); InvalidateRect(hwnd_, nullptr, FALSE); return 0; }
                if (hit == 1) Activate(0);
                else if (hit == 3) Activate(2);
                else Complete(LockedItemChoice::Cancel);
                return 0;
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wparam) != WA_INACTIVE) SetFocus(hwnd_);
            return 0;
        case WM_SYSKEYDOWN:
            if (!(lparam & (1 << 29)) && wparam != VK_F10) return Handle(WM_KEYDOWN, wparam, lparam);
            break;
        case WM_KEYDOWN:
            if (wparam == VK_PROCESSKEY) {
                const UINT vk = ImmGetVirtualKey(hwnd_);
                if (vk && vk != VK_PROCESSKEY) wparam = vk;
            }
            if (wparam == VK_ESCAPE) { Complete(LockedItemChoice::Cancel); return 0; }
            if (wparam == VK_RETURN || wparam == VK_SPACE) { Activate(FocusTarget(focus_)); return 0; }
            if (wparam == VK_TAB || wparam == VK_LEFT || wparam == VK_RIGHT) {
                const bool back = wparam == VK_LEFT || (wparam == VK_TAB && GetKeyState(VK_SHIFT) < 0);
                focus_ = (focus_ + (back ? FocusCount() - 1 : 1)) % FocusCount();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_CLOSE:
            Complete(LockedItemChoice::Cancel);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd_, &paint);
            Render();
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            KillTimer(hwnd_, kPingTimer);
            KillTimer(hwnd_, kContinueTimer);
            compositor_.Shutdown();
            done_ = true;
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_{ &compositor_ };
    LockedItemInfo info_;
    std::vector<bool> gone_;
    std::vector<HWND> windows_;
    std::vector<HICON> icons_;
    std::vector<ComPtr<ID2D1Bitmap>> bitmaps_;
    ComPtr<IWICImagingFactory> wic_;
    std::thread watcher_;
    std::atomic<bool> stop_{ false };
    std::atomic<uint64_t> watch_mask_{ 0 };
    LockedItemChoice choice_ = LockedItemChoice::Cancel;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    float scale_ = 1.0f;
    bool dark_ = false;
    bool backdrop_enabled_ = false;
    bool done_ = false;
    bool released_ = false;
    ULONGLONG released_at_ = 0;
    int hover_ = 0;
    int pressed_ = 0;
    int focus_ = 0;
    ComPtr<IDWriteTextFormat> head_format_;
    ComPtr<IDWriteTextFormat> sub_format_;
    ComPtr<IDWriteTextFormat> name_format_;
    ComPtr<IDWriteTextFormat> meta_format_;
    ComPtr<IDWriteTextFormat> watch_format_;
    ComPtr<IDWriteTextFormat> tag_format_;
    ComPtr<IDWriteTextFormat> small_button_format_;
    ComPtr<IDWriteTextFormat> button_format_;
    ComPtr<IDWriteTextFormat> glyph_format_;
    ComPtr<IDWriteTextFormat> app_glyph_format_;
};

} // namespace

FileOperationWindow::FileOperationWindow() : painter_(&compositor_) {}
FileOperationWindow::~FileOperationWindow() { Destroy(); }

bool FileOperationWindow::Create(HWND owner, FileOperationCallbacks callbacks) {
    if (hwnd_) return true;
    owner_ = owner;
    callbacks_ = std::move(callbacks);
    dark_ = ShouldUseDarkMode(ThemeMode::Auto);
    accent_ = GetAccentColor();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = kTransferClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    if (!GetClassInfoExW(wc.hInstance, kTransferClass, &wc)) RegisterClassExW(&wc);
    const UINT owner_dpi = pulse::compat::WindowDpi(owner_);
    const int width = MulDiv(static_cast<int>(kDlgW), owner_dpi, 96);
    const int height = MulDiv(static_cast<int>(kCollapsedH), owner_dpi, 96);
    hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kTransferClass, l10n::Get(l10n::StringId::OpWindow).c_str(),
        WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, width, height, owner_, nullptr,
        wc.hInstance, this);
    return hwnd_ != nullptr;
}

void FileOperationWindow::Destroy() {
    if (hwnd_) { HideComposedDialog(hwnd_); DestroyWindow(hwnd_); }
    hwnd_ = nullptr;
}

void FileOperationWindow::SetTheme(bool dark, D2D1_COLOR_F accent) {
    dark_ = dark;
    accent_ = accent;
    if (hwnd_) {
        ApplyWindowTheme();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void FileOperationWindow::SetTaskStart(uint64_t task_id,
                                       std::chrono::steady_clock::time_point started) {
    if (task_id == 0 || task_id != status_.task_id) return;
    if (started.time_since_epoch().count() == 0) return;
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    const ULONGLONG now = GetTickCount64();
    if (age <= 0 || static_cast<ULONGLONG>(age) >= now) return;
    task_start_tick_ = now - static_cast<ULONGLONG>(age);
}

void FileOperationWindow::CreateCardFormats() {
    auto* factory = compositor_.DwriteFactory();
    auto make = [&](ComPtr<IDWriteTextFormat>& out, typography::FontRole role, float size,
                    DWRITE_FONT_WEIGHT weight) {
        out.reset();
        if (!factory) return;
        typography::TextFormatSpec spec;
        spec.role = role;
        spec.size = size * scale_;
        spec.weight = weight;
        if (FAILED(typography::CreateTextFormat(factory, spec, &out)) || !out.get()) return;
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    };
    make(pct_format_, typography::FontRole::Display, 34.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    make(unit_format_, typography::FontRole::Display, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    make(title_format_, typography::FontRole::Text, 15.0f, DWRITE_FONT_WEIGHT_BOLD);
    make(speed_format_, typography::FontRole::Display, 17.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    make(label_format_, typography::FontRole::Text, 11.5f, DWRITE_FONT_WEIGHT_NORMAL);
    make(value_format_, typography::FontRole::Text, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    make(chip_format_, typography::FontRole::Text, 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    make(glyph_format_, typography::FontRole::Icon, 12.0f, DWRITE_FONT_WEIGHT_NORMAL);
    make(tile_glyph_format_, typography::FontRole::Icon, 18.0f, DWRITE_FONT_WEIGHT_NORMAL);
    if (glyph_format_.get()) glyph_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    if (tile_glyph_format_.get()) tile_glyph_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
}

namespace {
bool IsPartialFailure(const ops::OpStatus& status) {
    return !status.active && status.phase == ops::OpPhase::Failed &&
           (status.type == ops::OpType::Copy || status.type == ops::OpType::Move) &&
           status.failed_items > 0 && !status.failures.empty();
}

// "%s … %s" catalog text with grouped numbers.
std::wstring FormatCounts(l10n::StringId id, uint64_t first, uint64_t second = 0) {
    const std::wstring format = l10n::Get(id);
    const std::wstring a = pulse::format::GroupedInt(first);
    const std::wstring b = pulse::format::GroupedInt(second);
    wchar_t text[256];
    if (swprintf_s(text, format.c_str(), a.c_str(), b.c_str()) < 0) return a;
    return text;
}
}  // namespace

TransferFailureView FileOperationWindow::FailureView() const {
    TransferFailureView view;
    if (!board_) return view;
    view.on = true;
    view.can_open = !status_.destination_dir.empty() && callbacks_.open_folder != nullptr;
    if (callbacks_.retry_failed)
        view.can_retry = std::any_of(board_->rows.begin(), board_->rows.end(), [](const FailureRow& row) {
            return row.state == FailureRow::State::Failed;
        });
    return view;
}

bool FileOperationWindow::TracksRetry(uint64_t task_id) const {
    return board_ && task_id != 0 &&
           std::find(board_->retry_tasks.begin(), board_->retry_tasks.end(), task_id) != board_->retry_tasks.end();
}

size_t FileOperationWindow::VisibleFailureRows() const {
    return board_ ? (std::min)(board_->rows.size(), kFailMaxRows) : 0;
}

float FileOperationWindow::FailureBlockDip() const {
    if (!board_) return 0.0f;
    return static_cast<float>(VisibleFailureRows()) * kFailRowH +
           (board_->unlisted > 0 ? kFailMoreH : 0.0f) + kFailGap;
}

// The task that failed, with what its retries have fixed since: once nothing
// is left it reads as a success (and closes itself like one).
ops::OpStatus FileOperationWindow::BoardStatus() const {
    ops::OpStatus status = board_->status;
    uint64_t failed = board_->unlisted;
    for (const auto& row : board_->rows)
        if (row.state != FailureRow::State::Done) ++failed;
    status.failed_items = failed;
    if (failed == 0) {
        status.phase = ops::OpPhase::Completed;
        status.last_error.clear();
    }
    return status;
}

void FileOperationWindow::RetryFailureRows(const std::vector<size_t>& rows) {
    if (!board_ || !callbacks_.retry_failed) return;
    std::map<uint64_t, std::vector<size_t>> by_task;   // origin task -> its indices
    for (size_t r : rows) {
        if (r >= board_->rows.size()) continue;
        const FailureRow& row = board_->rows[r];
        if (row.state == FailureRow::State::Failed) by_task[row.origin_task].push_back(row.origin_index);
    }
    for (const auto& [task, indices] : by_task) {
        const auto spawns = callbacks_.retry_failed(task, indices);
        for (const auto& spawn : spawns) {
            board_->retry_tasks.push_back(spawn.task_id);
            for (size_t index : spawn.items) {
                for (auto& row : board_->rows) {
                    if (row.state != FailureRow::State::Failed || row.origin_task != task ||
                        row.origin_index != index) continue;
                    row.retry_task = spawn.task_id;
                    row.state = FailureRow::State::Retrying;
                }
            }
        }
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void FileOperationWindow::FinishRetry(const ops::OpStatus& done) {
    for (auto& row : board_->rows) {
        // Only rows still waiting on it: the same finish arrives repeatedly.
        if (row.state != FailureRow::State::Retrying || row.retry_task != done.task_id) continue;
        row.retry_task = 0;
        const auto found = std::find_if(done.failures.begin(), done.failures.end(), [&](const ops::OpFailure& failure) {
            return _wcsicmp(failure.source.c_str(), row.failure.source.c_str()) == 0;
        });
        if (done.phase == ops::OpPhase::Completed) {
            row.state = FailureRow::State::Done;
        } else if (found != done.failures.end()) {
            // Failed again: its next retry goes through the retry's list.
            row.failure = *found;
            row.origin_task = done.task_id;
            row.origin_index = static_cast<size_t>(found - done.failures.begin());
            row.state = FailureRow::State::Failed;
        } else if (done.failed_items == 0) {
            // The retry stopped as a whole (or was cancelled).
            row.state = FailureRow::State::Failed;
            if (!done.last_error.empty()) {
                const size_t bar = done.last_error.rfind(L" | ");
                row.failure.reason = l10n::ServiceErrorText(
                    bar == std::wstring::npos ? done.last_error : done.last_error.substr(0, bar));
            }
        } else {
            row.state = FailureRow::State::Done;
        }
    }
}

void FileOperationWindow::Update(const ops::OpStatus& incoming) {
    ops::OpStatus status = incoming;
    const bool had_board = board_.has_value();
    if (board_) {
        const bool spawned = incoming.task_id != 0 &&
            std::find(board_->retry_tasks.begin(), board_->retry_tasks.end(), incoming.task_id) !=
                board_->retry_tasks.end();
        if (spawned || incoming.task_id == board_->task_id) {
            if (spawned && !incoming.active && (incoming.phase == ops::OpPhase::Completed ||
                                                incoming.phase == ops::OpPhase::Failed))
                FinishRetry(incoming);
            // Retries that finished while another one was being reported.
            if (callbacks_.task_outcome) {
                std::vector<uint64_t> waiting;
                for (const auto& row : board_->rows)
                    if (row.state == FailureRow::State::Retrying && row.retry_task != incoming.task_id &&
                        std::find(waiting.begin(), waiting.end(), row.retry_task) == waiting.end())
                        waiting.push_back(row.retry_task);
                for (uint64_t task : waiting)
                    if (const auto outcome = callbacks_.task_outcome(task))
                        if (!outcome->active) FinishRetry(*outcome);
            }
            status = BoardStatus();
        } else {
            board_.reset();
            board_scroll_ = 0;
            failure_buttons_.clear();
        }
    }
    if (!board_ && IsPartialFailure(status)) {
        FailureBoard board;
        board.task_id = status.task_id;
        board.status = status;
        for (size_t i = 0; i < status.failures.size(); ++i) {
            FailureRow row;
            row.failure = status.failures[i];
            row.origin_task = status.task_id;
            row.origin_index = i;
            board.rows.push_back(std::move(row));
        }
        board.unlisted = status.failed_items > status.failures.size()
            ? status.failed_items - status.failures.size() : 0;
        board_ = std::move(board);
        board_scroll_ = 0;
    }
    if (had_board != board_.has_value()) ResizeForDetails(false);
    const bool new_task = status.task_id != status_.task_id;
    const ULONGLONG now = GetTickCount64();
    if (!new_task && status.active && status_.active) {
        const bool leaving_authorization = status.authorization == ops::AuthorizationState::None &&
            status_.authorization != ops::AuthorizationState::None;
        const bool retry_ended = status_.authorization_retrying && !status.authorization_retrying;
        if (hold_until_ == 0) {
            if (leaving_authorization) hold_until_ = now + kAuthorizationExitDelayMs;
            else if (retry_ended) hold_until_ = retry_shown_tick_ + kRetryMinVisibleMs;
        }
        if ((leaving_authorization || retry_ended) && now < hold_until_) {
            held_status_ = status;  // released by the render timer if it sticks
            has_held_status_ = true;
            return;
        }
    }
    hold_until_ = 0;
    has_held_status_ = false;
    if (status.authorization_retrying && !status_.authorization_retrying) retry_shown_tick_ = now;
    const bool was_active = status_.active;
    const bool was_succeeded = !new_task && !status_.active &&
        status_.phase == ops::OpPhase::Completed && status_.last_error.empty();
    if (new_task || status.authorization != status_.authorization) pressed_ = 0;
    status_ = ops::PresentOperationStatus(status);
    if (new_task) {
        graph_.Reset();
        SyncGraph();   // a card built late starts with the run so far
        task_start_tick_ = now;
        task_end_tick_ = 0;
        countdown_base_ = 0;
        kept_ = false;
        finish_unseen_ = false;
    }
    const bool finished = !status_.active &&
        (status_.phase == ops::OpPhase::Completed || status_.phase == ops::OpPhase::Failed);
    if (!finished) task_end_tick_ = 0;
    else if (task_end_tick_ == 0) task_end_tick_ = now;
    const bool succeeded = finished && status_.phase == ops::OpPhase::Completed &&
        status_.last_error.empty();
    // The countdown starts only when the finish happens in front of the user
    // in a window the app opened (SetAutoClose); a summary opened from the
    // pill stays until closed.
    if (!succeeded || !auto_close_ || kept_) {
        countdown_base_ = 0;
        finish_unseen_ = false;
    } else if (countdown_base_ == 0 && !was_succeeded && !closing_ && hwnd_ &&
               IsWindowVisible(hwnd_)) {
        if (IsIconic(hwnd_)) finish_unseen_ = true;
        else countdown_base_ = now;
    }
    if (!hwnd_) return;
    if (status_.authorization != ops::AuthorizationState::None) {
        RECT rect{};
        GetWindowRect(hwnd_, &rect);
        const int min_width = static_cast<int>(kDlgW * scale_);
        const int min_height = static_cast<int>(kCollapsedH * scale_);
        if (rect.right - rect.left < min_width || rect.bottom - rect.top < min_height)
            SetWindowPos(hwnd_, nullptr, 0, 0, std::max(min_width, static_cast<int>(rect.right - rect.left)),
                         std::max(min_height, static_cast<int>(rect.bottom - rect.top)),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    // The render timer stops once an operation ends; restart it for the next
    // one and for the auto-close countdown.
    if (((status_.active && !was_active) || countdown_base_ != 0) && IsWindowVisible(hwnd_))
        SetTimer(hwnd_, kRenderTimer, 33, nullptr);
}

void FileOperationWindow::Show(bool activate) {
    if (!hwnd_) return;
    const bool was_visible = IsWindowVisible(hwnd_) != FALSE;
    if (closing_) {
        // Reopened during the exit zoom: put the card back at full size.
        closing_ = false;
        compositor_.ClearZoom();
        ApplyWindowTheme();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (!positioned_) {
        RECT rect{};
        GetWindowRect(hwnd_, &rect);
        pulse::ui::CenterOwnedWindow(hwnd_, owner_, rect.right - rect.left, rect.bottom - rect.top);
        positioned_ = true;
    }
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER |
                 (activate ? 0 : SWP_NOACTIVATE) | SWP_SHOWWINDOW);
    // SWP_SHOWWINDOW sends no WM_SHOWWINDOW; start the animation timer here.
    if (!was_visible) {
        SyncGraph();   // fill in what ran while the card was hidden
        InvalidateRect(hwnd_, nullptr, FALSE);
        SetTimer(hwnd_, kRenderTimer, 33, nullptr);
    }
    if (activate) SetForegroundWindow(hwnd_);
}

void FileOperationWindow::SetAutoClose(bool on) {
    auto_close_ = on;
    if (on || countdown_base_ == 0) return;
    countdown_base_ = 0;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void FileOperationWindow::Hide() {
    countdown_base_ = 0;
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_HIDE);
    if (closing_) {
        closing_ = false;
        compositor_.ClearZoom();
        ApplyWindowTheme();
    }
}

bool FileOperationWindow::IsVisible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

void FileOperationWindow::ApplyWindowTheme() {
    backdrop_enabled_ = pulse::ui::ApplyBackdrop(hwnd_, dark_);
}

void FileOperationWindow::ResizeForDetails(bool preserve_center) {
    if (!hwnd_) return;
    RECT rect{};
    GetWindowRect(hwnd_, &rect);
    const int width = rect.right - rect.left;
    const int old_height = rect.bottom - rect.top;
    const int height = static_cast<int>(((detailed_ ? kDetailedH : kCollapsedH) + FailureBlockDip()) * scale_);
    const int y = preserve_center ? rect.top - (height - old_height) / 2 : rect.top;
    SetWindowPos(hwnd_, nullptr, rect.left, y, width, height,
                 SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
}

int FileOperationWindow::HitTestControl(float x, float y) const {
    if (closing_) return 0;
    const auto labels = MakeTransferLabels(status_, detailed_, FailureView());
    const auto chrome = MakeTransferChrome(scale_,
        static_cast<float>(compositor_.Width()), static_cast<float>(compositor_.Height()),
        painter_, labels);
    if (pulse::ui::ContainsRect(chrome.close, x, y)) return 1;
    if (pulse::ui::ContainsRect(chrome.minimize, x, y)) return 2;
    if (board_ && !status_.active) {
        for (const auto& [row, bounds] : failure_buttons_)
            if (pulse::ui::ContainsRect(bounds, x, y)) return kHitFailRow + static_cast<int>(row);
    }
    if (labels.show_open && pulse::ui::ContainsRect(chrome.open, x, y)) return kHitOpen;
    if (labels.show_retry && labels.actions_enabled && pulse::ui::ContainsRect(chrome.retry, x, y)) return 6;
    if (labels.show_skip && labels.actions_enabled && pulse::ui::ContainsRect(chrome.skip, x, y)) return 7;
    if (labels.show_pause && pulse::ui::ContainsRect(chrome.pause, x, y)) return 3;
    if (pulse::ui::ContainsRect(chrome.cancel, x, y)) return 4;
    if (labels.show_details && pulse::ui::ContainsRect(chrome.details, x, y)) return 5;
    return 0;
}

void FileOperationWindow::RequestClose() {
    if (closing_) return;
    const bool emptying = status_.type == ops::OpType::EmptyRecycle;
    if (status_.active && status_.phase != ops::OpPhase::Completed &&
        status_.phase != ops::OpPhase::Failed && !emptying) {
        if (callbacks_.cancel) callbacks_.cancel();
        return;
    }
    if (callbacks_.dismiss) callbacks_.dismiss();
    Hide();
}

bool FileOperationWindow::AutoClosePending() const {
    return countdown_base_ != 0 && !closing_ && !status_.active &&
           status_.phase == ops::OpPhase::Completed && status_.last_error.empty();
}

void FileOperationWindow::TickAutoClose(ULONGLONG now) {
    if (closing_) {
        if (now - closing_tick_ >= kCloseAnimMs) FinishAutoClose();
        return;
    }
    // Hovering does not hold the card (an untouched card closes itself); a
    // button held down or a minimized card restarts the countdown.
    countdown_hover_ = pressed_ != 0 || IsIconic(hwnd_);
    if (countdown_hover_) countdown_base_ = now;
    if (now - countdown_base_ >= kAutoCloseMs) {
        BeginAutoClose();
        return;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void FileOperationWindow::BeginAutoClose() {
    closing_ = true;
    closing_tick_ = GetTickCount64();
    countdown_base_ = 0;
    // The DWM backdrop does not follow the DComp zoom: drop it and paint the
    // card opaque first so it leaves as one piece.
    const DWORD none = DWMSBT_NONE;
    DwmSetWindowAttribute(hwnd_, DWMWA_SYSTEMBACKDROP_TYPE, &none, sizeof(none));
    // Without the backdrop, the frame extended over the client area is an
    // opaque DWM sheet that stayed behind the shrinking card as a blank
    // window. Without it the window is clear where the card is not.
    const MARGINS flat{ 0, 0, 0, 0 };
    DwmExtendFrameIntoClientArea(hwnd_, &flat);
    backdrop_enabled_ = false;
    Render();
    SetTimer(hwnd_, kCloseTimer, static_cast<UINT>(kCloseAnimMs), nullptr);
    // Toward the bottom-right, where the status pill sits.
    if (!compositor_.PlayZoom(static_cast<float>(compositor_.Width()),
                              static_cast<float>(compositor_.Height()), false,
                              static_cast<double>(kCloseAnimMs) / 1000.0))
        FinishAutoClose();
}

void FileOperationWindow::FinishAutoClose() {
    if (hwnd_) KillTimer(hwnd_, kCloseTimer);
    if (!closing_) return;
    closing_ = false;
    countdown_base_ = 0;
    if (callbacks_.dismiss) callbacks_.dismiss();
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
    compositor_.ClearZoom();
    ApplyWindowTheme();
}

// A click, key or drag on the finished card: the user is looking at it, so
// it stays until they close it.
void FileOperationWindow::KeepOpen() {
    if (closing_ || kept_ || status_.active) return;
    if (status_.phase != ops::OpPhase::Completed && status_.phase != ops::OpPhase::Failed) return;
    kept_ = true;
    finish_unseen_ = false;
    if (countdown_base_ == 0) return;
    countdown_base_ = 0;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void FileOperationWindow::SampleGraph(ULONGLONG now) {
    if (!graph_.Due(now)) return;
    if (status_.phase != ops::OpPhase::Running || ShowsPaused(status_)) {
        graph_.last_tick = now;
        return;
    }
    if (status_.speed_basis == ops::OpSpeedBasis::None) return;
    // Sample whichever unit this task tracks: byte transfers and
    // empty-recycle feed the byte rate, delete and restore the item rate.
    graph_.Sample(now, status_.percent, status_.speed_basis == ops::OpSpeedBasis::Items
        ? status_.items_per_second : status_.bytes_per_second);
}

// The engine samples every run in the background; while the card was not
// on screen it holds the only record, so take it whenever it knows more.
void FileOperationWindow::SyncGraph() {
    if (!callbacks_.speed_history || status_.task_id == 0) return;
    auto history = callbacks_.speed_history(status_.task_id);
    if (history.points.empty()) return;
    if (history.by_progress == graph_.by_progress && !graph_.points.empty() &&
        graph_.by_progress && history.points.back().x < graph_.points.back().x) return;
    if (history.by_progress == graph_.by_progress && !graph_.by_progress &&
        history.points.size() <= graph_.points.size()) return;
    const ULONGLONG tick = graph_.last_tick;
    graph_ = std::move(history);
    if (tick > graph_.last_tick) graph_.last_tick = tick;
}

void FileOperationWindow::Render() {
    if (compositor_.NeedsRecovery()) {
        if (!compositor_.Recover()) return;
        compositor_.RecreateTextFormats(scale_);
        painter_.SetCompositor(&compositor_);
        painter_.SetScale(scale_);
        CreateCardFormats();
    }
    auto* dc = compositor_.Dc();
    if (!dc) return;
    if (!pct_format_.get()) CreateCardFormats();
    const bool high_contrast = IsHighContrast();
    const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
    pulse::ui::BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_enabled_,
                 scale_);
    const float s = scale_;
    const float width = static_cast<float>(compositor_.Width());
    const float height = static_cast<float>(compositor_.Height());
    const float dip_w = width / std::max(s, 0.001f);
    const float content_w = dip_w - kPadX * 2.0f;
    auto* factory = compositor_.DwriteFactory();
    const auto labels = MakeTransferLabels(status_, detailed_, FailureView());
    const auto chrome = MakeTransferChrome(s, width, height, painter_, labels);
    const ULONGLONG now = GetTickCount64();

    const bool authorization = status_.authorization != ops::AuthorizationState::None;
    const bool requesting = status_.authorization == ops::AuthorizationState::Requesting;
    const bool failed = status_.phase == ops::OpPhase::Failed;
    const bool paused = ShowsPaused(status_);
    const bool pausing = PauseInFlight(status_);
    const bool waiting = status_.phase == ops::OpPhase::WaitingForConflict;
    const bool scanning = status_.phase == ops::OpPhase::Scanning;
    const bool completed = status_.phase == ops::OpPhase::Completed && !failed;
    const bool cancelling = status_.phase == ops::OpPhase::Cancelling;
    // Partial failure: the rest went through, the failed items are listed.
    const bool partial = failed && board_.has_value();
    const uint64_t partial_failed = partial ? status_.failed_items : 0;
    const uint64_t partial_done = status_.total_items > partial_failed ? status_.total_items - partial_failed : 0;
    const bool moving = status_.type == ops::OpType::Move;
    const bool deleting = status_.type == ops::OpType::RecycleDelete
                       || status_.type == ops::OpType::RealDelete;
    const bool restoring = status_.type == ops::OpType::RestoreRecycle;
    const bool emptying = status_.type == ops::OpType::EmptyRecycle;
    // 新建 and 重命名 go through the same window (opened from the status pill)
    // and must not read as a copy.
    const bool creating = status_.type == ops::OpType::CreateFolder
                       || status_.type == ops::OpType::CreateTextFile;
    const bool renaming = status_.type == ops::OpType::Rename
                       || status_.type == ops::OpType::BatchRename;
    const bool byte_basis = status_.speed_basis == ops::OpSpeedBasis::Bytes;
    const ops::OpSpeedBasis shown_basis = status_.speed_basis == ops::OpSpeedBasis::None
        ? ops::OpSpeedBasis::Bytes : status_.speed_basis;

    // State colour (spec "状态色"): accent while running, amber paused or
    // waiting for permission, violet waiting on a conflict, green done.
    const D2D1_COLOR_F amber = dark_ ? HexColor(0xF0B041) : HexColor(0xC98410);
    const D2D1_COLOR_F violet = dark_ ? HexColor(0xA78BFA) : HexColor(0x7C5CE6);
    const D2D1_COLOR_F green = dark_ ? HexColor(0x3CCF8E) : HexColor(0x16A36A);
    const D2D1_COLOR_F accent2 = dark_ ? HexColor(0x8F9CF0) : theme.accent;
    D2D1_COLOR_F tone = high_contrast ? theme.accent : accent2;
    if (failed) tone = theme.danger;
    else if (completed && !high_contrast) tone = green;
    else if ((authorization || paused) && !high_contrast) tone = amber;
    else if (waiting && !high_contrast) tone = violet;
    const D2D1_COLOR_F faint = WithAlpha(theme.text, dark_ ? 0.42f : 0.45f);
    const D2D1_COLOR_F well = OpWell(hwnd_, backdrop_enabled_, dark_);
    const D2D1_COLOR_F well_line = dark_ ? HexColor(0xFFFFFF, 0.06f) : HexColor(0x000000, 0.06f);
    const float fraction = status_.percent < 0.0f ? 0.0f
        : std::clamp(status_.percent / 100.0f, 0.0f, 1.0f);

    // 2-DIP progress hairline along the top edge.
    if (!authorization && (fraction > 0.0f || completed)) {
        const float bar_w = width * (completed ? 1.0f : fraction);
        painter_.FillRoundedRect(D2D1::RectF(0, 0, bar_w, ScaleDip(s, 2.0f)), 0, WithAlpha(tone, 0.9f));
    }

    // --- Header: gradient tile, title, route chips ---------------------------
    D2D1_COLOR_F tile_top = HexColor(0x7D8CF0), tile_bottom = HexColor(0x5260C0);
    const wchar_t* tile_glyph = L"\xE8C8";
    if (moving) { tile_top = HexColor(0xF0A85A); tile_bottom = HexColor(0xC9702A); tile_glyph = L"\xE7C2"; }
    else if (deleting || emptying) { tile_top = HexColor(0xF07A73); tile_bottom = HexColor(0xC4433D); tile_glyph = L"\xE74D"; }
    else if (restoring) { tile_top = HexColor(0x4CC2C9); tile_bottom = HexColor(0x2A8F96); tile_glyph = L"\xE777"; }
    else if (creating) tile_glyph = status_.type == ops::OpType::CreateFolder ? L"\xE8F4" : L"\xE8A5";
    else if (renaming) tile_glyph = L"\xE8AC";
    if (authorization) { tile_top = HexColor(0xF0B041); tile_bottom = HexColor(0xC98410); tile_glyph = L"\xEA18"; }
    else if (failed) { tile_top = HexColor(0xF07A73); tile_bottom = HexColor(0xC4433D); tile_glyph = L"\xE7BA"; }
    else if (completed) { tile_top = HexColor(0x4FD99C); tile_bottom = HexColor(0x1F9E66); tile_glyph = L"\xE73E"; }
    const auto tile = Rect(s, kPadX, 14, 40, 40);
    if (high_contrast) painter_.FillRoundedRect(tile, ScaleDip(s, 11.0f), theme.accent);
    else FillGradientTile(dc, tile, ScaleDip(s, 11.0f), tile_top, tile_bottom);
    painter_.DrawGlyphWithFormat(tile_glyph, tile, high_contrast ? theme.accent_text : HexColor(0xFFFFFF), tile_glyph_format_.get());

    const uint64_t item_count = status_.total_items > 0 ? status_.total_items
        : (std::max)(status_.completed_items, static_cast<uint64_t>(1));
    const std::wstring items_suffix = l10n::Get(l10n::StringId::OpItemsSuffix);
    std::wstring title;
    if (emptying) {
        if (failed) title = l10n::Get(l10n::StringId::OpCannotEmpty);
        else if (completed) title = l10n::Get(l10n::StringId::OpEmptied);
        else title = l10n::Get(l10n::StringId::OpEmptying);
    } else if (failed && (creating || renaming)) {
        title = l10n::Get(renaming ? l10n::StringId::CannotRename
            : status_.type == ops::OpType::CreateFolder ? l10n::StringId::CannotCreateFolder
            : l10n::StringId::CannotCreateFile);
    } else if (completed && (creating || renaming)) {
        title = l10n::Get(creating ? l10n::StringId::OpCreatedPrefix : l10n::StringId::OpRenamedPrefix) +
            pulse::format::GroupedInt((std::max)(status_.completed_items, static_cast<uint64_t>(1))) +
            items_suffix;
    } else if (!paused && !scanning && (creating || renaming)) {
        title = l10n::Get(creating ? l10n::StringId::OpCreatingPrefix : l10n::StringId::OpRenamingPrefix) +
            pulse::format::GroupedInt(item_count) + items_suffix;
    } else if (partial) {
        title = FormatCounts(moving ? l10n::StringId::OpMovePartialTitle : l10n::StringId::OpCopyPartialTitle,
                             partial_done, partial_failed);
    } else if (failed) {
        title = l10n::Get(deleting ? l10n::StringId::OpDeleteFailed : restoring ? l10n::StringId::OpRestoreFailed
            : moving ? l10n::StringId::OpMoveFailed : l10n::StringId::OpCopyFailed);
    } else if (completed) {
        title = l10n::Get(deleting ? l10n::StringId::OpDeletedPrefix : restoring ? l10n::StringId::OpRestoredPrefix
            : moving ? l10n::StringId::OpMovedPrefix : l10n::StringId::OpCopiedPrefix)
            + pulse::format::GroupedInt(status_.completed_items) + items_suffix;
    } else if (paused && !pausing) {
        title = l10n::Get(deleting ? l10n::StringId::OpDeletePaused : restoring ? l10n::StringId::OpRestorePaused
            : moving ? l10n::StringId::OpMovePaused : l10n::StringId::OpCopyPaused);
    } else if (scanning) {
        title = l10n::Get(l10n::StringId::OpPreparingList);
    } else {
        title = l10n::Get(deleting ? l10n::StringId::OpDeletingPrefix : restoring ? l10n::StringId::OpRestoringPrefix
            : moving ? l10n::StringId::OpMovingPrefix : l10n::StringId::OpCopyingPrefix)
            + pulse::format::GroupedInt(item_count) + items_suffix;
    }
    const float title_right = chrome.minimize.left / s - 6.0f;
    DrawEllipsizedText(compositor_, title_format_.get(), Rect(s, 70, 12, title_right - 70.0f, 24),
                       title, theme.text);

    // Route: [folder source] → [folder destination]; single chip otherwise.
    const std::wstring src = status_.source_label.empty()
        ? l10n::Get(l10n::StringId::OpSourceShort) : status_.source_label;
    const bool two_ends = !status_.destination_label.empty() && !deleting && !emptying && !restoring
                       && !creating && !renaming;
    const D2D1_COLOR_F folder = dark_ ? HexColor(0xF0B041) : HexColor(0xD99A1E);
    auto draw_chip = [&](float x, float max_w, const std::wstring& text) -> float {
        const float text_w = TextWidth(factory, compositor_.SmallFormat(), text) / s;
        const float w = (std::min)(max_w, text_w + 28.0f);
        if (w < 30.0f) return x;
        const auto chip = Rect(s, x, 37, w, 21);
        painter_.FillRoundedRect(chip, ScaleDip(s, 6.0f), well);
        painter_.StrokeRoundedRect(chip, ScaleDip(s, 6.0f), well_line);
        painter_.DrawGlyphWithFormat(L"\xE8B7", Rect(s, x + 4, 37, 16, 21), folder, glyph_format_.get());
        DrawEllipsizedText(compositor_, compositor_.SmallFormat(), Rect(s, x + 21, 39, w - 26, 17),
                           text, theme.text_secondary);
        return x + w;
    };
    if (emptying) {
        draw_chip(70, dip_w - 70 - kPadX, l10n::Get(l10n::StringId::OpEmptyingSub));
    } else if (two_ends) {
        const float avail = dip_w - 70.0f - kPadX - 22.0f;
        const float first_w = TextWidth(factory, compositor_.SmallFormat(), src) / s + 28.0f;
        const float second_w = TextWidth(factory, compositor_.SmallFormat(), status_.destination_label) / s + 28.0f;
        float a = first_w, b = second_w;
        if (a + b > avail) {
            const float half = avail * 0.5f;
            if (a < half) b = avail - a;
            else if (b < half) a = avail - b;
            else { a = half; b = half; }
        }
        const float after = draw_chip(70, a, src);
        painter_.DrawGlyphWithFormat(L"\xE72A", Rect(s, after + 3, 37, 16, 21), faint, glyph_format_.get());
        draw_chip(after + 22, b, status_.destination_label);
    } else {
        draw_chip(70, dip_w - 70 - kPadX, src);
    }

    // Title-bar buttons, with the auto-close ring around close.
    fluent::ControlState minimize{};
    minimize.hovered = hover_ == 2;
    minimize.pressed = pressed_ == 2;
    painter_.DrawTitleBarButton(chrome.minimize, fluent::TitleBarButtonRole::Minimize, {}, minimize);
    fluent::ControlState close{};
    close.hovered = hover_ == 1;
    close.pressed = pressed_ == 1;
    painter_.DrawTitleBarButton(chrome.close, fluent::TitleBarButtonRole::Close, {}, close);
    const bool counting = AutoClosePending() || closing_;
    if (counting) {
        const float left = closing_ || countdown_hover_ ? 1.0f
            : 1.0f - std::clamp(static_cast<float>(now - countdown_base_) / kAutoCloseMs, 0.0f, 1.0f);
        const D2D1_POINT_2F c = D2D1::Point2F((chrome.close.left + chrome.close.right) * 0.5f,
                                              (chrome.close.top + chrome.close.bottom) * 0.5f);
        const float r = ScaleDip(s, 11.0f);
        StrokeArc(dc, c, r, 0.0f, 360.0f, WithAlpha(green, 0.18f), ScaleDip(s, 2.0f));
        StrokeArc(dc, c, r, 360.0f * (1.0f - left), 360.0f * left, green, ScaleDip(s, 2.0f));
    }

    // --- Percent, state chip and speed -----------------------------------
    float chip_x = kPadX;
    if (!authorization && status_.percent >= 0.0f) {
        const int whole = static_cast<int>(std::floor(fraction * 100.0f + (completed ? 0.5f : 0.0f)));
        const std::wstring pct = std::to_wstring(completed ? 100 : whole);
        const float pct_w = TextWidth(factory, pct_format_.get(), pct) / s;
        painter_.DrawText(pct, Rect(s, kPadX - 1, 66, pct_w + 4, 44), pct_format_.get(),
                          failed ? theme.danger : theme.text);
        painter_.DrawText(L"%", Rect(s, kPadX + pct_w + 1, 78, 20, 26), unit_format_.get(),
                          theme.text_secondary);
        const float unit_w = TextWidth(factory, unit_format_.get(), L"%") / s;
        chip_x = kPadX + pct_w + unit_w + 12.0f;
    }
    std::wstring chip_text;
    if (authorization) chip_text = l10n::Get(requesting ? l10n::StringId::OpAuthorizationRequesting
                                                         : l10n::StringId::OpAuthorizationRequired);
    else if (partial) chip_text = l10n::Get(l10n::StringId::OpPartialBadge);
    else if (failed) chip_text = l10n::Get(l10n::StringId::OpFailedBadge);
    else if (completed) chip_text = l10n::Get(emptying ? l10n::StringId::OpEmptied : l10n::StringId::OpCompleted);
    else if (cancelling) chip_text = l10n::Get(l10n::StringId::OpCancelling);
    else if (pausing) chip_text = l10n::Get(l10n::StringId::OpPausing);
    else if (paused) chip_text = l10n::Get(l10n::StringId::OpPaused);
    else if (waiting) chip_text = l10n::Get(l10n::StringId::OpWaitingConflict);
    else if (scanning) chip_text = l10n::Get(l10n::StringId::OpPreparing);
    else if (emptying) chip_text = l10n::Get(l10n::StringId::OpEmptying);
    else chip_text = l10n::Get(deleting ? l10n::StringId::OpDeleting : restoring ? l10n::StringId::OpRestoringPrefix
        : moving ? l10n::StringId::OpMoving : l10n::StringId::OpCopying);
    while (!chip_text.empty() && chip_text.back() == L' ') chip_text.pop_back();
    {
        const float text_w = TextWidth(factory, chip_format_.get(), chip_text) / s;
        const float chip_w = text_w + 26.0f;
        const auto chip = Rect(s, chip_x, 80, chip_w, 22);
        painter_.FillRoundedRect(chip, ScaleDip(s, 11.0f), WithAlpha(tone, dark_ ? 0.16f : 0.12f));
        const D2D1_POINT_2F dot = D2D1::Point2F(ScaleDip(s, chip_x + 11.0f), ScaleDip(s, 91.0f));
        const bool live = status_.active && !paused && !waiting && !authorization && !cancelling;
        if (pausing || requesting || scanning) {
            const float spin = static_cast<float>(now % 900) / 900.0f * 360.0f;
            StrokeArc(dc, dot, ScaleDip(s, 4.0f), spin, 270.0f, tone, ScaleDip(s, 1.5f));
        } else {
            float alpha = 1.0f;
            if (live) alpha = 0.45f + 0.55f * std::fabs(std::cos(static_cast<float>(now % 1600) / 1600.0f * kPi));
            painter_.FillRoundedRect(D2D1::RectF(dot.x - ScaleDip(s, 3.0f), dot.y - ScaleDip(s, 3.0f),
                                                 dot.x + ScaleDip(s, 3.0f), dot.y + ScaleDip(s, 3.0f)),
                                     ScaleDip(s, 3.0f), WithAlpha(tone, alpha));
        }
        painter_.DrawText(chip_text, Rect(s, chip_x + 19, 80, text_w + 4, 22), chip_format_.get(), tone);
    }

    const double live_speed = status_.speed_basis == ops::OpSpeedBasis::Items
        ? status_.items_per_second : status_.bytes_per_second;
    const double peak_speed = status_.speed_basis == ops::OpSpeedBasis::Items
        ? status_.peak_items_per_second : status_.peak_bytes_per_second;
    const ULONGLONG end_tick = task_end_tick_ != 0 ? task_end_tick_ : now;
    const uint64_t elapsed_ms = task_start_tick_ != 0 && end_tick > task_start_tick_
        ? end_tick - task_start_tick_ : 0;
    const double elapsed_s = static_cast<double>(elapsed_ms) / 1000.0;
    const double done_amount = byte_basis ? static_cast<double>(status_.transferred_bytes)
                                          : static_cast<double>(status_.completed_items);
    const double average = elapsed_s >= 0.25 ? done_amount / elapsed_s : 0.0;
    if (!authorization) {
        std::wstring big;
        std::wstring sub;
        D2D1_COLOR_F big_color = theme.text;
        if (completed || partial) {
            big = average > 0.0 ? l10n::Get(l10n::StringId::OpAverageLead) + FormatSpeedText(shown_basis, average)
                                : std::wstring();
            sub = l10n::Get(l10n::StringId::OpTookLead) + FormatElapsed(elapsed_ms / 1000);
        } else if (scanning) {
            // Nothing measured yet: the chip already reads 准备中.
        } else if (status_.speed_basis == ops::OpSpeedBasis::None) {
            big = l10n::Get(l10n::StringId::OpNoEstimate);
            big_color = theme.text_secondary;
        } else if (failed || paused || waiting || cancelling) {
            big = L"—";
            big_color = theme.text_secondary;
            if (peak_speed > 0.0) sub = l10n::Get(l10n::StringId::OpPeak) + FormatSpeedText(shown_basis, peak_speed);
        } else if (live_speed <= 0.0) {
            big = l10n::Get(l10n::StringId::OpEstimating);
            big_color = theme.text_secondary;
        } else {
            big = FormatSpeedText(shown_basis, live_speed);
            if (peak_speed > 0.0) sub = l10n::Get(l10n::StringId::OpPeak) + FormatSpeedText(shown_basis, peak_speed);
        }
        const float right_w = 190.0f;
        painter_.DrawText(big, Rect(s, dip_w - kPadX - right_w, 70, right_w, 24), speed_format_.get(),
                          big_color, fluent::HorizontalAlignment::Right);
        painter_.DrawText(sub, Rect(s, dip_w - kPadX - right_w, 94, right_w, 16), label_format_.get(),
                          theme.text_secondary, fluent::HorizontalAlignment::Right);
    }

    if (authorization) {
        // Permission view replaces graph, item line and stats.
        const auto card = Rect(s, kPadX, kGraphTop, content_w, kStatsTop + kStatsH - kGraphTop);
        painter_.FillRoundedRect(card, ScaleDip(s, 10.0f), well);
        painter_.StrokeRoundedRect(card, ScaleDip(s, 10.0f), well_line);
        const auto badge = Rect(s, kPadX + 16, kGraphTop + 18, 36, 36);
        painter_.FillRoundedRect(badge, ScaleDip(s, 18.0f), WithAlpha(amber, 0.16f));
        painter_.DrawGlyphWithFormat(L"\xEA18", badge, amber, tile_glyph_format_.get());
        const std::wstring explanation = requesting
            ? l10n::Get(l10n::StringId::OpAuthorizationPrompt)
            : status_.last_error.empty() ? l10n::Get(l10n::StringId::OpAuthorizationNotGranted)
                                        : l10n::ServiceErrorText(status_.last_error);
        DrawEllipsizedText(compositor_, compositor_.SmallFormat(),
            Rect(s, kPadX + 66, kGraphTop + 16, content_w - 84, 42), explanation, theme.text, true);
        const std::wstring item = l10n::Get(l10n::StringId::OpItemPrefix) +
            (status_.current_item.empty() ? src : status_.current_item);
        DrawEllipsizedText(compositor_, compositor_.SmallFormat(),
            Rect(s, kPadX + 66, kGraphTop + 62, content_w - 84, 20), item, theme.text_secondary);
        if (requesting) {
            fluent::ProgressSpec bar;
            bar.bounds = Rect(s, kPadX + 66, kGraphTop + 96, content_w - 84, 4);
            bar.indeterminate = true;
            bar.animation_progress = static_cast<float>(
                std::fmod(static_cast<double>(now), 1952.0) / 1952.0);
            painter_.DrawProgressBar(bar);
        }
    } else {
        // --- Speed graph ---------------------------------------------------
        const auto plot = Rect(s, kPadX, kGraphTop, content_w, kGraphH);
        painter_.FillRoundedRect(plot, ScaleDip(s, 10.0f), well);
        painter_.StrokeRoundedRect(plot, ScaleDip(s, 10.0f), well_line);
        const D2D1_RECT_F inner = D2D1::RectF(plot.left + 1.0f, plot.top + 1.0f,
                                              plot.right - 1.0f, plot.bottom - 1.0f);
        dc->PushAxisAlignedClip(inner, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        for (int k = 1; k <= 3; ++k) {
            const float y = plot.top + (plot.bottom - plot.top) * k / 4.0f;
            painter_.FillRoundedRect(D2D1::RectF(inner.left, y, inner.right, y + 1.0f), 0,
                                     dark_ ? HexColor(0xFFFFFF, 0.035f) : HexColor(0x000000, 0.04f));
        }
        const float plot_w = inner.right - inner.left;
        const float shown_fraction = completed ? 1.0f : fraction;
        if (graph_.by_progress && status_.percent >= 0.0f && shown_fraction > 0.0f) {
            painter_.FillRoundedRect(D2D1::RectF(inner.left, inner.top,
                                                 inner.left + plot_w * shown_fraction, inner.bottom),
                                     0, WithAlpha(tone, 0.07f));
        }
        double max_value = 0.0;
        for (const auto& point : graph_.points) max_value = (std::max)(max_value, point.v);
        if (graph_.points.size() >= 2 && max_value > 0.0) {
            const float top = inner.top + ScaleDip(s, 16.0f);
            const float bottom = inner.bottom - ScaleDip(s, 3.0f);
            const double ceiling = max_value * 1.12;
            std::vector<D2D1_POINT_2F> points;
            points.reserve(graph_.points.size());
            const size_t span = (std::max)(graph_.points.size(), kTimeGraphPoints) - 1;
            for (size_t i = 0; i < graph_.points.size(); ++i) {
                const float x = graph_.by_progress ? inner.left + plot_w * graph_.points[i].x
                    : inner.left + plot_w * static_cast<float>(i) / static_cast<float>(span);
                const float y = bottom - (bottom - top) * static_cast<float>(graph_.points[i].v / ceiling);
                points.push_back(D2D1::Point2F(x, y));
            }
            // The pointer follows the percent every frame, the samples only
            // every 250 ms: carry the curve flat to the pointer so the live
            // dot rides on it instead of trailing behind.
            if (graph_.by_progress && !completed && status_.percent >= 0.0f) {
                const float head = inner.left + plot_w * shown_fraction;
                if (head > points.back().x + 0.5f) points.push_back(D2D1::Point2F(head, points.back().y));
            }
            const float strength = (paused || waiting || failed) ? 0.55f : 1.0f;
            DrawSpeedArea(dc, inner, points, tone, strength, s);
            ComPtr<ID2D1SolidColorBrush> dot_brush;
            if (!completed && SUCCEEDED(dc->CreateSolidColorBrush(tone, &dot_brush))) {
                const auto& last = points.back();
                dc->FillEllipse(D2D1::Ellipse(last, ScaleDip(s, 3.0f), ScaleDip(s, 3.0f)), dot_brush.get());
                if (status_.active && !paused && !waiting) {
                    const float pulse = static_cast<float>(now % 1400) / 1400.0f;
                    ComPtr<ID2D1SolidColorBrush> halo;
                    if (SUCCEEDED(dc->CreateSolidColorBrush(WithAlpha(tone, 0.35f * (1.0f - pulse)), &halo)))
                        dc->FillEllipse(D2D1::Ellipse(last, ScaleDip(s, 3.0f + 6.0f * pulse),
                                                      ScaleDip(s, 3.0f + 6.0f * pulse)), halo.get());
                }
            }
            painter_.DrawText(FormatSpeedText(shown_basis, max_value),
                              D2D1::RectF(inner.left, inner.top + ScaleDip(s, 2.0f),
                                          inner.right - ScaleDip(s, 8.0f), inner.top + ScaleDip(s, 16.0f)),
                              label_format_.get(), faint, fluent::HorizontalAlignment::Right);
        }
        // Progress pointer.
        if (graph_.by_progress && status_.percent >= 0.0f && shown_fraction > 0.0f && shown_fraction < 1.0f) {
            const float x = inner.left + plot_w * shown_fraction;
            painter_.FillRoundedRect(D2D1::RectF(x - 0.5f, inner.top, x + 0.5f + (s > 1.4f ? 0.5f : 0.0f), inner.bottom),
                                     0, WithAlpha(tone, 0.55f));
        }
        dc->PopAxisAlignedClip();

        // --- Current item -------------------------------------------------
        std::wstring item_line;
        D2D1_COLOR_F item_color = theme.text;
        const wchar_t* item_glyph = L"\xE8A5";
        if (partial) {
            item_line = FormatCounts(l10n::StringId::OpPartialLine, partial_done, partial_failed);
            item_color = theme.danger;
            item_glyph = L"\xE783";
        } else if (failed) {
            // pulse_shell errors are Simplified; known messages are localized and
            // file names are kept as they are on disk.
            item_line = status_.last_error.empty() ? l10n::Get(l10n::StringId::OpFailed)
                                                   : l10n::ServiceErrorText(status_.last_error);
            item_color = theme.danger;
            item_glyph = L"\xE783";
        } else if (completed) {
            item_line = status_.destination_label.empty() || !two_ends ? src
                : (src + L"  \x2192  " + status_.destination_label);
            item_color = theme.text_secondary;
            item_glyph = L"\xE73E";
        } else if (scanning) {
            item_line = l10n::Get(l10n::StringId::OpScanning);
            item_color = theme.text_secondary;
        } else if (emptying) {
            item_line = l10n::Get(l10n::StringId::OpEmptyingSub);
        } else {
            item_line = status_.current_item.empty() ? src : status_.current_item;
        }
        painter_.DrawGlyphWithFormat(item_glyph, Rect(s, kPadX, kItemTop, 16, 20), failed ? theme.danger : completed ? green : theme.text_secondary, glyph_format_.get());
        std::wstring counter;
        if (status_.total_items > 0 && !failed)
            counter = pulse::format::GroupedInt(status_.completed_items) + L" / " +
                      pulse::format::GroupedInt(status_.total_items);
        const float counter_w = counter.empty() ? 0.0f
            : TextWidth(factory, label_format_.get(), counter) / s + 10.0f;
        DrawEllipsizedText(compositor_, compositor_.SmallFormat(),
            Rect(s, kPadX + 22, kItemTop + 1, content_w - 22 - counter_w, 19), item_line, item_color);
        if (!counter.empty())
            painter_.DrawText(counter, Rect(s, dip_w - kPadX - counter_w, kItemTop, counter_w, 20),
                              label_format_.get(), theme.text_secondary, fluent::HorizontalAlignment::Right);

        // --- Failed items (partial failure) ----------------------------------
        const float fail_block = FailureBlockDip();
        failure_buttons_.clear();
        if (board_) {
            const float list_top = kStatsTop;
            const size_t visible = VisibleFailureRows();
            board_scroll_ = (std::min)(board_scroll_, board_->rows.size() - visible);
            const float list_h = static_cast<float>(visible) * kFailRowH +
                                 (board_->unlisted > 0 ? kFailMoreH : 0.0f);
            const auto box = Rect(s, kPadX, list_top, content_w, list_h);
            const D2D1_COLOR_F bad = theme.danger;
            // Everything retried: the list turns to the success tone.
            const bool board_done = status_.failed_items == 0;
            painter_.FillRoundedRect(box, ScaleDip(s, 9.0f),
                                     WithAlpha(board_done ? green : bad, board_done ? (dark_ ? 0.10f : 0.08f)
                                                                                 : (dark_ ? 0.15f : 0.10f)));
            const std::wstring retry_label = l10n::Get(l10n::StringId::RecoveryRetry);
            const std::wstring retrying_label = l10n::Get(l10n::StringId::Retrying);
            const std::wstring done_label = l10n::Get(l10n::StringId::OpCompleted);
            const float retry_w = TextWidth(factory, label_format_.get(), retry_label) / s + 20.0f;
            for (size_t v = 0; v < visible; ++v) {
                const size_t index = board_scroll_ + v;
                if (index >= board_->rows.size()) break;
                const FailureRow& row = board_->rows[index];
                const float y = list_top + kFailRowH * static_cast<float>(v);
                if (v > 0) {
                    const float line = ScaleDip(s, y);
                    painter_.FillRoundedRect(D2D1::RectF(box.left, line, box.right, line + 1.0f), 0,
                                             WithAlpha(theme.text, dark_ ? 0.07f : 0.08f));
                }
                const bool done = row.state == FailureRow::State::Done;
                const float alpha = done ? 0.6f : 1.0f;
                painter_.DrawGlyphWithFormat(done ? L"\xE73E" : L"\xE7BA", Rect(s, kPadX + 6, y, 20, kFailRowH), WithAlpha(done ? green : bad, alpha), glyph_format_.get());
                const std::wstring name = LeafName(row.failure.source);
                const float name_w = (std::min)(150.0f, TextWidth(factory, chip_format_.get(), name) / s + 2.0f);
                DrawEllipsizedText(compositor_, chip_format_.get(), Rect(s, kPadX + 30, y, name_w, kFailRowH),
                                   name, WithAlpha(theme.text, alpha));
                float action_left = kPadX + content_w - 8.0f;
                if (row.state == FailureRow::State::Failed && callbacks_.retry_failed) {
                    const auto button = Rect(s, action_left - retry_w, y + (kFailRowH - 24.0f) * 0.5f, retry_w, 24.0f);
                    failure_buttons_.push_back({ v, button });
                    const bool hot = hover_ == kHitFailRow + static_cast<int>(v);
                    const bool down = pressed_ == kHitFailRow + static_cast<int>(v);
                    painter_.FillRoundedRect(button, ScaleDip(s, 6.0f),
                        WithAlpha(theme.text, down ? 0.14f : hot ? 0.10f : (dark_ ? 0.06f : 0.05f)));
                    painter_.StrokeRoundedRect(button, ScaleDip(s, 6.0f), WithAlpha(theme.text, 0.14f));
                    painter_.DrawText(retry_label, button, label_format_.get(), theme.text,
                                      fluent::HorizontalAlignment::Center);
                    action_left -= retry_w + 8.0f;
                } else if (row.state == FailureRow::State::Retrying) {
                    const float w = TextWidth(factory, label_format_.get(), retrying_label) / s + 4.0f;
                    painter_.DrawText(retrying_label, Rect(s, action_left - w, y, w, kFailRowH),
                                      label_format_.get(), theme.text_secondary);
                    action_left -= w + 8.0f;
                }
                const float reason_x = kPadX + 30.0f + name_w + 10.0f;
                if (action_left > reason_x + 8.0f)
                    DrawEllipsizedText(compositor_, label_format_.get(),
                                       Rect(s, reason_x, y, action_left - reason_x, kFailRowH),
                                       done ? done_label : row.failure.reason,
                                       done ? WithAlpha(green, alpha) : theme.text_secondary);
            }
            if (board_->unlisted > 0) {
                DrawEllipsizedText(compositor_, label_format_.get(),
                    Rect(s, kPadX + 30, list_top + kFailRowH * static_cast<float>(visible), content_w - 40, kFailMoreH),
                    FormatCounts(l10n::StringId::OpMoreFailures, board_->unlisted), faint);
            }
            if (board_->rows.size() > visible && visible > 0) {
                // Wheel-scrolled: a thin thumb shows where the view is.
                const float track = static_cast<float>(visible) * kFailRowH - 8.0f;
                const float thumb = (std::max)(16.0f, track * static_cast<float>(visible) /
                                                          static_cast<float>(board_->rows.size()));
                const float at = list_top + 4.0f + (track - thumb) * static_cast<float>(board_scroll_) /
                                 static_cast<float>(board_->rows.size() - visible);
                painter_.FillRoundedRect(Rect(s, kPadX + content_w - 4.0f, at, 2.5f, thumb), ScaleDip(s, 1.25f),
                                         WithAlpha(theme.text, 0.25f));
            }
        }
        const float stats_top = kStatsTop + fail_block;

        // --- Stats: 剩余时间 / 剩余 / 已用时间 --------------------------------
        // The engine's first estimates come from a cold start (the file cache,
        // a slow first file) and can read hours for a copy of seconds. Hold
        // 正在估算 for the first 3 s, and while the estimate is far beyond what
        // the current rate would take.
        const uint64_t left_bytes = status_.total_bytes > status_.transferred_bytes
            ? status_.total_bytes - status_.transferred_bytes : 0;
        const double rate_eta = byte_basis && status_.bytes_per_second > 0.0
            ? static_cast<double>(left_bytes) / status_.bytes_per_second : 0.0;
        const bool eta_unsettled = elapsed_ms < 3000 ||
            (rate_eta > 0.0 && static_cast<double>(status_.eta_seconds) > rate_eta * 4.0 + 30.0);
        const std::wstring time_text = partial ? std::wstring(L"\x2014")
            : failed ? l10n::Get(l10n::StringId::OpIncomplete)
            : completed ? l10n::Get(l10n::StringId::OpCompleted)
            : paused ? l10n::Get(l10n::StringId::OpPaused)
            : waiting ? l10n::Get(l10n::StringId::OpWaiting)
            : emptying ? l10n::Get(l10n::StringId::OpEmptying)
            : status_.speed_basis == ops::OpSpeedBasis::None ? l10n::Get(l10n::StringId::OpNoEstimate)
            : status_.eta_seconds == 0 || eta_unsettled ? l10n::Get(l10n::StringId::OpEstimating)
            : l10n::Get(l10n::StringId::OpApprox) + FormatDuration(status_.eta_seconds);
        const uint64_t remain_items = status_.total_items > status_.completed_items
            ? status_.total_items - status_.completed_items : 0;
        const uint64_t remain_bytes = status_.total_bytes > status_.transferred_bytes
            ? status_.total_bytes - status_.transferred_bytes : 0;
        std::wstring remain_text;
        if (partial) remain_text = FormatCounts(l10n::StringId::OpFailedCount, partial_failed);
        else if (completed) remain_text = l10n::Get(l10n::StringId::OpZeroItems);
        else if (failed && status_.total_items == 0) remain_text = L"—";
        else if (status_.total_items == 0) remain_text = l10n::Get(l10n::StringId::OpCalculating);
        else if (byte_basis && status_.total_bytes > 0)
            remain_text = pulse::format::GroupedInt(remain_items) + l10n::Get(l10n::StringId::OpCountBytes)
                + pulse::format::ByteSize(remain_bytes) + L")";
        else remain_text = pulse::format::GroupedInt(remain_items) + l10n::Get(l10n::StringId::OpCountSuffix);
        const std::wstring stat_labels[3] = {
            l10n::Get(l10n::StringId::OpRemainingTime), l10n::Get(l10n::StringId::OpRemainingShort),
            l10n::Get(l10n::StringId::OpElapsed) };
        const std::wstring stat_values[3] = { time_text, remain_text, FormatElapsed(elapsed_ms / 1000) };
        const D2D1_COLOR_F stat_colors[3] = {
            partial ? theme.text_secondary : failed ? theme.danger : completed ? green : paused ? amber : theme.text,
            partial ? theme.danger : theme.text, theme.text };
        const float gap = 6.0f;
        const float box_w = (content_w - gap * 2.0f) / 3.0f;
        for (int i = 0; i < 3; ++i) {
            const float x = kPadX + (box_w + gap) * i;
            const auto box = Rect(s, x, stats_top, box_w, kStatsH);
            painter_.FillRoundedRect(box, ScaleDip(s, 9.0f), well);
            painter_.DrawText(stat_labels[i], Rect(s, x + 10, stats_top + 4, box_w - 20, 16),
                              label_format_.get(), theme.text_secondary);
            DrawEllipsizedText(compositor_, value_format_.get(),
                               Rect(s, x + 10, stats_top + 20, box_w - 16, 20), stat_values[i], stat_colors[i]);
        }

        // --- Details: 已处理 / 已传输 / 平均速度 / 峰值速度 ---------------------
        if (detailed_) {
            const std::wstring detail_labels[4] = {
                l10n::Get(l10n::StringId::OpProcessedItems), l10n::Get(l10n::StringId::OpTransferredBytes),
                l10n::Get(l10n::StringId::OpAverageSpeed), l10n::Get(l10n::StringId::OpPeakSpeed) };
            const std::wstring detail_values[4] = {
                status_.total_items > 0
                    ? pulse::format::GroupedInt(status_.completed_items) + L" / " + pulse::format::GroupedInt(status_.total_items)
                    : pulse::format::GroupedInt(status_.completed_items),
                byte_basis && status_.total_bytes > 0
                    ? pulse::format::ByteSize(status_.transferred_bytes) + L" / " + pulse::format::ByteSize(status_.total_bytes)
                    : std::wstring(L"—"),
                average > 0.0 ? FormatSpeedText(shown_basis, average) : std::wstring(L"—"),
                peak_speed > 0.0 ? FormatSpeedText(shown_basis, peak_speed) : std::wstring(L"—") };
            const float half_w = (content_w - 8.0f) * 0.5f;
            for (int i = 0; i < 4; ++i) {
                const float x = kPadX + (half_w + 8.0f) * (i % 2);
                const float y = kDetailsTop + fail_block + (kStatsH + 8.0f) * (i / 2);
                const auto box = Rect(s, x, y, half_w, kStatsH);
                painter_.FillRoundedRect(box, ScaleDip(s, 9.0f), well);
                painter_.StrokeRoundedRect(box, ScaleDip(s, 9.0f), well_line);
                painter_.DrawText(detail_labels[i], Rect(s, x + 12, y + 4, half_w - 24, 16),
                                  label_format_.get(), theme.text_secondary);
                DrawEllipsizedText(compositor_, value_format_.get(),
                                   Rect(s, x + 12, y + 20, half_w - 20, 20), detail_values[i], theme.text);
            }
        }
    }

    // --- Footer -----------------------------------------------------------
    const float footer_top = height - ScaleDip(s, kFooterH);
    painter_.FillRoundedRect(D2D1::RectF(0, footer_top, width, height), 0,
        OpFooter(hwnd_, backdrop_enabled_, dark_));
    painter_.FillRoundedRect(D2D1::RectF(0, footer_top, width, footer_top + 1.0f), 0, theme.stroke_divider);

    fluent::ControlState detail_state{};
    detail_state.hovered = hover_ == 5;
    detail_state.pressed = pressed_ == 5;
    if (labels.show_details) painter_.DrawButton({ chrome.details, labels.details,
        detailed_ ? L"\xE70E" : L"\xE70D", fluent::ButtonKind::Transparent, detail_state });
    if (labels.show_pause) {
        fluent::ControlState pause_state{};
        pause_state.enabled = status_.active && !failed && !cancelling && !waiting;
        pause_state.hovered = hover_ == 3;
        pause_state.pressed = pressed_ == 3;
        painter_.DrawButton({ chrome.pause, labels.pause, labels.pause_glyph,
                              paused ? fluent::ButtonKind::Primary : fluent::ButtonKind::Standard,
                              pause_state });
    }
    if (labels.show_retry) {
        fluent::ControlState retry_state{};
        retry_state.hovered = hover_ == 6;
        retry_state.pressed = pressed_ == 6;
        retry_state.enabled = labels.actions_enabled;
        painter_.DrawButton({ chrome.retry, labels.retry, L"\xEA18",
                              fluent::ButtonKind::Primary, retry_state });
    }
    if (labels.show_skip) {
        fluent::ControlState skip_state{};
        skip_state.hovered = hover_ == 7;
        skip_state.pressed = pressed_ == 7;
        skip_state.enabled = labels.actions_enabled;
        painter_.DrawButton({ chrome.skip, labels.skip, {},
                              fluent::ButtonKind::Standard, skip_state });
    }
    if (labels.show_open) {
        fluent::ControlState open_state{};
        open_state.hovered = hover_ == kHitOpen;
        open_state.pressed = pressed_ == kHitOpen;
        painter_.DrawButton({ chrome.open, labels.open, {}, fluent::ButtonKind::Standard, open_state });
    }
    fluent::ControlState cancel_state{};
    cancel_state.enabled = !cancelling;
    cancel_state.hovered = hover_ == 4;
    cancel_state.pressed = pressed_ == 4;
    painter_.DrawButton({ chrome.cancel, labels.cancel, {},
                          completed || labels.cancel_primary ? fluent::ButtonKind::Primary
                                                             : fluent::ButtonKind::Standard,
                          cancel_state });
    pulse::ui::EndSurface(compositor_);
}

LRESULT CALLBACK FileOperationWindow::WndProc(HWND hwnd, UINT message,
                                               WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<FileOperationWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<FileOperationWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    return self ? self->HandleMessage(message, wparam, lparam)
                : DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT FileOperationWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        scale_ = static_cast<float>(pulse::compat::WindowDpi(hwnd_)) / 96.0f;
        ApplyWindowTheme();
        if (!compositor_.Init(hwnd_)) return -1;
        compositor_.RecreateTextFormats(scale_);
        painter_.SetCompositor(&compositor_);
        painter_.SetScale(scale_);
        CreateCardFormats();
        // The window is created hidden and kept for reuse; its 33 ms
        // animation timer only runs while it is shown.
        return 0;
    case WM_SHOWWINDOW:
        if (wparam) SetTimer(hwnd_, kRenderTimer, 33, nullptr);
        else KillTimer(hwnd_, kRenderTimer);
        break;
    case WM_NCCALCSIZE:
        return 0;
    case WM_NCHITTEST: {
        const auto labels = MakeTransferLabels(status_, detailed_, FailureView());
        const auto chrome = MakeTransferChrome(scale_,
            static_cast<float>(compositor_.Width()), static_cast<float>(compositor_.Height()),
            painter_, labels);
        return pulse::ui::BorderlessHitTest(hwnd_, lparam, ScaleDip(scale_, kTitleH),
            D2D1::RectF(chrome.minimize.left, 0, chrome.close.right, chrome.close.bottom));
    }
    case WM_GETMINMAXINFO: {
        if (status_.authorization == ops::AuthorizationState::None) break;
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize = { static_cast<LONG>(kDlgW * scale_),
                                 static_cast<LONG>(kCollapsedH * scale_) };
        return 0;
    }
    case WM_SIZE:
        if (compositor_.Dc() && wparam != SIZE_MINIMIZED)
            compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
        if (wparam == SIZE_RESTORED && finish_unseen_) {
            // Finished while minimized: count down now that it is seen.
            finish_unseen_ = false;
            if (auto_close_ && !kept_ && !closing_ && !status_.active &&
                status_.phase == ops::OpPhase::Completed && status_.last_error.empty()) {
                countdown_base_ = GetTickCount64();
                SetTimer(hwnd_, kRenderTimer, 33, nullptr);
            }
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_ENTERSIZEMOVE:
        KeepOpen();
        break;
    case WM_MOVE:
        compositor_.UpdateTextRenderingParams(
            MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
        return 0;
    case WM_DPICHANGED: {
        scale_ = HIWORD(wparam) / 96.0f;
        compositor_.RecreateTextFormats(scale_);
        painter_.SetScale(scale_);
        CreateCardFormats();
        const auto* suggested = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_TIMER:
        if (wparam == kRenderTimer && !IsWindowVisible(hwnd_)) {
            KillTimer(hwnd_, kRenderTimer);
            return 0;
        }
        if (wparam == kCloseTimer) {
            FinishAutoClose();
            return 0;
        }
        if (wparam == kRenderTimer && has_held_status_ && GetTickCount64() >= hold_until_) {
            const ops::OpStatus held = held_status_;
            Update(held);
        }
        if (wparam == kRenderTimer) {
            const ULONGLONG now = GetTickCount64();
            if (closing_ || AutoClosePending()) {
                TickAutoClose(now);
                return 0;
            }
            // Only a running operation animates (speed graph, live dot); a
            // finished one is redrawn by Update and input alone.
            if (!status_.active) {
                KillTimer(hwnd_, kRenderTimer);
                return 0;
            }
            SampleGraph(now);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_MOUSEMOVE: {
        const int next = HitTestControl(static_cast<float>(GET_X_LPARAM(lparam)),
                                        static_cast<float>(GET_Y_LPARAM(lparam)));
        if (next != hover_) { hover_ = next; InvalidateRect(hwnd_, nullptr, FALSE); }
        TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
        TrackMouseEvent(&track);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (board_ && board_->rows.size() > kFailMaxRows) {
            const short delta = GET_WHEEL_DELTA_WPARAM(wparam);
            const size_t last = board_->rows.size() - kFailMaxRows;
            if (delta > 0 && board_scroll_ > 0) --board_scroll_;
            else if (delta < 0 && board_scroll_ < last) ++board_scroll_;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_MOUSELEAVE:
        hover_ = 0;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_RBUTTONDOWN:
        KeepOpen();
        return 0;
    case WM_LBUTTONDOWN:
        pressed_ = HitTestControl(static_cast<float>(GET_X_LPARAM(lparam)),
                                  static_cast<float>(GET_Y_LPARAM(lparam)));
        // Closing (1) and minimizing (2) are not a reason to keep the card.
        if (pressed_ != 1 && pressed_ != 2) KeepOpen();
        SetCapture(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        const int hit = HitTestControl(static_cast<float>(GET_X_LPARAM(lparam)),
                                       static_cast<float>(GET_Y_LPARAM(lparam)));
        const int pressed = pressed_;
        pressed_ = 0;
        ReleaseCapture();
        if (hit == pressed) {
            const auto labels = MakeTransferLabels(status_, detailed_, FailureView());
            if (hit == 4 && labels.cancel_retries && board_) {
                std::vector<size_t> rows(board_->rows.size());
                for (size_t i = 0; i < rows.size(); ++i) rows[i] = i;
                RetryFailureRows(rows);
            } else if (hit == 1 || hit == 4) {
                RequestClose();
            } else if (hit == kHitOpen && callbacks_.open_folder && !status_.destination_dir.empty()) {
                callbacks_.open_folder(status_.destination_dir);
            } else if (hit >= kHitFailRow && board_) {
                RetryFailureRows({ board_scroll_ + static_cast<size_t>(hit - kHitFailRow) });
            } else if (hit == 2) ShowWindow(hwnd_, SW_MINIMIZE);
            else if (hit == 3 && status_.active &&
                     status_.phase != ops::OpPhase::WaitingForConflict &&
                     status_.phase != ops::OpPhase::Cancelling) {
                if (ShowsPaused(status_)) {
                    if (callbacks_.resume) callbacks_.resume();
                } else if (callbacks_.pause) callbacks_.pause();
            } else if ((hit == 6 || hit == 7) && status_.active &&
                       status_.authorization == ops::AuthorizationState::ActionRequired &&
                       status_.phase != ops::OpPhase::Cancelling) {
                if (hit == 6 && callbacks_.retry_authorization)
                    callbacks_.retry_authorization(status_.task_id);
                else if (hit == 7 && status_.can_skip_authorization && callbacks_.skip_authorization)
                    callbacks_.skip_authorization(status_.task_id);
            } else if (hit == 5) {
                detailed_ = !detailed_;
                ResizeForDetails(true);
            }
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            RequestClose();
            return 0;
        }
        KeepOpen();
        break;
    case WM_CLOSE:
        RequestClose();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd_, &paint);
        Render();
        EndPaint(hwnd_, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        KillTimer(hwnd_, kRenderTimer);
        compositor_.Shutdown();
        hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

ConflictDialogResult ShowFileConflictDialog(HWND owner,
                                            const ops::ConflictItemInfo& conflict,
                                            bool dark,
                                            D2D1_COLOR_F accent) {
    ConflictWindow window;
    return window.Show(owner, conflict, dark, accent);
}

PermanentDeleteChoice ShowPermanentDeleteDialog(HWND owner, const PermanentDeleteInfo& info,
                                                bool dark, D2D1_COLOR_F accent) {
    PermanentDeleteWindow window;
    return window.Show(owner, info, dark, accent);
}

LockedItemChoice ShowLockedItemDialog(HWND owner, const LockedItemInfo& info, bool dark,
                                      D2D1_COLOR_F accent) {
    LockedItemWindow window;
    return window.Show(owner, info, dark, accent);
}


} // namespace pulse::ui
