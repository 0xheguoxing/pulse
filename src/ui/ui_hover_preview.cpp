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
constexpr float kCardWidth = 320.0f;
constexpr float kRadius = 10.0f;
constexpr float kInset = 6.0f;        // around the preview well
constexpr float kWellHeight = 180.0f;
constexpr float kTile = 40.0f;        // icon tile
constexpr float kGap = 10.0f;
constexpr float kStatsHeight = 46.0f;
constexpr float kBottom = 12.0f;
constexpr float kCardHeight = kInset + kWellHeight + kGap + kTile + kGap + kStatsHeight + kBottom;

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
    ID2D1DeviceContext* dc = compositor_->Dc();
    const float s = scale_;
    if (!view.visible) {
        stop_video();
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
    const float width = (std::min)(kCardWidth * s, client.right - client.left - 2.0f * margin);
    const float height = kCardHeight * s;
    if (width < 240.0f * s || height > client.bottom - client.top - 2.0f * margin) {
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
    const D2D1_RECT_F well = D2D1::RectF(card.left + kInset * s, card.top + kInset * s,
                                         card.right - kInset * s, card.top + (kInset + kWellHeight) * s);
    fill(well, (kRadius - kInset * 0.5f) * s, palette.hover);
    std::vector<PreviewProperty> properties;
    hover_cache_.Properties(view.path, view.attrs, 1, view.modified, view.size, properties);
    const D2D1_RECT_F content = D2D1::RectF(well.left + 4.0f * s, well.top + 4.0f * s,
                                            well.right - 4.0f * s, well.bottom - 4.0f * s);
    std::wstring preview_text;
    const PreviewDrawResult result = hover_cache_.Draw(dc, content, view.path, view.attrs, 1024u, 1,
        view.modified, view.size, 1.0f, &preview_text, nullptr, nullptr, true);
    const bool textual = result == PreviewDrawResult::Text || result == PreviewDrawResult::Hex ||
                         result == PreviewDrawResult::Markdown;
    if (textual) {
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
            const std::wstring excerpt = Excerpt(preview_text, 14);
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

    // Icon tile, name and type line.
    const float info_top = well.bottom + kGap * s;
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

}  // namespace pulse::ui
