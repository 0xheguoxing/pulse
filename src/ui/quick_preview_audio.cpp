// Quick Look audio preview: cover art, tags and a waveform above the shared
// playback bar. Playback itself is the MFPlay VideoPreview (audio-only files
// never show its child window); the waveform comes from AudioWaveform.
#include "quick_preview_window.h"
#include "../ipc/preview_protocol.h"
#include "../common/localization.h"
#include "cover_palette.h"
#include <d2d1helper.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iterator>

namespace pulse::ui {
namespace {
bool Inside(const D2D1_RECT_F& rect, POINT point) {
    return point.x >= rect.left && point.x < rect.right &&
        point.y >= rect.top && point.y < rect.bottom;
}

std::wstring PropertyValue(const std::vector<PreviewProperty>& properties, const wchar_t* label) {
    for (const auto& property : properties)
        if (property.label == label) return property.value;
    return {};
}

}  // namespace

bool QuickPreviewWindow::IsAudioPreview() const {
    return video_.active() && VideoPreview::IsAudio(item_.path);
}

bool QuickPreviewWindow::AudioHover(POINT client) {
    const float x = IsAudioPreview() && Inside(audio_wave_rect_, client) ? static_cast<float>(client.x) : -1.0f;
    const bool changed = (x < 0) != (audio_hover_x_ < 0) || std::abs(x - audio_hover_x_) >= 1.0f;
    audio_hover_x_ = x;
    return changed;
}

bool QuickPreviewWindow::AudioMouseDown(POINT client) {
    if (!IsAudioPreview() || !Inside(audio_wave_rect_, client)) return false;
    const float width = (std::max)(1.0f, audio_wave_rect_.right - audio_wave_rect_.left);
    video_.Seek(std::clamp((static_cast<float>(client.x) - audio_wave_rect_.left) / width, 0.0f, 1.0f));
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void QuickPreviewWindow::DrawAudio(ID2D1DeviceContext* dc, const VideoPreview::State& state,
                                   ID2D1SolidColorBrush* text_brush,
                                   ID2D1SolidColorBrush* secondary_brush) {
    (void)text_brush;
    (void)secondary_brush;
    if (!dc) return;
    const D2D1_RECT_F content = ContentRect();
    const float s = scale_;
    const float w = content.right - content.left;
    const float h = content.bottom - content.top;
    const float pad = 24.0f * s;
    const float title_h = 28.0f * s;
    const float line_h = 20.0f * s;
    const float wave_h = 52.0f * s;
    const float tip_h = 22.0f * s;   // room for the time bubble above the waveform
    const ChromePalette palette = Palette();

    std::vector<PreviewProperty> properties;
    thumbnails_.Properties(item_.path, item_.attrs, generation_, item_.modified, item_.size,
                           properties);
    std::wstring title = PropertyValue(properties, L"标题");
    if (title.empty()) {
        title = item_.name;
        if (const size_t dot = title.find_last_of(L'.'); dot != std::wstring::npos && dot > 0)
            title.resize(dot);
    }
    std::wstring subtitle = PropertyValue(properties, L"艺术家");
    if (const std::wstring album = PropertyValue(properties, L"专辑"); !album.empty())
        subtitle += (subtitle.empty() ? L"" : L" \x00B7 ") + album;
    std::wstring ext;
    if (const size_t dot = item_.name.find_last_of(L'.'); dot != std::wstring::npos) {
        ext = item_.name.substr(dot + 1);
        for (auto& c : ext) c = static_cast<wchar_t>(std::towupper(c));
    }
    // Footer: format, codec, length, then sample rate and channels.
    std::vector<std::wstring> facts;
    for (const wchar_t* label : {L"采样率", L"声道"})
        if (std::wstring value = PropertyValue(properties, label); !value.empty())
            facts.push_back(std::move(value));
    SetMediaFooter(state, std::move(facts));
    // The tag table beside the cover (wide windows only).
    std::vector<std::pair<std::wstring, std::wstring>> rows;
    for (const wchar_t* label : {L"比特率", L"采样率", L"声道", L"流派", L"年份"})
        if (std::wstring value = PropertyValue(properties, label); !value.empty())
            rows.emplace_back(pulse::l10n::ServiceText(label), std::move(value));

    // Wide: cover on the left, tags on the right, the waveform below both.
    // Narrow: everything stacked and centred.
    const float wide_cover = std::clamp(h - pad * 2.0f - tip_h - wave_h - 20.0f * s, 0.0f, 240.0f * s);
    const bool wide = w >= 620.0f * s && wide_cover >= 120.0f * s;
    const float text_block = 12.0f * s + title_h + line_h * 2.0f + 16.0f * s + tip_h + wave_h;
    const float cover = wide ? wide_cover
        : std::clamp((std::min)(w - pad * 2.0f, h - pad * 2.0f - text_block), 0.0f, 240.0f * s);
    const bool show_cover = cover >= 64.0f * s;
    const float cx = content.left + w * 0.5f;
    const float cover_gap = 28.0f * s;
    const float info_w = wide ? (std::min)(380.0f * s, w - pad * 2.0f - cover - cover_gap) : 0.0f;
    const float block_w = wide ? cover + cover_gap + info_w : 0.0f;
    const float total = wide ? cover + 20.0f * s + tip_h + wave_h
                             : (show_cover ? cover : 0.0f) + text_block;
    float y = content.top + (std::max)(pad * 0.5f, (h - total) * 0.5f);
    const D2D1_RECT_F cover_rect = wide
        ? D2D1::RectF(cx - block_w * 0.5f, y, cx - block_w * 0.5f + cover, y + cover)
        : D2D1::RectF(cx - cover * 0.5f, y, cx + cover * 0.5f, y + cover);

    // Waveform colours: the cover's own (left to right), else the tile's.
    // Without a cover the tile is tinted by format, so a FLAC never looks like an MP3.
    static constexpr uint32_t kTiles[][3] = {
        {0xF6C26B, 0xD9655B, 0x6B4AA8},   // MP3 and the rest
        {0x7DD3FC, 0x3B82F6, 0x1E3A8A},   // FLAC, APE, WavPack: lossless
        {0x86EFAC, 0x22A55E, 0x14532D},   // WAV, AIFF: uncompressed
        {0xFDA4AF, 0xE11D48, 0x7C2D12},   // M4A, AAC, ALAC
        {0xC4B5FD, 0x8B5CF6, 0x4C1D95},   // OGG, Opus
    };
    size_t tile = 0;
    if (ext == L"FLAC" || ext == L"APE" || ext == L"WV" || ext == L"TAK") tile = 1;
    else if (ext == L"WAV" || ext == L"AIF" || ext == L"AIFF" || ext == L"W64") tile = 2;
    else if (ext == L"M4A" || ext == L"AAC" || ext == L"ALAC" || ext == L"M4B") tile = 3;
    else if (ext == L"OGG" || ext == L"OGA" || ext == L"OPUS") tile = 4;
    uint32_t wave_colors[3] = {kTiles[tile][0], kTiles[tile][1], kTiles[tile][2]};
    bool grey_cover = false;

    // --- Cover: embedded album art through the preview host, else a tile. ---
    if (show_cover) {
        const D2D1_RECT_F rect = cover_rect;
        const uint32_t pixels = ipc::BucketPreviewPixelSize(static_cast<uint32_t>(cover));
        const PreviewDrawResult result = thumbnails_.Draw(dc, rect, item_.path, item_.attrs,
            pixels, generation_, item_.modified, item_.size, 1.0f, nullptr, nullptr, nullptr, true);
        if (result == PreviewDrawResult::Bitmap) {
            CoverPalette cover_palette;
            if (thumbnails_.Palette(item_.path, pixels, item_.modified, item_.size, cover_palette))
                std::copy(std::begin(cover_palette.colors), std::end(cover_palette.colors), wave_colors);
            else grey_cover = true;   // black-and-white art: the accent instead
        } else {
            const D2D1_GRADIENT_STOP stops[] = {
                {0.0f, D2D1::ColorF(kTiles[tile][0])}, {0.55f, D2D1::ColorF(kTiles[tile][1])},
                {1.0f, D2D1::ColorF(kTiles[tile][2])}};
            ComPtr<ID2D1GradientStopCollection> collection;
            ComPtr<ID2D1LinearGradientBrush> gradient;
            dc->CreateGradientStopCollection(stops, 3, &collection);
            if (collection.get())
                dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(rect.left, rect.top), D2D1::Point2F(rect.right, rect.bottom)),
                    collection.get(), &gradient);
            const float r = 12.0f * s;
            if (gradient.get())
                dc->FillRoundedRectangle(D2D1::RoundedRect(rect, r, r), gradient.get());
            // Vector eighth note, centred; the format underneath.
            ComPtr<ID2D1SolidColorBrush> white;
            dc->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF, 0.92f), &white);
            if (white.get()) {
                const float u = cover / 10.0f;
                const float ccx = (rect.left + rect.right) * 0.5f;
                const float nx = ccx - u * 0.6f, ny = rect.top + cover * 0.62f;
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(nx, ny), u * 1.05f, u * 0.8f), white.get());
                const float stem_x = nx + u * 0.95f;
                dc->FillRectangle(D2D1::RectF(stem_x - u * 0.16f, rect.top + cover * 0.22f, stem_x + u * 0.16f, ny),
                                  white.get());
                dc->FillRectangle(D2D1::RectF(stem_x, rect.top + cover * 0.22f, stem_x + u * 1.3f,
                                              rect.top + cover * 0.22f + u * 0.45f), white.get());
                if (!ext.empty() && cover >= 120.0f * s)
                    DrawUiText(dc, ext, compositor_.SmallFormat(),
                               D2D1::RectF(rect.left, rect.bottom - 34.0f * s, rect.right, rect.bottom - 10.0f * s),
                               D2D1::ColorF(0xFFFFFF, 0.85f), DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }
    }

    // --- Tags. ---
    std::wstring tech = ext;
    for (const wchar_t* label : {L"比特率", L"采样率"})
        if (const std::wstring value = PropertyValue(properties, label); !value.empty())
            tech += (tech.empty() ? L"" : L" \x00B7 ") + value;
    if (wide) {
        const float left = cover_rect.right + cover_gap, right = left + info_w;
        const float rows_h = static_cast<float>(rows.size()) * 24.0f * s;
        const float info_h = title_h + line_h + (rows.empty() ? 0.0f : 14.0f * s + rows_h);
        float ty = cover_rect.top + (std::max)(0.0f, (cover - info_h) * 0.5f);
        DrawUiText(dc, title, compositor_.HeaderFormat(), D2D1::RectF(left, ty, right, ty + title_h), palette.ink);
        ty += title_h;
        DrawUiText(dc, subtitle.empty() ? ext : subtitle, compositor_.TextFormat(),
                   D2D1::RectF(left, ty, right, ty + line_h), palette.ink2);
        ty += line_h + 14.0f * s;
        float label_w = 0.0f;
        for (const auto& row : rows) label_w = (std::max)(label_w, UiTextWidth(row.first, compositor_.SmallFormat()));
        for (const auto& row : rows) {
            DrawUiText(dc, row.first, compositor_.SmallFormat(),
                       D2D1::RectF(left, ty, left + label_w, ty + 24.0f * s), palette.ink3);
            DrawUiText(dc, row.second, compositor_.SmallFormat(),
                       D2D1::RectF(left + label_w + 16.0f * s, ty, right, ty + 24.0f * s), palette.ink);
            ty += 24.0f * s;
        }
        y = cover_rect.bottom + 20.0f * s;
    } else {
        if (show_cover) y += cover;
        y += 12.0f * s;
        const D2D1_RECT_F text_rect = D2D1::RectF(content.left + pad, y, content.right - pad, y + title_h);
        DrawUiText(dc, title, compositor_.HeaderFormat(), text_rect, palette.ink, DWRITE_TEXT_ALIGNMENT_CENTER);
        y += title_h;
        DrawUiText(dc, subtitle, compositor_.TextFormat(),
                   D2D1::RectF(text_rect.left, y, text_rect.right, y + line_h), palette.ink2,
                   DWRITE_TEXT_ALIGNMENT_CENTER);
        y += line_h;
        DrawUiText(dc, tech, compositor_.SmallFormat(),
                   D2D1::RectF(text_rect.left, y, text_rect.right, y + line_h), palette.ink2,
                   DWRITE_TEXT_ALIGNMENT_CENTER);
        y += line_h + 16.0f * s;
    }
    y += tip_h;

    // --- Waveform with the played part highlighted. ---
    const float wave_w = wide ? (std::max)(block_w, (std::min)(w - pad * 2.0f, 560.0f * s))
                              : (std::min)(w - pad * 2.0f, 560.0f * s);
    const D2D1_RECT_F wave = D2D1::RectF(cx - wave_w * 0.5f, y, cx + wave_w * 0.5f, y + wave_h);
    audio_wave_rect_ = wave;
    // Time under the pointer, in a bubble above the waveform.
    if (audio_hover_x_ >= 0.0f && state.duration > 0 && wave_w > 8.0f) {
        const double at = std::clamp((audio_hover_x_ - wave.left) / wave_w, 0.0f, 1.0f);
        const int64_t total_s = static_cast<int64_t>(at * static_cast<double>(state.duration)) / 10000000;
        wchar_t tip[32];
        if (state.duration >= 36000000000LL)
            swprintf_s(tip, L"%lld:%02lld:%02lld", total_s / 3600, (total_s / 60) % 60, total_s % 60);
        else swprintf_s(tip, L"%lld:%02lld", total_s / 60, total_s % 60);
        const float tw = UiTextWidth(tip, compositor_.SmallFormat()) + 14.0f * s;
        const float bx = std::clamp(audio_hover_x_, wave.left + tw * 0.5f, wave.right - tw * 0.5f);
        const D2D1_RECT_F bubble = D2D1::RectF(bx - tw * 0.5f, wave.top - tip_h, bx + tw * 0.5f, wave.top - 2.0f * s);
        ComPtr<ID2D1SolidColorBrush> fill;
        dc->CreateSolidColorBrush(D2D1::ColorF(0x2B2B2B, 0.96f), &fill);
        if (fill.get()) {
            dc->FillRoundedRectangle(D2D1::RoundedRect(bubble, 5.0f * s, 5.0f * s), fill.get());
            DrawUiText(dc, tip, compositor_.SmallFormat(), bubble, D2D1::ColorF(0xFFFFFF),
                       DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
    const float played = state.duration > 0
        ? std::clamp(static_cast<float>(state.position) / static_cast<float>(state.duration), 0.0f, 1.0f)
        : 0.0f;
    // Played: a left-to-right gradient of the cover's colours. Unplayed:
    // faint ink; under the pointer the gradient shows through at 60 %.
    D2D1_GRADIENT_STOP stops[3];
    const uint32_t accent_stops[3] = {dark_ ? 0x9BDFFFu : 0x2B88D8u, dark_ ? 0x60CDFFu : 0x005FB8u,
                                      dark_ ? 0x6E8BFFu : 0x3B3BB0u};
    for (int i = 0; i < 3; ++i)
        stops[i] = {i * 0.5f, D2D1::ColorF(grey_cover ? accent_stops[i] : CoverPaletteThemed(wave_colors[i], dark_))};
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> tint;
    dc->CreateGradientStopCollection(stops, 3, &collection);
    if (collection.get())
        dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(wave.left, 0.0f), D2D1::Point2F(wave.right, 0.0f)), collection.get(), &tint);
    ComPtr<ID2D1SolidColorBrush> rest, rest_reflection;
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0xFFFFFF, 0.265f) : D2D1::ColorF(0x000000, 0.23f), &rest);
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0xFFFFFF, 0.062f) : D2D1::ColorF(0x000000, 0.048f),
                              &rest_reflection);
    if (!tint.get() || !rest.get() || !rest_reflection.get() || wave_w <= 8.0f) return;
    std::vector<float> peaks;
    float progress = 0.0f;
    bool failed = true;
    if (!waveform_.Snapshot(peaks, progress, failed)) failed = true;
    if (failed || peaks.empty()) {
        const float mid = (wave.top + wave.bottom) * 0.5f;
        const float t = 1.5f * scale_;
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(wave.left, mid - t, wave.right, mid + t), t, t),
                                 rest.get());
        if (played > 0.0f)
            dc->FillRoundedRectangle(D2D1::RoundedRect(
                D2D1::RectF(wave.left, mid - t, wave.left + wave_w * played, mid + t), t, t), tint.get());
        return;
    }
    // Pills on whole device pixels: 3 px wide, 2 px apart, centred; they rise
    // from a baseline at 78 % of the height with a faint reflection below.
    const float bar = (std::max)(2.0f, std::round(3.0f * scale_));
    const float gap = (std::max)(1.0f, std::round(2.0f * scale_));
    const float step = bar + gap;
    const size_t bars = (std::max)(size_t{1}, static_cast<size_t>((wave_w + gap) / step));
    const float left = std::round(wave.left + (wave_w - (static_cast<float>(bars) * step - gap)) * 0.5f);
    const float base = std::round(wave.top + wave_h * 0.78f);
    const float rise = base - wave.top - 2.0f * scale_;
    const float drop = wave.bottom - base - 1.0f * scale_;
    const float radius = bar * 0.5f;
    for (size_t i = 0; i < bars; ++i) {
        const size_t from = i * peaks.size() / bars;
        const size_t to = (std::max)(from + 1, (i + 1) * peaks.size() / bars);
        float level = 0.0f;
        for (size_t k = from; k < to && k < peaks.size(); ++k) level = (std::max)(level, peaks[k]);
        const float x = left + static_cast<float>(i) * step;
        const bool done = (static_cast<float>(i) + 0.5f) / static_cast<float>(bars) < played;
        const bool hovered = !done && audio_hover_x_ >= 0.0f && x <= audio_hover_x_;
        const float up = (std::max)(bar, std::pow(level, 1.3f) * rise);
        const D2D1_RECT_F pill = D2D1::RectF(x, std::round(base - up), x + bar, base);
        if (done || hovered) {
            tint->SetOpacity(done ? 1.0f : 0.6f);
            dc->FillRoundedRectangle(D2D1::RoundedRect(pill, radius, radius), tint.get());
        } else {
            dc->FillRoundedRectangle(D2D1::RoundedRect(pill, radius, radius), rest.get());
        }
        const float down = (std::min)(drop, up * 0.28f);
        if (down <= 0.5f) continue;
        const D2D1_RECT_F mirror = D2D1::RectF(x, base + 1.5f * scale_, x + bar, base + 1.5f * scale_ + down);
        const float mr = (std::min)(radius, down * 0.5f);
        if (done) {
            tint->SetOpacity(0.22f);
            dc->FillRoundedRectangle(D2D1::RoundedRect(mirror, mr, mr), tint.get());
        } else {
            dc->FillRoundedRectangle(D2D1::RoundedRect(mirror, mr, mr), rest_reflection.get());
        }
    }
}

}  // namespace pulse::ui
