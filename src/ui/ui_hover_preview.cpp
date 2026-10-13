// ui_hover_preview.cpp - The hover preview card (HoverPreviewView): an
// acrylic flyout beside the hovered file with its preview, an icon tile with
// the name and type, and a strip of three key facts.
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "ffmpeg_playback.h"
#include "snack_style.h"
#include "typography.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <string>
#include <vector>

namespace pulse::ui {
namespace {

// Card geometry in DIPs.
constexpr float kRadius = 10.0f;
constexpr float kInset = 6.0f;        // around the preview well
constexpr float kTile = 40.0f;        // icon tile
constexpr float kGap = 10.0f;
constexpr float kStatsHeight = 46.0f;
constexpr float kBottom = 12.0f;
constexpr float kInfoHeight = kGap + kTile + kGap + kStatsHeight + kBottom;  // under the well
constexpr float kBareTop = 14.0f;     // no preview: the tile starts the card
constexpr float kMinCardWidth = 320.0f;  // the name, the type line and the three facts
constexpr float kMinWellHeight = 96.0f;
// 卡片尺寸 小 / 中 / 大: the base edge the shape is measured from.
constexpr float kBaseEdge[3] = {280.0f, 380.0f, 500.0f};
constexpr float kZoom = 1.75f;        // Ctrl held

// What the card shows, and so its shape.
enum HoverShape : int {
    kShapeGuess = 0,    // nothing decoded yet: a shape from the extension
    kShapePicture = 1,  // a bitmap (photo, page sketch, poster, video)
    kShapeText = 2,     // the first lines of a text file
    kShapeBare = 3,     // no preview: the icon tile, name and facts only
};

struct Fact {
    std::wstring label, value;
};

FILETIME ToFileTime(uint64_t value) {
    FILETIME time{};
    time.dwLowDateTime = static_cast<DWORD>(value & 0xFFFFFFFFull);
    time.dwHighDateTime = static_cast<DWORD>(value >> 32);
    return time;
}

std::wstring Extension(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 >= name.size()) return {};
    std::wstring ext = name.substr(dot + 1);
    for (auto& c : ext) c = static_cast<wchar_t>(towupper(c));
    return ext;
}

// Up to two facts the preview host read from the file (尺寸, 时长, 页数 ...),
// then its size; the modified date and the format fill a short list.
std::vector<Fact> Facts(const HoverPreviewView& view, const std::vector<PreviewProperty>& properties) {
    std::vector<Fact> facts;
    for (const auto& property : properties) {
        if (facts.size() >= 2) break;
        if (property.label.empty() || property.value.empty() || property.value.size() > 24) continue;
        facts.push_back({property.label, property.value});
    }
    facts.push_back({l10n::Pick(L"大小", L"Size"), format::ByteSize(view.size)});
    if (facts.size() < 3 && view.modified) {
        std::wstring date = format::LocalFileTime(ToFileTime(view.modified));
        if (date.size() > 10) date.resize(10);  // the day; the type line has the time
        if (!date.empty()) facts.push_back({l10n::Pick(L"修改日期", L"Modified"), date});
    }
    if (facts.size() < 3) {
        const std::wstring ext = Extension(view.name);
        facts.push_back({l10n::Pick(L"格式", L"Format"), ext.empty() ? std::wstring(L"-") : ext});
    }
    return facts;
}

// Word / Excel / PowerPoint / WPS / OpenDocument, by extension.
bool OfficeDocument(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = name.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    for (const wchar_t* known : {L".doc", L".docx", L".docm", L".dotx", L".rtf", L".wps", L".xls", L".xlsx",
                                 L".xlsm", L".et", L".ppt", L".pptx", L".pptm", L".pps", L".ppsx", L".dps",
                                 L".odt", L".ods", L".odp", L".odg"})
        if (ext == known) return true;
    return false;
}

// HEIF photos carry a small thumbnail the host returns in about 60 ms; the
// card asks for it first (kHeifQuickEdge) and for the photo once it is in.
constexpr uint32_t kHeifQuickEdge = 320;
bool HeifPhoto(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = name.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    return ext == L".heic" || ext == L".heif" || ext == L".hif" || ext == L".avif";
}

// The first guess at a file's proportions, before its preview is decoded:
// pages are A4 portrait, slides and clips 16:9, sheets 4:3.
float GuessAspect(const std::wstring& name, bool video) {
    if (video) return 16.0f / 9.0f;
    const size_t dot = name.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? std::wstring() : name.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    for (const wchar_t* page : {L".doc", L".docx", L".docm", L".dotx", L".rtf", L".wps", L".odt", L".pdf",
                                L".epub", L".xps", L".oxps", L".djvu"})
        if (ext == page) return 210.0f / 297.0f;
    for (const wchar_t* slide : {L".ppt", L".pptx", L".pptm", L".pps", L".ppsx", L".dps", L".odp"})
        if (ext == slide) return 16.0f / 9.0f;
    for (const wchar_t* sheet : {L".xls", L".xlsx", L".xlsm", L".et", L".ods"})
        if (ext == sheet) return 4.0f / 3.0f;
    return 16.0f / 10.0f;
}

// Files whose preview is only ever a hex dump or a listing (programs,
// archives) or only sometimes a picture (audio cover art, which grows the
// card when it arrives): the compact card from the first frame, rather than a picture-shaped card that shrinks once the
// answer arrives (browsing opens cards before the host has answered).
bool GuessBare(const std::wstring& path, const std::wstring& name) {
    if (VideoPreview::IsAudio(path)) return true;
    const size_t dot = name.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? std::wstring() : name.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    for (const wchar_t* bare : {L".exe", L".dll", L".msi", L".sys", L".zip", L".rar", L".7z", L".cab",
                                L".iso", L".tar", L".gz", L".tgz", L".bz2", L".xz", L".zst"})
        if (ext == bare) return true;
    return false;
}

// The first lines of a text preview, tabs as four spaces: enough to fill
// the well, never a whole file through DirectWrite every frame.
std::wstring Excerpt(const std::wstring& text, int max_lines) {
    std::wstring out;
    int lines = 0;
    for (size_t i = 0; i < text.size() && out.size() < 3000; ++i) {
        const wchar_t c = text[i];
        if (c == L'\n' && ++lines >= max_lines) break;
        if (c == L'\t') out += L"    ";
        else out += c;
    }
    return out;
}

}  // namespace

VideoPreview::State MainRenderer::HoverVideoState() const {
    return hover_video_ ? hover_video_->Snapshot() : VideoPreview::State{};
}

void MainRenderer::DrawHoverPreview(const HoverPreviewView& view, const D2D1_RECT_F& client,
                                    const Theme& theme, bool high_contrast) {
    hover_frame_ms_ = 0;
    const auto stop_video = [&] {
        if (hover_video_ && hover_video_->active()) hover_video_->Reset();
        hover_video_path_.clear();
        hover_video_failed_ = false;
        hover_frame_serial_ = ~0ull;
    };
    if (!compositor_ || !compositor_->Dc()) {
        stop_video();
        return;
    }
    if (hover_bench_) return;
    ID2D1DeviceContext* dc = compositor_->Dc();
    const float s = scale_;
    if (!view.visible) {
        stop_video();
        hover_card_w_ = hover_well_h_ = 0.0f;  // the next card opens at its size
        // The dwell on a slow format: an accent line fills the row's foot
        // until the card opens.
        if (view.dwell && view.dwell_row.right - view.dwell_row.left > 24.0f * s) {
            const SnackPalette palette = MakeSnackPalette(theme, high_contrast);
            ComPtr<ID2D1SolidColorBrush> line;
            if (SUCCEEDED(dc->CreateSolidColorBrush(palette.accent, &line)) && line.get()) {
                const D2D1_RECT_F row = view.dwell_row;
                const float left = row.left + 6.0f * s, right = row.right - 6.0f * s;
                const float p = std::clamp(view.dwell_progress, 0.0f, 1.0f);
                if (p > 0.0f) {
                    line->SetOpacity(0.9f);
                    const D2D1_RECT_F r = D2D1::RectF(left, row.bottom - 2.5f * s, left + (right - left) * p,
                                                      row.bottom - 0.5f * s);
                    dc->FillRoundedRectangle(D2D1::RoundedRect(r, 1.0f * s, 1.0f * s), line.get());
                }
            }
            hover_frame_ms_ = 16;
        }
        return;
    }
    const float margin = 8.0f * s;
    const float client_w = client.right - client.left, client_h = client.bottom - client.top;

    // The shape: what earlier frames learned of this file's preview, else a
    // guess from its extension.
    if (hover_shape_path_ != view.path) {
        hover_shape_path_ = view.path;
        hover_shape_kind_ = !view.video && GuessBare(view.path, view.name) ? kShapeBare : kShapeGuess;
        hover_shape_aspect_ = GuessAspect(view.name, view.video);
        hover_card_w_ = hover_well_h_ = 0.0f;
    }
    if (view.video && hover_shape_kind_ == kShapeBare) hover_shape_kind_ = kShapeGuess;  // the loop still plays
    const bool bare = hover_shape_kind_ == kShapeBare;
    const bool textual_shape = hover_shape_kind_ == kShapeText;
    const int laid_kind = hover_shape_kind_;
    const float laid_aspect = hover_shape_aspect_;

    // Its size: the 小/中/大 base edge (much larger while Ctrl is held),
    // never more than 45% of the window's width and 80% of its height.
    float base = kBaseEdge[std::clamp(view.size_step, 0, 2)] * s;
    float max_w = client_w * 0.45f, max_h = client_h * 0.8f;
    if (view.zoom) {
        base *= kZoom;
        max_w = client_w * 0.7f;
        max_h = client_h * 0.92f;
    }
    const float min_w = (std::min)(kMinCardWidth * s, client_w - 2.0f * margin);
    max_w = (std::max)(max_w, min_w);
    const float pad = 2.0f * kInset * s;
    const float max_well_h = max_h - (kInset + kInfoHeight) * s;
    float target_w = min_w, target_h = 0.0f;
    if (!bare) {
        float ww = 0.0f, wh = 0.0f;
        const float a = (std::max)(hover_shape_aspect_, 0.2f);
        if (textual_shape) {
            ww = (std::min)(base * 1.1f, max_w) - pad;
            wh = (std::min)(base * 0.66f, max_well_h);
        } else if (a >= 1.15f) {          // landscape: as wide as the step allows
            ww = (std::min)(base * 1.25f, max_w) - pad;
            wh = (std::min)(ww / a, max_well_h);
        } else if (a <= 0.87f) {          // portrait: as tall as the step allows
            wh = (std::min)(base * 0.95f, max_well_h);
            ww = wh * a;
            if (ww > max_w - pad) {
                ww = max_w - pad;
                wh = ww / a;
            }
        } else {                          // about square
            ww = (std::min)(base, max_w) - pad;
            wh = (std::min)(ww / a, max_well_h);
        }
        target_w = std::clamp(ww + pad, min_w, max_w);
        target_h = (std::max)(wh, kMinWellHeight * s);
    }

    // Eased toward a new shape (the preview arrived, Ctrl went down or up);
    // a new card opens at its size.
    const unsigned long long now = GetTickCount64();
    if (view.instant || hover_card_w_ <= 0.0f) {
        hover_card_w_ = target_w;
        hover_well_h_ = target_h;
    } else {
        const float dt = static_cast<float>((std::min)(now - hover_ease_ms_, 100ull));
        const float k = 1.0f - std::exp(-dt / 45.0f);
        hover_card_w_ += (target_w - hover_card_w_) * k;
        hover_well_h_ += (target_h - hover_well_h_) * k;
        if (std::fabs(target_w - hover_card_w_) < 0.5f) hover_card_w_ = target_w;
        if (std::fabs(target_h - hover_well_h_) < 0.5f) hover_well_h_ = target_h;
    }
    hover_ease_ms_ = now;
    const bool easing = hover_card_w_ != target_w || hover_well_h_ != target_h;
    const float width = std::round(hover_card_w_);
    const float well_h = std::round(hover_well_h_);
    const bool show_well = well_h >= 1.0f;
    const float height = show_well ? kInset * s + well_h + kInfoHeight * s
                                   : (kBareTop + kTile + kGap + kStatsHeight + kBottom) * s;
    if (width < 240.0f * s || height > client_h - 2.0f * margin) {
        stop_video();
        return;
    }

    // Beside the pointer on wide rows (details, list), beside the cell in
    // icon grids; flipped to the other side when it would leave the window.
    const float anchor_w = view.anchor.right - view.anchor.left;
    const float anchor_h = view.anchor.bottom - view.anchor.top;
    float x = 0.0f, y = 0.0f;
    if (anchor_w > anchor_h * 3.0f) {
        x = view.pointer.x + 20.0f * s;
        if (x + width > client.right - margin) x = view.pointer.x - 20.0f * s - width;
        y = (view.anchor.top + view.anchor.bottom - height) * 0.5f;
    } else {
        x = view.anchor.right + 8.0f * s;
        if (x + width > client.right - margin) x = view.anchor.left - 8.0f * s - width;
        y = view.anchor.top;
    }
    x = std::round(std::clamp(x, client.left + margin, client.right - margin - width));
    y = std::round(std::clamp(y, client.top + margin, client.bottom - margin - height));
    const D2D1_RECT_F card = D2D1::RectF(x, y, x + width, y + height);

    const SnackPalette palette = MakeSnackPalette(theme, high_contrast);
    DrawSnackPlate(dc, card, kRadius * s, palette, s, high_contrast);
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(palette.hover, &brush)) || !brush.get()) return;
    const auto fill = [&](const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& color) {
        brush->SetColor(color);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.get());
    };
    const auto text = [&](std::wstring_view value, const D2D1_RECT_F& r, IDWriteTextFormat* format,
                          const D2D1_COLOR_F& color) {
        painter_.DrawText(value, r, format, color, fluent::HorizontalAlignment::Left, palette.plate);
    };

    // The preview well: the picture, the first lines of text, or the icon.
    // A file without a preview has none; the cache is still asked (unseen)
    // so a late picture can grow the card.
    const D2D1_RECT_F well = show_well
        ? D2D1::RectF(card.left + kInset * s, card.top + kInset * s, card.right - kInset * s,
                      card.top + kInset * s + well_h)
        : D2D1::RectF(card.left + kInset * s, card.top, card.right - kInset * s, card.top);
    if (show_well) fill(well, (kRadius - kInset * 0.5f) * s, palette.hover);
    const D2D1_RECT_F content = show_well
        ? D2D1::RectF(well.left + 4.0f * s, well.top + 4.0f * s, well.right - 4.0f * s, well.bottom - 4.0f * s)
        : D2D1::RectF(card.left, card.top, card.left + 1.0f, card.top + 1.0f);
    uint32_t decoded_w = 0, decoded_h = 0;
    std::wstring preview_text;
    // Office documents ask for the thumbnail kind of preview: the embedded
    // or shell thumbnail, else the first-page sketch. The details kind only
    // tries the shell provider and leaves the icon when there is none (.doc
    // without Office); Quick Look shows those through the preview handler,
    // too slow and focus-hungry for a card.
    // Audio asks as well: the host reads the cover from the tags in a few ms
    // and, for the card (kPreviewRequestFlagCard), fails at once when there
    // is none instead of trying the shell and a hex dump.
    // HEIF: the embedded thumbnail first, then the photo; while the photo
    // decodes the cache keeps drawing the thumbnail (a stale size).
    const bool heif_wait = HeifPhoto(view.name) &&
        hover_cache_.Draw(dc, content, view.path, view.attrs, kHeifQuickEdge, 1, view.modified, view.size, 0.0f,
            nullptr, nullptr, nullptr, true) == PreviewDrawResult::Pending;
    const PreviewDrawResult result = heif_wait ? PreviewDrawResult::Pending :
        hover_cache_.Draw(dc, content, view.path, view.attrs, 1024u, 1,
        view.modified, view.size, show_well ? 1.0f : 0.0f, &preview_text, nullptr, nullptr,
        !OfficeDocument(view.name), nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr, nullptr, nullptr,
        &decoded_w, &decoded_h);
    const bool textual = result == PreviewDrawResult::Text || result == PreviewDrawResult::Hex ||
                         result == PreviewDrawResult::Markdown;
    // The facts after the preview: the host works one request at a time, and
    // a slow property read (video duration, HEIC metadata) asked for first
    // used to hold the picture back.
    std::vector<PreviewProperty> properties;
    hover_cache_.Properties(view.path, view.attrs, 1, view.modified, view.size, properties);
    // What this frame learned of the shape; the next frame eases to it.
    if (result == PreviewDrawResult::Bitmap && decoded_w && decoded_h) {
        hover_shape_kind_ = kShapePicture;
        if (!view.video || hover_shape_aspect_ <= 0.0f)
            hover_shape_aspect_ = static_cast<float>(decoded_w) / static_cast<float>(decoded_h);
    } else if (textual && result != PreviewDrawResult::Hex) {
        hover_shape_kind_ = kShapeText;
    } else if (result != PreviewDrawResult::Pending && !view.video) {
        // A hex dump (exe, dll, unknown binaries) tells nothing at a glance:
        // the compact card, like files with no preview at all.
        hover_shape_kind_ = kShapeBare;
    }
    if (!show_well) {
        // nothing to draw in a well that is not there
    } else if (textual) {
        if (!preview_mono_format_.get() && compositor_->DwriteFactory()) {
            typography::CreateTextFormat(compositor_->DwriteFactory(),
                {typography::FontRole::Monospace, 11.0f * s}, &preview_mono_format_);
            if (preview_mono_format_.get()) {
                preview_mono_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                preview_mono_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }
        }
        const D2D1_RECT_F lines = D2D1::RectF(content.left + 8.0f * s, content.top + 6.0f * s,
                                              content.right - 4.0f * s, content.bottom - 2.0f * s);
        if (preview_text.empty()) {
            text(l10n::Get(l10n::StringId::EmptyFile), lines, compositor_->SmallFormat(), palette.text_secondary);
        } else if (preview_mono_format_.get()) {
            const float line_h = 11.0f * s * 1.34f;
            const int max_lines = std::clamp(static_cast<int>((lines.bottom - lines.top) / line_h) + 1, 4, 80);
            const std::wstring excerpt = Excerpt(preview_text, max_lines);
            brush->SetColor(palette.text);
            dc->PushAxisAlignedClip(content, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            dc->DrawText(excerpt.data(), static_cast<UINT32>(excerpt.size()), preview_mono_format_.get(),
                         lines, brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            dc->PopAxisAlignedClip();
        }
    } else if (result != PreviewDrawResult::Bitmap) {
        // No picture of this file (yet): its icon, dimmed while the host works.
        const float icon = 64.0f * s;
        const float cx = (well.left + well.right) * 0.5f, cy = (well.top + well.bottom) * 0.5f;
        icon_cache_.Draw(dc, D2D1::RectF(cx - icon * 0.5f, cy - icon * 0.5f, cx + icon * 0.5f, cy + icon * 0.5f),
                         view.path, view.name, false, view.attrs,
                         result == PreviewDrawResult::Pending ? 0.45f : 1.0f);
    }

    // Videos: the poster above stays until the silent loop has a picture.
    bool loading = view.slow && result == PreviewDrawResult::Pending;
    const D2D1_RECT_F track = D2D1::RectF(well.left + 12.0f * s, well.bottom - 3.0f * s,
                                          well.right - 12.0f * s, well.bottom - 1.0f * s);
    if (view.video && view.owner) {
        // Opened only once the card has settled: browsing down a column of
        // clips must not spin up a player per row.
        const bool settled = GetTickCount64() - view.shown_ms >= 220;
        if (hover_video_path_ != view.path) {
            if (hover_video_ && hover_video_->active()) hover_video_->Reset();
            hover_video_path_.clear();
            hover_video_failed_ = false;
            hover_frame_serial_ = ~0ull;
            if (settled) {
                if (!hover_video_) hover_video_ = std::make_shared<VideoPreview>();
                hover_video_->Open(view.owner, view.path, true);
                hover_video_path_ = view.path;
            }
        }
        VideoPreview::State video;
        if (hover_video_ && hover_video_path_ == view.path) video = hover_video_->Snapshot();
        // No picture to show: an error, or Media Foundation would only play
        // the (muted) sound because the video decoder is missing.
        if (!hover_video_failed_ && hover_video_path_ == view.path &&
            (FAILED(video.error) || (video.ready && video.missing_decoder && !video.ffmpeg))) {
            hover_video_failed_ = true;
            hover_video_->Reset();
        }
        if (!hover_video_failed_ && video.ended) hover_video_->Play(true);  // loop
        if (video.width && video.height) {
            const float a = static_cast<float>(video.width) / static_cast<float>(video.height);
            if (std::fabs(a - hover_shape_aspect_) > 0.01f || hover_shape_kind_ != kShapePicture) {
                hover_shape_aspect_ = a;
                hover_shape_kind_ = kShapePicture;
            }
        }
        D2D1_RECT_F frame = content;
        if (video.width && video.height) {
            const float bw = content.right - content.left, bh = content.bottom - content.top;
            const float k = (std::min)(bw / static_cast<float>(video.width), bh / static_cast<float>(video.height));
            const float w = static_cast<float>(video.width) * k, h = static_cast<float>(video.height) * k;
            const float cx = (content.left + content.right) * 0.5f, cy = (content.top + content.bottom) * 0.5f;
            frame = D2D1::RectF(std::round(cx - w * 0.5f), std::round(cy - h * 0.5f),
                                std::round(cx + w * 0.5f), std::round(cy + h * 0.5f));
        }
        uint64_t serial = 0;
        std::shared_ptr<const FfmpegFrame> picture;
        if (video.ffmpeg && hover_video_) picture = hover_video_->Frame(serial);
        const bool showing = !hover_video_failed_ && video.ready && SUCCEEDED(video.error) &&
            (video.ffmpeg ? picture && picture->width && picture->height : video.position > 0);
        if (hover_video_ && hover_video_->active()) {
            const RECT bounds{std::lround(frame.left), std::lround(frame.top), std::lround(frame.right),
                              std::lround(frame.bottom)};
            hover_video_->Layout(bounds, showing && !video.ffmpeg, static_cast<int>(std::lround(5.0f * s)));
        }
        if (showing && video.ffmpeg) {
            ComPtr<ID2D1Device> device;
            dc->GetDevice(&device);
            const D2D1_SIZE_U size = D2D1::SizeU(picture->width, picture->height);
            bool upload = serial != hover_frame_serial_;
            if (!hover_frame_bitmap_.get() || hover_frame_device_.get() != device.get() ||
                hover_frame_bitmap_->GetPixelSize().width != size.width ||
                hover_frame_bitmap_->GetPixelSize().height != size.height) {
                hover_frame_bitmap_.reset();
                const auto props = D2D1::BitmapProperties(
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
                if (SUCCEEDED(dc->CreateBitmap(size, nullptr, 0, props, &hover_frame_bitmap_)))
                    hover_frame_device_ = std::move(device);
                upload = true;
            }
            if (hover_frame_bitmap_.get()) {
                if (upload) {
                    hover_frame_bitmap_->CopyFromMemory(nullptr, picture->pixels.data(), picture->width * 4);
                    hover_frame_serial_ = serial;
                }
                dc->DrawBitmap(hover_frame_bitmap_.get(), frame, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
        }
        if (showing) {
            // Where the loop is, in a hairline under the picture.
            const double fraction = video.duration > 0
                ? std::clamp(static_cast<double>(video.position) / static_cast<double>(video.duration), 0.0, 1.0)
                : 0.0;
            fill(track, 1.0f * s, palette.divider);
            if (fraction > 0.0) {
                fill(D2D1::RectF(track.left, track.top,
                                 track.left + (track.right - track.left) * static_cast<float>(fraction), track.bottom),
                     1.0f * s, palette.accent);
            }
            hover_frame_ms_ = 200;  // progress, and the loop's restart
        } else if (!hover_video_failed_) {
            loading = true;
        }
    } else if (hover_video_path_.size() || (hover_video_ && hover_video_->active())) {
        stop_video();
    }
    if (loading) {
        // Still decoding: a short accent segment sweeps the track.
        fill(track, 1.0f * s, palette.divider);
        const float span = track.right - track.left, seg = span * 0.3f;
        const float t = static_cast<float>(GetTickCount64() % 1200ull) / 1200.0f;
        const float head = track.left - seg + (span + seg) * t;
        dc->PushAxisAlignedClip(track, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        fill(D2D1::RectF(head, track.top, head + seg, track.bottom), 1.0f * s, palette.accent);
        dc->PopAxisAlignedClip();
        hover_frame_ms_ = 33;
    }

    // Still easing, or this frame learned a new shape: another frame soon.
    if (easing || laid_kind != hover_shape_kind_ || std::fabs(laid_aspect - hover_shape_aspect_) > 0.01f)
        hover_frame_ms_ = hover_frame_ms_ ? (std::min)(hover_frame_ms_, 16u) : 16u;

    // Icon tile, name and type line.
    const float info_top = show_well ? well.bottom + kGap * s : card.top + kBareTop * s;
    const D2D1_RECT_F tile = D2D1::RectF(card.left + 14.0f * s, info_top,
                                         card.left + (14.0f + kTile) * s, info_top + kTile * s);
    fill(tile, 9.0f * s, palette.hover);
    brush->SetColor(palette.stroke);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tile.left + 0.5f, tile.top + 0.5f,
        tile.right - 0.5f, tile.bottom - 0.5f), 9.0f * s, 9.0f * s), brush.get(), 1.0f);
    icon_cache_.Draw(dc, D2D1::RectF(tile.left + 6.0f * s, tile.top + 6.0f * s,
                                     tile.right - 6.0f * s, tile.bottom - 6.0f * s),
                     view.path, view.name, false, view.attrs);
    const float text_left = tile.right + 12.0f * s, text_right = card.right - 14.0f * s;
    IDWriteTextFormat* name_format = compositor_->FileNameFormat() ? compositor_->FileNameFormat()
                                                                   : compositor_->TextFormat();
    text(view.name, D2D1::RectF(text_left, tile.top - 3.0f * s, text_right, tile.top + 21.0f * s),
         name_format, palette.text);
    std::wstring type_line = view.type_text;
    if (view.modified) {
        const std::wstring when = format::LocalFileTime(ToFileTime(view.modified));
        if (!when.empty()) type_line += (type_line.empty() ? L"" : L" · ") + when;
    }
    text(type_line, D2D1::RectF(text_left, tile.top + 21.0f * s, text_right, tile.bottom + 3.0f * s),
         compositor_->SmallFormat(), palette.text_secondary);

    // Three facts in a quiet strip: label above value.
    const D2D1_RECT_F strip = D2D1::RectF(card.left + 12.0f * s, tile.bottom + kGap * s,
                                          card.right - 12.0f * s, tile.bottom + (kGap + kStatsHeight) * s);
    fill(strip, 8.0f * s, palette.hover);
    const std::vector<Fact> facts = Facts(view, properties);
    const float cell = (strip.right - strip.left) / 3.0f;
    for (size_t i = 0; i < 3 && i < facts.size(); ++i) {
        const float left = strip.left + cell * static_cast<float>(i);
        if (i) {
            brush->SetColor(palette.divider);
            dc->FillRectangle(D2D1::RectF(left, strip.top + 10.0f * s, left + 1.0f, strip.bottom - 10.0f * s),
                              brush.get());
        }
        text(facts[i].label, D2D1::RectF(left + 10.0f * s, strip.top + 5.0f * s, left + cell - 6.0f * s,
                                         strip.top + 22.0f * s),
             compositor_->SmallFormat(), palette.text_secondary);
        text(facts[i].value, D2D1::RectF(left + 10.0f * s, strip.top + 21.0f * s, left + cell - 6.0f * s,
                                         strip.bottom - 2.0f * s),
             compositor_->TextFormat(), palette.text);
    }
}

namespace {
struct BenchClock {
    LARGE_INTEGER frequency{}, start{};
    BenchClock() {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
    }
    double Ms() const {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return static_cast<double>(now.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
    }
};
}  // namespace

MainRenderer::HoverBenchSample MainRenderer::BenchHoverPreview(const std::wstring& path, DWORD attrs,
        uint64_t size, uint64_t modified, uint32_t pixel_size, uint32_t timeout_ms, bool properties_first) {
    HoverBenchSample sample;
    if (!compositor_ || !compositor_->Dc()) return sample;
    ID2D1DeviceContext* dc = compositor_->Dc();
    ComPtr<ID2D1Bitmap1> target;
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(512, 512), nullptr, 0, props, &target)) || !target.get()) return sample;
    ComPtr<ID2D1Image> previous;
    dc->GetTarget(&previous);
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const bool direct = !OfficeDocument(name);
    // The card asks at 1024 px; any other size is the warm run: the cache
    // is dropped first so the file is decoded again by the running host.
    const bool warm_run = pixel_size != 1024u;
    if (warm_run) hover_cache_.Evict();
    std::vector<PreviewProperty> properties;
    const BenchClock clock;
    // HEIF, as the card: the embedded thumbnail, timed on its own.
    if (!warm_run && HeifPhoto(name)) {
        for (;;) {
            dc->SetTarget(target.get());
            dc->BeginDraw();
            const PreviewDrawResult quick = hover_cache_.Draw(dc, D2D1::RectF(4.0f, 4.0f, 380.0f, 380.0f), path,
                attrs, kHeifQuickEdge, 1, modified, size, 1.0f, nullptr, nullptr, nullptr, true);
            dc->EndDraw();
            dc->SetTarget(previous.get());
            if (quick != PreviewDrawResult::Pending) {
                if (quick == PreviewDrawResult::Bitmap) sample.first_ms = clock.Ms();
                break;
            }
            if (clock.Ms() > static_cast<double>(timeout_ms)) break;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 1, QS_ALLINPUT);
        }
    }
    for (;;) {
        if (properties_first && sample.properties_ms < 0.0 &&
            hover_cache_.Properties(path, attrs, 1, modified, size, properties))
            sample.properties_ms = clock.Ms();
        dc->SetTarget(target.get());
        dc->BeginDraw();
        std::wstring text;
        uint32_t w = 0, h = 0;
        const double before = clock.Ms();
        const PreviewDrawResult result = hover_cache_.Draw(dc, D2D1::RectF(4.0f, 4.0f, 380.0f, 380.0f), path, attrs,
            pixel_size, 1, modified, size, 1.0f, &text, nullptr, nullptr, direct, nullptr, nullptr, nullptr, nullptr,
            nullptr, 0, nullptr, nullptr, nullptr, &w, &h);
        dc->EndDraw();
        dc->SetTarget(previous.get());
        // The thumbnail drawn in the photo's place is not the photo.
        const bool stand_in = sample.first_ms >= 0.0 && result == PreviewDrawResult::Bitmap &&
                              (std::max)(w, h) <= kHeifQuickEdge && pixel_size > kHeifQuickEdge;
        if (!properties_first && sample.properties_ms < 0.0 &&
            hover_cache_.Properties(path, attrs, 1, modified, size, properties))
            sample.properties_ms = clock.Ms();
        if (result != PreviewDrawResult::Pending && !stand_in) {
            sample.content_ms = clock.Ms();
            sample.upload_ms = sample.content_ms - before;
            sample.result = static_cast<int>(result);
            sample.width = w;
            sample.height = h;
            break;
        }
        if (clock.Ms() > static_cast<double>(timeout_ms)) {
            sample.content_ms = clock.Ms();
            break;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 1, QS_ALLINPUT);
    }
    // The facts strip may fill after the picture: how much later (2 s cap).
    while (!warm_run && sample.properties_ms < 0.0 && clock.Ms() < sample.content_ms + 2000.0) {
        if (hover_cache_.Properties(path, attrs, 1, modified, size, properties)) sample.properties_ms = clock.Ms();
        else MsgWaitForMultipleObjects(0, nullptr, FALSE, 2, QS_ALLINPUT);
    }
    return sample;
}

int MainRenderer::PrefetchHoverPreview(const std::wstring& path, DWORD attrs, uint64_t size, uint64_t modified,
                                       bool photo_first) {
    if (path.empty() || !compositor_ || !compositor_->Dc()) return -1;
    ID2D1DeviceContext* dc = compositor_->Dc();
    // The same request the card makes (size, kind), queued through a draw
    // into a throwaway target; a cached picture is drawn there unseen.
    ComPtr<ID2D1Bitmap1> target;
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(1, 1), nullptr, 0, props, &target)) || !target.get()) return -2;
    ComPtr<ID2D1Image> previous;
    dc->GetTarget(&previous);
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    dc->SetTarget(target.get());
    dc->BeginDraw();
    // HEIF: the thumbnail first, as the card does; the photo follows once
    // the card (or a second prefetch) sees the thumbnail in.
    PreviewDrawResult result = photo_first && HeifPhoto(name)
        ? hover_cache_.Draw(dc, D2D1::RectF(0.0f, 0.0f, 1.0f, 1.0f), path, attrs, kHeifQuickEdge,
            1, modified, size, 0.0f, nullptr, nullptr, nullptr, true)
        : PreviewDrawResult::Failed;
    if (result != PreviewDrawResult::Pending)
        result = hover_cache_.Draw(dc, D2D1::RectF(0.0f, 0.0f, 1.0f, 1.0f), path, attrs, 1024u,
            1, modified, size, 0.0f, nullptr, nullptr, nullptr, !OfficeDocument(name));
    dc->EndDraw();
    dc->SetTarget(previous.get());
    return static_cast<int>(result);
}

double MainRenderer::BenchHoverVideo(HWND owner, const std::wstring& path, uint32_t timeout_ms, double* ready_ms) {
    if (ready_ms) *ready_ms = -1.0;
    if (!hover_video_) hover_video_ = std::make_shared<VideoPreview>();
    if (hover_video_->active()) hover_video_->Reset();
    const BenchClock clock;
    hover_video_->Open(owner, path, true);
    double first = -1.0;
    bool laid_out = false;
    for (;;) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const VideoPreview::State state = hover_video_->Snapshot();
        if (ready_ms && *ready_ms < 0.0 && state.ready) *ready_ms = clock.Ms();
        if (!laid_out && hover_video_->active()) {
            hover_video_->Layout(RECT{16, 16, 656, 376}, false, 5);
            laid_out = true;
        }
        if (FAILED(state.error) || (state.ready && state.missing_decoder && !state.ffmpeg)) break;
        bool picture = false;
        if (state.ffmpeg) {
            uint64_t serial = 0;
            const auto frame = hover_video_->Frame(serial);
            picture = frame && frame->width && frame->height;
        } else {
            picture = state.ready && state.position > 0;
        }
        if (picture) {
            first = clock.Ms();
            break;
        }
        if (clock.Ms() > static_cast<double>(timeout_ms)) break;
        Sleep(2);
    }
    hover_video_->Reset();
    hover_video_path_.clear();
    return first;
}

}  // namespace pulse::ui
